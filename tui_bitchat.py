#!/usr/bin/env python3
# tui_bitchat.py
# Minimal Textual TUI that runs two bitchatd daemons (central & peripheral),
# tails their logs to build a peer list, and lets you chat with the selected peer.
# - Top bar shows local ID (adapter MAC or $BITCHAT_LOCAL_ID) and BLE status.
# - Input is disabled until a peer is selected AND central is "ready" (notify on).

from textual.containers import Horizontal, Vertical, Container
from textual.widgets import Footer, Static, Input, ListView, ListItem, Label
from textual.app import App, ComposeResult
import asyncio
import os
import re
import signal
import subprocess
import shutil
from dataclasses import dataclass, field
from datetime import datetime
from typing import Dict, List, Optional, Tuple
import io

# ======================= Domain state =======================


@dataclass
class Message:
    ts: datetime
    direction: str  # "in" | "out" | "sys"
    text: str


@dataclass
class Peer:
    peer_id: str
    display: str
    is_connected: bool = False
    history: List[Message] = field(default_factory=list)
    last_seen: Optional[datetime] = None


class ChatState:
    def __init__(self):
        self.peers: Dict[str, Peer] = {}
        self.current_peer: Optional[str] = None

    def upsert_peer(self, peer_id: str, display: Optional[str] = None) -> Peer:
        p = self.peers.get(peer_id)
        if not p:
            p = Peer(peer_id=peer_id, display=display or peer_id)
            self.peers[peer_id] = p
            if not self.current_peer:
                self.current_peer = peer_id
        if display:
            p.display = display
        p.last_seen = datetime.now()
        return p

    def add_msg(self, peer_id: str, msg: Message):
        p = self.upsert_peer(peer_id)
        p.history.append(msg)
        if len(p.history) > 1000:
            del p.history[:-1000]


# ======================= Small helpers =======================


def detect_local_id(adapter: str = "hci0") -> str:
    """Return a human-readable local ID: env override -> sysfs MAC -> hciconfig -> hostname."""
    env_id = os.environ.get("BITCHAT_LOCAL_ID")
    if env_id:
        return env_id
    sysfs = f"/sys/class/bluetooth/{adapter}/address"
    try:
        with open(sysfs, "r") as f:
            return f.read().strip()
    except Exception:
        pass
    try:
        out = subprocess.check_output(
            ["hciconfig", adapter], text=True, stderr=subprocess.DEVNULL
        )
        m = re.search(r"BD Address:\s*([0-9A-F:]{17})", out, re.I)
        if m:
            return m.group(1)
    except Exception:
        pass
    return os.uname().nodename


async def run_bitchatctl(sock_path: str, *args: str) -> None:
    """Run bitchatctl against a given AF_UNIX socket with args like ('send', 'hello')."""
    sock_path = os.path.expanduser(sock_path)
    cmd = ["./build/bin/bitchatctl", "--sock", sock_path, *args]
    p = await asyncio.create_subprocess_exec(
        *cmd, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT
    )
    out, _ = await p.communicate()
    if p.returncode != 0:
        msg = out.decode(errors="replace").strip()
        raise RuntimeError(
            f"bitchatctl {' '.join(args)} failed ({p.returncode}): {msg}"
        )


# ======================= Daemon manager =======================


