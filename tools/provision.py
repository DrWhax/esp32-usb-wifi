#!/usr/bin/env python3
"""Push Wi-Fi credentials to an esp32-usb-eth device over its CDC-ACM console.

Usage:
    provision.py <ssid> [password]              # open network if password omitted
    provision.py --port /dev/cu.usbmodemXXX <ssid> [password]

Finds the console port automatically (the device answers the console prompt),
sets the credentials, persists them to NVS, and prints the device's status.

Needs pyserial:  pip install pyserial   (or run inside an esp-idf environment).
"""

import argparse
import glob
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial (or '. ~/esp/esp-idf/export.sh' first)")

PROMPT = b"(set|show|save|clear) #"


def read_until_quiet(port, timeout=1.0):
    """Read until the device stops talking for `timeout` seconds."""
    out = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = port.read(4096)
        if chunk:
            out += chunk
            deadline = time.time() + 0.3
    return out


def command(port, line, quiet=False):
    port.write(line.encode() + b"\r")
    out = read_until_quiet(port)
    if not quiet:
        # first line is the echo of what we typed; the rest is the reply
        reply = out.split(b"\r\n", 1)[-1] if b"\r\n" in out else out
        print(reply.replace(PROMPT, b"").decode("utf-8", "replace").rstrip())
    return out


def find_console(explicit=None):
    candidates = [explicit] if explicit else \
        sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    for dev in candidates:
        try:
            port = serial.Serial(dev, 115200, timeout=0.2)
        except (OSError, serial.SerialException):
            continue
        time.sleep(0.5)
        port.reset_input_buffer()
        port.write(b"\rshow\r")
        if PROMPT in read_until_quiet(port):
            port.reset_input_buffer()
            return port
        port.close()
    return None


def main():
    ap = argparse.ArgumentParser(description="Provision esp32-usb-eth Wi-Fi credentials")
    ap.add_argument("ssid")
    ap.add_argument("password", nargs="?", default="", help="omit for an open network")
    ap.add_argument("--port", help="console device (default: autodetect)")
    ap.add_argument("--no-save", action="store_true", help="apply without persisting to NVS")
    args = ap.parse_args()

    port = find_console(args.port)
    if port is None:
        sys.exit("no esp32-usb-eth console found (is the device plugged in and running the app?)")
    print(f"console: {port.name}")

    command(port, f"set ssid {args.ssid}")
    command(port, f"set pass {args.password}", quiet=True)  # don't echo the password
    if not args.no_save:
        command(port, "save")

    # give the join a moment, then report status
    print("waiting for association", end="", flush=True)
    for _ in range(15):
        time.sleep(1)
        print(".", end="", flush=True)
        out = command(port, "show", quiet=True)
        if b"status:    associated" in out:
            print()
            reply = out.split(b"\r\n", 1)[-1]
            print(reply.replace(PROMPT, b"").decode("utf-8", "replace").rstrip())
            break
    else:
        print("\n[!] not associated yet -- check the SSID/password with 'show' on the console")

    port.close()


if __name__ == "__main__":
    main()
