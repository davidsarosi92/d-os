#!/usr/bin/env python3
"""dos-shell-test.py — boot d-os headless, type shell commands, capture serial.

Every automated check in this project is a grep over the serial log, and until
now every one of them was a hand-assembled QEMU invocation in somebody's
scrollback.  This is that invocation, written down once:

    scripts/dos-shell-test.py --arch i386 --cmd nettest --cmd "ping 127.0.0.1"

It boots the built image with no display, waits for the shell prompt to appear
on the serial line, types each command, waits for the box to go quiet, then
prints the whole log and exits non-zero if the guest never reached a prompt.

TWO INPUT PATHS, because the two ports genuinely differ:

  x86      the shell reads a virtual console fed by the keyboard IRQ, and COM1
           is an output sink.  Commands are typed through the QEMU monitor's
           `sendkey`, i.e. through the emulated PS/2 controller — the same path
           a person uses.  That is deliberate: §M49 found a whole class of bug
           living in the gap between "the path the tests take" and "the path a
           person takes".
  aarch64  `serial_shell.c` IS a REPL on the PL011, so the command goes down
           the same wire the log comes back on.

The serial line is a unix socket rather than a file for both, so the harness
can read it live (to spot the prompt) and write to it (aarch64) at once.
"""

import argparse
import os
import re
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# QEMU `sendkey` names for the characters a shell command can contain.  Anything
# not here is rejected loudly rather than silently dropped: a test that typed
# half a command and then passed would be worse than one that failed.
KEYMAP = {
    ' ': 'spc', '-': 'minus', '.': 'dot', '/': 'slash', ',': 'comma',
    '=': 'equal', ';': 'semicolon', "'": 'apostrophe', '\\': 'backslash',
    '[': 'bracket_left', ']': 'bracket_right', '`': 'grave_accent',
}
for _c in 'abcdefghijklmnopqrstuvwxyz0123456789':
    KEYMAP[_c] = _c
SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6',
           '&': '7', '*': '8', '(': '9', ')': '0', '_': 'minus', '+': 'equal',
           ':': 'semicolon', '"': 'apostrophe', '<': 'comma', '>': 'dot',
           '?': 'slash', '|': 'backslash', '~': 'grave_accent'}
for _c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ':
    SHIFTED[_c] = _c.lower()


def connect_unix(path, timeout=180):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            return s
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(0.2)
    raise RuntimeError("could not connect to " + path)


class Serial:
    """The guest's serial line: read continuously, mirror into a log file."""

    def __init__(self, path, logpath):
        self.sock = connect_unix(path)
        self.sock.settimeout(0.5)
        self.buf = bytearray()
        self.lock = threading.Lock()
        self.log = open(logpath, "wb", buffering=0)
        self.stop = False
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self):
        while not self.stop:
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                return
            if not data:
                return
            with self.lock:
                self.buf += data
                self.log.write(data)

    def text(self):
        with self.lock:
            return self.buf.decode("utf-8", "replace")

    def write(self, s):
        self.sock.sendall(s.encode())

    def close(self):
        self.stop = True
        try:
            self.sock.close()
        except OSError:
            pass


class Monitor:
    """The QEMU monitor over a unix socket (x86 key injection)."""

    def __init__(self, path):
        self.sock = connect_unix(path)
        self.sock.settimeout(2)
        time.sleep(0.3)
        self._drain()

    def _drain(self):
        try:
            while self.sock.recv(65536):
                pass
        except (socket.timeout, BlockingIOError, OSError):
            pass

    def cmd(self, line):
        self.sock.sendall((line + "\n").encode())
        time.sleep(0.015)
        self._drain()

    def type_line(self, text, delay):
        for ch in text:
            if ch in KEYMAP:
                self.cmd("sendkey " + KEYMAP[ch])
            elif ch in SHIFTED:
                self.cmd("sendkey shift-" + SHIFTED[ch])
            else:
                raise RuntimeError("no sendkey mapping for %r" % ch)
            time.sleep(delay)
        self.cmd("sendkey ret")

    def quit(self):
        try:
            self.cmd("quit")
        except OSError:
            pass


