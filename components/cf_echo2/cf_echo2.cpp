#include "cf_echo2.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

namespace esphome {
namespace cf_echo2 {

static const char *const TAG = "cf_echo2.reader";
const uint8_t CFEcho2Reader::REQ_FRAME[5] = {0x10, 0x5B, 0xFE, 0x59, 0x16};

void CFEcho2Reader::setup() {
  ESP_LOGCONFIG(TAG, "Setting up CF Echo II reader...");
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);  // LED off initially

  // Configure UART for 2400 8E1 (M-Bus default)
  this->parent_->set_baud_rate(2400);
  this->parent_->set_parity(uart::UART_CONFIG_PARITY_EVEN);
  this->parent_->set_data_bits(8);
  this->parent_->set_stop_bits(1);
  this->parent_->load_settings(false);
}

void CFEcho2Reader::loop() {
  // Non-blocking operation via polling component
}

void CFEcho2Reader::update() {
  ESP_LOGD(TAG, "Performing CF Echo II read cycle...");
  
  digitalWrite(LED_BUILTIN, LOW);   // LED on (active low)
  
  send_wakeup();
  send_request();
  bool success = read_frame();
  
  digitalWrite(LED_BUILTIN, HIGH);  // LED off
  
  if (!success) {
    ESP_LOGW(TAG, "Failed to read valid frame");
  }
}

void CFEcho2Reader::trigger_read() {
  this->update();
}

void CFEcho2ReadButton::press_action() { this->parent_->trigger_read(); }

void CFEcho2Reader::send_wakeup() {
  ESP_LOGV(TAG, "Wake-up: switching to 8N1");
  this->flush();

  // Switch to 8N1 for wakeup sequence
  this->parent_->set_parity(uart::UART_CONFIG_PARITY_NONE);
  this->parent_->set_data_bits(8);
  this->parent_->set_stop_bits(1);
  this->parent_->load_settings(false);
  delay(50);
  
  ESP_LOGV(TAG, "Sending %u wakeup bytes (8N1)...", WAKEUP_BYTES);
  for (uint16_t i = 0; i < WAKEUP_BYTES; i++) {
    this->write_byte(0x55);
    if (i % 32 == 0) yield();  // Feed WDT
  }
  this->flush();
  delay(WAKEUP_PAUSE_MS);
  
  // Switch back to 8E1 for M-Bus communication
  ESP_LOGV(TAG, "Switching to 8E1 for M-Bus");
  this->parent_->set_parity(uart::UART_CONFIG_PARITY_EVEN);
  this->parent_->set_data_bits(8);
  this->parent_->set_stop_bits(1);
  this->parent_->load_settings(false);
  delay(10);  // minimal settle time to avoid missing early bytes
}

void CFEcho2Reader::send_request() {
  ESP_LOGV(TAG, "Sending M-Bus REQ frame");
  this->write_array(REQ_FRAME, sizeof(REQ_FRAME));
  this->flush();
}

bool CFEcho2Reader::read_frame() {
  uint8_t buf[256];
  size_t pos = 0;
  uint32_t start = millis();
  
  // Wait for start byte 0x68 (long frame)
  while (millis() - start < FRAME_TIMEOUT_MS) {
    if (this->available()) {
      uint8_t b = this->read();
      if (b == 0x68) {
        buf[pos++] = b;
        ESP_LOGV(TAG, "Found start byte 0x68");
        break;
      }
    }
    yield();
  }
  if (pos == 0) {
    ESP_LOGD(TAG, "No 0x68 start byte received");
    return false;
  }
  
  uint8_t L = 0;
  size_t total = 0;
  
  // Read complete frame
  while (millis() - start < FRAME_TIMEOUT_MS && pos < sizeof(buf)) {
    if (this->available()) {
      buf[pos++] = this->read();
      
      // Once we have length field, calculate total frame size
      if (pos == 2) {
        L = buf[1];
        if (L == 0 || L > 250) {
          ESP_LOGW(TAG, "Suspicious L=%u, aborting frame", L);
          return false;
        }
        total = 4 + L + 1;  // 68 L L 68 + L bytes (C,A,CI,data...) + STOP
        if (total > sizeof(buf)) {
          ESP_LOGW(TAG, "Frame too long: total=%u > buf=%u", total, sizeof(buf));
          return false;
        }
        ESP_LOGV(TAG, "Expected total frame size: %u bytes", total);
      }
      
      // Check if we have complete frame
      if (total > 0 && pos >= total) {
        ESP_LOGI(TAG, "Received complete frame (%u bytes)", total);
        
        // Validate M-Bus long frame header
        if (buf[0] != 0x68 || buf[3] != 0x68) {
          ESP_LOGW(TAG, "Invalid long frame header (expected 68 .. 68)");
          return false;
        }
        
        uint8_t Lfield = buf[1];
        uint8_t C = buf[4];      // Control field
        uint8_t A = buf[5];      // Address field
        uint8_t CI = buf[6];     // CI-field
        
        ESP_LOGI(TAG, "Frame: C=%02X A=%02X CI=%02X L=%u", C, A, CI, Lfield);
        
        // Calculate checksum (sum of bytes from C-field to data end)
        size_t chk_index = 4 + Lfield - 2;
        if (chk_index >= total - 1) {
          ESP_LOGW(TAG, "Checksum index out of range");
          return false;
        }
        
        uint8_t chk = buf[chk_index];
        uint8_t sum = 0;
        for (size_t i = 4; i < 4 + Lfield - 1; i++) {
          sum += buf[i];
        }
        
        if (chk != sum) {
          ESP_LOGW(TAG, "Checksum mismatch: got %02X, calculated %02X", chk, sum);
        } else {
          ESP_LOGI(TAG, "Checksum OK");
        }
        
        // Extract application layer payload (after CI-field, before checksum)
        size_t payload_start = 7;  // After C(4),A(5),CI(6)
        size_t payload_len = chk_index - payload_start;
        
        if (payload_len > 0 && payload_len <= 254) {
          decode_mbus_payload(buf + payload_start, payload_len);
        } else {
          ESP_LOGW(TAG, "Invalid payload length: %u", payload_len);
        }
        
        return true;
      }
    }
    yield();
  }
  
  ESP_LOGD(TAG, "Frame read timeout");
  return false;
}

void CFEcho2Reader::decode_mbus_payload(uint8_t *payload, size_t len) {
  ESP_LOGI(TAG, "Decoding M-Bus payload (%u bytes)", len);

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
    for (size_t i = 0; i < l; i++) v |= (uint64_t)d[i] << (8 * i);
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
      case 0x05: l = 4; break;
      default:
        ESP_LOGW(TAG, "Unknown len code %02X", len_code);
        return;
    }
    if (!remaining(1 + l)) break;  // not enough for VIF+data
    uint8_t vif = payload[idx++];
    const uint8_t *data = payload + idx;
    idx += l;

