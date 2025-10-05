# Design Document for "bitchat-clone"

## 1. Introduction

* Goal: Ship a nearby messenger with end-to-end encryption over BLE (Linux + BlueZ).
* Scope (MVP): Single link; two GATT characteristics (TX=Notify, RX=Write with response); app-level fragmentation (~100 B payload/fragment); **PSK AEAD with XChaCha20‑Poly1305 (current)**; minimal IPC/CLI for control; **human-readable ID shown via HELLO after connect**.
* Non-goals: ACK/NAK/retransmit, safety numbers/QR, group/mesh, automatic key exchange, role contention.

## 2. Feature description

### 2.1 Architecture (layers)

* Transport: LoopbackTransport (in-process, same-thread callback). BlueZ GATT on Linux for real BLE.
* Proto: 12-byte fragment header + fragmenter/reassembler (payload <= 100 B/fragment).
* Crypto: **PskAead using XChaCha20‑Poly1305** (stream-safe nonce). HKDF-based session keys derived per connection.
* App: ChatService — plaintext -> AEAD -> fragment -> transport; reverse on RX.
* Ctl: Minimal AF_UNIX IPC line protocol between CLI and daemon.

### 2.2 Fixed constants

* Service UUID: 7e0f8f20-cc0b-4c6e-8a3e-5d21b2f8a9c4
* TX (Notify): 7e0f8f21-cc0b-4c6e-8a3e-5d21b2f8a9c4
* RX (Write w/ response): 7e0f8f22-cc0b-4c6e-8a3e-5d21b2f8a9c4
* Default app payload per fragment: 100 B
  * **Advertising**: includes the BitChat Service UUID only. We do **not** advertise user identity or hashed tags.
  * `LocalName` MAY be a generic string (e.g., `BitChat`) and SHOULD NOT leak the full `user_id`.

### 2.3 Roles

* Fixed central <-> peripheral; no dual-role negotiation.

### 2.4 Identity & Discovery (human-readable IDs)

* **Goal**: Let users see a human ID (e.g., `userAA`) in the config rather than raw BLE addresses.
* **Discovery**: Central scans for the BitChat Service UUID and selects a device (based on UI/operator choice).
* **HELLO after connect**: After a link is established, the peer sends a control-plane **HELLO** packet that carries its self‑chosen `user_id` (and capability bits). The UI updates the display name accordingly. No identity is embedded in advertisements.
* **Fallback**: If HELLO is not yet received, the UI shows the MAC address of the peer.

### 2.5 Fragment header (12 bytes, big-endian)

```txt
|-------|---------|----------|-------|---------|-------|
| 1 B     | 1 B       | 4 B        | 2 B     | 2 B       | 2 B     |
| ------- | --------- | ---------- | ------- | --------- | ------- |
| ver:1   | flags:1   | msg_id:4   | seq:2   | total:2   | len:2   |
| ------- | --------- | ---------- | ------- | --------- | ------- |
```

* msg_id groups fragments into a message.
* seq/total bound each fragment’s slot and message length.
* len is payload bytes following the header.

## 3. CLI

Protocol (one line per connection):

```bash
SEND <text…>
TAIL on|off
QUIT
```

Usage:
```bash
bitchatctl send <text...>
bitchatctl tail on|off
bitchatctl quit
  [--sock <path>]    # default: ~/.cache/bitchat-clone/ctl.sock
```

## 4. Documentation — Design rationale

### 4.1 TX/RX split (Notify vs. Write-with-response)

* What we do: Two characteristics: TX = Notify, RX = Write(with response).
* Why:
  * Fast outbound: Notifications are fire-and-forget (no app-level ACK), ideal for pushing chat payloads with low latency.
  * Confirmed inbound: Writes with response give a delivery confirmation when we receive control/user data.
  * Simple mapping: This splits concerns cleanly and matches BlueZ APIs (StartNotify, WriteValue), so implementation stays small and predictable.
* Failure model: Notifications can drop; we accept best-effort in the MVP. If a reliable path is needed later, switch specific flows to indications or add an app-layer retry.

### 4.2 App-level fragmentation (100-byte payload + 12-byte header)

* What we do: Always fragment ciphertext into <= 100 B payload chunks, each prefixed by a 12-byte header:

```txt
|-------|---------|----------|-------|---------|-------|
| ver:1 | flags:1 | msg_id:4 | seq:2 | total:2 | len:2 |
|-------|---------|----------|-------|---------|-------|
```

* Why:
  * MTU portability: BLE’s usable payload per notification is ATT_MTU − 3 and varies widely by device/OS. Fixing 100 B avoids edge-case failures without per-device tuning.
  * Deterministic plumbing: Upper layers never care about platform MTU; fragmenter owns sizing.
