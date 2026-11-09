# Warehouse Fire Detection System

## Overview

This educational prototype uses an ESP32-S3 to monitor three warehouse zones for signs of fire. Each zone has five flame inputs and one MQ-2 smoke and gas sensor. When the firmware confirms a detection, it commands the corresponding door relay, starts the local alarm sequence, and queues an SMS identifying the affected warehouse.

> **Educational use only.** This prototype is uncertified and must not be used as a building's fire detection, evacuation, or suppression system. Its relay outputs demonstrate door control. The project includes no automatic extinguishing function.

## Features

- Independent detection and alarm state for three warehouse zones.
- Three consecutive trigger readings before the firmware latches an alarm.
- SMS alerts through a SIM800L modem to a configured administrator number.
- Zone-specific door relay control and a local two-tone siren.
- Optional prerecorded warehouse announcements from LittleFS.
- Serial commands for checking status, controlling relay outputs, and clearing alarms.

## Components

| Component | Quantity | Purpose |
| --- | ---: | --- |
| ESP32-S3 DevKitC-1 N16R8 | 1 | Reads sensors and controls alarms, relays, and communications |
| Digital flame sensor module | 15 | Detects flame in the three zones |
| MQ-2 module | 3 | Provides an analog reading for smoke and combustible gases |
| Relay module | 3 | Provides one door-control output per zone |
| SIM800L EVB and active SIM card | 1 each | Sends SMS alerts over a compatible cellular network |
| PCM5102A I²S DAC | 1 | Converts digital audio to an analog signal |
| XH-M567 amplifier and suitable speaker | 1 set | Plays announcements and the siren |
| Power supplies and wiring | As needed | Powers and connects the modules |

## Circuit Diagram

Open the [Wokwi project](https://wokwi.com/projects/480222497882261505) to view the circuit. Use the wiring images for the physical assembly. The following assignments match the published simulation sketch.

| Zone | Flame inputs | MQ-2 analog input | Relay input |
| --- | --- | --- | --- |
| Warehouse 1 | GPIO1, GPIO2, GPIO3, GPIO7, GPIO8 | GPIO4 | GPIO11 |
| Warehouse 2 | GPIO9, GPIO10, GPIO12, GPIO13, GPIO14 | GPIO5 | GPIO47 |
| Warehouse 3 | GPIO15, GPIO16, GPIO17, GPIO18, GPIO21 | GPIO6 | GPIO48 |

| Interface | Signal mapping |
| --- | --- |
| SIM800L UART | ESP32 GPIO41 (RX) ← modem TX, ESP32 GPIO42 (TX) → modem RX |
| PCM5102A I²S | GPIO38 → BCK, GPIO39 → LRCK, GPIO40 → DIN |

The Wokwi diagram uses slide switches for the flame inputs. The modem, DAC, and amplifier are represented by labeled breadboard connections. The relay load contacts are left open in the simulation.

### Project files

| File or asset | Purpose |
| --- | --- |
| `sketch.ino` | Arduino firmware in the Wokwi project |
| `diagram.json` | Wokwi component and wiring configuration |
| Wiring images | Visual reference for module connections |
| PDF guide | Project explanation and reference material |
| PowerPoint presentation | Project summary for demonstrations and presentations |

## How It Works

1. The ESP32-S3 samples the active-low flame inputs and compares the MQ-2 readings against the threshold set for each zone. The threshold is a raw ADC value and requires calibration for each module.
2. Three consecutive flame or gas trigger readings latch the alarm for that warehouse.
3. The firmware commands that warehouse's relay, queues an SMS, and starts the audio sequence. A valid WAV announcement plays before the siren when available. Otherwise, the firmware proceeds to the generated siren if I²S is available.
4. The alarm remains latched until the firmware no longer detects a hazard and an authorized reset command is issued. The relay can then be closed with a separate command. Relay state records the command sent to the output. Physical door position requires separate feedback.

Example SMS for Warehouse 1:

```text
FIRE ALERT! Warehouse 1. Door OPEN commanded (unverified). Evacuate immediately.
```

## Setup & Usage

1. Open the Arduino sketch in Arduino IDE. Use the Arduino-ESP32 3.x board package with support for `ESP_I2S.h` and select an ESP32-S3 board profile that matches your hardware.
2. Check the wiring against the pin definitions in the sketch. Before powering the hardware, verify the board revision, module supply requirements, logic levels, and relay trigger polarity. In particular, check GPIO47 and GPIO48 on your board revision and verify that each MQ-2 analog output is conditioned for the ESP32-S3 ADC input.
3. Set `ADMIN_NUMBER` to the recipient's phone number in international format. Keep personal numbers out of public source files. Verify that the modem's SIM can register on a compatible network and send SMS.
4. Calibrate each entry in `gasThreshold` using measurements from the corresponding sensor under controlled conditions.
5. For optional announcements, select a flash partition scheme with a filesystem partition and upload `voice1.wav`, `voice2.wav`, and `voice3.wav` to the LittleFS root. Use mono, 16-bit PCM WAV at 16 kHz.
6. Upload the firmware and open the Serial Monitor at **115200 baud**. Set the line ending to **New Line**. Check for the `[BOOT]` line before sending commands.

| Command | Expected behavior |
| --- | --- |
| `status` | Prints the sensor, alarm, and commanded relay states for all warehouses |
| `open1`, `open2`, `open3` | Commands the corresponding relay to open |
| `reset1`, `reset2`, `reset3` | Clears the corresponding latch when the firmware reports no active hazard |
| `close1`, `close2`, `close3` | Commands closure when the zone is clear and its latch has been reset |

## Testing

**Observed in Wokwi.** The sketch compiled and the simulated ESP32-S3 started booting. The serial log reported a missing `spiffs` partition during LittleFS initialization. The subsequent `[BOOT]` line and a response to `status` were not observed in that run. Resolve or isolate startup before using the simulation to verify the alarm sequence.

| Test | Expected result |
| --- | --- |
| Trigger one flame input per warehouse in turn | The matching warehouse alarm latches and the correct relay output changes |
| Raise one MQ-2 input above its threshold | The matching alarm latches after three consecutive trigger readings |
| Request reset or closure while a hazard is active | The firmware rejects the command |
| Clear the input, reset the zone, and issue the close command | The alarm clears and the relay returns to its closed-command state |
| Trigger a zone on the hardware with the modem connected | The administrator's phone receives an SMS naming the correct warehouse |
| Test audio with and without a valid WAV file | The announcement and siren play with the file. The generated siren plays by itself when the file is unavailable and I²S is ready. |

Use simulated inputs or controlled bench signals for testing. Verify SMS receipt, audible output, and relay behavior on the physical hardware.

## Future Improvements

- Add separate trip and clear thresholds with sustained clear readings before reset.
- Add door-position feedback so messages can report the measured door state.
- Limit SMS retries and prioritize new alarms when an earlier message fails.
- Add sensor fault detection and log alarm events for troubleshooting.
- Add a simulation mode that exercises the detection and relay logic independently of the audio and modem.

## License

No license has been specified for this project. Check with the project owner before copying, modifying, or redistributing the materials.