class DaemonProc:
    """Wraps one bitchatd instance and ensures forceful cleanup on exit."""

    def __init__(self, role: str, sock: str, env_extra: dict):
        self.role = role
        self.sock = os.path.expanduser(sock)
        self.env_extra = env_extra
        self.proc: Optional[asyncio.subprocess.Process] = None
        # File logging members
        self.log_dir: Optional[str] = None
        self.log_path: Optional[str] = None
        self.log_fp: Optional[io.TextIOBase] = None

    async def start(self):
        os.makedirs(os.path.dirname(self.sock), exist_ok=True)
        env = os.environ.copy()
        env.update(
            {
                "BITCHAT_TRANSPORT": "bluez",
                "BITCHAT_ROLE": self.role,
                "BITCHAT_CTL_SOCK": self.sock,
                "BITCHAT_LOG_LEVEL": os.environ.get("BITCHAT_LOG_LEVEL", "INFO"),
            }
        )
        env.update(self.env_extra or {})
        cmd = ["./build/bin/bitchatd"]
        stdbuf = shutil.which("stdbuf")
        if stdbuf:
            cmd = [stdbuf, "-oL", "-eL"] + cmd  # line-buffer both streams

        # Prepare log file path (one per daemon start)
        self.log_dir = os.path.expanduser(
            os.environ.get("BITCHAT_TUI_LOG_DIR", "~/.cache/bitchat-clone/logs")
        )
        os.makedirs(self.log_dir, exist_ok=True)
        self.log_path = os.path.join(self.log_dir, f"{self.role}.log")
        # Open as line-buffered text file
        try:
            self.log_fp = open(self.log_path, "a", encoding="utf-8", buffering=1)
        except Exception:
            self.log_fp = None

        self.proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
            env=env,
            preexec_fn=os.setsid,  # own process group
        )

    async def stop(self):
        """Try QUIT; then TERM process group; then KILL process group."""
        if not self.proc:
            self._close_logfile()
            return

        proc = self.proc
        self.proc = None  # avoid races

        try:
            pgid = os.getpgid(proc.pid)
        except Exception:
            pgid = None

        async def wait_done(timeout: float) -> bool:
            try:
                await asyncio.wait_for(proc.wait(), timeout)
                return True
            except asyncio.TimeoutError:
                return False

        try:
            # 1) Graceful via control socket
            try:
                await run_bitchatctl(self.sock, "quit")
            except Exception:
                pass
            if await wait_done(1.5):
                return

            # 2) SIGTERM group
            try:
                if pgid is not None:
                    os.killpg(pgid, signal.SIGTERM)
                else:
                    proc.terminate()
            except ProcessLookupError:
                return
            if await wait_done(1.5):
                return

            # 3) SIGKILL group
            try:
                if pgid is not None:
                    os.killpg(pgid, signal.SIGKILL)
                else:
                    proc.kill()
            except ProcessLookupError:
                return
            await proc.wait()
        finally:
            self._close_logfile()

    def _close_logfile(self):
        if self.log_fp:
            try:
                self.log_fp.flush()
                self.log_fp.close()
            except Exception:
                pass
            self.log_fp = None
            self.log_path = None
            self.log_dir = None


