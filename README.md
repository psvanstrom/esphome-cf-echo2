# CF Echo II Heat Meter Reader

An [ESPHome](https://esphome.io/) external component that reads an **Itron/Actaris CF Echo II**
district-heating meter through its optical (IR) interface and publishes the values to
Home Assistant.

The meter speaks [M-Bus](https://m-bus.com/) over the optical head at 2400 baud. This
component performs the optical wake-up sequence, requests a data frame, validates it, and
decodes the measured values into standard ESPHome sensors.

## Features

- Reads energy, volume, power, flow/return temperature and more (see below)
- Non-blocking read cycle — the main loop and Wi-Fi/API stay responsive while polling
- M-Bus long-frame checksum validation (bad frames are discarded, not published)
- Optional on-demand read via a Home Assistant **button** or an ESPHome **action**
- Optional activity LED that lights while a read is in progress

### Sensors

All sensors are optional — declare only the ones you want.

| Key           | Unit  | Device class | State class        |
|---------------|-------|--------------|--------------------|
| `energy`      | kWh   | energy       | total_increasing   |
| `volume`      | m³    | volume       | total_increasing   |
| `power`       | W     | power        | measurement        |
| `volume_flow` | m³/h  | –            | measurement        |
| `flow_temp`   | °C    | temperature  | measurement        |
| `return_temp` | °C    | temperature  | measurement        |
| `delta_t`     | K     | –            | measurement        |

## Hardware

- An **ESP8266** board — the reference configuration uses a Wemos/LOLIN **D1 mini**.
  (An ESP32 works too; adjust the `esp8266:`/`uart:` sections accordingly.)
- An **optical M-Bus / IEC 62056-21 read head** (an "IR eye") with TTL-level TX/RX,
  positioned over the meter's optical port.

### Wiring

The component uses the ESP8266 hardware UART (UART0), so it must be wired to GPIO1/GPIO3.

| IR read head | D1 mini pin      |
|--------------|------------------|
| TX           | GPIO3 (RX / D9)  |
| RX           | GPIO1 (TX / D10) |
| VCC          | 3.3V             |
| GND          | GND              |

> **Note:** TX on the read head goes to **RX** on the ESP and vice-versa. Because UART0 is
> shared with the USB-serial console, **serial logging must be disabled** (`logger:` with
> `baud_rate: 0`) — otherwise log output collides with meter communication.

## Requirements

- ESPHome (developed and tested against recent 2025.x / 2026.x releases). The component
  auto-adapts to the `register_action(synchronous=…)` API that newer ESPHome versions
  introduced, so it also works on older releases.

## Setup

### 1. Add the external component

In your ESPHome YAML, pull the component straight from this repository:

```yaml
external_components:
  - source: github://psvanstrom/esphome-cf-echo2@main
    components: [cf_echo2]
```

### 2. Create `secrets.yaml`

Copy `secrets.yaml.example` to `secrets.yaml` and fill in your values. The reference
configuration uses these secrets:

```yaml
wifi_ssid: "your_wifi_ssid"
wifi_password: "your_wifi_password"
fallback_password: "fallback_hotspot_password"
ota_password: "your_ota_password"
encryption_key: "your_home_assistant_api_encryption_key"
```

### 3. Configure the UART and sensor

A minimal working configuration:

```yaml
logger:
  baud_rate: 0            # required: UART0 is shared with the meter

uart:
  tx_pin: GPIO1           # D1 mini TX
  rx_pin: GPIO3           # D1 mini RX
  baud_rate: 2400
  data_bits: 8
  parity: EVEN
  stop_bits: 1

sensor:
  - platform: cf_echo2
    update_interval: 30min
    energy:
      name: "CF Echo Energy"
    volume:
      name: "CF Echo Volume"
    power:
      name: "CF Echo Power"
    flow_temp:
      name: "CF Echo Flow Temp"
    return_temp:
      name: "CF Echo Return Temp"
```

See [`cf_echo2.yaml`](cf_echo2.yaml) for a complete, ready-to-flash example including
Wi-Fi, the Home Assistant API, OTA, a read button and the activity LED.

### 4. Compile and flash

```bash
esphome run cf_echo2.yaml
```

## Configuration reference

The `cf_echo2` platform extends the standard ESPHome polling-component and UART-device
schemas.

| Option            | Type            | Default | Description                                                        |
|-------------------|-----------------|---------|--------------------------------------------------------------------|
| `uart_id`         | ID              | –       | UART bus to use (if you have more than one).                       |
| `update_interval` | time            | `30s`   | How often to poll the meter. `30min` or more is recommended.       |
| `activity_led`    | pin             | –       | Output pin driven high while a read is in progress. Use `inverted: true` for active-low LEDs (e.g. the D1 mini onboard LED on GPIO2). |
| `read_button`     | button          | –       | Creates a Home Assistant button that triggers an on-demand read.   |
| `energy` … `delta_t` | sensor       | –       | Optional sensors (see the table above). Each accepts the usual ESPHome sensor options (`name`, `id`, `filters`, `accuracy_decimals`, …). |

The UART must be configured for **2400 baud, 8 data bits, even parity, 1 stop bit**. The
component briefly switches the UART to 8N1 during the optical wake-up burst and back to
8E1 for the data exchange.

### On-demand reads

Trigger a read from any ESPHome automation with the `cf_echo2.read` action:

```yaml
sensor:
  - platform: cf_echo2
    id: heat_meter
    # ...

# e.g. read whenever a template button is pressed
button:
  - platform: template
    name: "Read Heat Meter Now"
    on_press:
      - cf_echo2.read: heat_meter
```

Or expose a dedicated button directly from the platform:

```yaml
sensor:
  - platform: cf_echo2
    read_button:
      name: "Read Heat Meter"
```

## How it works

Each read cycle runs as a non-blocking state machine:

1. **Wake-up** — switch the UART to 8N1 and stream ~528 `0x55` bytes to wake the optical
   interface, then pause.
2. **Request** — switch back to 8E1 and send an M-Bus REQ_UD2 short frame
   (`10 5B FE 59 16`).
3. **Read** — receive the M-Bus long-frame response (`68 L L 68 … CS 16`), verify the
   header and checksum, then decode the application-layer records.

Values are decoded from the M-Bus data records by their VIF (value information field):

| VIF    | Sensor        | Scaling      |
|--------|---------------|--------------|
| `0x06` | `energy`      | kWh          |
| `0x14` | `volume`      | × 0.01 m³    |
| `0x2D` | `power`       | × 100 W      |
| `0x3B` | `volume_flow` | × 0.001 m³/h |
| `0x5A` | `flow_temp`   | × 0.1 °C     |
| `0x5E` | `return_temp` | × 0.1 °C     |
| `0x61` | `delta_t`     | × 0.01 K     |

> **Meter-specific decoding:** the frame layout (including a fixed application-header
> offset) and the VIF/scaling table above were reverse-engineered from a specific CF
> Echo II unit. Other firmware revisions may lay out records differently. If your readings
> look wrong, temporarily enable UART logging on a spare console and inspect the raw frame
> against your meter's documentation.

## Troubleshooting

- **No data / "Frame read timeout" or "Failed to read valid frame":** check TX/RX aren't
  swapped, confirm the read head is seated correctly over the optical port, and verify
  UART framing is 2400 8E1.
- **"Checksum mismatch … discarding frame":** communication is noisy or the framing is
  wrong; re-seat the read head and confirm parity/baud.
- **Nothing in the logs about the meter:** remember `logger:` must have `baud_rate: 0`;
  view logs over the network (`esphome logs cf_echo2.yaml`) or the Home Assistant API, not
  the USB serial console.
- **Reads are infrequent by design:** the meter is polled on `update_interval` (30 min in
  the example). Use the read button/action for an immediate reading.

## License

[MIT](LICENSE) © Pär Svanström
