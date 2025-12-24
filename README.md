# CF Echo II Heat Meter Reader

ESPHome external component for reading Itron CF Echo II heat meters via optical interface.

## Wiring
- TX (GPIO1) → IR eye TX
- RX (GPIO3) → IR eye RX  
- 3.3V → IR eye VCC
- GND → IR eye GND

## Home Assistant trigger
Expose a manual read action via the ESPHome API service and call it from Home Assistant:

```yaml
cf_echo2:
  id: heat_meter
  energy:
    name: "Heat Energy"

api:
  services:
    - service: cf_echo2_read
      then:
        - cf_echo2.read: id: heat_meter

Auto-discovered button:

```yaml
cf_echo2:
  id: heat_meter
  energy:
    name: "Heat Energy"
  read_button:
    name: "Read Heat Meter"
```
```
