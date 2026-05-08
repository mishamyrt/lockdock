# lockdock [![Quality Assurance](https://github.com/mishamyrt/lockdock/actions/workflows/qa.yml/badge.svg)](https://github.com/mishamyrt/lockdock/actions/workflows/qa.yml) [![Version](https://img.shields.io/github/v/tag/mishamyrt/lockdock?label=version)](https://github.com/mishamyrt/lockdock/releases/latest)

`lockdock` pins the Dock in macOS to a selected display.

## What this solves

If you have multiple monitors, macOS can move the Dock between them uncontrollably. `lockdock` lets you lock the Dock to a specific display and return it there after reconnecting a monitor.

## Features

- Pins the Dock to the selected display;
- Moves the Dock between displays;
- Runs in the background as a daemon;
- Uses **very** few resources. The daemon is written in C and consumes no more than 10 megabytes of RAM.

## Installation

> ☝️ The daemon requires Accessibility permission to manage the Dock's position.

### From brew

```sh
brew install mishamyrt/tap/lockdock
lockdock enable # enable background daemon
```

The `lockdock enable` command creates `~/Library/LaunchAgents/co.myrt.lockdock.plist` and starts the daemon in the background.

### From sources

Clone the repository, open it in your terminal, and run the following commands:

```sh
make
make install
lockdock enable # enable background daemon
```

By default, the binaries are installed in `~/.local/bin`. If `lockdock` isn't found, add this directory to your `PATH`.

## Usage

### Raycast

The most convenient way to use `lockdock` is through the [Raycast extension](https://github.com/mishamyrt/lockdock-raycast). It displays a list of your monitors, the current Dock position, and lets you pin it to whichever display you want.

### CLI

Control commands:

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

The daemon IPC protocol is described in [docs/ipc.md](docs/ipc.md).

## License

MIT
