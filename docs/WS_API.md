# App API — HTTP & WebSocket

App-facing interface for the ESP32-S3 master firmware. The phone/app talks to the robot over Wi-Fi; motors and stream pause run on the S3, optional slave telemetry over UART.

Default mode: **station** — the robot joins the home router (`CONFIG_ROBOT_WIFI_SSID`) and takes a DHCP
address, advertised over mDNS as `robot.local`. The app must be on the same Wi-Fi network.
SoftAP mode (`CONFIG_ROBOT_WIFI_MODE_AP`, IP `192.168.4.1`) is still available in menuconfig.
(Ports are Kconfig; defaults below assume `CONFIG_ROBOT_STREAM_PORT=80`.)

```
App  --HTTP / WebSocket-->  ESP32-S3 (port 80 control, port 81 stream)
                               |
                            UART
                               |
                            ESP32 Slave
```

Related: [UART protocol](../../docs/UART_PROTOCOL.md) · [Pin assignment](../../docs/PIN_ASSIGNMENT.md)

---

## Connection summary

| Service | URL (STA default) | Notes |
|---------|-------------------|-------|
| Control HTTP | `http://robot.local:80` | Health, info, capture, WS upgrade |
| WebSocket | `ws://robot.local:80/ws` | Commands + telemetry |
| MJPEG stream | `http://robot.local:81/stream` | Separate server so video does not block commands |

`robot.local` resolves via mDNS; the DHCP address printed on the serial log works the same and is the
fallback when a network blocks multicast. In SoftAP mode the host is `192.168.4.1`.

Discover live URLs with `GET /api/info` after joining the robot network.

---

## HTTP endpoints

### `GET /`

HTML status page with links to stream, capture, and `/api/info`.

### `GET /health`

**Response** `application/json`:

```json
{"status":"ok"}
```

### `GET /api/info`

**Response** `application/json` (fields filled with the device IP and configured ports):

| Field | Type | Meaning |
|-------|------|---------|
| `ip` | string | Device IP |
| `port` | number | Control HTTP / WS port (default 80) |
| `stream_port` | number | MJPEG port (default 81) |
| `health_path` | string | `/health` |
| `ws_path` | string | `/ws` |
| `stream_path` | string | `/stream` |
| `capture_path` | string | `/capture` |
| `health_url` | string | Full health URL |
| `ws_url` | string | Full WebSocket URL |
| `stream_url` | string | Full stream URL |
| `capture_url` | string | Full capture URL |

### `GET /capture`

**Response** `image/jpeg` — single camera snapshot.  
On failure: HTTP 500.

### `GET /stream` (port 81)

MJPEG multipart stream (`multipart/x-mixed-replace; boundary=frame`).

| Query | Values | Effect |
|-------|--------|--------|
| `quality` | `low` | QQVGA (160x120), JPEG Q≈18 — lowest latency |
| `quality` | `medium` (default) | QVGA (320x240), JPEG Q≈16 |
| `quality` | `high` | VGA (640x480), JPEG Q≈12 — detail over latency |

Example: `http://robot.local:81/stream?quality=high`

Any path on port 81 serves the stream; only `?quality=` is parsed. The sensor has one set of
registers, so with several viewers connected the most recent request wins for all of them.

This port is **not** an `esp_http_server` instance. That component services one request at a
time per server and an MJPEG handler never returns, so one viewer would hold the task forever
and every other viewer would hang — which presented as "the stream takes a minute to load"
whenever the webapp and a browser tab were both open. It is a plain TCP server
(`main/stream_server.c`) that fans one captured frame out to up to 3 viewers, sends with a
per-frame deadline, and drops any viewer that cannot keep up. A 4th viewer gets HTTP 503
rather than waiting.

Frames are capped at `CAM_STREAM_FRAME_MS` (50 ms → 20 fps) and grabbed newest-first
(`CAMERA_GRAB_LATEST`, 2 buffers). Both matter for latency: the cap keeps total bitrate below
what the link absorbs so queues stay empty, and newest-first means what you see is current
rather than a backlog of stale frames.

