# PoolLab 1.0 — Bluetooth LE protocol

Reconstructed from the official *PoolLab 1.0 Bluetooth API, Interface Documentation
v2, 02/03/2022* (Water-i.d. GmbH / 420nm UG), https://poollab.org/static/api/BLE.pdf.
The official PDF is image/font-encoded and hard for tools to read — this is a clean,
machine-readable transcription for building against.

## Device / connection (GAP)

| | |
|---|---|
| Advertised name | `PoolLab` |
| MAC prefix | `60:44:7A:xx:xx:xx` |
| Pairing/bonding | **None.** Do **not** bond — the device reports a bogus success if you try. |
| Security | Mode 1, no auth/encryption |
| ATT MTU | 23 bytes |
| Conn interval | 10–500 ms, supervision timeout 5000 ms |

## GATT — custom service `PoolLabSvc`

| Characteristic | UUID | Properties | Use |
|---|---|---|---|
| Service | `A7EE04A9-507B-4910-A528-B619D5501924` | — | — |
| `CommandMOSI` | `91BFA536-3036-4901-8813-3635FCED7B90` | write | send a command (≤128 B) |
| `CommandMISO` | `2FF18B59-195D-4EE1-B78C-0CBDE3EFF9C2` | read | read the response (**always 250 B**) |
| `MISO_Signal` | `C2296C06-C7E0-4657-B42E-C8330826454C` | notify | fires when a response is ready |

Each characteristic has a CCCD (`0x2902`) and CUDD (`0x2901`).

## Command/response flow

1. Connect (no bond).
2. Discover `PoolLabSvc` and its three characteristics.
3. Enable notifications on `MISO_Signal` (write `01 00` to its CCCD). Must be redone
   every connection (no bonding).
4. **Send command:** write to `CommandMOSI`. Min 1 byte, max 128; all bytes after the
   last used byte must be `0x00`.
5. **Wait** for the `MISO_Signal` notification. Its payload is *not* the result — it is
   only a "response ready" signal; discard it.
6. **Read** `CommandMISO` → 250 bytes (read it completely). Parse per the command.

Command-and-response, except `RESET_DEVICE`/`SLEEP_DEVICE` (no response — connection
just drops). All multi-byte integers are **little-endian**.

### Command frame (to `CommandMOSI`)

| B0 | B1 | B2 | B3… |
|---|---|---|---|
| `0xAB` preamble | cmd ID low | cmd ID high | parameter bytes |

### Commands

| Name | ID | Params | Notes |
|---|---|---|---|
| `GET_INFO` | `0x0001` | none | device info incl. **result_count** |
| `SET_TIME` | `0x0002` | unixtime (u32) | set device clock |
| `RESET_DEVICE` | `0x0003` | none | restart (no response) |
| `SLEEP_DEVICE` | `0x0004` | none | deep sleep (no response) |
| `GET_MEASURES` | `0x0005` | flash cell id (u16), half (u8: 0=lower,1=upper) | read stored results |
| `RESET_MEASURES` | `0x0006` | none | **erase all** stored results |
| `SET_CONTRAST_PLUS` | `0x0008` | none | LCD contrast +1 (18–38) |
| `SET_CONTRAST_MINUS` | `0x0009` | none | LCD contrast −1 |
| `GET_PPM_MGL` | `0x000A` | none | unit mode: 0=ppm, 1=mg/L |
| `SET_PPM_MGL` | `0x000B` | mode (u8) | set unit mode |

## `GET_INFO` (0x01) response

```c
struct dev_info_response {  // bytes from CommandMISO, after 0xAB preamble
  uint8_t  preamble;       // 0xAB
  uint16_t active_id;      // OEM id
  uint16_t fw_version;
  uint16_t result_count;   // number of stored measurements  <-- use to size readout
  uint64_t unix_time;      // device clock (seconds since 1970)  *8 bytes in frame
  uint8_t  mac_address[6];
  uint16_t battery_level;  // 0..100 %
};
```
(Frame layout: preamble B0; active_id B1–2; fw B3–4; result_count B5–6; device-time
B7–14 (8 bytes); mac B15–20; battery B21–22.)

## `GET_MEASURES` (0x05) — reading the batch

Memory = **16 flash cells × 16 measurements**, each cell split into a **lower half**
(first 8) and **upper half** (last 8). One `GET_MEASURES` returns **one half (8
records)**. **Read the lower half first.**

