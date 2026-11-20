# Windhawk desktop mods

A macOS-style Windows 11 desktop built on [Windhawk](https://windhawk.net): a top bar with an island, a dock, restyled Start, panels and dialogs, plus small fixes. Everything here is source; DLLs are built locally and are not committed.

## Layout

| Path | What |
|---|---|
| `local@*.wh.cpp` | My own mods (source of truth for each) |
| `topbar/` | Top bar source split into parts; `build-bar.ps1` assembles them into `local@top-bar.wh.cpp` |
| `build-*.ps1`, `install-*.ps1` | Build (both architectures where needed) and elevated install scripts |
| `installed/sources/` | Snapshot of every installed mod's source, including third-party ones as installed |
| `installed/settings/` | Each mod's registry key (`HKLM\SOFTWARE\Windhawk\Engine\Mods\<id>`), with its settings |
| `installed/icon-theme-macos/` | Icon theme used by `icon-resource-redirect` (WhiteSur, GPL-3.0) |
| `specs/` | Design notes and research for the bar |
| `crashdiag/` | Crash-dump and memory diagnostic tools |

## My mods

| Mod | Runs in | Version |
|---|---|---|
| `local@top-bar` | its own tool process (32 and 64-bit) | 4.9 |
| `local@tray-bridge` | explorer.exe | 1.11 |
| `local@panel-split` | ShellExperienceHost, ShellHost | 1.14 |
| `local@start-menu-pop` | StartMenuExperienceHost, SearchHost | 2.5 |
| `local@taskbar-dock-seelen-wave` | explorer.exe (patched taskbar-dock-animation) | 1.9.2-seelenwave |
| `local@translucent-windows` | File Explorer windows (patched Translucent Windows) | 1.8.2.5 |
| `local@parallax-wallpaper-idle` | its own tool process (patched parallax-wallpaper: stops when idle, pauses behind maximized windows) | 0.9.2.1 |
| `local@dark-shutdown-dialog` | explorer.exe | 1.0.2 |
| `local@dark-nsis-installers` | NSIS installers | 1.0 |
| `local@edge-fullscreen-zoom` | msedge.exe | 1.2 |

## Third-party mods used (from the Windhawk catalog)

explorer-details-better-file-sizes 1.6, file-hover-info 1.1.2, icon-resource-redirect 1.3, invisible-borders 1.0.0, lock-screen-clock-styler 0.5.0, modern-run-dialog-ui 3.0.6, radial-window-switcher 1.0.0, taskbar-icon-size 1.3.11, taskbar-start-button-colorizer 1.1, taskbar-volume-control 1.3.1, win32-ui-modernizer 1.0.4, windows-11-file-explorer-styler 1.7.2, windows-11-notification-center-styler 1.8, windows-11-start-menu-styler 1.8, windows-11-taskbar-styler 1.10, windows-animations 1.3.7.
Installed but disabled: dynamic-island-for-windows, parallax-wallpaper (replaced by the idle fork), taskbar-dock-animation (replaced by the dock fork), windows-11-settings-styler.

## radial-window-switcher local build (touchpad swipe and Lookup)

`radial-window-switcher.wh.cpp` is a local build of the catalog mod (same id, so a catalog update overwrites it). It adds:

- Three-finger horizontal swipe opens the Alt+Tab wheel (raw HID touchpad reports), plus parity features (Ctrl+Alt+Tab sticky, Delete closes a window, mouse, 3-finger up/down for Task View and Show Desktop).
- **Lookup**: a three-finger tap shows a macOS-style word popover. The word comes from the selection under the pointer, else UI Automation, else OCR (English only). Cards: WordNet (offline), Wikipedia summary, and a local model note (qwen3:1.7b through ollama at `D:\ai\bin\ollama`, started on demand, unloaded right after).

Files in `radial/`: `touchpad.inc` + `patch.py` (swipe), `patch-parity.py`, `lookup.inc` + `patch-lookup.py`, `build-wordnet.py`, and the upstream / intermediate copies (`*.orig.wh.cpp`, `*.pre-parity.wh.cpp`, `*.pre-lookup.wh.cpp`).

Rebuild onto a fresh upstream: copy it to `radial-window-switcher.wh.cpp`, run `python -I radial\patch.py`, add the swipe clamp in `MoveSelection` by hand (not in patch.py), save as `radial\*.pre-parity.wh.cpp`, run `patch-parity.py`, save as `radial\*.pre-lookup.wh.cpp`, run `patch-lookup.py`. Then bump `$serial` in both `build-radial.ps1` and `install-lookup.ps1`, run `build-radial.ps1` (32 and 64-bit) and `install-lookup.ps1` elevated.

Data: `D:\lookup\wordnet.tsv`, made by `python -I radial\build-wordnet.py <WordNet 3.0 dict dir> D:\lookup\wordnet.tsv`. Run the patch scripts from this folder; they patch `radial-window-switcher.wh.cpp` in place. Log: `D:\lookup\lookup.log`.

## Setting up another device

1. Install Windhawk.
2. Third-party mods: install each from the catalog, then import its settings: `reg import installed\settings\<id>.reg` (elevated). After an import, bump `SettingsChangeTime` on the key or restart Windhawk, or running mods keep the old values.
3. My mods: either open Windhawk, create a new mod, paste the `.wh.cpp` and compile; or build with the `build-*.ps1` scripts and install with the matching `install-*.ps1`. Tool mods (`top-bar`, `parallax-wallpaper-idle`) need both 32 and 64-bit DLLs because `windhawk.exe` is 32-bit.
4. Icons: copy `installed/icon-theme-macos` somewhere and point `icon-resource-redirect`'s `themePaths[0]` at its `theme.ini`.

Known limits: the scripts still use this machine's paths (`D:\backups\windhawk-2026-11-06`, `C:\Users\shiva`); edit them or run from the same layout. `taskbar-icon-size` values must match the taskbar theme. Never hide tray parts with `Visibility=Collapsed` (it caused Explorer crash loops at login).
