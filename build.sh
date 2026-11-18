#!/bin/sh
# Builds winmm.dll, wtfix.hdll, wtdlss.dll and wtfsr.dll into build/. DLSS_SDK: the NVIDIA DLSS SDK (default: sdk/,
# fetched); FSR_SDK: AMD's FidelityFX SDK fork (default: fsr-sdk/, fetched).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/build
SDK=${DLSS_SDK:-$HERE/sdk}
FSR_SDK=${FSR_SDK:-$HERE/fsr-sdk}
CC=${CC:-x86_64-w64-mingw32-gcc}
CXX=${CXX:-x86_64-w64-mingw32-g++}
DLLTOOL=${DLLTOOL:-dlltool}
IMGUI=$HERE/third_party/imgui
# No fused multiply-add: skinjoints.c must round as the game's JIT does.
CFLAGS="-O2 -ffp-contract=off -Wall -Wextra -Werror -Wno-unused-parameter -static-libgcc"
CXXFLAGS="-O2 -std=c++17 -fno-exceptions -fno-rtti -I$IMGUI -I$HERE/src"
mkdir -p "$OUT/obj"

{ echo "LIBRARY winmm"; echo "EXPORTS"
  sed -nE 's/^ *NAMED\(([A-Za-z0-9_]+), ([0-9]+)\).*/    \1=proxy_\1 @\2/p; s/^ *UNNAMED\(([0-9]+)\).*/    proxy_ord_\1 @\1 NONAME/p' \
    "$HERE/proxy/exports.h"; } > "$OUT/winmm.def"
$CC $CFLAGS -shared -o "$OUT/winmm.dll" "$HERE/proxy/winmm.c" "$OUT/winmm.def"

# libhl.dll is the game's: linked by name, never shipped.
$DLLTOOL -d "$HERE/src/libhl.def" -l "$OUT/libhl.a"
OBJS=""
for f in "$HERE"/src/*.c "$HERE"/src/fixes/*.c; do
  o="$OUT/obj/$(echo "${f#"$HERE"/src/}" | tr '/' '_').o"
  $CC $CFLAGS -c "$f" -o "$o"
  OBJS="$OBJS $o"
done
for f in imgui imgui_draw imgui_tables imgui_widgets backends/imgui_impl_dx11 backends/imgui_impl_win32; do
  o="$OUT/obj/imgui_$(basename "$f").o"
  $CXX $CXXFLAGS -c "$IMGUI/$f.cpp" -o "$o"
  OBJS="$OBJS $o"
done
$CXX $CXXFLAGS -Wall -Wextra -Werror -Wno-unused-parameter -c "$HERE/src/overlay.cpp" -o "$OUT/obj/overlay.o"
$CXX -shared -static -o "$OUT/wtfix.hdll" $OBJS "$OUT/obj/overlay.o" "$OUT/libhl.a" \
  -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -limm32 -lgdi32
# The game loads only system DLLs and libhl next to wtfix: no MinGW runtime may be needed.
if objdump -p "$OUT/wtfix.hdll" "$OUT/winmm.dll" | grep -iE "DLL Name: (libgcc|libstdc|libwinpthread)"; then
  echo "wtfix needs MinGW runtime DLLs"; exit 1
fi

python -I "$HERE/tools/get_dlss_sdk.py" "$SDK"
cmd //c "$(cygpath -w "$HERE/dlss/build.cmd")" "$(cygpath -w "$OUT")" "$(cygpath -w "$SDK")"
python -I "$HERE/tools/get_fsr_sdk.py" "$FSR_SDK"
cmd //c "$(cygpath -w "$HERE/fsr/build.cmd")" "$(cygpath -w "$OUT")" "$(cygpath -w "$FSR_SDK")"
