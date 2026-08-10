# Robot Test Webapp

Local browser UI for testing ESP32 master firmware — camera preview + motor D-pad. Not shipped on the robot.

## Requirements

- Laptop/phone on the same router as the robot (`Dialog 4G 856` by default; SoftAP builds use `Robot-Camera`)
- Node.js 20+

## Run

```powershell
cd C:\Robot\Robot_Firmware\webapp
npm install
npm run dev
```

Open the URL Vite prints (usually `http://localhost:5173`).

1. Confirm the robot address (default `robot.local`; if mDNS fails on your network, type the IP the
   firmware prints on the serial log — `wifi: Got IP: …`). The last address you used is remembered.
2. Click **Connect** — the camera starts automatically
3. Hold D-pad / WASD / arrows to drive; release to stop. Video keeps running while you drive
4. Optional: **Stop camera** if you want the link entirely to yourself

Watch **TX** / **RX** under the IP field — you should see `move:forward` and `ack …` on the serial log as `cmd_handler` / `motor`.

Stream: `http://<ip>:81/stream` · WebSocket: `ws://<ip>:80/ws`

See [docs/WS_API.md](../docs/WS_API.md) for the protocol.
