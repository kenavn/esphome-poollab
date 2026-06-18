#include "poollab.h"

#ifdef USE_ESP32
#include "esphome/core/log.h"
#include <cstring>
#include <ctime>

// HA event firing uses api::CustomAPIDevice (included via poollab.h under USE_API).

namespace esphome {
namespace poollab {

static const char *const TAG = "poollab";

// measure_type -> (name, unit). Verified against python-poollab-ble and PoolLabIo.
// See PROTOCOL.md. Unit "" = unitless (pH).
struct TypeInfo {
  uint8_t code;
  const char *name;
  const char *unit;
};
static const TypeInfo POOLLAB_TYPES[] = {
    {1, "Total Chlorine", "ppm"}, {2, "Ozone", "ppm"}, {3, "Chlorine Dioxide", "ppm"},
    {5, "Active Oxygen", "ppm"}, {6, "Bromine", "ppm"}, {7, "Hydrogen Peroxide", "ppm"},
    {8, "Free Chlorine", "ppm"}, {9, "pH", ""}, {10, "Total Alkalinity", "ppm"},
    {11, "Cyanuric Acid", "ppm"}, {12, "Hydrogen Peroxide HR", "ppm"},
    {13, "Total Hardness HR", "ppm"}, {14, "Isothiazilinone", "ppm"}, {15, "Nitrite LR", "ppm"},
    {16, "Nitrate", "ppm"}, {17, "Phosphate", "ppm"}, {18, "Iron LR", "ppm"},
    {19, "Dissolved Oxygen", "ppm"}, {20, "Ammonia", "ppm"}, {21, "Silica", "ppm"},
    {22, "Copper", "ppm"}, {23, "Calcium", "ppm"}, {24, "Ozone i.p.o. Chlorine", "ppm"},
    {25, "Magnesium", "ppm"}, {26, "Potassium", "ppm"}, {27, "pH HR", ""}, {28, "pH LR", ""},
    {29, "pH HR (Saltwater)", ""}, {30, "pH HR (Seawater)", ""}, {31, "pH LR (Saltwater)", ""},
    {32, "pH LR (Seawater)", ""}, {33, "pH MR (Saltwater)", ""}, {34, "pH MR (Seawater)", ""},
    {35, "Total Hardness", "ppm"}, {36, "pH MR", ""}, {37, "Iodine", "ppm"}, {38, "Urea", "ppm"},
    {39, "PHMB", "ppm"}, {40, "Total Alkalinity (Seawater)", "ppm"},
    {41, "Total Chlorine (liquid)", "ppm"}, {42, "Ozone (liquid)", "ppm"},
    {43, "Chlorine Dioxide (liquid)", "ppm"}, {44, "Active Oxygen (liquid)", "ppm"},
    {45, "Bromine (liquid)", "ppm"}, {46, "Hydrogen Peroxide (liquid)", "ppm"},
    {47, "Free Chlorine (liquid)", "ppm"}, {48, "pH (liquid)", ""},
    {49, "Ozone i.p.o. Chlorine (liquid)", "ppm"},
};
static const TypeInfo *poollab_type_info(uint8_t code) {
  for (auto &t : POOLLAB_TYPES)
    if (t.code == code) return &t;
  return nullptr;
}

// ============================================================================
//  NOTE: this is a spec-correct SCAFFOLD. The GATT calls below follow ESPHome's
//  ble_client idioms but have NOT been validated on hardware — exact handle
//  resolution / notify registration may need adjustment during bench bring-up.
// ============================================================================

void PoolLab::setup() {
  // Stay idle until a read is triggered. Otherwise the ble_client scans
  // continuously for its MAC, needlessly contending with WiFi for the shared
  // 2.4 GHz radio (and spinning forever if the device is out of range).
  this->parent()->set_enabled(false);
}

void PoolLab::dump_config() {
  ESP_LOGCONFIG(TAG, "PoolLab 1.0 BLE photometer");
  ESP_LOGCONFIG(TAG, "  reset_after_read: %s", YESNO(this->reset_after_read_));
  ESP_LOGCONFIG(TAG, "  event: %s", this->event_name_.c_str());
#ifdef USE_SENSOR
  ESP_LOGCONFIG(TAG, "  parameter sensors: %u", (unsigned) this->param_sensors_.size());
#endif
}

void PoolLab::loop() {
  // If a read was requested while disconnected, the BLEClient will connect; once
  // established and handles are resolved we kick the sequence off from
  // gattc_event_handler (SEARCH_CMPL). Nothing to poll here.
}

void PoolLab::trigger_read() {
  if (this->state_ != PoolLabState::IDLE) {
    ESP_LOGW(TAG, "read already in progress (state=%u)", (unsigned) this->state_);
    return;
  }
  ESP_LOGI(TAG, "PoolLab read requested");
  this->read_requested_ = true;
  // Ask the BLEClient to connect (no bonding). When connected + services
  // discovered, ESP_GATTC_SEARCH_CMPL_EVT drives start_().
  this->parent()->set_enabled(true);
}

void PoolLab::reset_measures() {
  if (this->node_state != espbt::ClientState::ESTABLISHED) {
    ESP_LOGW(TAG, "not connected; cannot reset");
    return;
  }
  this->state_ = PoolLabState::RESET;
  this->send_command_(PCMD_RESET_MEASURES, nullptr, 0);
}

// ---------------------------------------------------------------------------
//  GATT event handling
// ---------------------------------------------------------------------------
void PoolLab::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                  esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      if (!this->resolve_handles_()) {
        ESP_LOGE(TAG, "PoolLabSvc characteristics not found");
        this->parent()->set_enabled(false);
        return;
      }
      // Enable notifications on MISO_Signal, then start the sequence.
      esp_ble_gattc_register_for_notify(gattc_if, this->parent()->get_remote_bda(),
                                        this->miso_signal_handle_);
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      // Write 01 00 to the MISO_Signal CCCD (0x2902) to actually enable notify.
      auto *descr = this->parent()->get_config_descriptor(this->miso_signal_handle_);
      if (descr != nullptr) {
        uint8_t v[2] = {0x01, 0x00};
        esp_ble_gattc_write_char_descr(gattc_if, this->parent()->get_conn_id(),
                                       descr->handle, sizeof(v), v,
                                       ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
      }
      // Notifications requested — begin the protocol.
      if (this->read_requested_) {
        this->read_requested_ = false;
        this->start_();
      }
      break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
      // MISO_Signal fired => a response is ready. The payload is NOT the result;
      // read CommandMISO to get the 250-byte response.
      if (param->notify.handle == this->miso_signal_handle_)
        this->read_response_();
      break;
    }
    case ESP_GATTC_READ_CHAR_EVT: {
      if (param->read.status == ESP_GATT_OK && param->read.handle == this->miso_handle_)
        this->on_response_(param->read.value, param->read.value_len);
      break;
    }
    case ESP_GATTC_WRITE_CHAR_EVT:
      // command write acked; we now wait for the MISO_Signal notify.
      break;
    case ESP_GATTC_DISCONNECT_EVT:
      this->state_ = PoolLabState::IDLE;
      this->mosi_handle_ = this->miso_handle_ = this->miso_signal_handle_ = 0;
      break;
    default:
      break;
  }
}

