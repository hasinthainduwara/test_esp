# Robot Test Webapp

Local browser UI for testing ESP32 master firmware — camera preview + motor D-pad. Not shipped on the robot.

## Requirements

- Laptop/phone on the same Wi‑Fi as the robot (SoftAP `Robot-Camera` or STA LAN)
- Node.js 20+

## Run

```powershell
cd C:\Robot\Robot_Firmware\webapp
npm install
npm run dev
```

Open the URL Vite prints (usually `http://localhost:5173`).

1. Confirm robot IP (default `192.168.4.1`)
2. Click **Connect** (WebSocket only — camera stays off so motors stay responsive)
3. Hold D-pad / WASD / arrows to drive; release to stop
4. Optional: **Start camera** when you want MJPEG preview

Watch **TX** / **RX** under the IP field — you should see `move:forward` and `ack …` on the serial log as `cmd_handler` / `motor`.

Stream: `http://<ip>:81/stream` · WebSocket: `ws://<ip>:80/ws`

See [docs/WS_API.md](../docs/WS_API.md) for the protocol.
