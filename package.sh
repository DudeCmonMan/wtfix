#!/bin/sh
# Builds and zips a release into dist/, laid out as the game folder: winmm.dll and wtfix-README.txt next to
# Wartales.exe, everything else in wtfix\. VERSION names it; locally a timestamp, so an open older zip never blocks one.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=${DLSS_SDK:-$HERE/sdk}
VERSION=${VERSION:-$(date +%Y%m%d-%H%M%S)}
sh "$HERE/build.sh"
STAGE=$HERE/dist/.stage_$VERSION
mkdir -p "$STAGE/wtfix/dll" "$STAGE/wtfix/licenses"
cp "$HERE/build/winmm.dll" "$STAGE/"
cp "$HERE/build/wtfix.hdll" "$HERE/build/wtdlss.dll" "$SDK/lib/Windows_x86_64/rel/nvngx_dlss.dll" "$STAGE/wtfix/dll/"
cp "$HERE/wtfix.ini" "$STAGE/wtfix/"
cp "$HERE/LICENSE" "$STAGE/wtfix/licenses/wtfix-GPL-3.0.txt"
cp "$HERE/LICENSE-EXCEPTION" "$STAGE/wtfix/licenses/wtfix-exception.txt"
cp "$HERE/third_party/imgui/LICENSE.txt" "$STAGE/wtfix/licenses/dear-imgui.txt"
cp "$SDK/LICENSE.txt" "$STAGE/wtfix/licenses/nvidia-dlss.txt"
cat > "$STAGE/wtfix-README.txt" <<EOF
wtfix $VERSION for Wartales (Windows, DirectX 11).

Install: extract into the game folder: winmm.dll and this file go next to Wartales.exe, the rest into its wtfix folder.
Uninstall: delete winmm.dll, wtfix-README.txt and the wtfix folder.

wtfix\wtfix.ini switches each fix (name=0 turns it off) and holds their settings. In game, Insert opens the settings
panel. Each start writes wtfix\logs\wtfix.log: one line per fix, "on" or "OFF" with the reason. A game update that
changes what a fix patches turns that fix off; the game then runs as it would without it.

DirectX 11 only: started with Steam's "Wartales (OpenGL)" option, the game closes with a message saying so.
NVIDIA DLSS needs an NVIDIA RTX GPU; elsewhere the game's own anti-aliasing runs. Licenses: wtfix\licenses.
EOF
ZIP=$HERE/dist/wtfix_$VERSION.zip
(cd "$STAGE" && python -I -m zipfile -c "$ZIP" winmm.dll wtfix-README.txt wtfix)
rm -rf "$STAGE"
echo "packaged: $ZIP"