bool PoolLab::resolve_handles_() {
  auto svc = espbt::ESPBTUUID::from_raw(POOLLAB_SERVICE_UUID);
  auto get = [&](const char *cu) -> uint16_t {
    auto *c = this->parent()->get_characteristic(svc, espbt::ESPBTUUID::from_raw(cu));
    return c == nullptr ? 0 : c->handle;
  };
  this->mosi_handle_ = get(POOLLAB_CHR_MOSI_UUID);
  this->miso_handle_ = get(POOLLAB_CHR_MISO_UUID);
  this->miso_signal_handle_ = get(POOLLAB_CHR_MISOSIGNAL_UUID);
  return this->mosi_handle_ && this->miso_handle_ && this->miso_signal_handle_;
}

// ---------------------------------------------------------------------------
//  Command/response
// ---------------------------------------------------------------------------
void PoolLab::send_command_(uint16_t cmd, const uint8_t *params, size_t params_len) {
  // frame: 0xAB | cmd_lo | cmd_hi | params...  (min 1 byte; device ignores >128)
  uint8_t frame[16] = {0};
  frame[0] = POOLLAB_PREAMBLE;
  frame[1] = (uint8_t) (cmd & 0xFF);
  frame[2] = (uint8_t) (cmd >> 8);
  size_t len = 3;
  if (params != nullptr && params_len > 0) {
    memcpy(&frame[3], params, params_len);
    len += params_len;
  }
  ESP_LOGD(TAG, "-> cmd 0x%04X (%u bytes)", cmd, (unsigned) len);
  esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                           this->mosi_handle_, len, frame,
                           ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
}

void PoolLab::read_response_() {
  esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                          this->miso_handle_, ESP_GATT_AUTH_REQ_NONE);
}