class DaemonManager:
    """Starts/stops both roles; parses logs to keep status/peers; proxies SEND."""

    def __init__(self, state: ChatState):
        self.state = state
        self.central = DaemonProc(
            "central", "~/.cache/bitchat-clone/central.sock", env_extra={}
        )
        self.periph = DaemonProc(
            "peripheral", "~/.cache/bitchat-clone/peripheral.sock", env_extra={}
        )
        self._tasks: List[asyncio.Task] = []

        # Top bar status
        self.local_id: str = detect_local_id("hci0")
        self.user_id_env: str = (os.environ.get("BITCHAT_USER_ID") or "").strip()
        self.central_connected = False
        self.central_ready = False
        self.central_discovering = False
        self.periph_adv = False

        # Regex from current daemon logs
        ## peer state
        self.rx_recv = re.compile(r"\[RECV\]\s+(.*)$")
        self.rx_found = re.compile(r"found\s+(\S+)\s+addr=([0-9A-F:]{17})", re.I)
        self.rx_connected = re.compile(r"Device connected:\s+(\S+)")
        self.rx_connected_prop = re.compile(r"Connected property became true \((\S+)\)")
        self.rx_disconnected_path = re.compile(r"Disconnected\s+\((\S+)\)")
        self.rx_iface_removed = re.compile(r"InterfacesRemoved -> cleared device (\S+)")
        self.rx_ready = re.compile(r"Notifications enabled; ready")
        self.rx_start_disc = re.compile(r"StartDiscovery OK", re.I)
        self.rx_stop_disc = re.compile(r"StopDiscovery OK", re.I)
        self.rx_adv_ok = re.compile(r"LE advertisement registered successfully")
        self.rx_listen = re.compile(r"Listening on\s+(\S+)")
        # parse user id
        self.rx_ctrl_hello_in = re.compile(
            r"\[CTRL\]\s+HELLO in:\s+user='([^']*)'\s+caps=0x([0-9A-Fa-f]{8})"
        )
        self.rx_ctrl_hello_out = re.compile(
            r"\[CTRL\]\s+HELLO out:\s+user='([^']*)'\s+caps=0x([0-9A-Fa-f]{8})"
        )
        # KEX / SEC events from daemon logs
        self.rx_kex_ok = re.compile(r"\[KEX\]\s+complete\.", re.I)
        self.rx_kex_fail = re.compile(r"\[KEX\].*(install failed|no/invalid PSK)", re.I)
        self.rx_psk_fail = re.compile(
            r"\[SEC\].*AEAD decrypt failed.*dropping frame", re.I
        )
        self.sec_warn: bool = False  # any security warning
        self.aead_active: bool = False  # AEAD session installed and on

        # record current active peer's mac
        self.active_mac: Optional[str] = None

        # check if PSK is enabled on local/peer
        self.psk_local = bool(os.environ.get("BITCHAT_PSK"))
        self.psk_peer = False

        # track /org/bluez/... dev path -> MAC
        self.dev_to_mac: Dict[str, str] = {}

    async def start(self):
        await self.central.start()
        await self.periph.start()
        if self.central.proc and self.central.proc.stdout:
            self._tasks.append(
                asyncio.create_task(
                    self._read_loop("central", self.central.proc.stdout)
                )
            )
        if self.periph.proc and self.periph.proc.stdout:
            self._tasks.append(
                asyncio.create_task(
                    self._read_loop("peripheral", self.periph.proc.stdout)
                )
            )

        # Wait until sockets exist then enable [RECV] printing on both daemons
        await self._wait_sock(self.central.sock)
        await self._wait_sock(self.periph.sock)
        for s in (self.central.sock, self.periph.sock):
            try:
                await run_bitchatctl(s, "tail", "on")
            except Exception:
                pass

    async def stop(self):
        to_cancel = [t for t in self._tasks if isinstance(t, asyncio.Task)]
        for t in to_cancel:
            t.cancel()
        if to_cancel:
            await asyncio.gather(*to_cancel, return_exceptions=True)
        self._tasks.clear()
        await self.central.stop()
        await self.periph.stop()
        self.central_connected = False
        self.central_ready = False
        self.central_discovering = False
        self.periph_adv = False
        self.dev_to_mac.clear()
        self.sec_warn = False

    async def _read_loop(self, tag: str, stream: asyncio.StreamReader):
        while True:
            line = await stream.readline()
            if not line:
                if tag == "central":
                    self.central_ready = False
                    self.central_connected = False
                    self.central_discovering = False
                elif tag == "peripheral":
                    self.periph_adv = False
                return
            text = line.decode(errors="replace").rstrip()
            # Tee to per-daemon log file
            try:
                fp = self.central.log_fp if tag == "central" else self.periph.log_fp
                if fp:
                    fp.write(text + "\n")
            except Exception:
                pass
            self._on_log(tag, text)

    def _on_log(self, tag: str, line: str):
        # Sec/KEX event first
        # AEAD decrypt failed, likely PSK mismatch: frames are dropped
        if self.rx_psk_fail.search(line):
            self.sec_warn = True
            self.aead_active = False
            mac = self.active_mac or self.state.current_peer or "peer"
            self.state.add_msg(
                mac,
                Message(
                    datetime.now(),
                    "sys",
                    "PSK mismatch: decryption failed, messages are being dropped...",
                ),
            )
            return

        # KEX complete: AEAD is enabled
        if self.rx_kex_ok.search(line):
            self.aead_active = True
            self.sec_warn = False
            mac = self.active_mac or self.state.current_peer or "peer"
            self.state.add_msg(mac, Message(datetime.now(), "sys", "AEAD enabled"))
            return

        # KEX failed: unusable until user fixed
        if self.rx_kex_fail.search(line):
            self.aead_active = False
            self.sec_warn = True
            mac = self.active_mac or self.state.current_peer or "peer"
            self.state.add_msg(
                mac,
                Message(
                    datetime.now(),
                    "sys",
                    "KEX failed. Please check BITCHAT_PSK and retry again",
                ),
            )
            return

        # 0) Set socket path (daemon might override)
        m = self.rx_listen.search(line)
        if m:
            path = m.group(1)
            if tag == "central":
                self.central.sock = path
            else:
                self.periph.sock = path
            return

        # 1) Incoming payload
        m = self.rx_recv.search(line)
        if m:
            pid = self.state.current_peer or tag
            self.state.add_msg(pid, Message(datetime.now(), "in", m.group(1)))
            return

        # 2) Discovery: remember dev_path -> MAC, create/refresh peer
        m = self.rx_found.search(line)
        if m:
            dev_path, mac = m.group(1), m.group(2)
            self.dev_to_mac[dev_path] = mac
            self.state.upsert_peer(mac, display=mac)
            return

        # 3) Connected (two shapes)
        m = self.rx_connected.search(line) or self.rx_connected_prop.search(line)
        if m:
            dev_path = m.group(1)
            mac = self.dev_to_mac.get(dev_path, self.state.current_peer or "peer")
            p = self.state.upsert_peer(mac, display=mac)
            p.is_connected = True
            self.central_connected = True
            self.active_mac = mac
            self.central_ready = False
            self.state.add_msg(
                mac, Message(datetime.now(), "sys", "link up, resolving services...")
            )
            return

        # 4) Ready (enable is_connected for UI too)
        if tag == "central" and self.rx_ready.search(line):
            self.central_ready = True
            mac = self.state.current_peer
            if mac and mac in self.state.peers:
                self.state.peers[mac].is_connected = True
            self.state.add_msg(
                mac or "peer",
                Message(datetime.now(), "sys", "ready - notifications enabled"),
            )
            return

        # 5) Disconnect (two shapes)
        m = self.rx_disconnected_path.search(line) or self.rx_iface_removed.search(line)
        if m:
            dev_path = m.group(1)
            mac = self.dev_to_mac.get(dev_path, self.state.current_peer or "peer")
            p = self.state.upsert_peer(mac, display=mac)
            p.is_connected = False
            self.central_connected = False
            self.central_ready = False
            self.active_mac = None
            self.psk_peer = False
            self.aead_active = False
            self.state.add_msg(mac, Message(datetime.now(), "sys", "link down"))
            return

        # 6) Discovery & advertising toggles (for top bar)
        if tag == "central" and self.rx_start_disc.search(line):
            self.central_discovering = True
            return
        if tag == "central" and self.rx_stop_disc.search(line):
            self.central_discovering = False
            return
        if tag == "peripheral" and self.rx_adv_ok.search(line):
            self.periph_adv = True
            return

        # 7) Parse HELLO in/out and update UI
        m = self.rx_ctrl_hello_in.search(line)
        if m:
            user = m.group(1)
            caps = int(m.group(2), 16)
            if self.active_mac:
                disp = (
                    user if user else self.active_mac
                )  # if user id is not set, use mac addr
                self.state.upsert_peer(self.active_mac, display=disp)
                self.psk_peer = bool(caps & 0x1)  # bit0 = AEAD_PSK_SUPPORTED
                self.state.add_msg(
                    self.active_mac,
                    Message(
                        datetime.now(),
                        "sys",
                        f"(hello) peer id is '{user or '<none>'}'",
                    ),
                )
            return

        # 8) Show my PSK and peer's PSK state
        m = self.rx_ctrl_hello_out.search(line)
        if m and tag == "central":
            caps = int(m.group(2), 16)
            self.psk_local = bool(caps & 0x1)
            return

    def has_selected_peer(self) -> bool:
        return bool(
            self.state.current_peer and self.state.current_peer in self.state.peers
        )

    def is_ready(self) -> bool:
        return self.has_selected_peer() and self.central_ready

    async def send_text(self, text: str):
        if not self.is_ready():
            raise RuntimeError("not connected/subscribed")
        await run_bitchatctl(self.central.sock, "send", text)

    async def switch_peer(self, peer_mac: str):
        # cleanup finished tasks to avoid growth
        self._tasks = [t for t in self._tasks if not t.done()]
        # Restart central with a peer filter env
        await self.central.stop()
        self.central = DaemonProc(
            "central",
            "~/.cache/bitchat-clone/central.sock",
            env_extra={"BITCHAT_PEER_ADDR": peer_mac},
        )
        self.central_connected = False
        self.central_ready = False
        self.dev_to_mac.clear()
        await self.central.start()
        if self.central.proc and self.central.proc.stdout:
            self._tasks.append(
                asyncio.create_task(
                    self._read_loop("central", self.central.proc.stdout)
                )
            )
        # Re-enable tail for the new central daemon so [RECV] shows up
        await self._wait_sock(self.central.sock)
        try:
            await run_bitchatctl(self.central.sock, "tail", "on")
        except Exception:
            pass
        self.state.current_peer = peer_mac

    def status_summary(self) -> str:
        c = (
            "ready"
            if self.central_ready
            else (
                "link"
                if self.central_connected
                else ("scan" if self.central_discovering else "idle")
            )
        )
        p = "adv" if self.periph_adv else "no-adv"
        # SEC badge
        if self.aead_active and self.psk_local and self.psk_peer and not self.sec_warn:
            sec = "🔐"  # AEAD on
        elif self.psk_local and self.psk_peer and self.sec_warn:
            sec = "🔐⚠"  # Both support PSK but mismatch or likely decrypt fail
        else:
            sec = "🔓"  # plaintext (missing PSK or no KEX)

        peer_disp = "-"
        if self.active_mac and self.active_mac in self.state.peers:
            peer_disp = self.state.peers[self.active_mac].display
        myid = self.user_id_env or self.local_id
        return f"My ID: {myid} | central: {c} | peripheral: {p} | peer: {peer_disp} | sec: {sec}"

    async def _wait_sock(self, path: str, timeout: float = 3.0):
        """Wait until the AF_UNIX control socket is listening."""
        path = os.path.expanduser(path)
        loop = asyncio.get_running_loop()
        end = loop.time() + timeout
        while loop.time() < end:
            if os.path.exists(path):
                try:
                    reader, writer = await asyncio.open_unix_connection(path)
                    writer.close()
                    try:
                        await writer.wait_closed()
                    except Exception:
                        pass
                    return
                except Exception:
                    pass
            await asyncio.sleep(0.05)


