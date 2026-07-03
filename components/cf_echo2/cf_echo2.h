#pragma once

#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace cf_echo2 {

class CFEcho2Reader : public PollingComponent, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return esphome::setup_priority::DATA; }
  void trigger_read();

  void set_activity_led(GPIOPin *pin) { activity_led_ = pin; }
  void set_energy_sensor(sensor::Sensor *energy_sensor) { energy_sensor_ = energy_sensor; }
  void set_volume_sensor(sensor::Sensor *volume_sensor) { volume_sensor_ = volume_sensor; }
  void set_power_sensor(sensor::Sensor *power_sensor) { power_sensor_ = power_sensor; }
  void set_volume_flow_sensor(sensor::Sensor *volume_flow_sensor) { volume_flow_sensor_ = volume_flow_sensor; }
  void set_flow_temp_sensor(sensor::Sensor *flow_temp_sensor) { flow_temp_sensor_ = flow_temp_sensor; }
  void set_return_temp_sensor(sensor::Sensor *return_temp_sensor) { return_temp_sensor_ = return_temp_sensor; }
  void set_delta_t_sensor(sensor::Sensor *delta_t_sensor) { delta_t_sensor_ = delta_t_sensor; }

 protected:
  // Read cycle is driven as a non-blocking state machine from loop() so the
  // main loop is never stalled while talking to the meter at 2400 baud.
  enum class State : uint8_t {
    IDLE,           // nothing to do
    WAKEUP_SETTLE,  // switched to 8N1, waiting for UART to settle
    WAKEUP_SEND,    // streaming the 0x55 wake-up burst without blocking
    WAKEUP_DRAIN,   // burst queued, waiting for TX to finish + pause
    REQUEST,        // switched back to 8E1, waiting to send REQ frame
    READING,        // reading and parsing the M-Bus response frame
  };

  void start_read();
  void set_led(bool on);
  void switch_to_wakeup();  // 8N1
  void switch_to_mbus();    // 8E1
  void read_frame_step();
  void finish_read(bool success);
  void process_frame(size_t total);
  void decode_mbus_payload(uint8_t *payload, size_t len);

  GPIOPin *activity_led_{nullptr};

  sensor::Sensor *energy_sensor_{nullptr};
  sensor::Sensor *volume_sensor_{nullptr};
  sensor::Sensor *power_sensor_{nullptr};
  sensor::Sensor *volume_flow_sensor_{nullptr};
  sensor::Sensor *flow_temp_sensor_{nullptr};
  sensor::Sensor *return_temp_sensor_{nullptr};
  sensor::Sensor *delta_t_sensor_{nullptr};

  State state_{State::IDLE};
  uint32_t phase_start_{0};   // millis() when the current timed phase began
  uint32_t wakeup_start_{0};  // millis() when the wake-up burst started
  uint32_t wakeup_sent_{0};   // wake-up bytes handed to the UART so far
  uint32_t read_start_{0};    // millis() when the response read began

  uint8_t rx_buf_[256];
  size_t rx_pos_{0};
  size_t frame_total_{0};  // expected frame length once the L field is known

  static const uint16_t WAKEUP_BYTES = 528;
  static const uint32_t BYTES_PER_SEC = 240;    // 2400 baud, 8N1 -> 10 bits/byte
  static const uint32_t TX_FIFO_MARGIN = 96;    // keep FIFO below this so writes never block
  static const uint32_t SETTLE_MS = 50;         // UART settle after a mode switch
  static const uint32_t REQUEST_SETTLE_MS = 10; // settle before sending the REQ frame
  static const uint32_t WAKEUP_PAUSE_MS = 350;  // pause after the wake-up burst
  static const uint32_t FRAME_TIMEOUT_MS = 5000;
  static const uint8_t REQ_FRAME[5];
};

template<typename... Ts> class CFEcho2ReadAction : public Action<Ts...>, public Parented<CFEcho2Reader> {
 public:
  void play(Ts... x) override { this->parent_->trigger_read(); }
};

class CFEcho2ReadButton : public button::Button, public Parented<CFEcho2Reader> {
 protected:
  void press_action() override;
};

}  // namespace cf_echo2
}  // namespace esphome