* Header fields (practical intent):
  * `ver/flags`: format control & future bits without breaking older peers.
  * `msg_id`: groups fragments of one logical message; prevents cross-mix with other messages.
  * `seq/total`: 0-based index and final count; enables complete-only delivery and out-of-order assembly.
  * `len`: exact bytes of payload in this fragment (last chunk is shorter).
* Reassembly rules: Buffer by (sender, msg_id); accept out-of-order; deliver only when all seq in [0,total) arrive. Evict incomplete messages after a timeout to cap memory.

### 4.3 Crypto boundary (PSK AEAD; XChaCha20‑Poly1305)

* We use **XChaCha20‑Poly1305** with a 32‑byte PSK. Transport/proto layers are unchanged by the choice of AEAD.
* We fragment ciphertext+tag across chunks. Every fragment carries its header and uses identical AAD derivation, so the receiver rejects mismatched or replayed pieces.

### 4.4 Fixed roles (central/peripheral)

* Why fixed roles: Avoids role arbitration and halves state space. Connection logic and error handling stay simple; we can generalize later if needed.

### 4.5 Human-readable identity (discovery)

* Identity is **self‑chosen** by the user and transported via **HELLO** after connect.

### 4.6 Mailbox & 1‑to‑many messaging

* 1‑to‑many messaging is achieved by iterating over connected peers and sending the same message to each via the existing TX/RX characteristics.
* All **incoming** messages are also written to the **Mailbox**. If a peer is connected, the same message **appears both in the chat window and in the Mailbox**.
* Each peer maintains its own fragmenter/reassembler state; messages are not interleaved.
* There is no mesh or multi‑hop routing. Messages are only delivered over direct BLE connections.

### 4.7 Explicit non-features (by design)

* No ACK/NAK/retransmit layer.
* No safety numbers/verification, group/mesh, etc.
* No platform-specific MTU tuning in upper layers (fragmenter owns sizing).

## 5. Workflow Example: A connects to B and sends a message

```
+----------------+                      +-------------------+
|       A        |                      |       B           |
| (Central Role) |                      | (Peripheral Role) |
+----------------+                      +-------------------+
       |                                      |
       |       1. BLE Scan                    |
       |------------------------------------->|
       |                                      |
       |       2. Advertise BitChat Service   |
       |<-------------------------------------|
       |                                      |
       |       3. Connect to Peripheral       |
       |------------------------------------->|
       |                                      |
       |       4. Discover GATT Services      |
       |------------------------------------->|
       |                                      |
       |       5. Enable Notifications (TX)   |
       |------------------------------------->|
       |                                      |
       |       6. Send "HELLO" Control Msg    |
       |------------------------------------->|
       |                                      |
       |       7. Receive "HELLO" Reply       |
       |<-------------------------------------|
       |                                      |
       |    **Connection Established**        |
       |                                      |
       |  8. Send Encrypted Message Payload   |
       |------------------------------------->|
       |                                      |
       |  9. Peripheral Notifies Data (TX)    |
       |<-------------------------------------|
       |                                      |
       |       **Message Delivered!**         |
       |                                      |
```

## 6. Man-in-the-Middle (MITM) Attack Scenario: PSK Mismatch

In the event of a man-in-the-middle (MITM) attack where the pre-shared key (PSK) between A and B does not match, the following occurs:

```
+----------------+                      +----------------+
|       A        |                      |   MITM Device  |
| (Central Role) |                      | (Impersonator) |
+----------------+                      +----------------+
       |                                      |
       |       1. BLE Scan                    |
       |------------------------------------->|
       |       2. Advertise Fake Service      |
       |<-------------------------------------|
       |                                      |
       |       3. Connect to Fake Peripheral  |
       |------------------------------------->|
       |                                      |
       |       4. Discover GATT Services      |
       |------------------------------------->|
       |                                      |
       |       5. Enable Notifications (TX)   |
       |------------------------------------->|
       |                                      |
       |       6. Send "HELLO" Control Msg    |
       |------------------------------------->|
       |                                      |
       |       7. HELLO Reply (Fake ID)       |
       |<-------------------------------------|
       |                                      |
       |    **Connection Established**        |
       |                                      |
       |  8. Send Encrypted Message Payload   |
       |------------------------------------->|
       |  9. Decryption Fails (PSK Mismatch)  |
       |------------------------------------->|
       |                                      |
       |  **Message Not Delivered!**          |
       |                                      |
```

### Notes on MITM Prevention:
- The mismatch in PSK causes decryption to fail, ensuring the integrity and confidentiality of messages.