def qemu_argv(a, sersock, monsock):
    if a.arch == "aarch64":
        argv = [
            "qemu-system-aarch64", "-M", "virt,gic-version=2", "-cpu", "cortex-a72",
            "-smp", str(a.smp), "-m", a.mem,
            "-kernel", "build/aarch64/kernel.bin", "-display", "none",
            "-global", "virtio-mmio.force-legacy=false",
            # §M24 — the NIC.  Attached here AND in run_qemu.sh on purpose:
            # §M48 found a whole class of bug living in the gap between the
            # path the tests take and the path a person takes.
            "-netdev", "user,id=net0", "-device", "virtio-net-device,netdev=net0",
            "-serial", "unix:%s,server,nowait" % sersock,
            "-monitor", "unix:%s,server,nowait" % monsock, "-no-reboot",
        ]
    else:
        qemu = "qemu-system-i386" if a.arch == "i386" else "qemu-system-x86_64"
        argv = [
            qemu, "-cdrom", "build/%s/d-os.iso" % a.arch,
            "-smp", str(a.smp), "-m", a.mem, "-display", "none",
            "-serial", "unix:%s,server,nowait" % sersock,
            "-monitor", "unix:%s,server,nowait" % monsock, "-no-reboot",
            "-netdev", "user,id=net0", "-device", "virtio-net-pci,netdev=net0",
        ]
        if a.disk:
            argv += ["-drive", "if=virtio,file=%s,format=raw" % a.disk]
    return argv + list(a.extra)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", default="i386", choices=["i386", "x86_64", "aarch64"])
    ap.add_argument("--cmd", action="append", default=[],
                    help="a shell command to type (repeatable)")
    ap.add_argument("--boot-timeout", type=float, default=180)
    ap.add_argument("--settle", type=float, default=8,
                    help="seconds to wait after the last command")
    ap.add_argument("--between", type=float, default=3,
                    help="seconds to wait between commands")
    ap.add_argument("--key-delay", type=float, default=0.02)
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--mem", default="512M")
    ap.add_argument("--disk", default="")
    ap.add_argument("--log", default="")
    ap.add_argument("--screenshot", default="",
                    help="after the commands, dump the framebuffer to this "
                         "PPM path (the QEMU monitor's screendump) — the only "
                         "honest check for anything that is drawn")
    # What "the shell is up" looks like ON THE SERIAL LINE — which is not the
    # prompt: the prompt is written to the virtual console, and only kprintf
    # output is teed to COM1.  x86's shell announces its pane; the ARM serial
    # REPL prints its own banner.
    ap.add_argument("--boot-marker", default=r"\[pane \d+ ready|serial shell ready")
    ap.add_argument("extra", nargs="*", help="extra QEMU arguments")
    a = ap.parse_args()

    os.chdir(ROOT)
    log = a.log or "/tmp/dos-test-%s.log" % a.arch
    sersock = "/tmp/dos-test-%s.ser" % a.arch
    monsock = "/tmp/dos-test-%s.mon" % a.arch
    for p in (sersock, monsock):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    argv = qemu_argv(a, sersock, monsock)
    print("+ " + " ".join(argv), file=sys.stderr)
    proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    rc = 0
    ser = None
    try:
        ser = Serial(sersock, log)
        mon = Monitor(monsock)

        rx = re.compile(a.boot_marker)
        deadline = time.time() + a.boot_timeout
        booted = False
        while time.time() < deadline:
            if proc.poll() is not None:
                break
            if rx.search(ser.text()):
                booted = True
                break
            time.sleep(0.25)

        if not booted:
            print("!! guest never reached a shell prompt", file=sys.stderr)
            rc = 2
        else:
            time.sleep(1.5)
            for c in a.cmd:
                print("+ typing: %s" % c, file=sys.stderr)
                if a.arch == "aarch64":
                    ser.write(c + "\r")
                else:
                    mon.type_line(c, a.key_delay)
                time.sleep(a.between)
            time.sleep(a.settle)
            if a.screenshot:
                mon.cmd("screendump " + os.path.abspath(a.screenshot))
                time.sleep(2)
        mon.quit()
    finally:
        if ser:
            ser.close()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    with open(log, "rb") as fh:
        sys.stdout.write(fh.read().decode("utf-8", "replace"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