Buffers are allocated for VGA at init even though the default is QVGA, so `?quality=high`
can raise the framesize at runtime without overflowing them.

While at least one stream client is connected **and** streaming is not paused via WS, telemetry reports `camera.streaming: true`.

---

## WebSocket `/ws`

- **Upgrade:** `GET /ws` with WebSocket handshake on the control server.
- **Frames:** text JSON only (UTF-8).
- **Clients:** up to 4 sockets are tracked for telemetry broadcast.
- **Server → client:** telemetry every **500 ms**, **disabled by default** — see the note below.
- **Client → server:** command messages; each handled command gets an `ack` or `error` reply on the same connection.
- Non-`command` JSON types are ignored (no error).

> **Telemetry push is off by default** (`CONFIG_ROBOT_WS_TELEMETRY`, default `n`).
> The push and the incoming-command reader share one `esp_http_server` task, and a push
> that fails to return there stops commands being read at all — motors go dead while the
> browser still shows a healthy, open WebSocket. Command `ack`/`error` replies are
> unaffected: they are sent with `httpd_ws_send_frame()` from inside the handler, in
> response to a read, never asynchronously. Enable telemetry only if your app consumes
> it, and watch the log for `Telemetry frames skipped`.

### Client → server: command

```json
{
  "type": "command",
  "id": "cmd-001",
  "action": "move",
  "payload": { }
}
```

| Field | Required | Meaning |
|-------|----------|---------|
| `type` | yes | Must be `"command"` |
| `id` | no | Echoed in `ack` / `error` as `commandId` |
| `action` | yes | See actions below |
| `payload` | action-dependent | Object; may be omitted when unused |

#### Actions

##### `move`

```json
{
  "type": "command",
  "id": "1",
  "action": "move",
  "payload": { "direction": "forward" }
}
```

| `direction` | Slave UART |
|-------------|------------|
| `forward` | `CMD:F` |
| `reverse` | `CMD:B` |
| `left` | `CMD:L` |
| `right` | `CMD:R` |
| `stop` | `CMD:S` |

Missing/invalid direction → `error` with `code: "INVALID_MESSAGE"`.  
UART failure → `error` with `code: "SLAVE_ERROR"`.

##### `led`

```json
{
  "type": "command",
  "id": "2",
  "action": "led",
  "payload": { "enabled": true }
}
```

`enabled: true` → `CMD:LED_ON`; `false` or missing/non-true → `CMD:LED_OFF`.

##### `estop`

```json
{ "type": "command", "id": "3", "action": "estop" }
```

Sends `CMD:ESTOP`.

##### `estop_reset`

```json
{ "type": "command", "id": "4", "action": "estop_reset" }
```

Sends `CMD:ESTOP_CLR`.

##### `stream`

Temporarily pause or resume the MJPEG stream on port 81 (HTTP clients stay connected; frames stop until resumed).

```json
{
  "type": "command",
  "id": "4b",
  "action": "stream",
  "payload": { "enabled": false }
}
```

| `enabled` | Effect |
|-----------|--------|
| `false` | Pause video (saves Wi‑Fi / CPU) |
| `true` or omitted | Resume video |

Telemetry `camera.streaming` becomes `false` while paused (even if a stream client is still connected).

##### `set_mode`

```json
{
  "type": "command",
  "id": "5",
  "action": "set_mode",
  "payload": { "mode": "AUTO_DRIVE" }
}
```

Only `mode: "AUTO_DRIVE"` triggers slave `CMD:ADJUST` (automatic width adjustment). Other mode values are accepted with `ack` but send nothing to the slave.

Unknown `action` → `error` with `code: "UNKNOWN_ACTION"`.

### Server → client: ack

```json
{
  "type": "ack",
  "commandId": "1",
  "success": true
}
```

Sent after a command is accepted and forwarded successfully.

### Server → client: error

```json
{
  "type": "error",
  "commandId": "1",
  "code": "INVALID_MESSAGE",
  "message": "Missing direction"
}
```

