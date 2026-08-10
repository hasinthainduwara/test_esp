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

Put the laptop on the same router as the robot, open the Vite URL, Connect to `robot.local`
(or the IP the firmware prints on boot). Details: [webapp/README.md](webapp/README.md).

## Wi-Fi

Station mode by default — the robot joins your router and the app reaches it over the LAN:

| Setting | Value | Where |
|---------|-------|-------|
| SSID | `Dialog 4G 856` | `CONFIG_ROBOT_WIFI_SSID` |
| Password | set in menuconfig | `CONFIG_ROBOT_WIFI_PASSWORD` |
| Name on the LAN | `robot.local` (mDNS) | `CONFIG_ROBOT_MDNS_HOSTNAME` |

Change networks with `idf.py menuconfig` → *Robot Camera Configuration*, then rebuild and flash;
the same menu switches back to SoftAP mode if you need the robot to run off-network.

If it does not connect, the serial log says why — a wrong password shows repeated
`Disconnected (reason 15)` / `(reason 205)`. The firmware keeps retrying every 5 s
rather than giving up, so fixing the router is enough to bring it back.

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
