#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_API
#include "esphome/components/api/custom_api_device.h"
#endif

#include <map>
#include <vector>

#ifdef USE_ESP32

namespace esphome {
namespace poollab {

namespace espbt = esphome::esp32_ble_tracker;

// --- PoolLabSvc custom GATT service (see PROTOCOL.md) ---
static const char *const POOLLAB_SERVICE_UUID = "A7EE04A9-507B-4910-A528-B619D5501924";
static const char *const POOLLAB_CHR_MOSI_UUID = "91BFA536-3036-4901-8813-3635FCED7B90";
static const char *const POOLLAB_CHR_MISO_UUID = "2FF18B59-195D-4EE1-B78C-0CBDE3EFF9C2";
static const char *const POOLLAB_CHR_MISOSIGNAL_UUID = "C2296C06-C7E0-4657-B42E-C8330826454C";

static const uint8_t POOLLAB_PREAMBLE = 0xAB;

enum PoolLabCommand : uint16_t {
  PCMD_GET_INFO = 0x0001,
  PCMD_SET_TIME = 0x0002,
  PCMD_RESET_DEVICE = 0x0003,
  PCMD_SLEEP_DEVICE = 0x0004,
  PCMD_GET_MEASURES = 0x0005,
  PCMD_RESET_MEASURES = 0x0006,
  PCMD_GET_PPM_MGL = 0x000A,
  PCMD_SET_PPM_MGL = 0x000B,
};

// Driver state machine. The device is command/response: write MOSI -> wait
// MISO_Signal notify -> read MISO.
enum class PoolLabState : uint8_t {
  IDLE,
  DISCOVERING,      // waiting for char handles + notify registration
  GET_INFO,         // sent GET_INFO, awaiting response
  READ_CELLS,       // looping GET_MEASURES over cells/halves
  RESET,            // sent RESET_MEASURES
};

// One parsed 16-byte measurement record.
struct PoolLabMeasurement {
  uint16_t id;
  uint8_t type;
  uint8_t status;     // 0 ok, 1 underrange, 2 overrange
  uint32_t timestamp; // measured_at (unix seconds, device clock)
  float value;
};

class PoolLab : public ble_client::BLEClientNode,
                public Component
#ifdef USE_API
                ,
                public api::CustomAPIDevice
#endif
{
 public:
  void setup() override {}
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  // --- public actions ---
  void trigger_read();      // kick off a full batch readout
  void reset_measures();    // erase device memory (use only after a read)

  // --- config setters (codegen) ---
  void set_reset_after_read(bool v) { this->reset_after_read_ = v; }
  void set_event_name(const std::string &v) { this->event_name_ = v; }
#ifdef USE_SENSOR
  void set_result_count_sensor(sensor::Sensor *s) { this->result_count_sensor_ = s; }
  void set_battery_sensor(sensor::Sensor *s) { this->battery_sensor_ = s; }
  void register_parameter_sensor(uint8_t type, sensor::Sensor *s) {
    this->param_sensors_[type] = s;
  }
#endif

 protected:
  // BLE plumbing
  bool resolve_handles_();                          // find MOSI/MISO/MISO_Signal handles
  void send_command_(uint16_t cmd, const uint8_t *params, size_t params_len);
  void read_response_();                            // read CommandMISO (250 bytes)
  void on_response_(const uint8_t *data, uint16_t len);

  // protocol steps
  void start_();                                    // begin: send GET_INFO
  void request_next_half_();                        // send next GET_MEASURES
  void parse_info_(const uint8_t *d, uint16_t len);
  void parse_measures_(const uint8_t *d, uint16_t len);
  void emit_measurement_(const PoolLabMeasurement &m);  // event + sensor
  void finish_();

  uint16_t mosi_handle_{0};
  uint16_t miso_handle_{0};
  uint16_t miso_signal_handle_{0};

  PoolLabState state_{PoolLabState::IDLE};
  bool read_requested_{false};   // a read was asked for while disconnected
  bool reset_after_read_{false};
  std::string event_name_{"esphome.poollab_measurement"};

  // batch progress
  uint16_t result_count_{0};
  uint16_t records_seen_{0};
  uint8_t cur_cell_{0};
  uint8_t cur_half_{0};          // 0 lower, 1 upper

#ifdef USE_SENSOR
  sensor::Sensor *result_count_sensor_{nullptr};
  sensor::Sensor *battery_sensor_{nullptr};
  std::map<uint8_t, sensor::Sensor *> param_sensors_;
#endif
};

// ---- actions ----
template<typename... Ts> class PoolLabReadAction : public Action<Ts...>, public Parented<PoolLab> {
 public:
  void play(Ts... x) override { this->parent_->trigger_read(); }
};
template<typename... Ts>
class PoolLabResetMeasuresAction : public Action<Ts...>, public Parented<PoolLab> {
 public:
  void play(Ts... x) override { this->parent_->reset_measures(); }
};

}  // namespace poollab
}  // namespace esphome

#endif  // USE_ESP32
