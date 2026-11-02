#pragma once

#include <deque>
#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/uart/uart.h"

namespace esphome {
namespace bluestar_ac {

// Blue Star AC on its Tuya Wi-Fi port. Speaks the Tuya MCU serial protocol directly
// instead of using the tuya component: this MCU only answers heartbeats after the
// module's handshake, so the tuya component's init sequence stalls.
// Most state travels in DP101 as `*LLKKV..CCCC#` records (see PROTOCOL.md).
class BlueStarClimate : public climate::Climate, public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Write one DP101 record, e.g. send_record("15", "1") = display light on.
  void send_record(const std::string &key, const std::string &value);
  // Replay the module handshake (normally only needed after the MCU restarts).
  void handshake();
  void send_frame(uint8_t cmd, const std::vector<uint8_t> &payload) { this->queue_frame_(cmd, payload); }

  void add_on_record_callback(std::function<void(std::string, std::string)> &&cb) {
    this->record_callback_.add(std::move(cb));
  }

 protected:
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

  void queue_frame_(uint8_t cmd, const std::vector<uint8_t> &payload = {});
  void handle_byte_(uint8_t c);
  void handle_frame_(uint8_t cmd, const uint8_t *data, size_t len);
  void handle_datapoints_(const uint8_t *data, size_t len);
  void on_record_(const std::string &rec);
  void handle_key_(const std::string &key, const std::string &value);
  void publish_();

  CallbackManager<void(std::string, std::string)> record_callback_;

  std::vector<uint8_t> rx_;
  uint32_t last_rx_byte_{0};
  uint32_t last_frame_{0};
  std::deque<std::vector<uint8_t>> tx_queue_;
  uint32_t last_tx_{0};

  bool power_{false};
  int mode_{2};
  int fan_{7};
  int vane_{6};
  int side_{6};
  bool boost_{false};
  bool eco_{false};
  bool sleep_{false};
  bool fahrenheit_{false};  // k12: MCU then reports/accepts k02 and k99 in °F x10
};

class RecordTrigger : public Trigger<std::string, std::string> {
 public:
  explicit RecordTrigger(BlueStarClimate *parent) {
    parent->add_on_record_callback([this](std::string key, std::string value) { this->trigger(key, value); });
  }
};

}  // namespace bluestar_ac
}  // namespace esphome
