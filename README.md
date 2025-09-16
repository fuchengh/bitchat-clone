# BitChat-clone

Minimal BLE 1:1 end-to-end encrypted messenger

**Features**
- Minimal TUI (Textual) that runs both daemons and shows chats
- CLI <-> daemon IPC
- Fragmentation over small MTU links (12-byte header)
- Encrpytion: AEAD with `XChaCha20-Poly1305` (PSK)
- BLE (BlueZ) transport

---

## Dependencies (Ubuntu/Debian)

For build dependencies:
```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake pkg-config \
  libsystemd-dev \
  bluez
```

- libsodium: requires latest version (1.0.20)
  - Installation guide [https://doc.libsodium.org/installation](https://doc.libsodium.org/installation)

Python (for TUI):
```bash
pip install --upgrade pip
pip install textual rich
```

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j
```

- BlueZ transport is enabled automatically when `libsystemd-dev` is present.
- Binary executables in `build/bin/`

## What runs

- TUI (chat app, also spawns central & peripheral): `tui_bitchat.py`
- Daemon: `build/bin/bitchatd` - BLE central or peripheral
- CLI: `build/bin/bitchatctl` - talk to daemon via control socket

Default control socket (when started manually): `~/.cache/bitchat-clone/ctl.sock`
> TUI uses role-specific sockets:  ~/.cache/bitchat-clone/central.sock, ~/.cache/bitchat-clone/peripheral.sock

## Quickstart (recommended): TUI

Environment variables: please refer to `.env` file.
> Ensure bluetoothd is running on your system.

```bash
# 1) Configure and activate .env
source .env

# 2) Run the TUI (spawns bitchatd central+peripheral and shows UI)
python ./tui_bitchat.py
```

- Left panel = discovered peers.
- Middle = chat window.
- Input is **disabled** until a peer is selected and central is `ready`.
- Top bar shows: My ID and BLE status.
- Press q (or ctrl-q) to quit.

## Quickstart (manual, without TUI)

Terminal A - start peripheral role:

```bash
BITCHAT_ROLE=peripheral BITCHAT_TRANSPORT=bluez \
BITCHAT_CTL_SOCK=/tmp/bitchat-peripheral.sock \
./build/bin/bitchatd
```

Terminal B — start central role:

```bash
BITCHAT_ROLE=central BITCHAT_TRANSPORT=bluez \
BITCHAT_CTL_SOCK=/tmp/bitchat-central.sock \
./build/bin/bitchatd
```

Terminal C — talk to central daemon:

```bash
./build/bin/bitchatctl --sock /tmp/bitchat-central.sock tail on
./build/bin/bitchatctl --sock /tmp/bitchat-central.sock send "hello"
./build/bin/bitchatctl --sock /tmp/bitchat-central.sock quit
```

CLI Usage

```bash
bitchatctl [--sock <path>] <command> [args]

Commands:
  send <text...>   # send one line of text
  tail on|off      # toggle printing of received messages in daemon logs
  quit             # ask daemon to exit
```

## Security (AEAD)

By default the daemon uses a **Noop AEAD** (plaintext)

Enable `XChaCha20-Poly1305` with a 32-byte PSK:

```bash
export BITCHAT_PSK="$(openssl rand -hex 32)"
./build/bin/bitchatd
```

- `BITCHAT_PSK` = 64 hex chars (32 bytes).
- Wire format: `[24B nonce | ciphertext || tag]`.
- AEAD is applied **before** fragmentation. Reassembly happens **before** decryption.

> ⚠️ Without `BITCHAT_PSK`, traffic is **NOT** encrypted
>> ⚠️ If PSK mismatched on local/peer, all messages will be dropped

## Expected daemon log snippets (with BlueZ enabled)

Central:

```bash
[INFO]  [BLUEZ][central] StartDiscovery OK on /org/bluez/hci0
[DEBUG] [BLUEZ][central] found /org/bluez/hci0/dev_XX addr=AA:BB:CC:DD:EE:FF (svc hit)
[INFO]  [BLUEZ][central] Device connected: /org/bluez/hci0/dev_...
[INFO]  [BLUEZ][central] Notifications enabled on .../char...
[DEBUG] [BLUEZ][central] Notifications enabled; ready
```

Peripheral:

```bash
[DEBUG] [BLUEZ] tx.StartNotify
[DEBUG] [BLUEZ] rx.WriteValue len=...
[INFO]  [RECV] hello world
```

## Design sketch

```text
bitchatctl
   │  (AF_UNIX line-based IPC)
   ▼
bitchatd on_line()
   │
   ▼
ChatService
   ├─ AEAD.seal/open (XChaCha20-Poly1305)
   ├─ frag.make_chunks / reassembler (12-byte header)
   └─ transport (BlueZ)
```

## Tests

```bash
ctest --test-dir build --output-on-failure
```