- Params: `flash_cell_id` (u16, 0–15) + `half` (u8: `0x00` lower / `0x01` upper).
- Response: `0xAB` preamble, then **8 × 16-byte** records (128 B). Unused records are
  all-zero (e.g. 5 stored in a half ⇒ first 81 bytes used, rest zero).
- Number of reads needed: best-practice `1 + result_count/8` (from `GET_INFO`). Each
  cell is addressed twice (lower then upper) except possibly the last.

### Measurement record (16 bytes)

```c
struct flash_measurement_result {
  uint16_t measure_id;        // B0-1   sequence id (reset by RESET_MEASURES)
  uint8_t  measure_type;      // B2     parameter code (see table — TODO source)
  uint8_t  measure_status;    // B3     0=OK, 1=underrange, 2=overrange
  uint32_t measure_timestamp; // B4-7   unix seconds, WHEN MEASURED
  float    measure_value;     // B8-11  IEEE-754 single precision
  uint8_t  reserved[4];       // B12-15
};
```

`measure_value` unit is ppm or mg/L per the device's `GET_PPM_MGL` mode (pH/ORP/etc.
are unitless / mV regardless).

## `RESET_MEASURES` (0x06)

Erases all flash cells and resets the id counter. Irreversible — only call after a
successful full readout. Response: `0xAB`, result code (`0x01` OK / `0x02` err).

## Measurement type codes (`measure_type`)

Sourced from the API doc (later version, pp. 21–22) and **cross-verified against two
independent implementations** ([ceilingduster/python-poollab-ble](https://github.com/ceilingduster/python-poollab-ble),
[100prznt/PoolLabIo](https://github.com/100prznt/PoolLabIo)). Format: code → name (unit, decimals).

| # | Parameter | Unit | # | Parameter | Unit |
|---|---|---|---|---|---|
| 1 | Total Chlorine | Cl₂ ppm | 26 | Potassium | K ppm |
| 2 | Ozone | O₃ ppm | 27 | pH HR | pH |
| 3 | Chlorine Dioxide | ClO₂ ppm | 28 | pH LR | pH |
| 5 | Active Oxygen | O₂ ppm | 29 | pH HR (Saltwater) | pH |
| 6 | Bromine | Br ppm | 30 | pH HR (Seawater) | pH |
| 7 | Hydrogen Peroxide | H₂O₂ ppm | 31 | pH LR (Saltwater) | pH |
| 8 | **Free Chlorine** | fCl ppm | 32 | pH LR (Seawater) | pH |
| 9 | **pH** | pH | 33 | pH MR (Saltwater) | pH |
| 10 | Total Alkalinity | TA ppm | 34 | pH MR (Seawater) | pH |
| 11 | Cyanuric Acid | Cya ppm | 35 | Total Hardness | CaCO₃ ppm |
| 12 | Hydrogen Peroxide HR | H₂O₂ ppm | 36 | pH MR | pH |
| 13 | Total Hardness HR | CaCO₃ ppm | 37 | Iodine | I₂ ppm |
| 14 | Isothiazilinone | C₃H₃NOS ppm | 38 | Urea | CH₄N₂O ppm |
| 15 | Nitrite LR | NO₂ ppm | 39 | PHMB | PHMB ppm |
| 16 | Nitrate | NO₃ ppm | 40 | Total Alkalinity (Seawater) | TA ppm |
| 17 | Phosphate | PO₄ ppm | 41 | Total Chlorine (liquid) | tCl ppm |
| 18 | Iron LR | Fe ppm | 42 | Ozone (liquid) | O₃ ppm |
| 19 | Dissolved Oxygen | DO₂ ppm | 43 | Chlorine Dioxide (liquid) | ClO₂ ppm |
| 20 | Ammonia | NH₄ ppm | 44 | Active Oxygen (liquid) | O₂ ppm |
| 21 | Silica | SiO₂ ppm | 45 | Bromine (liquid) | Br ppm |
| 22 | Copper | Cu ppm | 46 | Hydrogen Peroxide (liquid) | H₂O₂ ppm |
| 23 | Calcium | CaCO₃ ppm | 47 | Free Chlorine (liquid) | fCl ppm |
| 24 | Ozone i.p.o. Chlorine | O₃ ppm | 48 | pH (liquid) | pH |
| 25 | Magnesium | Mg ppm | 49 | Ozone i.p.o. Chlorine (liquid) | O₃ ppm |

(Code 4 is unused. Codes 27–36 / 41–49 are OEM/in-development variants.) Status:
`0`=OK, `1`=underrange, `2`=overrange. Value unit is ppm or mg/L per `GET_PPM_MGL`.
