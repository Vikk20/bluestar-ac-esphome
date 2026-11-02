#include "bluestar_ac.h"
#include "esphome/core/log.h"

#include <cmath>
#include <cstring>

namespace esphome {
namespace bluestar_ac {

static const char *const TAG = "bluestar_ac";

static const uint8_t DP_POWER = 1;
static const uint8_t DP_RECORD = 101;

// Tuya serial commands (module -> MCU unless noted).
static const uint8_t CMD_HEARTBEAT = 0x00;
static const uint8_t CMD_PRODUCT_QUERY = 0x01;
static const uint8_t CMD_WORKING_MODE = 0x02;
static const uint8_t CMD_NET_STATUS = 0x03;
static const uint8_t CMD_DP_SET = 0x06;
static const uint8_t CMD_DP_REPORT = 0x07;  // MCU -> module
static const uint8_t CMD_QUERY_ALL = 0x08;
static const uint8_t NET_STATUS_CLOUD = 0x04;
static const uint8_t DP_TYPE_STRING = 0x03;

// The MCU is slow: replies go missing when frames come faster than this.
static const uint32_t TX_SPACING_MS = 400;
static const uint32_t RX_FRAME_TIMEOUT_MS = 300;
static const uint32_t HEARTBEAT_MS = 15000;
// Room temperature arrives every 60 s, so silence this long means the MCU lost us.
static const uint32_t SILENCE_REHANDSHAKE_MS = 180000;

// k04 fan values: remote Low/Mid/High/Turbo are 2, 3, 4, 6; 7 auto; 1 is a hidden step
// below Low (shown as Quiet). 0 (indoor fan off) is deliberately not exposed.
static const char *const FAN_TURBO = "Turbo";

static uint16_t crc16_modbus(const char *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= static_cast<uint8_t>(data[i]);
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

static long parse_hex(const std::string &s) { return strtol(s.c_str(), nullptr, 16); }
static float f_to_c(float f) { return (f - 32.0f) * 5.0f / 9.0f; }

void BlueStarClimate::setup() {
  this->set_supported_custom_fan_modes({FAN_TURBO});
  this->mode = climate::CLIMATE_MODE_OFF;
  this->handshake();
  this->set_interval("heartbeat", HEARTBEAT_MS, [this]() {
    if (millis() - this->last_frame_ > SILENCE_REHANDSHAKE_MS) {
      ESP_LOGW(TAG, "No data from the AC for %u s, redoing handshake", static_cast<unsigned>(SILENCE_REHANDSHAKE_MS / 1000));
      this->last_frame_ = millis();
      this->handshake();
    } else {
      this->queue_frame_(CMD_HEARTBEAT);
    }
  });
}

// Same init sequence a Tuya Wi-Fi module sends, then ask for
// the full state dump (record key 00). Every write makes the AC beep, so keep it to one.
void BlueStarClimate::handshake() {
  ESP_LOGI(TAG, "Sending module handshake");
  this->queue_frame_(CMD_PRODUCT_QUERY);
  this->queue_frame_(CMD_WORKING_MODE);
  this->queue_frame_(CMD_NET_STATUS, {NET_STATUS_CLOUD});
  this->queue_frame_(CMD_QUERY_ALL);
  this->queue_frame_(CMD_HEARTBEAT);
  this->send_record("00", "0");
}

void BlueStarClimate::queue_frame_(uint8_t cmd, const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> f = {0x55, 0xAA, 0x00, cmd, static_cast<uint8_t>(payload.size() >> 8),
                            static_cast<uint8_t>(payload.size() & 0xFF)};
  f.insert(f.end(), payload.begin(), payload.end());
  uint8_t sum = 0;
  for (uint8_t b : f)
    sum += b;
  f.push_back(sum);
  this->tx_queue_.push_back(std::move(f));
}

void BlueStarClimate::loop() {
  uint8_t c;
  while (this->available() && this->read_byte(&c)) {
    uint32_t now = millis();
    if (!this->rx_.empty() && now - this->last_rx_byte_ > RX_FRAME_TIMEOUT_MS)
      this->rx_.clear();
    this->last_rx_byte_ = now;
    this->handle_byte_(c);
  }
  if (!this->tx_queue_.empty() && millis() - this->last_tx_ >= TX_SPACING_MS && this->rx_.empty()) {
    auto &f = this->tx_queue_.front();
    this->write_array(f.data(), f.size());
    this->tx_queue_.pop_front();
    this->last_tx_ = millis();
  }
}

void BlueStarClimate::handle_byte_(uint8_t c) {
  auto &rx = this->rx_;
  rx.push_back(c);
  size_t n = rx.size();
  if (n == 1 && c != 0x55) {
    rx.clear();
    return;
  }
  if (n == 2 && c != 0xAA) {
    rx.clear();
    if (c == 0x55)
      rx.push_back(c);
    return;
  }
  if (n < 7)
    return;
  size_t len = (rx[4] << 8) | rx[5];
  if (n < 7 + len)
    return;
  uint8_t sum = 0;
  for (size_t i = 0; i < 6 + len; i++)
    sum += rx[i];
  if (sum != rx[6 + len]) {
    ESP_LOGW(TAG, "Bad frame checksum %02X != %02X", rx[6 + len], sum);
  } else {
    this->last_frame_ = millis();
    this->handle_frame_(rx[3], rx.data() + 6, len);
  }
  rx.clear();
}

void BlueStarClimate::handle_frame_(uint8_t cmd, const uint8_t *data, size_t len) {
  switch (cmd) {
    case CMD_HEARTBEAT:
      // 00 = first heartbeat since the MCU booted: it has forgotten the handshake.
      if (len >= 1 && data[0] == 0x00) {
        ESP_LOGI(TAG, "AC controller restarted");
        this->handshake();
      }
      break;
    case CMD_PRODUCT_QUERY:
      ESP_LOGI(TAG, "Product: %.*s", static_cast<int>(len), reinterpret_cast<const char *>(data));
      break;
    case CMD_DP_REPORT:
      this->handle_datapoints_(data, len);
      break;
    default:
      ESP_LOGV(TAG, "Frame cmd 0x%02X len %u", cmd, static_cast<unsigned>(len));
      break;
  }
}

void BlueStarClimate::handle_datapoints_(const uint8_t *data, size_t len) {
  size_t i = 0;
  while (i + 4 <= len) {
    uint8_t id = data[i];
    size_t dlen = (data[i + 2] << 8) | data[i + 3];
    const uint8_t *val = data + i + 4;
    if (i + 4 + dlen > len)
      break;
    if (id == DP_RECORD) {
      this->on_record_(std::string(reinterpret_cast<const char *>(val), dlen));
    } else if (id == DP_POWER && dlen == 1) {
      this->power_ = val[0] != 0;
      this->publish_();
    }
    i += 4 + dlen;
  }
}

void BlueStarClimate::dump_config() {
  LOG_CLIMATE("", "Blue Star AC", this);
}

climate::ClimateTraits BlueStarClimate::traits() {
  auto traits = climate::ClimateTraits();
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  traits.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_COOL, climate::CLIMATE_MODE_DRY,
                              climate::CLIMATE_MODE_FAN_ONLY, climate::CLIMATE_MODE_AUTO});
  traits.set_supported_fan_modes({climate::CLIMATE_FAN_AUTO, climate::CLIMATE_FAN_QUIET, climate::CLIMATE_FAN_LOW,
                                  climate::CLIMATE_FAN_MEDIUM, climate::CLIMATE_FAN_HIGH});
  traits.set_supported_swing_modes({climate::CLIMATE_SWING_OFF, climate::CLIMATE_SWING_VERTICAL,
                                    climate::CLIMATE_SWING_HORIZONTAL, climate::CLIMATE_SWING_BOTH});
  traits.set_supported_presets({climate::CLIMATE_PRESET_NONE, climate::CLIMATE_PRESET_BOOST,
                                climate::CLIMATE_PRESET_ECO, climate::CLIMATE_PRESET_SLEEP});
  traits.set_visual_min_temperature(16);
  traits.set_visual_max_temperature(30);
  traits.set_visual_target_temperature_step(0.5);
  traits.set_visual_current_temperature_step(0.1);
  return traits;
}

void BlueStarClimate::send_record(const std::string &key, const std::string &value) {
  std::string body = key + value;
  char head[4];
  snprintf(head, sizeof(head), "*%02X", static_cast<unsigned>(body.size()));
  std::string rec = head + body;
  char tail[6];
  snprintf(tail, sizeof(tail), "%04X#", crc16_modbus(rec.data(), rec.size()));
  rec += tail;
  ESP_LOGD(TAG, "Send %s", rec.c_str());
  std::vector<uint8_t> payload = {DP_RECORD, DP_TYPE_STRING, static_cast<uint8_t>(rec.size() >> 8),
                                  static_cast<uint8_t>(rec.size() & 0xFF)};
  payload.insert(payload.end(), rec.begin(), rec.end());
  this->queue_frame_(CMD_DP_SET, payload);
}

void BlueStarClimate::on_record_(const std::string &rec) {
  if (rec.size() < 10 || rec.front() != '*' || rec.back() != '#') {
    ESP_LOGW(TAG, "Malformed record '%s'", rec.c_str());
    return;
  }
  size_t len = parse_hex(rec.substr(1, 2));
  if (len < 2 || rec.size() != 3 + len + 5) {
    ESP_LOGW(TAG, "Bad record length '%s'", rec.c_str());
    return;
  }
  uint16_t crc = parse_hex(rec.substr(3 + len, 4));
  if (crc != crc16_modbus(rec.data(), 3 + len)) {
    ESP_LOGW(TAG, "Bad record CRC '%s'", rec.c_str());
    return;
  }
  std::string key = rec.substr(3, 2);
  std::string value = rec.substr(5, len - 2);
  ESP_LOGD(TAG, "Record %s=%s", key.c_str(), value.c_str());
  this->handle_key_(key, value);
  this->record_callback_.call(key, value);
}

void BlueStarClimate::handle_key_(const std::string &key, const std::string &value) {
  int k = atoi(key.c_str());
  switch (k) {
    case 0:  // full state dump; positions per PROTOCOL.md
      if (value.size() < 34)
        return;
      this->power_ = value[0] == '1';
      this->target_temperature = parse_hex(value.substr(1, 3)) / 10.0f;
      this->mode_ = value[4] - '0';
      this->fan_ = value[5] - '0';
      this->vane_ = value[6] - '0';
      this->side_ = value[7] - '0';
      this->current_temperature = parse_hex(value.substr(31, 3)) / 10.0f;
      // Re-emit the light so the YAML template switch picks it up.
      this->record_callback_.call("15", value.substr(23, 1));
      this->record_callback_.call("05", value.substr(6, 1));
      this->record_callback_.call("08", value.substr(9, 1));
      this->record_callback_.call("09", value.substr(10, 4));
      this->record_callback_.call("10", value.substr(14, 4));
      break;
    case 1:
      this->power_ = value == "1";
      break;
    case 2: {
      float t = parse_hex(value) / 10.0f;
      this->target_temperature = this->fahrenheit_ ? roundf(f_to_c(t) * 2.0f) / 2.0f : t;
      break;
    }
    case 3:
      this->mode_ = parse_hex(value);
      break;
    case 4:
      this->fan_ = parse_hex(value);
      break;
    case 5:
      this->vane_ = parse_hex(value);
      break;
    case 6:
      this->side_ = parse_hex(value);
      break;
    case 7:
      this->boost_ = value != "0";
      break;
    case 11:
      this->sleep_ = value != "0";
      break;
    case 22:
      this->eco_ = value != "0";
      break;
    case 12:
      this->fahrenheit_ = value == "1";
      break;
    case 99: {
      float t = parse_hex(value) / 10.0f;
      this->current_temperature = this->fahrenheit_ ? f_to_c(t) : t;
      break;
    }
    default:
      return;
  }
  this->publish_();
}

void BlueStarClimate::publish_() {
  if (!this->power_) {
    this->mode = climate::CLIMATE_MODE_OFF;
  } else {
    switch (this->mode_) {
      case 0:
        this->mode = climate::CLIMATE_MODE_FAN_ONLY;
        break;
      case 1:
        this->mode = climate::CLIMATE_MODE_HEAT;
        break;
      case 3:
        this->mode = climate::CLIMATE_MODE_DRY;
        break;
      case 4:
        this->mode = climate::CLIMATE_MODE_AUTO;
        break;
      default:
        this->mode = climate::CLIMATE_MODE_COOL;
        break;
    }
  }

  switch (this->fan_) {
    case 1:
      this->set_fan_mode_(climate::CLIMATE_FAN_QUIET);
      break;
    case 2:
      this->set_fan_mode_(climate::CLIMATE_FAN_LOW);
      break;
    case 3:
      this->set_fan_mode_(climate::CLIMATE_FAN_MEDIUM);
      break;
    case 4:
      this->set_fan_mode_(climate::CLIMATE_FAN_HIGH);
      break;
    case 6:
      this->set_custom_fan_mode_(FAN_TURBO);
      break;
    default:
      this->set_fan_mode_(climate::CLIMATE_FAN_AUTO);
      break;
  }

  bool v = this->vane_ == 0, h = this->side_ == 0;
  this->swing_mode = v && h ? climate::CLIMATE_SWING_BOTH
                     : v    ? climate::CLIMATE_SWING_VERTICAL
                     : h    ? climate::CLIMATE_SWING_HORIZONTAL
                            : climate::CLIMATE_SWING_OFF;

  this->preset = this->boost_ ? climate::CLIMATE_PRESET_BOOST
                 : this->eco_ ? climate::CLIMATE_PRESET_ECO
                 : this->sleep_ ? climate::CLIMATE_PRESET_SLEEP
                                : climate::CLIMATE_PRESET_NONE;

  this->publish_state();
}

// State is published only when the MCU echoes each write (it corrects rejected values itself).
void BlueStarClimate::control(const climate::ClimateCall &call) {
  if (call.get_mode().has_value()) {
    auto mode = *call.get_mode();
    if (mode == climate::CLIMATE_MODE_OFF) {
      this->send_record("01", "0");
    } else {
      if (!this->power_)
        this->send_record("01", "1");
      int raw = mode == climate::CLIMATE_MODE_FAN_ONLY ? 0
                : mode == climate::CLIMATE_MODE_DRY    ? 3
                : mode == climate::CLIMATE_MODE_AUTO   ? 4
                                                       : 2;
      if (raw != this->mode_ || !this->power_)
        this->send_record("03", to_string(raw));
    }
  }

  if (call.get_target_temperature().has_value()) {
    float t = clamp(*call.get_target_temperature(), 16.0f, 30.0f);
    int tenths = this->fahrenheit_ ? static_cast<int>(lroundf(t * 9.0f / 5.0f + 32.0f)) * 10
                                   : static_cast<int>(lroundf(t * 2.0f)) * 5;
    char buf[4];
    snprintf(buf, sizeof(buf), "%03X", tenths);
    this->send_record("02", buf);
  }

  int fan = -1;
  if (call.has_custom_fan_mode()) {
    if (call.get_custom_fan_mode() == FAN_TURBO)
      fan = 6;
  } else if (call.get_fan_mode().has_value()) {
    switch (*call.get_fan_mode()) {
      case climate::CLIMATE_FAN_QUIET:
        fan = 1;
        break;
      case climate::CLIMATE_FAN_LOW:
        fan = 2;
        break;
      case climate::CLIMATE_FAN_MEDIUM:
        fan = 3;
        break;
      case climate::CLIMATE_FAN_HIGH:
        fan = 4;
        break;
      default:
        fan = 7;
        break;
    }
  }
  if (fan >= 0)
    this->send_record("04", to_string(fan));

  if (call.get_swing_mode().has_value()) {
    auto s = *call.get_swing_mode();
    bool v = s == climate::CLIMATE_SWING_VERTICAL || s == climate::CLIMATE_SWING_BOTH;
    bool h = s == climate::CLIMATE_SWING_HORIZONTAL || s == climate::CLIMATE_SWING_BOTH;
    if (v != (this->vane_ == 0))
      this->send_record("05", v ? "0" : "6");
    if (h != (this->side_ == 0))
      this->send_record("06", h ? "0" : "6");
  }

  if (call.get_preset().has_value()) {
    auto p = *call.get_preset();
    bool boost = p == climate::CLIMATE_PRESET_BOOST;
    bool eco = p == climate::CLIMATE_PRESET_ECO;
    bool sleep = p == climate::CLIMATE_PRESET_SLEEP;
    // Clear the old preset first so they never overlap on the MCU.
    if (this->boost_ && !boost)
      this->send_record("07", "0");
    if (this->eco_ && !eco)
      this->send_record("22", "0");
    if (this->sleep_ && !sleep)
      this->send_record("11", "0");
    if (boost && !this->boost_)
      this->send_record("07", "3");
    if (eco && !this->eco_)
      this->send_record("22", "1");
    if (sleep && !this->sleep_)
      this->send_record("11", "1");
  }
}

}  // namespace bluestar_ac
}  // namespace esphome