| `code` | When |
|--------|------|
| `INVALID_MESSAGE` | Bad JSON, missing `action` / `direction`, etc. |
| `UNKNOWN_ACTION` | Unrecognized `action` (`message` is the action string) |
| `SLAVE_ERROR` | Move rejected by UART gateway |
| `ERROR` | Generic fallback |

### Server → client: telemetry

Pushed ~every 200 ms to all connected WebSocket clients:

```json
{
  "type": "telemetry",
  "ts": 1710000000123,
  "robotId": "HVAC-Robot-001",
  "mode": "MANUAL",
  "left_distance": 30.5,
  "right_distance": 32.1,
  "roll": 1.2,
  "pitch": -0.5,
  "status": "OK",
  "adjustment": "IDLE",
  "slave_connected": true,
  "sensors": {
    "ultrasonic": { "leftCm": 30.5, "rightCm": 32.1 },
    "imu": { "rollDeg": 1.2, "pitchDeg": -0.5 }
  },
  "stability": { "status": "STABLE" },
  "camera": {
    "streaming": false,
    "signalStrengthDbm": -50
  }
}
```

| Field | Type | Meaning |
|-------|------|---------|
| `ts` | number | Device time, Unix ms |
| `robotId` | string | Fixed id `HVAC-Robot-001` |
| `mode` | string | Currently always `MANUAL` |
| `left_distance` / `right_distance` | number | Wall distance cm (legacy flat fields) |
| `roll` / `pitch` | number | IMU degrees (legacy flat fields) |
| `status` | string | Slave status (`OK`, `WARNING`, `CRITICAL`, …) |
| `adjustment` | string | `IDLE`, `RUNNING`, `DONE`, … |
| `slave_connected` | boolean | Slave telemetry seen within last 3 s |
| `sensors.ultrasonic.*` | number | Same distances nested |
| `sensors.imu.*` | number | Same angles nested |
| `stability.status` | string | `STABLE` when slave status is empty/`OK`; otherwise slave status string |
| `camera.streaming` | boolean | ≥1 MJPEG client on port 81 and stream not paused |
| `camera.signalStrengthDbm` | number | First SoftAP client RSSI, or `-50` if none |

Flat distance/IMU fields and nested `sensors` mirror the same values for older and newer clients.

---

## Quick start (app)

1. Join the same Wi-Fi network as the robot (`Dialog 4G 856` by default), or SoftAP `Robot-Camera` if built in AP mode.
2. `GET http://robot.local/api/info` → read `ws_url` and `stream_url` (these carry the real IP).
3. Open WebSocket to `ws_url`; listen for `type: "telemetry"`.
4. Send commands with `type: "command"`; wait for matching `ack` / `error` via `commandId`.
5. Show video from `stream_url` (optional `?quality=low|medium|high`).

---

## Implementation notes

| Piece | Source | Role |
|-------|--------|------|
| Control HTTP server | `main/http_stream.c` | `/`, `/health`, `/api/info`, `/capture`, `/ws` upgrade |
| MJPEG video | `main/stream_server.c` | Port 81, plain TCP, multi-viewer; WS pause via `stream_server_set_enabled` |
| WebSocket endpoint + telemetry task | `main/ws_server.c` | `/ws`, client list, optional 500 ms broadcast |
| Command parse / ack / error | `main/cmd_handler.c` | JSON → motors / stream pause / UART |
| UART forward | `main/uart_gateway.c` | Maps optional slave actions to `CMD:*` lines |
| Slave sensor cache | `main/slave_telemetry.c` | Feeds telemetry JSON |

Ports: control = `CONFIG_ROBOT_STREAM_PORT` (default 80); stream = control + 1 (default 81).

Telemetry interval: `WS_TELEMETRY_INTERVAL_MS` (500), only when `CONFIG_ROBOT_WS_TELEMETRY` is
enabled. Max tracked WS clients: `WS_MAX_CLIENTS` (4).
