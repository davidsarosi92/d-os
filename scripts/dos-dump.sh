#!/bin/sh
# =============================================================================
# dos-dump.sh — probe a (possibly FROZEN) d-os QEMU guest via its monitor socket.
#
# run_qemu.sh exposes the QEMU monitor on a unix socket.  When the guest hangs
# (silent 0%-CPU HALT with IRQs off, or a spinning deadlock), the in-guest
# diagnostics can't always report — but the monitor still answers, so we can ask
# the emulator itself where every vCPU is stuck.  Run this WHILE the guest is
# frozen and send the output back:
#
#     ./scripts/dos-dump.sh              # dumps registers (EIP/RIP + EFLAGS)
#
# EIP is the instruction pointer of the halt/spin; EFLAGS bit 9 (IF) tells us
# whether interrupts are disabled (a permanent halt) — map EIP to a function via
#     nm build/i386/kernel.bin | sort   (or scripts/dos-sym.sh <addr>)
# =============================================================================
set -u
MONSOCK="${DOS_MONITOR:-/tmp/dos-monitor.sock}"
[ -S "$MONSOCK" ] || { echo "dos-dump: no monitor socket at $MONSOCK (is run_qemu.sh running?)" >&2; exit 1; }

python3 - "$MONSOCK" <<'PY'
import socket, sys, time
sock = sys.argv[1]
s = socket.socket(socket.AF_UNIX); s.connect(sock); s.settimeout(2)
time.sleep(0.2)
out = b""
# QMP-free HMP: just send monitor commands.  info registers = current CPU;
# info registers -a = all vCPUs (SMP).  cpu-add safe to ignore.
for cmd in ("info registers -a\n", "info registers\n", "info cpus\n"):
    s.sendall(cmd.encode()); time.sleep(0.5)
try:
    while True:
        d = s.recv(65536)
        if not d: break
        out += d
except Exception:
    pass
s.close()
txt = out.decode(errors="replace")
print(txt)
# Highlight the key fields.
for ln in txt.splitlines():
    u = ln.strip()
    if u.startswith("EIP=") or u.startswith("RIP=") or "EFL=" in u or "RFL=" in u:
        print(">>", u)
PY
