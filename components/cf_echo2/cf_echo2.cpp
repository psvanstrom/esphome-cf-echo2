#include "cf_echo2.h"
#include "esphome/core/log.h"
#include <cstring>

namespace esphome {
namespace cf_echo2 {

static const char *const TAG = "cf_echo2.reader";
const uint8_t CFEcho2Reader::REQ_FRAME[5] = {0x10, 0x5B, 0xFE, 0x59, 0x16};

void CFEcho2Reader::setup() {
  ESP_LOGCONFIG(TAG, "Setting up CF Echo II reader...");
  if (this->activity_led_ != nullptr) {
    this->activity_led_->setup();
  }
  this->set_led(false);
  // UART framing (2400 8E1) comes from the `uart:` block in the YAML; we only
  // switch parity at runtime for the wake-up burst.
}

void CFEcho2Reader::update() {
  if (this->state_ != State::IDLE) {
    ESP_LOGW(TAG, "Read already in progress, skipping this update");
    return;
  }
  this->start_read();
}

void CFEcho2Reader::trigger_read() {
  if (this->state_ != State::IDLE) {
    ESP_LOGW(TAG, "Read already in progress, ignoring trigger");
    return;
  }
  this->start_read();
}

void CFEcho2ReadButton::press_action() { this->parent_->trigger_read(); }

void CFEcho2Reader::start_read() {
  ESP_LOGD(TAG, "Starting CF Echo II read cycle...");
  this->set_led(true);
  this->flush();
  this->switch_to_wakeup();
  this->phase_start_ = millis();
  this->state_ = State::WAKEUP_SETTLE;
}

void CFEcho2Reader::set_led(bool on) {
  if (this->activity_led_ != nullptr) {
    this->activity_led_->digital_write(on);
  }
}

void CFEcho2Reader::switch_to_wakeup() {
  ESP_LOGV(TAG, "Switching UART to 8N1 for wake-up");
  this->parent_->set_parity(uart::UART_CONFIG_PARITY_NONE);
  this->parent_->set_data_bits(8);
  this->parent_->set_stop_bits(1);
  this->parent_->load_settings(false);
}

void CFEcho2Reader::switch_to_mbus() {
  ESP_LOGV(TAG, "Switching UART to 8E1 for M-Bus");
  this->parent_->set_parity(uart::UART_CONFIG_PARITY_EVEN);
  this->parent_->set_data_bits(8);
  this->parent_->set_stop_bits(1);
  this->parent_->load_settings(false);
}

void CFEcho2Reader::loop() {
  switch (this->state_) {
    case State::IDLE:
      return;

    case State::WAKEUP_SETTLE:
      if (millis() - this->phase_start_ >= SETTLE_MS) {
        this->wakeup_sent_ = 0;
        this->wakeup_start_ = millis();
        this->state_ = State::WAKEUP_SEND;
        ESP_LOGV(TAG, "Sending %u wake-up bytes (8N1)...", WAKEUP_BYTES);
      }
      break;

    case State::WAKEUP_SEND: {
      // Estimate how many bytes have physically left the UART and only queue
      // enough to keep the TX FIFO partly full, so write_byte() never blocks.
      uint32_t elapsed = millis() - this->wakeup_start_;
      uint32_t drained = elapsed * BYTES_PER_SEC / 1000;
      uint32_t in_fifo = this->wakeup_sent_ > drained ? this->wakeup_sent_ - drained : 0;
      while (this->wakeup_sent_ < WAKEUP_BYTES && in_fifo < TX_FIFO_MARGIN) {
        this->write_byte(0x55);
        this->wakeup_sent_++;
        in_fifo++;
      }
      if (this->wakeup_sent_ >= WAKEUP_BYTES) {
        this->state_ = State::WAKEUP_DRAIN;
      }
      break;
    }

    case State::WAKEUP_DRAIN: {
      // Wait for the whole burst to be transmitted before touching the UART
      // config, then hold the post-wake-up pause.
      uint32_t tx_ms = (uint32_t) WAKEUP_BYTES * 1000 / BYTES_PER_SEC;
      if (millis() - this->wakeup_start_ >= tx_ms + WAKEUP_PAUSE_MS) {
        this->switch_to_mbus();
        this->phase_start_ = millis();
        this->state_ = State::REQUEST;
      }
      break;
    }

    case State::REQUEST:
      if (millis() - this->phase_start_ >= REQUEST_SETTLE_MS) {
        ESP_LOGV(TAG, "Sending M-Bus REQ frame");
        this->write_array(REQ_FRAME, sizeof(REQ_FRAME));
        this->rx_pos_ = 0;
        this->frame_total_ = 0;
        this->read_start_ = millis();
        this->state_ = State::READING;
      }
      break;

    case State::READING:
      this->read_frame_step();
      break;
  }
}

void CFEcho2Reader::read_frame_step() {
  if (millis() - this->read_start_ >= FRAME_TIMEOUT_MS) {
    ESP_LOGD(TAG, "Frame read timeout (got %u bytes)", this->rx_pos_);
    this->finish_read(false);
    return;
  }

  // Process the bytes that are ready now, but cap the work per loop() call so
  // we always yield back to the rest of the application.
  int budget = 64;
  while (budget-- > 0 && this->available()) {
    uint8_t b = this->read();

    // Discard anything until the long-frame start byte 0x68.
    if (this->rx_pos_ == 0) {
      if (b == 0x68) {
        this->rx_buf_[this->rx_pos_++] = b;
        ESP_LOGV(TAG, "Found start byte 0x68");
      }
      continue;
    }

    this->rx_buf_[this->rx_pos_++] = b;

    // Once the length field arrives, work out the full frame size.
    if (this->rx_pos_ == 2) {
      uint8_t l = this->rx_buf_[1];
      if (l == 0 || l > 250) {
        ESP_LOGW(TAG, "Suspicious L=%u, aborting frame", l);
        this->finish_read(false);
        return;
      }
      // 68 L L 68 + L user bytes (C,A,CI,data...) + checksum. The trailing
      // 0x16 stop byte is not needed to decode, so we stop at the checksum.
      this->frame_total_ = 4 + l + 1;
      if (this->frame_total_ > sizeof(this->rx_buf_)) {
        ESP_LOGW(TAG, "Frame too long: total=%u > buf=%u", this->frame_total_, (unsigned) sizeof(this->rx_buf_));
        this->finish_read(false);
        return;
      }
      ESP_LOGV(TAG, "Expected total frame size: %u bytes", this->frame_total_);
    }

    if (this->frame_total_ > 0 && this->rx_pos_ >= this->frame_total_) {
      ESP_LOGI(TAG, "Received complete frame (%u bytes)", this->frame_total_);
      this->process_frame(this->frame_total_);
      this->finish_read(true);
      return;
    }

    if (this->rx_pos_ >= sizeof(this->rx_buf_)) {
      ESP_LOGW(TAG, "Buffer overrun without complete frame");
      this->finish_read(false);
      return;
    }
  }
}

void CFEcho2Reader::finish_read(bool success) {
  this->set_led(false);
  this->state_ = State::IDLE;
  if (!success) {
    ESP_LOGW(TAG, "Failed to read valid frame");
  }
}

void CFEcho2Reader::process_frame(size_t total) {
  uint8_t *buf = this->rx_buf_;

  // Validate M-Bus long frame header: 68 L L 68 ...
  if (buf[0] != 0x68 || buf[3] != 0x68) {
    ESP_LOGW(TAG, "Invalid long frame header (expected 68 .. 68)");
    return;
  }

  uint8_t Lfield = buf[1];
  uint8_t C = buf[4];   // Control field
  uint8_t A = buf[5];   // Address field
  uint8_t CI = buf[6];  // CI-field
  ESP_LOGI(TAG, "Frame: C=%02X A=%02X CI=%02X L=%u", C, A, CI, Lfield);

  // Checksum is the byte right after the L user bytes, i.e. the last byte we
  // read (total - 1). It is the mod-256 sum of all L user bytes (C..data end).
  size_t chk_index = 4 + Lfield;
  if (chk_index != total - 1) {
    ESP_LOGW(TAG, "Frame length inconsistent (chk_index=%u total=%u)", (unsigned) chk_index, (unsigned) total);
    return;
  }

  uint8_t chk = buf[chk_index];
  uint8_t sum = 0;
  for (size_t i = 4; i < 4 + Lfield; i++) {
    sum += buf[i];
  }
  if (chk != sum) {
    ESP_LOGW(TAG, "Checksum mismatch: got %02X, calculated %02X - discarding frame", chk, sum);
    return;
  }
  ESP_LOGI(TAG, "Checksum OK");

  // Application-layer payload sits after C(4),A(5),CI(6) and before the checksum.
  size_t payload_start = 7;
  size_t payload_len = chk_index - payload_start;  // == Lfield - 3

  if (payload_len == 0 || payload_len > 254) {
    ESP_LOGW(TAG, "Invalid payload length: %u", (unsigned) payload_len);
    return;
  }
  this->decode_mbus_payload(buf + payload_start, payload_len);
}

void CFEcho2Reader::decode_mbus_payload(uint8_t *payload, size_t len) {
  ESP_LOGI(TAG, "Decoding M-Bus payload (%u bytes)", (unsigned) len);

  auto to_bcd = [](const uint8_t *data, size_t l, uint64_t &out) -> bool {
    uint64_t val = 0;
    uint64_t mult = 1;
    for (size_t i = 0; i < l; i++) {
      uint8_t lo = data[i] & 0x0F;
      uint8_t hi = (data[i] >> 4) & 0x0F;
      if (lo > 9 || hi > 9) return false;
      val += lo * mult;
      mult *= 10;
      val += hi * mult;
      mult *= 10;
    }
    out = val;
    return true;
  };

  if (len < 12) {
    ESP_LOGW(TAG, "Payload too short");
    return;
  }
  size_t idx = 12;  // skip fixed application header seen on this meter
  auto remaining = [&](size_t need) { return idx + need <= len; };

  auto read_le = [](const uint8_t *d, size_t l) -> uint64_t {
    uint64_t v = 0;
    for (size_t i = 0; i < l; i++) v |= (uint64_t) d[i] << (8 * i);
    return v;
  };

  auto publish = [&](const char *label, sensor::Sensor *s, float value) {
    if (s != nullptr) {
      ESP_LOGD(TAG, "%s = %.3f", label, value);
      s->publish_state(value);
    }
  };

  while (idx + 2 <= len) {  // need at least DIF+VIF
    uint8_t dif = payload[idx++];
    if (dif == 0x2F) continue;  // filler
    uint8_t len_code = dif & 0x0F;
    size_t l = 0;
    bool is_real = false;
    switch (len_code) {
      case 0x00: l = 0; break;
      case 0x01:
      case 0x09: l = 1; break;
      case 0x02:
      case 0x0A: l = 2; break;
      case 0x03:
      case 0x0B: l = 3; break;
      case 0x04:
      case 0x0C: l = 4; break;
      case 0x06:
      case 0x0E: l = 6; break;
      case 0x07:
      case 0x0F: l = 8; break;
      case 0x05: l = 4; is_real = true; break;  // 32-bit IEEE-754 real
      default:
        ESP_LOGW(TAG, "Unknown len code %02X", len_code);
        return;
    }
    if (!remaining(1 + l)) break;  // not enough for VIF+data
    uint8_t vif = payload[idx++];
    const uint8_t *data = payload + idx;
    idx += l;

    uint64_t raw = 0;
    float real_val = 0.0f;
    bool ok = true;
    bool is_bcd = (len_code == 0x09 || len_code == 0x0A || len_code == 0x0B || len_code == 0x0C || len_code == 0x0E);
    if (is_real) {
      uint32_t bits = (uint32_t) read_le(data, 4);
      memcpy(&real_val, &bits, sizeof(real_val));
    } else if (is_bcd) {
      ok = to_bcd(data, l, raw);
    } else {
      raw = read_le(data, l);
    }
    if (!ok) {
      ESP_LOGW(TAG, "Invalid BCD at vif %02X", vif);
      continue;
    }
    float value = is_real ? real_val : static_cast<float>(raw);

    switch (vif) {
      case 0x06:  // energy, raw is kWh
        publish("energy_kwh", energy_sensor_, value);
        break;
      case 0x14:  // volume m3, 2 decimals
        publish("volume_m3", volume_sensor_, value * 0.01f);
        break;
      case 0x2D:  // power -> W, 2 decimals scaled up
        publish("power_w", power_sensor_, value * 100.0f);
        break;
      case 0x3B:  // volume flow m3/h, 3 decimals
        publish("volume_flow_m3h", volume_flow_sensor_, value * 0.001f);
        break;
      case 0x5A:  // flow temp, 1 decimal
        publish("flow_temp_c", flow_temp_sensor_, value * 0.1f);
        break;
      case 0x5E:  // return temp, 1 decimal
        publish("return_temp_c", return_temp_sensor_, value * 0.1f);
        break;
      case 0x61:  // delta T, 2 decimals
        publish("delta_t_k", delta_t_sensor_, value * 0.01f);
        break;
      default:
        break;
    }
  }
}

void CFEcho2Reader::dump_config() {
  ESP_LOGCONFIG(TAG, "CF Echo II Reader:");
  ESP_LOGCONFIG(TAG, "  Wakeup bytes: %u", WAKEUP_BYTES);
  ESP_LOGCONFIG(TAG, "  Wakeup pause: %u ms", WAKEUP_PAUSE_MS);
  ESP_LOGCONFIG(TAG, "  Frame timeout: %u ms", FRAME_TIMEOUT_MS);
  LOG_PIN("  Activity LED: ", this->activity_led_);
  LOG_SENSOR("  ", "energy", energy_sensor_);
  LOG_SENSOR("  ", "volume", volume_sensor_);
  LOG_SENSOR("  ", "power", power_sensor_);
  LOG_SENSOR("  ", "volume_flow", volume_flow_sensor_);
  LOG_SENSOR("  ", "flow_temp", flow_temp_sensor_);
  LOG_SENSOR("  ", "return_temp", return_temp_sensor_);
  LOG_SENSOR("  ", "delta_t", delta_t_sensor_);
}

}  // namespace cf_echo2
}  // namespace esphome
