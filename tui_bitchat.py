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
from dataclasses import dataclass, field
from datetime import datetime
from typing import Dict, List, Optional, Tuple

# ---------- domain state ----------


@dataclass
class Message:
    ts: datetime
    direction: str   # "in" | "out" | "sys"
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

# ---------- small helpers ----------


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
            ["hciconfig", adapter], text=True, stderr=subprocess.DEVNULL)
        # extract "BD Address: XX:XX:..."
        m = re.search(r"BD Address:\s*([0-9A-F:]{17})", out, re.I)
        if m:
            return m.group(1)
    except Exception:
        pass
    # last resort
    return os.uname().nodename


async def run_bitchatctl(sock_path: str, *args: str) -> None:
    sock_path = os.path.expanduser(sock_path)
    cmd = ["./build/bin/bitchatctl", "--sock", sock_path, *args]
    p = await asyncio.create_subprocess_exec(
        *cmd, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT
    )
    out, _ = await p.communicate()
    if p.returncode != 0:
        msg = out.decode(errors="replace").strip()
        raise RuntimeError(
            f"bitchatctl {' '.join(args)} failed ({p.returncode}): {msg}")

# ---------- daemon manager ----------


class DaemonProc:
    """Wraps one bitchatd instance and ensures forceful cleanup on exit."""

    def __init__(self, role: str, sock: str, env_extra: dict):
        self.role = role
        self.sock = os.path.expanduser(sock)
        self.env_extra = env_extra
        self.proc: Optional[asyncio.subprocess.Process] = None

    async def start(self):
        os.makedirs(os.path.dirname(self.sock), exist_ok=True)
        # Leave ~/.cache permissions as-is; bitchatd will unlink its own socket on exit.
        env = os.environ.copy()
        env.update({
            "BITCHAT_TRANSPORT": "bluez",
            "BITCHAT_ROLE": self.role,
            "BITCHAT_CTL_SOCK": self.sock,
            "BITCHAT_LOG_LEVEL": os.environ.get("BITCHAT_LOG_LEVEL", "INFO"),
        })
        env.update(self.env_extra or {})
        cmd = ["./build/bin/bitchatd"]
        self.proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
            env=env,
            # start new process group (so we can TERM/KILL the group)
            preexec_fn=os.setsid,
        )

    async def stop(self):
        """Try QUIT; then TERM process group; then KILL process group."""
        if not self.proc:
            return
        proc = self.proc
        self.proc = None  # prevent double-stop races

        # Get process group id once
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

        # 1) Graceful via control socket
        try:
            await run_bitchatctl(self.sock, "quit")
        except Exception:
            pass
        if await wait_done(1.5):
            return

        # 2) SIGTERM process group
        try:
            if pgid is not None:
                os.killpg(pgid, signal.SIGTERM)
            else:
                proc.terminate()
        except ProcessLookupError:
            return
        if await wait_done(1.5):
            return

        # 3) SIGKILL process group
        try:
            if pgid is not None:
                os.killpg(pgid, signal.SIGKILL)
            else:
                proc.kill()
        except ProcessLookupError:
            return
        await proc.wait()


