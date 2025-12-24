#pragma once

#include "esphome/core/component.h"
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

  void set_energy_sensor(sensor::Sensor *energy_sensor) { energy_sensor_ = energy_sensor; }
  void set_volume_sensor(sensor::Sensor *volume_sensor) { volume_sensor_ = volume_sensor; }
  void set_power_sensor(sensor::Sensor *power_sensor) { power_sensor_ = power_sensor; }
  void set_volume_flow_sensor(sensor::Sensor *volume_flow_sensor) { volume_flow_sensor_ = volume_flow_sensor; }
  void set_flow_temp_sensor(sensor::Sensor *flow_temp_sensor) { flow_temp_sensor_ = flow_temp_sensor; }
  void set_return_temp_sensor(sensor::Sensor *return_temp_sensor) { return_temp_sensor_ = return_temp_sensor; }
  void set_delta_t_sensor(sensor::Sensor *delta_t_sensor) { delta_t_sensor_ = delta_t_sensor; }

 protected:
  sensor::Sensor *energy_sensor_{nullptr};
  sensor::Sensor *volume_sensor_{nullptr};
  sensor::Sensor *power_sensor_{nullptr};
  sensor::Sensor *volume_flow_sensor_{nullptr};
  sensor::Sensor *flow_temp_sensor_{nullptr};
  sensor::Sensor *return_temp_sensor_{nullptr};
  sensor::Sensor *delta_t_sensor_{nullptr};

  void send_wakeup();
  void send_request();
  bool read_frame();
  void decode_mbus_payload(uint8_t *buf, size_t total);

  const uint16_t WAKEUP_BYTES = 528;
  const uint32_t WAKEUP_PAUSE_MS = 350;   // short pause after wakeup burst
  const uint32_t FRAME_TIMEOUT_MS = 5000; // generous read timeout
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
