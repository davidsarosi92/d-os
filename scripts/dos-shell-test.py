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

# -----------------------------------------------------------------------------
# THE CRASH SCAN.  Every run greps the log for its OWN result string — and until
# 2026-09-04 no run had ever grepped it for the machine having died.  A page
# fault reproducible in half of runs survived a whole milestone that way: the
# feature string was present, the screenshot taken before the fault looked
# perfect, and nobody looked at the six lines in between.
#
# Same shape as §M48's missing NIC and §M60's silent ARM harness — the thing not
# being checked was whether the box crashed — so the check belongs HERE, in the
# one place every run passes through, and not in each caller's grep.
#
# A hit is reported LOUDLY and makes the run fail, whatever else it found: a
# test that passes on a machine that faulted is not evidence about the feature,
# it is evidence about the grep.  `--allow-crash` is for the tests that CAUSE
# one on purpose (`hardlock`, `splash faultkernel`, `drv crash`, `faulttest`)
# — deliberate, named, and never the default.
CRASH_PATTERNS = [
    re.compile(rb"!! EXCEPTION "),                 # ring-0 fault dump (both x86)
    re.compile(rb"!! NMI HARD-LOCKUP"),            # §M31 L3 hardware watchdog
    re.compile(rb"!! PIT STARVED"),                # §M82 session: IRQ0 held off
    re.compile(rb"!! KTIMER"),                     # an armed timer outlived its stack
    re.compile(rb"PREVIOUS BOOT ENDED UNCLEANLY"), # §M47 NVRAM breadcrumb
    re.compile(rb"softlockup"),                    # §M31 L2 per-CPU sweep
    re.compile(rb"spinlock deadlock"),             # §M46 lock-order report
    re.compile(rb"INPUT EVENT DROPPED"),           # §M69 ring overflow, once
    re.compile(rb"STILL QUEUED"),                  # §M57 runqueue audit
    # §M71 — a runtime INVARIANT violation.  Distinct from a crash in kind and
    # identical in consequence for a test result: a run in which a driver was
    # executing in the wrong ring, or a dead driver still held its hardware, is
    # not evidence about whatever feature the run was testing.  The audits are
    # designed to be silent unless something is wrong, so this pattern costs
    # nothing on a healthy machine.
    re.compile(rb"!! AUDIT FAIL"),
    re.compile(rb"!! audit "),                     # the individual violation lines
]


def scan_for_crashes(raw):
    """Return the log lines that say this machine did not have a good time."""
    hits = []
    for line in raw.splitlines():
        for pat in CRASH_PATTERNS:
            if pat.search(line):
                hits.append(line.decode("utf-8", "replace").rstrip())
                break
    return hits

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
        # None = we have not yet found out whether this guest echoes typed
        # characters to the serial line.  See type_line.
        self.echoes = None
        # A SHORT drain timeout.  With the obvious 2 s the drain after every
        # command blocked until it expired whenever the monitor had nothing
        # more to say — so each mouse_move cost ~1.6 s and a "drag" delivered
        # under one event a second.  A harness that cannot reproduce the input
        # rate cannot measure what that rate costs.
        self.sock.settimeout(0.05)
        time.sleep(0.3)
        self._drain()

    # Where monitor REPLIES go.  They used to be read and thrown away, so a
    # monitor command that failed (a `pmemsave` with a bad argument) failed in
    # silence and an inspection command (`info registers`) had no way to report
    # anything.  Set by main() from --log: <log>.mon.
    reply_log = None

    def _drain(self):
        try:
            while True:
                b = self.sock.recv(65536)
                if not b:
                    break
                if self.reply_log:
                    with open(self.reply_log, "ab") as f:
                        f.write(b)
        except (socket.timeout, BlockingIOError, OSError):
            pass

    def cmd(self, line):
        self.sock.sendall((line + "\n").encode())
        time.sleep(0.015)
        self._drain()

    def _type_raw(self, text, delay):
        for ch in text:
            if ch in KEYMAP:
                self.cmd("sendkey " + KEYMAP[ch])
            elif ch in SHIFTED:
                self.cmd("sendkey shift-" + SHIFTED[ch])
            else:
                raise RuntimeError("no sendkey mapping for %r" % ch)
            time.sleep(delay)

    def type_line(self, text, delay, ser=None, tries=3):
        """Type `text` and press Enter, verifying the echo WHERE ONE EXISTS.

        `sendkey` is fire-and-forget: QEMU injects a key event and nothing tells
        the sender whether the guest took it.  Under load it did not — the
        keyboard driver read one byte per interrupt and lost the rest — and the
        result was the worst kind of failure, because it looked like a guest
        bug: `drv domain edu user` arrived as `drv domain ed` and the guest
        said "not a domain".  That cause is fixed in the kernel; this is the
        belt to its braces, and a harness that silently loses INPUT is exactly
        as dangerous as one that loses output (§M57), for the same reason — it
        is trusted.

        WHETHER THE GUEST ECHOES IS DECIDED ONCE, NOT GUESSED PER COMMAND.  On
        aarch64 the shell IS the serial line and echoes; on x86 it runs on a VC
        and the serial carries only kernel output.  A per-attempt guess reads
        that kernel output as a failed echo and "corrects" it with backspaces,
        which is how this check spent one round corrupting the very commands it
        was meant to protect.  So the first command calibrates: if its text
        comes back, verification is on for the session; if not, it stands down
        for good and says nothing further.
        """
        if ser is None or self.echoes is False:
            self._type_raw(text, delay)
            self.cmd("sendkey ret")
            return True

        for attempt in range(tries):
            before = len(ser.text())
            self._type_raw(text, delay)
            time.sleep(0.25)
            echo = ser.text()[before:]
            if text in echo:
                self.echoes = True
                self.cmd("sendkey ret")
                return True
            if self.echoes is None:
                # Calibration: this guest does not echo to serial.  Not a
                # failure of the typing — a fact about where its shell lives.
                self.echoes = False
                self.cmd("sendkey ret")
                return True
            print("+ retyping (guest took %r, wanted %r)"
                  % (echo.strip()[-40:], text), file=sys.stderr)
            for _ in range(len(text) + 8):
                self.cmd("sendkey backspace")
                time.sleep(0.01)
            time.sleep(0.3)

        print("+ WARNING: could not type %r after %d attempts"
              % (text, tries), file=sys.stderr)
        self.cmd("sendkey ret")
        return False

    def quit(self):
        try:
            self.cmd("quit")
        except OSError:
            pass


