# BitChat-clone

Minimal BLE 1:1 end-to-end encrypted messenger

## **Features**

- Minimal TUI (Textual) that runs both daemons and shows chats
- CLI <-> daemon IPC
- Fragmentation over small MTU links (12-byte header)
- Encrpytion: AEAD with `XChaCha20-Poly1305` (PSK)
- BLE (BlueZ) transport

---

## Demo

1. 1-to-1 messaging
<video src="https://github.com/user-attachments/assets/0a428946-08bd-4923-b568-e4164c54a4bc" height="480"/>

2. 1-to-many messaging
<video src="https://github.com/user-attachments/assets/1aec2156-03a2-4c0d-af59-d1beeeb13da5" height="480"/>

3. Man-in-the-middle attack (drop message if PSK conflicts)
<video src="https://github.com/user-attachments/assets/5a4962ae-b145-4f5f-9a19-4badabb6474b" height="480"/>

## Design doc

For design rationale and detailed architecture, please refer to [doc/design_doc.md](https://github.com/fuchengh/bitchat-clone/doc/design_doc.md).


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
> [!NOTE]
> TUI uses role-specific sockets:  `~/.cache/bitchat-clone/[central|peripheral].sock`

## Quickstart (recommended):

> [!NOTE]
> Ensure `bluetoothd` is running on your system.

1. Download the latest release archive from [Releases](https://github.com/fuchengh/bitchat-clone/releases) (this contains prebuilt binaries and scripts).
2. (Optional) Adjust settings in `config` file as needed. (`config` is copied from `.env`)
3. Start BitChat via the launcher script: `./bitchat.sh`, this will run the central and peripheral daemons and enable 1‑to‑many messaging with TUI.
4. Use the TUI or CLI to send messages

> [!NOTE]
> Config: please refer to `.env` file.

TUI:
- Left panel = discovered peers.
- Middle = chat window.
- Input is **disabled** until a peer is selected and central is `ready`.
- Top bar shows: My ID and BLE status.
- Press q (or ctrl-q) to quit.

## Quickstart (manual)

### Python TUI

```bash
# 1) Configure and activate .env
source .env

# 2) Run the TUI (spawns bitchatd central+peripheral and shows UI)
python ./tui_bitchat.py
```

### Manual daemon + CLI

- Example: Sending from **Central A** to **Peripheral B**

Terminal A — start central role:

```bash
BITCHAT_ROLE=central BITCHAT_TRANSPORT=bluez BITCHAT_CTL_SOCK=/tmp/bitchat-central.sock ./build/bin/bitchatd
```

Terminal B - start peripheral role:

```bash
BITCHAT_ROLE=peripheral BITCHAT_TRANSPORT=bluez BITCHAT_CTL_SOCK=/tmp/bitchat-peripheral.sock ./build/bin/bitchatd
```

Terminal C — talk to central (B) daemon via socket:

```bash
./build/bin/bitchatctl --sock /tmp/bitchat-central.sock tail on
./build/bin/bitchatctl --sock /tmp/bitchat-central.sock send "hello"
./build/bin/bitchatctl --sock /tmp/bitchat-central.sock quit
```

CLI Usage

```bash
bitchatctl [--sock <path>] <command> [args]

Commands:
  send <text...>             # send one line of text
  tail on|off                # toggle printing of received messages in daemon logs
  peers                      # list known peers
  connect AA:BB:CC:DD:EE:FF  # connect to peer (central only)
  disconnet                  # disconnect from current peer (central only)
  quit                       # ask daemon to exit
```

## Security (AEAD)

By default the daemon uses a **Noop AEAD** (plaintext)

Enable `XChaCha20-Poly1305` with a 32-byte PSK:

```bash
export BITCHAT_PSK="$(openssl rand -hex 32)" # or in config file
./build/bin/bitchatd
```

- `BITCHAT_PSK` = 64 hex chars (32 bytes).
- Wire format: `[24B nonce | ciphertext || tag]`.
- AEAD is applied **before** fragmentation. Reassembly happens **before** decryption.

> [!IMPORTANT]
> Without `BITCHAT_PSK`, traffic is **NOT** encrypted
> 
> If PSK mismatched on local/peer, all messages will be dropped

## Tests

```bash
ctest --test-dir build --output-on-failure
```
