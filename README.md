# BitChat-clone

Minimal BLE 1:1 E2E-encrypted messenger (Linux + BlueZ).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j
```

## CLI

Daemon `build/bin/bitchatd`

```bash
Usage:
  bitchatd
```

Client `build/bin/bitchatctl`

```bash
Usage:
  bitchatctl [--sock <path>] <command> [args]

Commands:
  send <text...>
  tail on|off
  quit
```

## Quickstart

```bash
# 1. Start a temporary listener daemon (terminal A)
./build/bin/bitchatd

# 2. Send a command (terminal B)
./build/bin/bitchatctl send "hello world"
# -- Listener should print: "[RECV] hello world"
./build/bin/bitchatctl quit
# -- Listener should print: "Received QUIT command, exiting..."

# TODO:
./build/bin/bitchatctl tail on/off
```