class DaemonManager:
    """Starts/stops both roles; parses logs to keep status/peers; proxies SEND."""

    def __init__(self, state: ChatState):
        self.state = state
        self.central = DaemonProc(
            "central", "~/.cache/bitchat-clone/central.sock", env_extra={})
        self.periph = DaemonProc(
            "peripheral", "~/.cache/bitchat-clone/peripheral.sock", env_extra={})
        self._tasks: List[asyncio.Task] = []

        # Status flags for top bar
        self.local_id: str = detect_local_id("hci0")
        self.central_connected: bool = False
        self.central_ready: bool = False
        self.central_discovering: bool = False
        self.periph_adv: bool = False

        # Regexes pulled from current bitchat logs
        self.rx_recv = re.compile(r"\[RECV\]\s+(.*)$")
        self.rx_found = re.compile(
            r"found\s+(\S+)\s+addr=([0-9A-F:]{17})", re.I)
        self.rx_connected = re.compile(r"Device connected:\s+(\S+)")
        self.rx_disconnected = re.compile(r"Disconnected\s+\((\S+)\)")
        self.rx_ready = re.compile(r"Notifications enabled; ready")
        self.rx_start_disc = re.compile(r"StartDiscovery OK", re.I)
        self.rx_stop_disc = re.compile(r"StopDiscovery OK", re.I)
        self.rx_adv_ok = re.compile(
            r"LE advertisement registered successfully")

    async def start(self):
        await self.central.start()
        await self.periph.start()
        if self.central.proc and self.central.proc.stdout:
            self._tasks.append(asyncio.create_task(
                self._read_loop("central", self.central.proc.stdout)))
        if self.periph.proc and self.periph.proc.stdout:
            self._tasks.append(asyncio.create_task(
                self._read_loop("peripheral", self.periph.proc.stdout)))

    async def stop(self):
        for t in self._tasks:
            t.cancel()
        self._tasks.clear()
        await self.central.stop()
        await self.periph.stop()
        self.central_connected = False
        self.central_ready = False
        self.central_discovering = False
        self.periph_adv = False

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
            self._on_log(tag, text)

    def _on_log(self, tag: str, line: str):
        # Incoming chat payload (already parsed by daemon)
        m = self.rx_recv.search(line)
        if m:
            pid = self.state.current_peer or tag
            self.state.add_msg(pid, Message(datetime.now(), "in", m.group(1)))
            return

        # Found device during scan -> build peer list (using MAC as key/display)
        m = self.rx_found.search(line)
        if m:
            _, mac = m.group(1), m.group(2)
            self.state.upsert_peer(mac, display=mac)
            return

        # Central connect/disconnect
        m = self.rx_connected.search(line)
        if m:
            self.central_connected = True
            self.central_ready = False
            pid = self.state.current_peer or "peer"
            self.state.add_msg(pid, Message(
                datetime.now(), "sys", "[central] connected"))
            return
        m = self.rx_disconnected.search(line)
        if m:
            self.central_connected = False
            self.central_ready = False
            pid = self.state.current_peer or "peer"
            self.state.add_msg(pid, Message(
                datetime.now(), "sys", "[central] disconnected"))
            return

        # Notify enabled -> ready to chat
        if tag == "central" and self.rx_ready.search(line):
            self.central_ready = True
            pid = self.state.current_peer or "peer"
            self.state.add_msg(pid, Message(
                datetime.now(), "sys", "[central] ready"))
            return

        # Discovery on/off
        if tag == "central" and self.rx_start_disc.search(line):
            self.central_discovering = True
            return
        if tag == "central" and self.rx_stop_disc.search(line):
            self.central_discovering = False
            return

        # Peripheral advertising OK
        if tag == "peripheral" and self.rx_adv_ok.search(line):
            self.periph_adv = True
            return

    def has_selected_peer(self) -> bool:
        return bool(self.state.current_peer and self.state.current_peer in self.state.peers)

    def is_ready(self) -> bool:
        return self.has_selected_peer() and self.central_ready

    async def send_text(self, text: str):
        if not self.is_ready():
            raise RuntimeError("not connected/subscribed")
        await run_bitchatctl(self.central.sock, "send", text)

    async def switch_peer(self, peer_mac: str):
        # Restart central with a peer filter env
        await self.central.stop()
        self.central = DaemonProc(
            "central", "~/.cache/bitchat-clone/central.sock",
            env_extra={"BITCHAT_PEER_ADDR": peer_mac}
        )
        self.central_connected = False
        self.central_ready = False
        await self.central.start()
        if self.central.proc and self.central.proc.stdout:
            self._tasks.append(asyncio.create_task(
                self._read_loop("central", self.central.proc.stdout)))
        self.state.current_peer = peer_mac

    def status_summary(self) -> str:
        c = "ready" if self.central_ready else ("connected" if self.central_connected else (
            "scanning" if self.central_discovering else "idle"))
        p = "adv" if self.periph_adv else "no-adv"
        return f"My ID: {self.local_id} | central: {c} | peripheral: {p}"

# ---------- UI ----------


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

    def compose(self) -> ComposeResult:
        yield Label("Peers", id="peers-title")
        yield self.list

    def refresh_peers(self):
        snapshot = tuple((pid, p.display, p.is_connected)
                         for pid, p in self.app_ref.state.peers.items())
        if snapshot == self._last_snapshot:
            return
        self._last_snapshot = snapshot

        current_pid = self.app_ref.state.current_peer
        keys = list(self.app_ref.state.peers.keys())
        current_idx = keys.index(current_pid) if current_pid in keys else None

        self.list.clear()
        for pid, p in self.app_ref.state.peers.items():
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
        pid = list(self.app_ref.state.peers.keys())[idx]
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
            who = "You" if m.direction == "out" else (
                "Peer" if m.direction == "in" else "Sys")
            lines.append(f"[{t}] {who}: {m.text}")
        self.chat_content.update("\n".join(lines) or "(empty)")


class InputBar(Static):
    def __init__(self, app_ref: "BitChat"):
        super().__init__()
        self.app_ref = app_ref
        self.input = Input(
            placeholder="Type message… (Enter to send)", id="chat-input")
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
            self.input.placeholder = "Type message… (Enter to send)"
        else:
            if not self.app_ref.manager.has_selected_peer():
                self.input.placeholder = "Select a peer to start chatting"
            elif not self.app_ref.manager.central_ready:
                self.input.placeholder = "Waiting for connection…"
            else:
                self.input.placeholder = "Not ready"

    async def on_input_submitted(self, event: Input.Submitted):
        text = event.value.strip()
        if not text:
            return
        if not self.enabled:
            pid = self.app_ref.state.current_peer or "peer"
            self.app_ref.state.add_msg(pid, Message(
                datetime.now(), "sys", "send blocked (not ready)"))
            self.app_ref.refresh_all()
            self.input.value = ""
            return

        pid = self.app_ref.state.current_peer or "peer"
        self.app_ref.state.add_msg(pid, Message(datetime.now(), "out", text))
        self.app_ref.refresh_all()
        try:
            await self.app_ref.manager.send_text(text)
        except Exception as e:
            self.app_ref.state.add_msg(pid, Message(
                datetime.now(), "sys", f"send failed: {e}"))
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
        # Periodically refresh UI and top bar status/clock
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
        """Override default quit to ensure daemons are stopped before exit."""
        try:
            await self.manager.stop()
        finally:
            self.exit()


async def main():
    state = ChatState()
    mgr = DaemonManager(state)
    app = BitChat(mgr, state)

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(
                sig, lambda s=sig: asyncio.create_task(mgr.stop()))
        except NotImplementedError:
            pass

    try:
        await app.run_async()
    finally:
        # Hard guarantee: even if the UI crashes or Ctrl-C happens mid-frame
        await mgr.stop()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
