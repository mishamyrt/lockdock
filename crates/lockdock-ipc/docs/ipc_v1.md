# lockdock IPC protocol

A Unix socket is used to communicate with `lockdock`.
The socket is located at `~/Library/Caches/co.myrt.lockdock/control.sock`.

## Getting state

Request:
```json
{"cmd": "get_state"}
```

Response when the Dock **is** pinned:
```json
{"displays":["Mi 27 NU","Built-in Display"],"location":1,"target":1}
```

Response when the Dock **is not** pinned:
```json
{"displays":["Mi 27 NU","Built-in Display"],"location":1}
```

### Fields

- `displays` — list of connected displays;
- `location` — index of the display where the Dock is currently located;
- `target` — index of the display where the Dock is pinned.

If the pinned display is disconnected, `target` disappears because the active lock is
removed. When the same physical display is connected again, `lockdock` moves the
Dock back to it and `target` appears again automatically.

## Pin Dock to the specified display

Request:
```json
{"cmd":"set_state","target":1}
```

Response (success):
```json
{"success":true}
```

Response (error)
```json
{"success":false,"reason":"error reason"}
```

### Fields

- `target` — index of the display on which the Dock should be pinned;
- `success` — operation success indicator;
- `reason` — textual reason for the error.

## Unpin Dock

Unlocks the Dock's position and allows it to be placed on any display.

Request:
```json
{"cmd":"unlock"}
```

Response:
```json
{"success":true}
```