# ======================= UI =======================


class TopBar(Static):
    """Simple top bar (no command palette), shows clock + ID + BLE status."""

    def __init__(self, app_ref: "BitChat"):
        super().__init__(id="top")
        self.app_ref = app_ref
        self._text = ""

    def refresh_bar(self):
        now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        stat = self.app_ref.manager.status_summary()
        text = f" BitChat — {stat} — {now} "
        if text != self._text:
            self._text = text
            self.update(self._text)


class PeersPanel(Static):
    def __init__(self, app_ref: "BitChat"):
        super().__init__()
        self.app_ref = app_ref
        self.list = ListView()
        self._last_snapshot: Optional[Tuple[Tuple[str, str, bool], ...]] = None
        self.view_pids: List[str] = []

    def compose(self) -> ComposeResult:
        yield Label("Peers", id="peers-title")
        yield self.list

    def refresh_peers(self):
        snapshot = tuple(
            (pid, p.display, p.is_connected)
            for pid, p in self.app_ref.state.peers.items()
        )
        if snapshot == self._last_snapshot:
            return
        self._last_snapshot = snapshot

        self.list.clear()
        peers_sorted = sorted(
            self.app_ref.state.peers.items(),
            key=lambda kv: (kv[1].display.lower(), kv[0]),
        )
        self.view_pids = [pid for pid, _ in peers_sorted]
        current_pid = self.app_ref.state.current_peer
        current_idx = (
            self.view_pids.index(current_pid) if current_pid in self.view_pids else None
        )

        for pid, p in peers_sorted:
            dot = "● " if p.is_connected else "○ "
            self.list.append(ListItem(Label(f"{dot}{p.display}")))

        if current_idx is not None:
            try:
                self.list.index = current_idx
            except Exception:
                pass

    async def on_list_view_selected(self, event: ListView.Selected):
        idx = event.index
        if idx is None:
            return
        if idx < 0 or idx >= len(self.view_pids):
            return
        pid = self.view_pids[idx]
        await self.app_ref.manager.switch_peer(pid)
        self.app_ref.refresh_all()