def audio_backend(a):
    """Which host audio backend to attach, or None for "no sound card".

    THE HARNESS HAD NO AUDIO DEVICE AT ALL, which is the fifth appearance of a
    shape this project keeps paying for: the machine the tests run on and the
    machine a person boots were different machines.  `run_qemu.sh` has attached
    a sound card since §M23 and this did not, so every automated run booted a
    box with no audio hardware — `lsaudio` empty, the taskbar indicator in its
    "no device" state, and any regression in the audio path invisible here.

    `none` is deliberately reachable rather than incidental: §M23's THIRD
    indicator state IS the no-device machine, and a state nobody can boot into
    is a state nobody has tested.  That is what --no-audio selects.

    The default is `none` as a BACKEND, not as a device: an automated run has no
    speakers and must not fight the host for its audio hardware, so the card is
    attached to QEMU's null backend.  The guest sees a real sound card either
    way, which is the thing under test — and `--audio-backend wav` is how a test
    captures what was played (§M23 measured every one of its numbers that way).
    """
    if a.no_audio:
        return None
    return a.audio_backend


def aarch64_dtb_args(a):
    """The device tree for THIS run's machine, loaded where the kernel looks.

    §M86 stage 3 (2026-09-26): QEMU's direct-ELF `-kernel` entry passes no DTB,
    so the kernel falls back to a built-in 256 MiB / default-CPU picture unless
    one is loaded at 0x48000000 — and this harness never loaded one.  So
    `--mem` and `--smp` were SILENTLY IGNORED on aarch64: every ARM run, at any
    size, managed 247 MiB, and the first `--mem 8G` run tested a quarter of a
    gigabyte while saying 8.  The seventh appearance of the harness shape (the
    path under test was not the machine asked for).  The tree is dumped from
    the same -M/-cpu/-smp/-m every run, because those are exactly the facts the
    kernel reads from it (/memory, /cpus); a DTB made once for one size is
    wrong for every other size."""
    dtb = "build/aarch64/test-%d.dtb" % os.getpid()
    subprocess.run(["qemu-system-aarch64", "-M", "virt,gic-version=2,dumpdtb=" + dtb,
                    "-cpu", "cortex-a72", "-smp", str(a.smp), "-m", a.mem,
                    "-display", "none"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return ["-device", "loader,file=%s,addr=0x48000000,force-raw=on" % dtb]


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
            # THE DISPLAY, for the same reason as the NIC above.  Without a
            # virtio-gpu this arch boots serial-only, so nothing that draws —
            # the GUI, the wallpaper, §M61's mode setting — could be tested
            # here at all, and "aarch64 declines" stayed true by accident.
            # The keyboard comes with it: a framebuffer boot puts the shell on
            # a VC, and `sendkey` needs a device to arrive through.
        ]
        # ...unless the test wants the OTHER boot path.  aarch64 has two:
        # with a framebuffer it runs the full shell.c on a VC, without one it
        # runs serial_shell.c as a REPL on the PL011 — different shells, and
        # only the second one answers on the wire the harness reads.  Leaving
        # the GPU permanently attached made the serial path untestable, which
        # is the same shape as the bug the comment above describes.
        if not a.no_display:
            argv += [
                "-device", "virtio-gpu-device",
                "-device", "virtio-keyboard-device",
                "-device", "virtio-mouse-device",
            ]
        # --disk used to be honoured on x86 ONLY, silently: the flag was
        # accepted, the ARM guest simply never saw a disk, and anything that
        # needed persistent storage "failed" on this arch for no visible
        # reason.  On -M virt the drive has to be attached to a virtio-MMIO
        # slot explicitly (there is no if=virtio auto-wiring here).
        # §M23 — the sound card, for the same reason as the NIC and the GPU
        # above.  This arch has no AC97 (a PCI card, and `virt` has no slot for
        # one), so it is virtio-sound on a virtio-MMIO slot — the same way this
        # machine gets every other device.
        be = audio_backend(a)
        if be:
            argv += ["-audiodev", "%s,id=snd0" % be,
                     "-device", "virtio-sound-device,audiodev=snd0"]
        if a.disk:
            argv += ["-drive", "if=none,id=hd0,file=%s,format=raw" % a.disk,
                     "-device", "virtio-blk-device,drive=hd0"]
    else:
        qemu = "qemu-system-i386" if a.arch == "i386" else "qemu-system-x86_64"
        argv = [
            qemu, "-cdrom", "build/%s/d-os.iso" % a.arch,
            "-smp", str(a.smp), "-m", a.mem, "-display", "none",
            "-serial", "unix:%s,server,nowait" % sersock,
            "-monitor", "unix:%s,server,nowait" % monsock, "-no-reboot",
            "-netdev", "user,id=net0", "-device", "virtio-net-pci,netdev=net0",
            # THE SAME MACHINE run_qemu.sh GIVES A PERSON.  Until now the harness
            # left these out, so every GUI test ran WITHOUT the page flip (the
            # std-VGA default has no room for a second 1920x1200 frame), without
            # the hardware watchdog, and with a different RTC — i.e. the measured
            # path and the used path were different machines.  That is the §M48
            # missing-NIC / §M49 missing-smp shape for the fourth time, and it is
            # why a lockup a user hit on the everyday run could not appear in any
            # test here.  The watchdog in particular is wanted: if a test wedges
            # the guest, the NMI report is the evidence.
            "-vga", "none", "-device", "VGA,vgamem_mb=32",
            "-rtc", "base=localtime",
            "-device", "ib700", "-action", "watchdog=inject-nmi",
        ]
        # §M23 — AC97, the codec both x86 arches drive.  `hda` is the other
        # one and is a §M67 module here, so the built-in path is what an
        # ordinary boot exercises and what this attaches.
        be = audio_backend(a)
        if be:
            argv += ["-audiodev", "%s,id=snd0" % be,
                     "-device", "AC97,audiodev=snd0"]
        if a.iommu:
            # §M33 stage 5 — a machine with DMA remapping hardware.
            #
            # NOT the default, and that is the honest choice: the everyday
            # machine has no IOMMU, so making it standard here would mean the
            # no-IOMMU path — the one almost every user boots — stopped being
            # tested.  The reverse of the §M48/§M49/§4.66 mistake, which was
            # testing a machine nobody runs.  Both paths are reachable and both
            # get measured.
            #
            # intel-iommu needs q35: the DMAR describes a PCIe root complex, and
            # on the i440fx machine QEMU refuses the device outright.
            argv += ["-machine", "q35", "-device", "intel-iommu"]
        elif a.amd_iommu:
            argv += ["-machine", "q35", "-device", "amd-iommu"]
        if a.disk:
            argv += ["-drive", "if=virtio,file=%s,format=raw" % a.disk,
                     # A FORMATTED image carries a boot signature, and SeaBIOS
                     # then boots the (empty) disk instead of the CD — the guest
                     # hangs with NO serial output at all, which reads exactly
                     # like a kernel that died before the first kprintf.
                     # CLAUDE.md documents the trap; the harness should not make
                     # everyone rediscover it.
                     "-boot", "d"]
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
    # 1024M: what run_qemu.sh gives, and past that point i386 can actually
    # use the extra RAM (§M48 raised the identity map to 1 GiB).
    ap.add_argument("--mem", default="1024M")
    ap.add_argument("--disk", default="")
    # --empty is the harness spelling of run_qemu.sh's flag of the same name: a
    # FRESHLY FORMATTED, empty volume.  A test that reuses whatever the last run
    # left behind is a test whose result depends on the order the tests ran in,
    # and the failure that produces (a stale /mnt/store shadowing a rebuild) has
    # already cost this project a debugging session.  No disk at all remains the
    # default — that is what "neither flag" has always meant here.
    ap.add_argument("--empty", nargs="?", const="", default=None,
                    metavar="PATH",
                    help="attach a freshly formatted, EMPTY 64 MiB exFAT disk "
                         "(default path: build/<arch>/test-empty.img)")
    ap.add_argument("--amd-iommu", action="store_true",
                    help="boot on a machine with AMD's IOMMU instead of Intel's "
                         "(q35 + amd-iommu).  The point is that everything above "
                         "the backend seam is unchanged: same verdicts, same "
                         "commands, a different chipset underneath")
    ap.add_argument("--iommu", action="store_true",
                    help="boot on a machine that HAS DMA remapping hardware "
                         "(q35 + intel-iommu).  Off by default so the ordinary "
                         "no-IOMMU machine stays the one under test.")
    ap.add_argument("--no-audio", action="store_true",
                    help="attach NO sound card.  §M23's third taskbar indicator "
                         "state is exactly this machine, and a state nobody can "
                         "boot into is a state nobody has tested")
    ap.add_argument("--audio-backend", default="none",
                    help="host audio backend for the attached card "
                         "(none | wav | coreaudio | pa | alsa).  Default `none` "
                         "is QEMU's null backend: the GUEST still sees a real "
                         "sound card, which is what is under test, while an "
                         "automated run does not fight the host for its speakers. "
                         "Use `wav` with -- -audiodev ... to CAPTURE what was "
                         "played, which is how §M23 measured every one of its "
                         "numbers")
    ap.add_argument("--no-display", action="store_true",
                    help="aarch64 only: attach no virtio-gpu, so the guest "
                         "boots the SERIAL shell (serial_shell.c) instead of "
                         "the framebuffer one — the only way to drive that "
                         "path, and the only way to read its output")
    ap.add_argument("--log", default="")
    ap.add_argument("--allow-crash", action="store_true",
                    help="do not fail the run on a fault/lockup in the log — "
                         "for the tests that deliberately cause one (hardlock, "
                         "splash faultkernel, drv crash, faulttest)")
    ap.add_argument("--via", default="keys", choices=["keys", "serial"],
                    help="x86: how --cmd reaches the guest.  'keys' (default) "
                         "types through the emulated keyboard, i.e. the path a "
                         "person uses, so keyboard tests stay tests of it; "
                         "'serial' writes the line to COM1, where the kernel's "
                         "command channel runs it regardless of which window "
                         "holds focus (the fix for 'cannot type once a GUI "
                         "window has focus').  Output of a serial command is "
                         "echoed as 'serial-cmd> <line>'.")
    ap.add_argument("--monitor-cmd", action="append", default=[],
                    help="a raw QEMU monitor command to run after the shell "
                         "commands (repeatable) — mouse_move/mouse_button for "
                         "driving the GUI, screendump for looking at it.  "
                         "Prefix with 'sleep <s>' to pause.")
    ap.add_argument("--screenshot", default="",
                    help="after the commands, dump the framebuffer to this "
                         "PPM path (the QEMU monitor's screendump) — the only "
                         "honest check for anything that is drawn")
    # What "the shell is up" looks like ON THE SERIAL LINE — which is not the
    # prompt: the prompt is written to the virtual console, and only kprintf
    # output is teed to COM1.  x86's shell announces its pane; the ARM serial
    # REPL prints its own banner.
    # NOTE: the aarch64 alternative is the REPL's actual banner.  It used to
    # read "serial shell ready", which serial_shell.c has not printed for a
    # long time — so every ARM run silently failed the boot wait and typed
    # NOTHING, then printed a log that looks like a healthy boot.  A harness
    # that quietly stops driving the guest is worse than one that crashes.
    ap.add_argument("--boot-marker",
                    default=r"\[pane \d+ ready|serial shell ready|Type 'help'")
    ap.add_argument("extra", nargs="*", help="extra QEMU arguments")
    a = ap.parse_args()

    os.chdir(ROOT)

    if a.empty is not None:
        # mkfs.exfat lives in the build container, not on a Mac host.  Formatting
        # is arch-independent, so the x86 image does it for every arch.
        a.disk = a.empty or "build/%s/test-empty.img" % a.arch
        os.makedirs(os.path.dirname(a.disk) or ".", exist_ok=True)
        if os.path.exists(a.disk):
            os.remove(a.disk)
        print("test: --empty — formatting a fresh exFAT disk at %s" % a.disk)
        rc = subprocess.call(
            ["docker", "run", "--rm", "--platform=linux/amd64",
             "-v", "%s:/src" % ROOT, "d-os-build", "bash", "-c",
             "dd if=/dev/zero of=/src/%s bs=1M count=64 status=none && "
             "mkfs.exfat -n DOS /src/%s >/dev/null" % (a.disk, a.disk)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if rc != 0:
            # Refuse rather than silently run without one: the whole point of
            # the flag is a known starting state, and a run that quietly lost it
            # would report a pass about a machine nobody asked for.
            raise SystemExit("test: could not format %s (is the d-os-build "
                             "image present?)" % a.disk)

    log = a.log or "/tmp/dos-test-%s.log" % a.arch
    sersock = "/tmp/dos-test-%s.ser" % a.arch
    monsock = "/tmp/dos-test-%s.mon" % a.arch
    for p in (sersock, monsock):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    argv = qemu_argv(a, sersock, monsock)
    if a.arch == "aarch64":
        argv += aarch64_dtb_args(a)
    print("+ " + " ".join(argv), file=sys.stderr)
    proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    rc = 0
    ser = None
    try:
        ser = Serial(sersock, log)
        Monitor.reply_log = log + ".mon"
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
                # --via serial: the COM1 command channel (serial_cmd.c), which
                # runs a line whatever holds keyboard focus.  aarch64's shell
                # IS the serial line, so it always goes this way.
                if a.arch == "aarch64" or a.via == "serial":
                    ser.write(c + "\r")
                else:
                    mon.type_line(c, a.key_delay, ser)
                time.sleep(a.between)
            time.sleep(a.settle)
            for mc in a.monitor_cmd:
                if mc.startswith("sleep "):
                    time.sleep(float(mc.split()[1]))
                    continue
                # "serial <line>" — send a command over COM1 at THIS point in
                # the monitor sequence, i.e. after the mouse has put the
                # desktop into the state to be inspected.  (x86 only; the
                # kernel's serial_cmd.c runs it whatever holds focus.)
                if mc.startswith("serial "):
                    print("+ serial: %s" % mc[7:], file=sys.stderr)
                    ser.write(mc[7:] + "\r")
                    time.sleep(a.between)
                    continue
                mon.cmd(mc)
                # Mouse MOTION is paced fast on purpose: a real mouse delivers
                # ~100 events a second, and a harness that sends eight is not
                # reproducing the load it claims to measure.
                time.sleep(0.008 if mc.startswith("mouse_move") else 0.12)
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
        raw = fh.read()
    sys.stdout.write(raw.decode("utf-8", "replace"))

    # See CRASH_PATTERNS.  This runs AFTER the log is printed so the evidence is
    # already on screen when the banner names it.
    hits = scan_for_crashes(raw)
    if hits and not a.allow_crash:
        sys.stdout.write("\n" + "=" * 70 + "\n")
        sys.stdout.write("!! THE GUEST CRASHED DURING THIS RUN (%d line(s))\n" % len(hits))
        sys.stdout.write("   Whatever else this run measured, it measured it on a\n"
                         "   machine that faulted.  Pass --allow-crash only if the\n"
                         "   test is SUPPOSED to produce one.\n")
        sys.stdout.write("=" * 70 + "\n")
        for h in hits[:20]:
            sys.stdout.write("   " + h + "\n")
        if len(hits) > 20:
            sys.stdout.write("   ... and %d more\n" % (len(hits) - 20))
        return 3
    if hits:
        sys.stdout.write("\n(crash lines present and allowed: %d)\n" % len(hits))
    return rc


if __name__ == "__main__":
    sys.exit(main())