    uint64_t raw = 0;
    bool ok = true;
    bool is_bcd = (len_code == 0x09 || len_code == 0x0A || len_code == 0x0B || len_code == 0x0C || len_code == 0x0E);
    if (is_bcd) {
      ok = to_bcd(data, l, raw);
    } else {
      raw = read_le(data, l);
    }
    if (!ok) {
      ESP_LOGW(TAG, "Invalid BCD at vif %02X", vif);
      continue;
    }

    switch (vif) {
      case 0x06: {  // energy Wh -> kWh
        float energy_kwh = static_cast<float>(raw) * 1.0f;       // raw is kWh
        publish("energy_kwh", energy_sensor_, energy_kwh);
        break;
      }
      case 0x14: {  // volume m3, BCD with 2 decimals
        float vol = static_cast<float>(raw) * 0.01f;
        publish("volume_m3", volume_sensor_, vol);
        break;
      }
      case 0x2D: {  // power, BCD with 2 decimals -> W
        float power = static_cast<float>(raw) * 100.0f;
        publish("power_w", power_sensor_, power);
        break;
      }
      case 0x3B: {  // volume flow, BCD with 3 decimals -> m3/h
        float vf = static_cast<float>(raw) * 0.001f;
        publish("volume_flow_m3h", volume_flow_sensor_, vf);
        break;
      }
      case 0x5A: {  // flow temp, BCD with 1 decimal
        float tf = static_cast<float>(raw) * 0.1f;
        publish("flow_temp_c", flow_temp_sensor_, tf);
        break;
      }
      case 0x5E: {  // return temp, BCD with 1 decimal
        float tr = static_cast<float>(raw) * 0.1f;
        publish("return_temp_c", return_temp_sensor_, tr);
        break;
      }
      case 0x61: {  // delta T, BCD with 2 decimals
        float dt = static_cast<float>(raw) * 0.01f;
        publish("delta_t_k", delta_t_sensor_, dt);
        break;
      }
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
