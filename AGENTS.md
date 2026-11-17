# wtfix: public release repository

wtfix fixes Wartales (Shiro Games; HashLink/Heaps, DirectX 11) in memory at launch: `winmm.dll` (a proxy the game
loads first) hands the game's bytecode to `wtfix.hdll`, which patches it before the VM compiles it and holds the natives
the patched code calls; `wtdlss.dll` wraps NVIDIA DLSS. No game file is changed.

Fixes are developed and tested against the game in the owner's private development repository, then ported here. This
repository is what gets published.

## Rules

1. **Privacy.** No file, commit or message may hold personal or local data: names other than the DudeCmonMan handle,
   emails other than GitHub noreply, local paths, machine names, the developer's hardware, Steam IDs, tokens, or any
   reference to the private repository or its notes. `tools/privacy_check.py` enforces it:
   - `--staged` and `--msg` run from the hooks in `.githooks`; enable them once per clone with
     `git config core.hooksPath .githooks`.
   - `--all` runs in CI over every tracked file and every commit.
   - Personal terms (real name, personal email, user and machine names) go in `.git/info/privacy-denylist`, one per
     line. It lives inside `.git`, so it is never committed.
2. **Build files only.** Sources, build and package scripts, `wtfix.ini`, licenses, CI, the privacy check, `README.md`
   and this file. No notes, measurements, tests, captures, development tools or binaries.
3. **Independent of the game.** Nothing reads game files at build time: `libhl.dll` is linked by name from
   `src/libhl.def`. Fixes find their targets by function name and shape at launch and turn themselves off on a mismatch.
4. **Comments minimal.** One line where possible; why, never what; no history, measurements, dates or references to
   notes. Variables get no comments.
5. **No debug features.** No remote control or development plugins, bytecode dumps, test exports, tracing or periodic
   diagnostic logging, and NVIDIA's release DLSS DLL only (the developer one has an overlay and hotkeys). The log keeps
   one line per fix, setup lines and failures; the settings panel shows each feature's status.
6. **CI.** GitHub Actions (`.github/workflows/build.yml`) runs the privacy check, then builds and packages on
   windows-latest and uploads the zip; a `v*` tag also drafts a GitHub release with it (`wtfix_<tag>.zip`), which
   the owner publishes after writing its notes.
7. **Git.** Commit as `DudeCmonMan <24796508+DudeCmonMan@users.noreply.github.com>`, no trailers of any kind
   (Co-Authored-By, Signed-off-by, "Generated with"). Local commits only: never push or add a remote unless the owner
   asks.

## Layout

- `proxy/`: `winmm.dll`, forwarding every export to the system's and handing the boot bytecode to `wtfix.hdll`.
- `src/`: `wtfix.hdll`. `hlcode.*` reads, edits and writes HashLink bytecode; `fix.h`, `fixkit.c`, `patch.c` run the
  fixes; `fixes/*.c` are the fixes (registered in `fixes.c`, switched in `wtfix.ini [fixes]`); the other files are the
  natives the patched code binds (`hlp_<name>`, library "wtfix"), the settings panel (`overlay.cpp`, Dear ImGui) and
  its live values (`live.c`).
- `dlss/`: `wtdlss.dll`, built with MSVC because NVIDIA's static library needs its runtime.
- `third_party/imgui/`: Dear ImGui as released.
- `wtfix.ini`: the switches and settings shipped in the zip.

## Install layout

The zip extracts into the game folder: `winmm.dll` and `wtfix-README.txt` next to `Wartales.exe` (the proxy must be
there to load), everything else in `wtfix/`: `wtfix.ini`, `dll/` (`wtfix.hdll`, `wtdlss.dll`, `nvngx_dlss.dll`),
`licenses/`, and `logs/` made at run time (`wtfix.log`, `proxy.log`, NVIDIA's DLSS logs). Anything wtfix adds later,
such as caches it generates, goes in `wtfix/` too. Uninstalling is deleting `winmm.dll`, `wtfix-README.txt` and
`wtfix/`. The proxy loads `wtfix/dll/wtfix.hdll` by path before the game's VM binds lib "wtfix" by bare name, which
then gets the loaded module; `wtfix.hdll` finds `wtfix/` as the parent of its own `dll` folder (`wt_root`).

## Build

- Local, from Git Bash: MinGW-w64 GCC (WinLibs or MSYS2 MINGW64, msvcrt), Visual Studio 2022 C++ tools and Python.
  `sh build.sh` builds into `build/`; `sh package.sh` writes `dist/wtfix_<version>.zip` in the install layout: CI
  names it by commit; a local build by timestamp (`VERSION` overrides), so nothing ever blocks a local build.
- The NVIDIA DLSS SDK is never committed: `tools/get_dlss_sdk.py` fetches the files the build needs from NVIDIA/DLSS
  at a pinned commit into `sdk/` (or `DLSS_SDK`), each checked by SHA-256; `build.sh` runs it. A new SDK version means
  a new commit and new hashes there.
- CI uses the same scripts. `build/`, `dist/` and `sdk/` are ignored.
- Warnings are errors. `wtfix.hdll` must not need MinGW runtime DLLs (`build.sh` checks).

## Not shipped

- shadersetup (no measured gain) and the deployment-spot texture (it is built from game files and would ship modified
  game art).

## Porting a change from development

The sources here are the development repository's shared code, the same code with shorter comments; its
development-only code lives in modules of its own that never come here. `wt_extend` (`fix.h`) is the point those
modules attach to and stays empty in this repository. Copy the changed sources, keep the fix's target shapes as
tested, cut comments to the rules above, build, and commit (the hooks run the privacy check). Whether every fix still applies to the current game
build is checked outside this repository, where the game files are; never add game files or tests here to check it.

## License

GPL-3.0 (`LICENSE`) with an additional permission for NVIDIA's DLSS libraries (`LICENSE-EXCEPTION`). Dear ImGui is
MIT (`third_party/imgui/LICENSE.txt`). The release zip carries all of them and NVIDIA's DLSS license.
