# MQTT contract

The ESP32 firmware (`src/main.cpp`), the backend (`backend/src/parking.js`) and the
simulator (`backend/src/simulator.js`) all use these topics. Topic names are defined
once in `backend/src/topics.js` and at the top of `src/main.cpp`.

## Gate controller → backend

| Topic | Retained | Payload |
| --- | --- | --- |
| `parking/device/online` | yes | `online` when connected. The broker publishes `offline` (last will) when the link dies. |
| `parking/status` | yes | JSON snapshot, sent on every change and every 30 s (see below). |
| `parking/entry_log` | no | JSON event: a card was accepted at the entry gate. |
| `parking/exit_log` | no | JSON event: a card was accepted at the exit gate. |
| `parking/denied_log` | no | JSON event: a card was refused. |
| `parking/cache/poll_request` | no | The card-list version the device holds (`-1` = none). The backend answers with `parking/cache/update` if it differs. |
| `parking/occupied_persistent/request` | no | Sent once when the device has no stored counter (first boot, flash erased). |

Status payload:

```json
{ "device": "esp32-gate", "occupied": 4, "total": 10, "free": 6,
  "entryGate": "closed", "exitGate": "open", "queued": 0,
  "cacheVersion": 7, "ip": "192.168.8.140", "rssi": -58, "uptime": 1234 }
```

Event payload:

```json
{ "id": "esp-12-3", "device": "esp32-gate", "uid": "A1:B2:C3:D4", "owner": "Jubayer Alam",
  "gate": "entry", "passed": true, "ts": 1794855600, "offline": false, "occupied": 5 }
```

* `id` is unique per event (`esp-<boot number>-<sequence>`). The backend stores each id once, so re-sending is always safe.
* `passed` is `false` when the gate opened but no vehicle drove through before the timeout. The counter does not change in that case.
* `ts` is unix time in seconds, or `0` if the device had no clock (booted without network). The backend then uses the arrival time and marks the event "time is approximate".
* `offline` is `true` when the event was first stored in flash and delivered later.
* Denied events carry `"reason"` instead of `passed`: `unknown_card`, `lot_full` or `cache_not_ready`. The backend turns `unknown_card` into `blocked_card` when the card exists but is blocked.

## Backend → gate controller

| Topic | Retained | Payload |
| --- | --- | --- |
| `parking/ack` | no | An event `id`. The device may now delete its copy. |
| `parking/time` | no | Unix time in seconds, every 30 s and whenever a device comes online. Lets the device timestamp events without internet/NTP. |
| `parking/cards_version` | yes | Current card-list version number. |
| `parking/cache/update` | yes | The card list: first line `v=<version>`, then one `UID|Owner` per line. Only active cards are included. |
| `parking/occupied_persistent/response` | no | Last known occupied count, answer to the request above. |
| `parking/cmd` | no | JSON command (see below). |

Commands:

```json
{ "cmd": "open_entry" }
{ "cmd": "open_exit" }
{ "cmd": "set_occupied", "value": 3 }
{ "cmd": "set_total", "value": 12 }
{ "cmd": "sync" }
```

## Store & forward, step by step

1. A card is scanned. The decision is made on the device from the card list in flash. No network round trip.
2. The event is published. The device keeps it in RAM until `parking/ack` arrives.
3. No ack within 5 s, or no connection at all: the event is appended to `/offline_queue.txt` in LittleFS.
4. When the broker is reachable again the queue is sent in batches of 10. A line is removed from flash only after its ack.
5. The backend ignores ids it already has, so an event can never be counted twice.

The counter (`occupied`, `total`), the card-list version and a boot counter are stored in NVS (`Preferences`), so a power cut does not reset the lot.

## Watching the traffic

```bash
mosquitto_sub -h <broker-ip> -t 'parking/#' -v
```
