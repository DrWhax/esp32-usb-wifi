#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "pyserial>=3.5",
#     "textual>=1.0",
# ]
# ///
"""TUI for the esp32-usb-eth management console.

    uv run tools/tui.py              # autodetect the console port
    uv run tools/tui.py --port /dev/cu.usbmodemXXX
    uv run tools/tui.py --check     # no TUI: probe the port, run `show`, exit

Buttons cover the common commands; the input line takes anything the console
understands (join 3, use 2, del 1, set ssid ..., set pass ...). The log shows
everything the device prints, including the `dbg:` stream when debug is on.
"""

import argparse
import glob
import sys
import time

import serial

PROMPT = b"(set|scan|list|use|save) #"
GLOBS = ("/dev/cu.usbmodem*", "/dev/ttyACM*")


def candidate_ports(explicit=None):
    if explicit:
        return [explicit]
    return sorted(p for g in GLOBS for p in glob.glob(g))


def probe(dev):
    """Return an open Serial if `dev` answers the console prompt, else None."""
    try:
        port = serial.Serial(dev, 115200, timeout=0.2)
    except (OSError, serial.SerialException):
        return None
    time.sleep(0.4)
    port.reset_input_buffer()
    port.write(b"\rshow\r")
    deadline = time.time() + 1.5
    buf = b""
    while time.time() < deadline:
        buf += port.read(256)
        if PROMPT in buf:
            port.reset_input_buffer()
            return port
    port.close()
    return None


def find_console(explicit=None):
    for dev in candidate_ports(explicit):
        port = probe(dev)
        if port:
            return port
    return None


def check_mode(explicit):
    port = find_console(explicit)
    if not port:
        sys.exit("no esp32-usb-eth console found")
    print(f"console: {port.name}")
    port.write(b"show\r")
    time.sleep(1.0)
    print(port.read(8192).decode("utf-8", "replace"))
    port.close()


# --- TUI ---------------------------------------------------------------------

from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal
from textual.widgets import Button, Footer, Header, Input, RichLog


class ConsoleTUI(App):
    TITLE = "esp32-usb-eth console"

    CSS = """
    RichLog { height: 1fr; border: round $primary; padding: 0 1; }
    #buttons { height: 3; }
    #buttons Button { min-width: 8; margin: 0 1 0 0; }
    Input { dock: bottom; }
    """

    BINDINGS = [
        Binding("ctrl+q", "quit", "quit"),
        Binding("f1", "send('show')", "show"),
        Binding("f2", "send('scan')", "scan"),
        Binding("f3", "send('list')", "list"),
        Binding("f4", "toggle_debug", "debug on/off"),
    ]

    def __init__(self, explicit_port=None):
        super().__init__()
        self.explicit_port = explicit_port
        self.port = None
        self.rxbuf = b""
        self.debug_on = False

    def compose(self) -> ComposeResult:
        yield Header()
        yield RichLog(highlight=False, markup=False, wrap=True)
        with Horizontal(id="buttons"):
            yield Button("show", id="show")
            yield Button("scan", id="scan")
            yield Button("list", id="list")
            yield Button("debug", id="debug")
            yield Button("save", id="save")
        yield Input(placeholder="command (help, join 3, use 2, set ssid ..., set pass ...)")
        yield Footer()

    def on_mount(self):
        self.log_w = self.query_one(RichLog)
        self.set_interval(0.05, self.pump_serial)
        self.set_interval(2.0, self.ensure_port)
        self.ensure_port()
        self.query_one(Input).focus()

    # --- serial plumbing ---

    def ensure_port(self):
        if self.port is not None:
            return
        self.port = find_console(self.explicit_port)
        if self.port:
            self.log_w.write(f"[connected: {self.port.name}]")
            self.send("show")
        else:
            self.log_w.write("[no console found; is the device plugged in? retrying...]")

    def drop_port(self):
        if self.port:
            try:
                self.port.close()
            except Exception:
                pass
        self.port = None
        self.log_w.write("[console lost; retrying...]")

    def pump_serial(self):
        if self.port is None:
            return
        try:
            data = self.port.read(4096)
        except (OSError, serial.SerialException):
            self.drop_port()
            return
        if not data:
            return
        self.rxbuf += data.replace(b"\r", b"")
        *lines, self.rxbuf = self.rxbuf.split(b"\n")
        for line in lines:
            self.log_w.write(line.decode("utf-8", "replace"))
        # flush a dangling prompt so it doesn't sit invisible in the buffer
        if self.rxbuf.endswith(b"# "):
            self.log_w.write(self.rxbuf.decode("utf-8", "replace"))
            self.rxbuf = b""

    def send(self, command: str):
        if self.port is None:
            self.log_w.write("[not connected]")
            return
        if command == "set debug on":
            self.debug_on = True
        elif command == "set debug off":
            self.debug_on = False
        try:
            self.port.write(command.encode() + b"\r")
        except (OSError, serial.SerialException):
            self.drop_port()

    # --- UI events ---

    def action_send(self, command: str):
        self.send(command)

    def action_toggle_debug(self):
        self.send("set debug off" if self.debug_on else "set debug on")

    def on_button_pressed(self, event: Button.Pressed):
        if event.button.id == "debug":
            self.action_toggle_debug()
        else:
            self.send(event.button.id)
        self.query_one(Input).focus()

    def on_input_submitted(self, event: Input.Submitted):
        line = event.value.strip()
        event.input.clear()
        if line:
            self.send(line)


def main():
    ap = argparse.ArgumentParser(description="TUI for the esp32-usb-eth console")
    ap.add_argument("--port", help="console device (default: autodetect)")
    ap.add_argument("--check", action="store_true",
                    help="no TUI: probe the console, print `show`, exit")
    args = ap.parse_args()

    if args.check:
        check_mode(args.port)
        return

    ConsoleTUI(args.port).run()


if __name__ == "__main__":
    main()
