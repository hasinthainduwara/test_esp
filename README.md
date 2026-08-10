# ESP32 Robot Firmware (Master)

ESP32-S3 camera + Wi-Fi gateway. Drives 2× BTS7960 motors locally; optionally links a UART slave for sensors.

## Architecture

```
Mobile App  --Wi-Fi/WebSocket-->  ESP32-S3 (this firmware)
                                      |
                         BTS7960 motors (GPIO 39-42, 47/48/21/20)
                                      |
                                 UART GPIO1/2 (optional slave)
```

## Pin map & protocol

- [docs/WS_API.md](docs/WS_API.md) — HTTP & WebSocket app API
- [docs/PIN_ASSIGNMENT.md](../docs/PIN_ASSIGNMENT.md)
- [docs/UART_PROTOCOL.md](../docs/UART_PROTOCOL.md)

## Test webapp (camera + motors)

```powershell
cd C:\Robot\Robot_Firmware\webapp
npm install
npm run dev
```

Join robot Wi‑Fi, open the Vite URL, Connect to `192.168.4.1`. Details: [webapp/README.md](webapp/README.md).

## Build

```powershell
cd C:\Robot\Robot_Firmware
idf.py build
idf.py -p COMx flash monitor
```

## Motor wiring (Freenove ESP32-S3 right header)

| Signal | Left | Right |
|--------|------|-------|
| RPWM | GPIO39 | GPIO47 |
| LPWM | GPIO40 | GPIO48 |
| R_EN | GPIO41 | GPIO21 |
| L_EN | GPIO42 | GPIO45 |
| LED | GPIO0 | — |

Avoid GPIO35–38 (PSRAM), GPIO43/44 (USB console), and **GPIO19/20 (native USB D-/D+)**.

## UART wiring (optional slave)

```
S3 GPIO1 (TX) ──► Slave RX
S3 GPIO2 (RX) ◄── Slave TX
GND common
```

Do **not** use GPIO43/44 on the S3 for this link (USB/console pins).