class ChatView(Static):
    def __init__(self, app_ref: "BitChat"):
        super().__init__()
        self.app_ref = app_ref
        self.chat_content = Static("", id="chat-content", expand=True)

    def compose(self) -> ComposeResult:
        yield Label("Chat", id="chat-title")
        yield self.chat_content

    def refresh_chat(self):
        pid = self.app_ref.state.current_peer
        if not pid or pid not in self.app_ref.state.peers:
            self.chat_content.update("(no peer)")
            return
        lines = []
        for m in self.app_ref.state.peers[pid].history[-300:]:
            t = m.ts.strftime("%H:%M:%S")
            who = (
                "You"
                if m.direction == "out"
                else ("Peer" if m.direction == "in" else "Sys")
            )
            lines.append(f"[{t}] {who}: {m.text}")
        self.chat_content.update("\n".join(lines) or "(empty)")


class InputBar(Static):
    def __init__(self, app_ref: "BitChat"):
        super().__init__()
        self.app_ref = app_ref
        self.input = Input(placeholder="Type message... (Enter to send)", id="chat-input")
        self.enabled: bool = True

    def compose(self) -> ComposeResult:
        yield self.input

    def set_enabled(self, enabled: bool):
        self.enabled = enabled
        try:
            self.input.disabled = not enabled
        except Exception:
            pass
        if enabled:
            # If PSK mismatched: notify user that text will be dropped
            if (
                self.app_ref.manager.sec_warn
                and self.app_ref.manager.psk_local
                and self.app_ref.manager.psk_peer
            ):
                self.input.placeholder = "Connected, but PSK mismatch (messages dropped)"
            else:
                self.input.placeholder = "Type message... (Enter to send)"

        else:
            if not self.app_ref.manager.has_selected_peer():
                self.input.placeholder = "Select a peer to start chatting"
            elif not self.app_ref.manager.central_ready:
                self.input.placeholder = "Waiting for connection..."
            else:
                self.input.placeholder = "Not ready"

    async def on_input_submitted(self, event: Input.Submitted):
        text = event.value.strip()
        if not text:
            return
        if not self.enabled:
            pid = self.app_ref.state.current_peer or "peer"
            self.app_ref.state.add_msg(
                pid, Message(datetime.now(), "sys", "send blocked (not ready)")
            )
            self.app_ref.refresh_all()
            self.input.value = ""
            return

        pid = self.app_ref.state.current_peer or "peer"
        self.app_ref.state.add_msg(pid, Message(datetime.now(), "out", text))
        self.app_ref.refresh_all()
        try:
            await self.app_ref.manager.send_text(text)
        except Exception as e:
            self.app_ref.state.add_msg(
                pid, Message(datetime.now(), "sys", f"send failed: {e}")
            )
            self.app_ref.refresh_all()
        finally:
            self.input.value = ""


