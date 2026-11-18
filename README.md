# wtfix

Performance and quality-of-life fixes for Wartales (Windows, DirectX 11), applied in memory at launch: no game file is
changed.

- NVIDIA DLSS or AMD FSR 3.1 (anti-aliasing or upscaling) in place of the game's TAA, with motion vectors.
- Faster frames: the main render pass drawn with worker threads, cached battle shadows, prop culling, skin joints in
  C, fewer GC collections while loading.
- Battle camera, HUD and save-list fixes, and an in-game settings panel (Insert).

## Install

- Download the zip from the latest release.
- Drag and drop its contents into your Wartales folder (in Steam: right-click Wartales > Manage > Browse local files).
- `winmm.dll` and `wtfix-README.txt` go next to `Wartales.exe`; everything else goes in a `wtfix` folder there.

Reinstalling or updating: delete the `wtfix` folder first (keep a copy of `wtfix/wtfix.ini` if you changed settings).

## Settings

- In game, Insert opens the settings panel.
- `wtfix/wtfix.ini` switches each fix on or off and holds its settings.
- Logs go to `wtfix/logs`.

## Uninstall

- Delete `winmm.dll`, `wtfix-README.txt` and the `wtfix` folder.

## Requirements

- DirectX 11 only: launched with Steam's "Wartales (OpenGL)" option, the game closes with a message saying so.
- Upscaling needs the game's anti-aliasing on. NVIDIA RTX GPUs get NVIDIA DLSS or AMD FSR 3.1 (DLSS by default);
  other GPUs get FSR.

## Credits

AMD FSR 3.1 on DirectX 11 is AMD's FidelityFX SDK with metarutaiga's DirectX 11 backend
([FidelityFX-SDK-DX11](https://github.com/metarutaiga/FidelityFX-SDK-DX11)) and MapleHinata's fixes
([FidelityFX-SDK](https://github.com/MapleHinata/FidelityFX-SDK)), the port
[OptiScaler](https://github.com/optiscaler/OptiScaler) uses.

## License

GPL-3.0 (`LICENSE`), with an additional permission for NVIDIA's DLSS libraries (`LICENSE-EXCEPTION`). Dear ImGui and
AMD's FidelityFX SDK are MIT. Not affiliated with Shiro Games, NVIDIA or AMD.
