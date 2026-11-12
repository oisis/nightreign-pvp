#!/bin/sh
# Build the launcher for Linux (GTK 3) and Windows (Win32) into $W/dist/linux and $W/dist/windows.
# Needs gcc + GTK 3 headers, zig in $W/tools, and mod/build.sh run first.
set -e
. "$(dirname "$0")/../re/common.sh"
ZIG="$W/tools/zig/zig"
D="$W/dist"
L="$R/launcher"
mkdir -p "$D/linux/data" "$D/windows/data"

# zstd decompression comes from Zig's standard library (launcher/zstd.zig), as one object per target.
O="$W/dist/obj"
mkdir -p "$O"
(cd "$O" && "$ZIG" build-obj -OReleaseSmall -fstrip -fcompiler-rt -target x86_64-linux-gnu -fPIC --name zstd-linux "$L/zstd.zig" \
         && "$ZIG" build-obj -OReleaseSmall -fstrip -fcompiler-rt -target x86_64-windows-gnu --name zstd-windows "$L/zstd.zig")

gcc -O2 -Wall -Wextra -o "$D/linux/nightreign-invasions" "$L/gtk.c" "$L/core.c" "$O/zstd-linux.o" $(pkg-config --cflags --libs gtk+-3.0)
gcc -O2 -Wall -Wextra -o "$D/linux/cli" "$L/cli.c" "$L/core.c" "$O/zstd-linux.o"
# The build machine's C runtime objects may be marked as needing a newer CPU level than our code
# uses (x86-64-v4 on CachyOS); the loader then refuses the binary on older CPUs, the Steam Deck among
# them. Our own code is plain x86-64 (-O2, no -march), so the marking goes.
objcopy --remove-section .note.gnu.property "$D/linux/nightreign-invasions"
objcopy --remove-section .note.gnu.property "$D/linux/cli"
"$ZIG" cc -target x86_64-windows-gnu -O2 -Wall -Wextra -Wl,--subsystem,windows -o "$D/windows/NightreignInvasions.exe" \
  "$L/win.c" "$L/core.c" "$O/zstd-windows.obj" -lole32 -lshell32 -ladvapi32 -lgdi32
"$ZIG" cc -target x86_64-windows-gnu -O2 -o "$D/windows/cli.exe" "$L/cli.c" "$L/core.c" "$O/zstd-windows.obj" -ladvapi32

# What the launcher ships besides itself: the client DLL, our icons, and (Linux) the Oodle helper.
for d in linux windows; do
  cp "$W/build/native/invasion.dll" "$W"/build-launcher-data/icon_*.bc7 "$D/$d/data/"
done
cp "$W/build/unkrak.exe" "$D/linux/data/"
echo "launcher built in $D/linux and $D/windows"
