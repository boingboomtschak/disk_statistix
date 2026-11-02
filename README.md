# Disk Statistix

WinDirStat-style disk usage treemap for Windows, built with Dear ImGui (Win32 + DirectX 11).

## Build

Requires Visual Studio 2022 (C++ workload) and git; CMake fetches Dear ImGui.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\disk_statistix.exe
```

## Use

- Pick a drive (or "Scan a folder...") in the startup dialog.
- Files are coloured by extension; the legend is in the lower-right panel.
- Hovering a folder's heading (or its row in the side list) draws a thick white border around all of its blocks.
- **Click a folder heading** to open that folder. **Click** a block to select it; **Ctrl+click** to select
  several (Esc clears the selection).
- **Double-click** a picture or video to preview it, any other file to open it with its default app (folders zoom in).
- **Right-click** for Open / Preview / Show in Explorer / Zoom / Copy path.
- **Drag** a block (or a row in the side lists) out of the window to drop the file into another app or an
  Explorer folder, like dragging from Explorer. Dragging a selected item takes the whole selection. If the drop
  moves files away (e.g. Shift-drag into another folder), they're removed from the map.
- **Mouse back / forward** (or Alt+←/→, or the ← → buttons) step through the folders you've visited, like a browser.
  **Backspace**, Up, or the breadcrumb go to a parent folder.
- **Del** (or right-click > Delete...) asks before moving the selected items to the Recycle Bin or deleting them
  permanently; permanent delete asks a second yes/no (Enter and Esc both mean "no"). The map updates in place.
- **Thumbnails** fills picture and video blocks with their thumbnail (cropped to the block; the same thumbnails
  Explorer uses). Hover one for a large preview; videos play silently on a loop, and the **scroll wheel** scrubs
  through them (down = forward, each notch ≈ 2% of the video). While duplicates are highlighted, only duplicates get
  thumbnails.
- **Double-click** a picture or video (or right-click > Preview) to view it large in a dialog sized to
  it. Videos there play with sound (volume slider; click the speaker to mute), can be paused (click / Space) and
  scrubbed (wheel, ←/→, or dragging the seek bar, which updates the picture as you drag).
  "Open in default app" hands the file to its usual player or viewer; "Delete..." (or Del) opens the delete dialog.
- The **?** next to the unreadable count lists the folders the scan couldn't open, and why.
- **Find duplicates...** finds identical files and folders. Non-duplicates are dimmed, duplicate folders get teal
  headings, and hovering a copy outlines all its other copies. The Duplicates tab lists groups by reclaimable space.
  Only files that share an exact size are read: first 16 KB, then the full file only if those still match.

`disk_statistix.exe D:\` starts scanning a drive or folder immediately.
Run as administrator to include folders that need elevated access.

Colours follow the "Sonoran Gothic" Windows Terminal scheme; they live in `src/theme.h`.

`logo/` holds the icon drafts (`drafts.html`), the chosen icon (`icon.svg`, `disk_statistix.ico`).
The Thumbnails setting is remembered between launches (in `HKCU\Software\Disk Statistix`).
