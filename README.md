# lockdock

`lockdock` pins the Dock in macOS to a selected display. The project consists of a background daemon, `lockdockd`, and a CLI, `lockdock`, which installs the daemon as a `LaunchAgent` and sends commands to it.

## What this solves

If you have multiple monitors, macOS can move the Dock between them uncontrollably. `lockdockd` lets you lock the Dock to a specific display and return it there after reconnecting a monitor.

## Features

- displays a list of active displays and the current Dock position;
- pins the Dock to a selected display;
- runs in the background via a user `LaunchAgent`;
- automatically returns the Dock to the same physical monitor after it is reconnected.

## Requirements

- macOS;
- `Accessibility` access for `lockdockd`, otherwise the daemon won't be able to detect and move the Dock.

## Installation

### From sources

Clone the repository, open it in your terminal, and run the following commands:

```sh
make
make install
lockdock enable # enable background daemon
```

By default, the binaries are installed in `~/.local/bin`. If `lockdock` isn't found, add this directory to your `PATH`.

The `lockdock enable` command creates `~/Library/LaunchAgents/co.myrt.lockdockd.plist` and starts the daemon in the background.
If macOS asks for `Accessibility` access, grant it to `lockdockd`.

## CLI

Basic commands:

```sh
lockdock list
lockdock lock 1
lockdock unlock
lockdock disable
```

Usage example:

```sh
$ lockdock list
0 Mi 27 NU [current]
1 Built-in Display

$ lockdock lock 1
Locked Dock to display 1

$ lockdock list
0 Mi 27 NU
1 Built-in Display [current, locked]

$ lockdock unlock
Unlocked Dock
```

`lockdock help` shows the full list of commands.

## Additional Information

The IPC protocol between the CLI and the daemon is described in [docs/ipc.md](docs/ipc.md).

## License

MIT
