# esphome-poollab

ESPHome external component to read a **PoolLab 1.0** (Water-i.d.) BLE photometer from an
ESP32, on demand, and push every stored measurement to Home Assistant — both as a
**HA event per record** and as **per-parameter sensors**.

Built for the ESP32-S3 PoolX board, but works on any ESP32 with BLE.

> ⚠️ **Status: scaffold / work-in-progress.** The protocol (`PROTOCOL.md`) is complete and
> verified against the official API doc. The C++ component is a structurally-correct
> skeleton that has **not yet been tested against a real device** — the BLE GATT state
> machine needs bench iteration. Treat it as a strong starting point, not a finished driver.

## What it does

On a trigger (a Nextion button, a Home Assistant button/service, or an automation):
1. Connects to the PoolLab over BLE (no pairing).
2. `GET_INFO` → reads how many measurements are stored.
3. Loops `GET_MEASURES` over the flash cells, parsing each 16-byte record.
4. For **each** record:
   - **Fires a HA event** `esphome.poollab_measurement` with
     `{id, type, type_name, value, unit, status, measured_at, read_at}`.
   - **Updates the per-parameter sensor** (if one is configured for that `measure_type`)
     to the latest value.
5. Optionally `RESET_MEASURES` afterwards (config flag) so each batch is read once.

`measured_at` is the device's own timestamp from when the test was taken; `read_at` is the
ESP's SNTP time when it read the record.

## Usage (target API — see `example.yaml`)

```yaml
api:
  homeassistant_services: true   # REQUIRED — lets the component fire the per-record HA events
time:
  - platform: sntp               # provides the "read_at" timestamp

external_components:
  - source: github://kenavn/esphome-poollab@main

esp32_ble_tracker:
ble_client:
  - mac_address: 60:44:7A:XX:XX:XX     # your PoolLab
    id: poollab_ble

poollab:
  id: poollab
  ble_client_id: poollab_ble
  reset_after_read: false              # true = erase device memory after a successful read
  event_name: esphome.poollab_measurement
  result_count:                        # optional diagnostic sensors
    name: "PoolLab Stored Results"
  battery_level:
    name: "PoolLab Battery"
  parameters:                          # per-parameter sensors (measure_type codes in PROTOCOL.md)
    - type: 9                          # pH
      name: "Pool pH"
    - type: 8                          # Free Chlorine
      name: "Pool Free Chlorine"
      unit_of_measurement: "ppm"

# trigger from anywhere:
button:
  - platform: template
    name: "Read PoolLab"
    on_press:
      - poollab.read: poollab
```

Nextion button → same `poollab.read` action from the display's touch event.

## Home Assistant side

- **Event:** an automation triggering on `esphome.poollab_measurement` can log all records
  (type, value, both timestamps) to wherever you like.
- **Sensors:** the per-parameter sensors graph the latest value per parameter normally.

## Files

- `PROTOCOL.md` — full BLE protocol (UUIDs, command flow, record format)
- `components/poollab/` — the external component
- `example.yaml` — complete usage example

## License
MIT
