#include "cf_echo2.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include <MBusinoLib.h>     // M-Bus payload decoder [web:123]
#include <ArduinoJson.h>    // JSON array for decoded fields [web:124]

namespace esphome {
namespace cf_echo2 {

static const char *const TAG = "cf_echo2.reader";

// M-Bus payload decoder instance
MBusinoLib mbdecoder(254);  // Max payload size 254 bytes [web:123]

void CFEcho2Reader::setup() {
  ESP_LOGCONFIG(TAG, "Setting up CF Echo II reader...");
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);  // LED off initially
  
  // Configure UART for 2400 8E1 (M-Bus default)
  this->set_uart_baud_rate(2400);
  this->set_uart_config(UART_CONFIG_PARITY_EVEN, UART_CONFIG_DATA_BITS_8, UART_CONFIG_STOP_BITS_1);
}

void CFEcho2Reader::loop() {
  // Non-blocking operation via polling component
}

void CFEcho2Reader::update() {
  ESP_LOGD(TAG, "Performing CF Echo II read cycle...");
  
  digitalWrite(LED_BUILTIN, LOW);   // LED on (active low)
  
  send_wakeup();
  bool success = read_frame();
  
  digitalWrite(LED_BUILTIN, HIGH);  // LED off
  
  if (!success) {
    ESP_LOGW(TAG, "Failed to read valid frame");
  }
}

void CFEcho2Reader::send_wakeup() {
  ESP_LOGV(TAG, "Wake-up: switching to 8N1");
  this->uart()->flush();
  
  // Switch to 8N1 for wakeup sequence
  this->set_uart_config(UART_CONFIG_PARITY_NONE, UART_CONFIG_DATA_BITS_8, UART_CONFIG_STOP_BITS_1);
  delay(50);
  
  ESP_LOGV(TAG, "Sending %u wakeup bytes (8N1)...", WAKEUP_BYTES);
  for (uint8_t i = 0; i < WAKEUP_BYTES; i++) {
    this->write_byte(0x55);
    if (i % 32 == 0) yield();  // Feed WDT
  }
  this->uart()->flush();
  delay(WAKEUP_PAUSE_MS);
  
  // Switch back to 8E1 for M-Bus communication
  ESP_LOGV(TAG, "Switching to 8E1 for M-Bus");
  this->set_uart_config(UART_CONFIG_PARITY_EVEN, UART_CONFIG_DATA_BITS_8, UART_CONFIG_STOP_BITS_1);
  delay(100);
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
  
  // Hex dump of raw payload for debugging
  ESP_LOGI(TAG, "Raw payload:");
  for (size_t i = 0; i < len; i++) {
    if (i % 16 == 0) ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "%02X ", payload[i]);
  }
  ESP_LOGI(TAG, "");
  
  // Decode with MBusinoLib
  DynamicJsonDocument doc(4096);  // Large enough for all records
  JsonArray root = doc.to<JsonArray>();
  
  uint8_t fields = mbdecoder.decode(payload, (uint8_t)len, root);
  
  if (fields == 0) {
    uint8_t err = mbdecoder.getError();
    ESP_LOGW(TAG, "MBusinoLib decode failed, error code: %u", err);
    return;
  }
  
  ESP_LOGI(TAG, "Successfully decoded %u fields:", fields);
  
  // Process each decoded field
  for (uint8_t i = 0; i < fields; i++) {
    JsonObject field = root[i];
    
    const char* name = field["name"] | "<unknown>";
    const char* units = field["units"] | "";
    double value = field["value_scaled"] | 0.0;
    const char* value_str = field["value_string"] | "";
    
    ESP_LOGI(TAG, "  Field %u: %s = %.6f %s (%s)", 
             i, name, value, units, value_str);
    
    // TODO: Update sensor values based on field names/units
    // Example:
    // if (strcmp(name, "heat_energy_total") == 0 && strcmp(units, "kWh") == 0) {
    //   if (energy_sensor_) energy_sensor_->publish_state(value);
    // }
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
  LOG_SENSOR("  ", "flow_temp", flow_temp_sensor_);
  LOG_SENSOR("  ", "return_temp", return_temp_sensor_);
  LOG_SENSOR("  ", "delta_t", delta_t_sensor_);
}

}  // namespace cf_echo2
}  // namespace esphome
