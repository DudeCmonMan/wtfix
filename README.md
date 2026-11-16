# wtfix

Performance and quality-of-life fixes for Wartales (Windows, DirectX 11), applied in memory at launch: no game file is
changed.

- NVIDIA DLSS (DLAA or upscaling) in place of the game's anti-aliasing, with motion vectors.
- Faster frames: cached battle shadows, prop culling, skin joints in C, fewer GC collections while loading.
- Battle camera, HUD and save-list fixes, and an in-game settings panel (Insert).

## Install

Download the zip from the latest build, extract it into the game folder next to `Wartales.exe`, and edit `wtfix.ini`
to switch fixes. Delete the extracted files to uninstall. DirectX only: launched with Steam's "Wartales (OpenGL)"
option, the game closes with a message saying so.

## License

GPL-3.0 (`LICENSE`), with an additional permission for NVIDIA's DLSS libraries (`LICENSE-EXCEPTION`). Dear ImGui is
MIT. Not affiliated with Shiro Games or NVIDIA.
