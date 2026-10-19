# Modular sensor acquisition update

## Wiring

| Sensor | Signal | ESP32-S3 GPIO | Reading |
| --- | --- | --- | --- |
| DHT11 | DATA | 5 | Temperature and humidity |
| BH1750 | SDA / SCL | 8 / 9 | Illuminance (lux) |
| PIR HW-740 | OUT | 15 | Motion / No motion |
| LD2410B | TX | 4 (UART RX, 256000 baud, 8N1) | Present / Absent |
| MQ-2 | AO | 1 (ADC1) | Response voltage (V) |
| GP2Y1014AU0F | Vo via 1k upper / 560 ohm lower divider | 2 (ADC1) | Sensor-side voltage (V); dashboard estimated dust (µg/m³) |
| GP2Y1014AU0F | BC549 base via 1k; 150k B-E pull-down | 6 | LED pulse control |

Move the LD2410B signal connection from OUT to TX; leave its RX and OUT unused. All grounds remain common. Other confirmed connections remain unchanged. No module-presence wire is added. MQ-2 remains response voltage. Dust is displayed as an uncalibrated estimate using max(0, (Vo - 0.9) * 200) µg/m³, with supplier GP2Y1010AU0F nominal parameters selected by the user; this does not identify the physical sensor model or measure PM2.5. ADC inputs must remain within the ESP32-S3 input limits; software cannot protect an input from overvoltage.

## Operation

- DHT11 and BH1750 recover automatically after successful reads. BH1750 retries initialization at either supported I2C address.
- LD2410B accepts framed basic and engineering reports. No valid report for 2 seconds makes the next published reading missing. The dashboard updates on its existing 5-second refresh cycle; radio/upload delay adds latency.
- PIR, MQ-2 and dust are disabled by default in the web interface. Open **Sensor settings** on the relevant node card and enable the connected sensors. Disable them after unplugging. No automatic unplug detection is claimed for these three interfaces.
- Firmware continuously acquires the enabled build-time interfaces. Web switches control server acceptance/display, not sensor power or firmware sampling. Allow the MQ-2 to stabilize before enabling its readings; no concentration calibration or automatic warm-up detection is provided.
- PIR uses GPIO15 INPUT, with no software startup waiting period. Hardware warm-up may still affect initial readings. Sampling remains approximately every 5 seconds, with no motion-event latch.
- Unavailable values remain JSON/SQL null with non-valid quality, never numeric zero. Invalid/stale/disabled sensor rows are hidden from node information. A node with no available readings shows **NoData**. Valid zero, **No motion** and **Absent** remain visible.
- Switches persist in SQLite per node. Enabling waits for measurements captured after the change. Disabled-interval data cannot become valid after delayed upload. Old records are retained; pre-update manual-channel readings are treated as unverified by the API because no enable history exists.
- Historical charts retain valid automatic-channel history with gaps for missing samples; current unavailable values use **NoData**. Manual-channel charts are hidden when disabled. Missing data cannot trigger a threshold alert.
- Dust sampling uses a separate task, a 10 ms target cycle, a hardware timer to turn off the LED at 320 microseconds, and an ADC read initiated near 280 microseconds. The 1560/560 divider gain is applied once in firmware. The 1.56k output load may alter sensor readings and needs physical validation. ADC rail saturation and missed acquisition windows produce missing values. Physical pulse timing and analogue accuracy still require hardware validation.

## Apply the update

1. Stop the existing gateway and Flask processes.
2. Compile and upload `firmware/nodes/Node1/Node1.ino`, `Node2/Node2.ino` and `Node3/Node3.ino` to their matching boards. All boards, including relays and Root, must use this revision: ESP-NOW packet format is now version 3 and is incompatible with version 2.
3. Start `scripts/start-app.ps1` and `scripts/start-gateway.ps1` as before. Server startup adds the sensor-control table without deleting existing data.
4. Hard-refresh the browser. Enable only connected PIR/MQ-2/dust modules in each node's Sensor settings.

## Verification

Automated tests use isolated SQLite files and do not modify the live database. Run `scripts/test.ps1` for Python regression tests; run `node tests/ui_smoke.cjs` for UI logic tests. Actual sensor insertion/removal, UART baud configuration, ADC voltages, dust timing and radio operation must be checked on the hardware after flashing.

LD2410B frame reference: Hi-Link serial protocol V1.06, section 2.3: https://seengreat.com/upload/file/86/LD2410B-serial-communication-protocol.pdf

## 2026-10-08 update

The database, gateway and API retain dust_voltage in volts. The dashboard converts current values, history and alert text consistently to estimated dust concentration. Existing dust_voltage threshold settings remain in volts; no thresholds are enabled or changed. Missing values remain missing, while voltages below 0.9 V map to a valid zero estimate. Historical voltages are preserved and displayed using the same nominal formula; wiring accuracy of old measurements is not retroactively validated.
