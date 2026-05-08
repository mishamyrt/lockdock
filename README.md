<p align="center">
    <img width="80" src="./docs/logo@2x.png" alt="Lockdock logo" />
</p>

<h1 align="center">Lockdock</h1>

<p align="center">Lock the macOS Dock to a specific display</p>

<p align="center">
    <a href="https://github.com/mishamyrt/lockdock/actions/workflows/qa.yml">
        <img src="https://github.com/mishamyrt/lockdock/actions/workflows/qa.yml/badge.svg" alt="Quality Assurance badge" />
    </a>
    <a href="https://github.com/mishamyrt/lockdock/releases/latest">
        <img src="https://img.shields.io/github/v/tag/mishamyrt/lockdock?label=version" alt="Version badge" />
    </a>
</p>

<hr />

Lockdock keeps the macOS Dock on the display you choose.
It lets you choose which display the dock should be on and prevents macOS from moving it to other displays.

## Features

- Pins the macOS Dock to a selected display.
- Runs in the background as a daemon.
- Use minimal resources: the daemon is written in C and typically stays under 10 MB of RAM

## Requirements

- macOS 15+
- Apple Silicon and Intel Macs are supported.
- Accessibility permission is required for the daemon.

## Installation

> ☝️ The daemon requires Accessibility permission to manage the Dock's position.

The first time you lock a position (`lockdock lock`), a message will appear requesting Accessibility permission for the Lockdock app.
Make sure you grant this permission, as the app won't work without it.

### brew

```sh
brew install mishamyrt/tap/lockdock
lockdock enable # enable background daemon
```

The `lockdock enable` command creates `~/Library/LaunchAgents/co.myrt.lockdock.plist` and starts the daemon in the background.

### Script

The script downloads the latest release binary from GitHub Releases.

```sh
curl -fsSL https://raw.githubusercontent.com/mishamyrt/lockdock/main/install.sh | sh
```

The script will install the application to `~/.local/bin/lockdock` and will prompt you to activate the service once the installation is complete.

To update Lockdock, simply run this script again.

### From sources

To build Lockdock, you only need the clang compiler, which is pre-installed on macOS by default.
It can be built on both Intel and Apple Silicon.

Clone the repository, open it in your terminal, and run the following commands:

```sh
make
make install
lockdock enable # enable background daemon
```

By default, the binary is installed in `~/.local/bin`.

You can override the installation prefix:

```sh
make install PREFIX=/usr/local
```

## Usage

### Raycast

The most convenient way to use Lockdock is through the [Raycast extension](https://github.com/mishamyrt/lockdock-raycast). It displays a list of your monitors, the current Dock position, and lets you pin it to whichever display you want.

The extension uses the installed `lockdock` IPC, so install and enable Lockdock first.

### CLI

| Command                 | Meaning                                              |
| ----------------------- | ---------------------------------------------------- |
| `lockdock list`         | Show connected displays and current Dock display     |
| `lockdock lock <index>` | Pin the Dock to display `<index>`                    |
| `lockdock unlock`       | Stop pinning the Dock, but keep the daemon available |
| `lockdock enable`       | Install and start the LaunchAgent                    |
| `lockdock disable`      | Stop and remove the LaunchAgent                      |

`lockdock help` shows the full list of commands.

Usage example:

```sh
$ lockdock list
0 Mi 27 NU [current]
1 Built-in Display

$ lockdock lock <display-id>
Locked Dock to display 1

$ lockdock list
0 - Mi 27 NU
1 - Built-in Display [current, locked]

$ lockdock unlock
Unlocked Dock
```

- `[current]` marks the display that currently owns the Dock.
- `[locked]` marks the display Lockdock will keep the Dock on.

## How it works

Lockdock tracks mouse movements near the screen edges and forces the system to ignore events that would move the Dock to another display.

Lockdock does not collect telemetry or send any data anywhere. It only needs
Accessibility permission to control the Dock position locally.

## Privacy

Lockdock runs locally and does not send any data over the network.
The daemon only uses Accessibility permission to move the Dock back to the
selected display.

## Compatibility notes

Due to the way the algorithm works, the screens must have an area that does not overlap (at least 2 pixels).

There are also currently some issues with the Dock when it's pinned to the right or left. It's a bit hit-and-miss in those cases.

Except for this, it works with full-screen apps, has no issues with virtual displays or clamshell mode, and works with an auto-hiding Dock.

## Development

The daemon IPC protocol is described in [docs/ipc.md](docs/ipc.md).

## License

MIT