void PoolLab::on_response_(const uint8_t *data, uint16_t len) {
  if (len == 0 || data[0] != POOLLAB_PREAMBLE) {
    ESP_LOGW(TAG, "bad response (len=%u)", len);
    return;
  }
  switch (this->state_) {
    case PoolLabState::GET_INFO:
      this->parse_info_(data, len);
      break;
    case PoolLabState::READ_CELLS:
      this->parse_measures_(data, len);
      break;
    case PoolLabState::RESET:
      ESP_LOGI(TAG, "RESET_MEASURES result=0x%02X", len > 1 ? data[1] : 0);
      this->finish_();
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
//  Protocol sequence
// ---------------------------------------------------------------------------
void PoolLab::start_() {
  this->records_seen_ = 0;
  this->cur_cell_ = 0;
  this->cur_half_ = 0;
  this->state_ = PoolLabState::GET_INFO;
  this->send_command_(PCMD_GET_INFO, nullptr, 0);
}

void PoolLab::parse_info_(const uint8_t *d, uint16_t len) {
  if (len < 23) return;
  // B5-6 result_count, B7-14 device unix time (8B), B15-20 mac, B21-22 battery
  this->result_count_ = (uint16_t) d[5] | ((uint16_t) d[6] << 8);
  uint16_t battery = (uint16_t) d[21] | ((uint16_t) d[22] << 8);
  ESP_LOGI(TAG, "device has %u stored measurements, battery %u%%", this->result_count_,
           battery);
#ifdef USE_SENSOR
  if (this->result_count_sensor_) this->result_count_sensor_->publish_state(this->result_count_);
  if (this->battery_sensor_) this->battery_sensor_->publish_state(battery);
#endif
  if (this->result_count_ == 0) {
    this->finish_();
    return;
  }
  this->state_ = PoolLabState::READ_CELLS;
  this->request_next_half_();
}

void PoolLab::request_next_half_() {
  uint8_t params[3];
  params[0] = (uint8_t) (this->cur_cell_ & 0xFF);   // flash cell id (u16, LE)
  params[1] = (uint8_t) (this->cur_cell_ >> 8);
  params[2] = this->cur_half_;                       // 0 lower / 1 upper
  this->send_command_(PCMD_GET_MEASURES, params, sizeof(params));
}

void PoolLab::parse_measures_(const uint8_t *d, uint16_t len) {
  // 0xAB preamble, then up to 8 x 16-byte records.
  for (int i = 0; i < 8; i++) {
    const uint8_t *r = d + 1 + i * 16;
    if (1 + (i + 1) * 16 > len) break;
    // all-zero record => unused slot
    bool empty = true;
    for (int k = 0; k < 16; k++) if (r[k] != 0) { empty = false; break; }
    if (empty) continue;

    PoolLabMeasurement m;
    m.id = (uint16_t) r[0] | ((uint16_t) r[1] << 8);
    m.type = r[2];
    m.status = r[3];
    m.timestamp = (uint32_t) r[4] | ((uint32_t) r[5] << 8) | ((uint32_t) r[6] << 16) |
                  ((uint32_t) r[7] << 24);
    memcpy(&m.value, &r[8], 4);  // IEEE-754 LE
    this->emit_measurement_(m);
    this->records_seen_++;
  }

  // advance: lower -> upper -> next cell. Stop when we've seen result_count.
  if (this->records_seen_ >= this->result_count_) {
    if (this->reset_after_read_) {
      this->state_ = PoolLabState::RESET;
      this->send_command_(PCMD_RESET_MEASURES, nullptr, 0);
    } else {
      this->finish_();
    }
    return;
  }
  if (this->cur_half_ == 0) {
    this->cur_half_ = 1;
  } else {
    this->cur_half_ = 0;
    this->cur_cell_++;
  }
  if (this->cur_cell_ >= 16) { this->finish_(); return; }
  this->request_next_half_();
}

void PoolLab::emit_measurement_(const PoolLabMeasurement &m) {
  uint32_t read_at = (uint32_t) ::time(nullptr);  // needs SNTP/time: configured
  ESP_LOGI(TAG, "measurement id=%u type=%u status=%u value=%.3f measured_at=%u",
           m.id, m.type, m.status, m.value, m.timestamp);

#ifdef USE_SENSOR
  auto it = this->param_sensors_.find(m.type);
  if (it != this->param_sensors_.end() && it->second != nullptr)
    it->second->publish_state(m.value);
#endif

#ifdef USE_API
  // Fire a Home Assistant event per record with all fields.
  const TypeInfo *ti = poollab_type_info(m.type);
  std::map<std::string, std::string> data;
  data["id"] = to_string(m.id);
  data["type"] = to_string(m.type);
  data["type_name"] = ti ? ti->name : "Unknown";
  data["unit"] = ti ? ti->unit : "";
  data["status"] = to_string(m.status);
  data["value"] = to_string(m.value);
  data["measured_at"] = to_string(m.timestamp);  // device clock (unix)
  data["read_at"] = to_string(read_at);          // ESP SNTP time (unix)
  this->fire_homeassistant_event(this->event_name_, data);
#endif
}

void PoolLab::finish_() {
  ESP_LOGI(TAG, "PoolLab read complete: %u measurements", this->records_seen_);
  this->state_ = PoolLabState::IDLE;
  // Disconnect to save the PoolLab's battery (it stays advertising).
  this->parent()->set_enabled(false);
}

}  // namespace poollab
}  // namespace esphome

#endif  // USE_ESP32