class BitChat(App):
    CSS = """
    Screen { layout: vertical; }
    #top { height: 1; content-align: left middle; background: $panel; }
    #main { height: 1fr; }
    #input { height: 3; }
    Horizontal { height: 1fr; }
    #left { width: 28; border: round $accent; }
    #center { border: round $accent; }
    #chat-content { height: 1fr; overflow: auto; }
    """
    # Disable built-in command palette/search (keep only quit)
    BINDINGS = [
        ("q", "quit", "Quit"),
        ("/", "noop", ""),
        ("ctrl+k", "noop", ""),
        ("ctrl+p", "noop", ""),
    ]

    def __init__(self, manager: DaemonManager, state: ChatState):
        super().__init__()
        self.manager = manager
        self.state = state
        self.topbar = TopBar(self)
        self.peers_panel = PeersPanel(self)
        self.chat_view = ChatView(self)
        self.input_bar = InputBar(self)
        self._ui_updater: Optional[asyncio.Task] = None

    # Safety override even if framework provides a palette:
    def action_command_palette(self):  # type: ignore[override]
        pass

    def action_noop(self):
        pass

    def compose(self) -> ComposeResult:
        yield self.topbar
        with Horizontal(id="main"):
            with Vertical(id="left"):
                yield self.peers_panel
            with Vertical(id="center"):
                yield self.chat_view
        with Container(id="input"):
            yield self.input_bar
        yield Footer()

    async def on_mount(self):
        await self.manager.start()
        self._ui_updater = asyncio.create_task(self._refresh_loop())

    async def _refresh_loop(self):
        while True:
            self.refresh_all()
            await asyncio.sleep(0.25)

    def _update_input_enabled(self):
        self.input_bar.set_enabled(self.manager.is_ready())

    def refresh_all(self):
        self.topbar.refresh_bar()
        self.peers_panel.refresh_peers()
        self.chat_view.refresh_chat()
        self._update_input_enabled()

    async def action_quit(self) -> None:
        """Stop daemons before exit."""
        try:
            await self.manager.stop()
        finally:
            self.exit()


# ======================= Entrypoint =======================


async def main():
    state = ChatState()
    mgr = DaemonManager(state)
    app = BitChat(mgr, state)

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, lambda s=sig: asyncio.create_task(mgr.stop()))
        except NotImplementedError:
            pass

    try:
        await app.run_async()
    finally:
        await mgr.stop()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
