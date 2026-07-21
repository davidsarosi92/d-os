#!/bin/sh
set -e
TC=/src/third_party/musl-cross-i686
SYS=$TC/i686-linux-musl
export PATH=$TC/bin:$PATH
CC=$TC/bin/i686-linux-musl-gcc
# Backup the current (1.2.3) sysroot libc + a version marker.
mkdir -p /src/third_party/.musl123-backup/lib
cp -a $SYS/lib/libc.a $SYS/lib/libc.so /src/third_party/.musl123-backup/lib/ 2>/dev/null || true
echo "=== building musl 1.2.5 with $CC ==="
cd /src/third_party/musl
make distclean >/dev/null 2>&1 || true
./configure --host=i686-linux-musl --prefix=$SYS --syslibdir=$SYS/lib CC="$CC" >/tmp/mcfg.log 2>&1 || { tail -20 /tmp/mcfg.log; exit 1; }
make -j4 >/tmp/mmake.log 2>&1 || { tail -25 /tmp/mmake.log; exit 1; }
make install >/tmp/minst.log 2>&1 || { tail -20 /tmp/minst.log; exit 1; }
echo "=== installed musl version into sysroot: ==="
cat $SYS/lib/../VERSION 2>/dev/null; strings $SYS/lib/libc.so | grep -oE '1\.2\.[0-9]+' | head -1
echo "=== toolchain sanity: compile + link a dynamic hello ==="
printf '#include <stdio.h>\nint main(){puts("musl125 ok");return 0;}\n' > /tmp/h.c
$CC -O2 /tmp/h.c -o /tmp/h.elf && $TC/bin/i686-linux-musl-readelf -h /tmp/h.elf | grep -E "Class|Machine" && echo "COMPILE OK"
