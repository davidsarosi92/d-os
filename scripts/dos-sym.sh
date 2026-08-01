#!/bin/sh
# dos-sym.sh <hex-addr> [arch] — map an EIP/RIP (from dos-dump.sh) to the
# enclosing kernel function.  e.g.  ./scripts/dos-sym.sh 0013cb63
set -u
ADDR="${1:?usage: dos-sym.sh <hexaddr> [i386|x86_64]}"
ARCH="${2:-i386}"
exec python3 - "$ADDR" "build/$ARCH/kernel.bin" <<'PY'
import sys, subprocess, os
target=int(sys.argv[1],16); binf=sys.argv[2]
if not os.path.exists(binf): sys.exit("no %s (build it first)"%binf)
out=subprocess.run(["nm",binf],capture_output=True,text=True).stdout
syms=[]
for line in out.splitlines():
    p=line.split()
    if len(p)>=3:
        try: a=int(p[0],16)
        except: continue
        syms.append((a,p[2]))
syms.sort()
best=None
for a,n in syms:
    if a<=target: best=(a,n)
    else: break
print("0x%x is in %s (+0x%x)"%(target,best[1],target-best[0]) if best else "0x%x: no symbol"%target)
PY
