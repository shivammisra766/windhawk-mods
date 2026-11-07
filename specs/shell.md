# Shell spec: top bar plus island

Status: research only, nothing built or installed. Written 2026-11-07.

Sources and abbreviations:

- **OLD** = `C:\ProgramData\Windhawk\ModsSource\dynamic-island-for-windows.wh.cpp` (24,016 lines, v1.4.0, MIT)
- **BAR** = `D:\backups\windhawk-2026-11-06\src\topbar-core.cpp` (v2.1, 1,717 lines)
- **BUILD** = `build-bar.ps1`, **SPLIT** = `local@panel-split.wh.cpp`, **BRIDGE** = `local@tray-bridge.wh.cpp`

How far to trust the numbers below:

- **Measured** means read from the running system or compiler today.
- **Estimated** means reasoned from pixel counts and experience, not timed. Phase 0 replaces the estimates with timings.
- Nothing here was run on screen.

## 0. Decision summary

| Question | Answer |
|---|---|
| Windows | Two layered windows on one thread. The bar stays the full-width app bar and draws the left and right bubbles and the two buttons. The island is a separate window owned by the bar. It draws the centre bubble in every state. |
| Renderer | Direct2D and DirectWrite through a DC render target into a retained DIB, presented with `UpdateLayeredWindowIndirect` and a dirty rect. This is the path OLD already runs. Try the software target first. |
| Not chosen | Resizing the bar window on expansion; DirectComposition with swap chains. |
| GDI+ | Removed once the bar has moved over. Language support decides this more than speed does. |
| 32-bit build | Works. Headers syntax-checked and libraries found today; OLD's 32-bit DLL already exists on this machine. |
| Notch | Feasible. OLD's Notch is flat-topped but has no shoulders and anchors to the work area, which would put it under our bar. Section 3 gives the corrected geometry. |
| Bell and calendar run | Feasible and low risk. Draw them in the bar window, under the island. The real work is keeping hover alive across the empty gap. |
| Volume popup over the pill | Possible, with one z-order rule and a "covered" state. The popup's window class on this Windows build is still unidentified. |

## 1. Where things are in the old mod

OLD has no explicit state machine and no module registry. Each frame, its render loop re-derives everything from the current state.

| Topic | Lines | Notes |
|---|---|---|
| Kinds of island | 945-961 | `IslandKind`. Volume, Brightness, Lookup are dropped for us. |
| Priorities | 20867-20923 | `ChooseActivities`. Priority is push order: Lookup, Clipboard, Caps, Device, Bluetooth, DND, Volume, Brightness, Notification, BatteryLow, Progress, Timer, Media, otherwise Idle. |
| Sizes per kind | 20788-20865 | `ActivityForKind`, 150x44 up to 360x58, times `sizeScale`. |
| Frame selection | 22980-22988 | `kinds[0]` is primary, `kinds[1]` is secondary. |
| Split | 23243-23246, 11887-11904 | Width = sum + 12, height = max. Drawn side by side. |
| Expanded | 23166-23169, 23198-23202 | Only Idle and Media expand, to 380x184 (`MediaLayout`, 672-673). An alert on screen blocks expansion. |
| Transient alerts | 23056-23066 | The set of kinds that count as alerts. |
| Alert expiry | 22929-22967 | Per-module `active`/`expiresAt`, checked every frame under a lock. |
| Hidden | 23071-23104, 23227-23233 | Target size goes to 0, then the window is hidden (parked), 23603-23618. |
| Dispatch | 12798-12842 | Switch to `Draw*`. |
| Tab "registry" | 1906-1961 | `ActiveTabCount`, tab indexes and `StepIdleTab`. The comment at 1899-1905 admits this arithmetic drifted when copied around. |
| Layout constants | 671-739, 826-849, 856, 663-664 | `MediaLayout`, `IdleStripLayout`, `MediaPillLayout`, render padding. |
| Stage | 3975-3983, 4007-4058 | Fixed window of 880x320 content, the pill animates inside it. |
| Dirty rects | 11833-11858, 11927-11937 | Bind only the dirty region; `prcDirty` on present. |
| Renderer setup | 11753-11785 | Single-threaded factory, shared DWrite factory, DC render target. |
| Spring | 1608-1634 | Semi-implicit Euler at a fixed 0.5 ms step. |
| Spring constants | 23262-23308 | Expand 380/26, collapse 200/28, multiplied by the animation style. |
| Tab spring | 15481-15538 | 250/31, carousel in `DrawTabTransition`, 15471-15590. |
| Frame pacing | 23620-23711 | `DwmFlush` while springs move, 23682; high-resolution timer scoped to animation, 22740-22762, 23671-23674. |
| Hover | 23002-23031, 23080 | Polled: `GetCursorPos` against the pill rect every pass. |
| Hover scale | 11881-11883, 12751-12755 | 1.025 grow on hover. |
| Input hit test | 21984-22012 | `WM_NCHITTEST` against the last pill rect. |
| Click-through | 10770-10785, 23330-23333 | Toggles `WS_EX_TRANSPARENT`, also Ctrl+hover see-through. |
| Parked wake | 21020-21112 | `WH_MOUSE_LL` only while parked, on its own thread. |
| Mouse messages | 22154-22299 | Move, leave, capture change. |
| Scroll | 22541-22576 | Accumulates delta, cooldown 85 ms for notches and 200 ms for touchpads, wheel up = previous tab. |
| Right-click menu | 22578-22583, 11223-11376 | Commands 11392-11560. Source popup 11209. |
| Hide/show hotkey | 3861-3884, 22592-22636, 2939-2963 | Fullscreen override logic is at 22598-22608. |
| Thread rule for hotkeys | 655-660, 3400-3408 | Hotkey changes are posted to the window's thread. |
| Pin | 1747, 11399-11404, 22534-22539 | See the "do not copy" list. |
| Languages | 1968-1982, 1986-2015, 2068-2127, 3041-3051 | English text is the lookup key. 35 rows, 12 columns. Option list in the settings page at 459. |
| Clock | 2022-2066, 3025-3030, 14959-14974, 476-492 | 12/24, seconds, custom date, date first, first day of week. |
| Text size and font | 3022, 2832-2839, 12220-12226 | Scale is clamped to 0.7-1.6. |
| Settings page | 118-590 | YAML block. |
| Settings loading | 2794-3414 | Replaced whole under a leaf lock, 1649-1657, 3358-3384. |
| Right-click overrides | 3137-3146 | Override wins until the Mod Settings value changes. Useful pattern to keep. |
| Monitor selection | 2700-2726, 2820-2823, 129 | `primary`, `follow` (-1), or a number. |
| Monitor tracking and DPI | 2729-2742, 23343-23361 | Checked every frame. |
| Fullscreen | 2652-2688, 23106-23119, 22851-22861 | Client rect vs monitor, every 0.5 s; every 1.5 s while parked. |
| Z-order upkeep | 4063-4074, 4080-4106, 4117-4176, 23124-23129 | See the "do not copy" list. |
| Threads | 23733-23760 | Nine threads started, or eight without the notification listener. |
| Scaffold | 23839-24016 | The tool-mod boilerplate. |

### Old behaviour not to copy

1. **It wakes constantly.** The idle branch waits 16 ms, then loops (23680), so it runs about 60 times a second with nothing on screen moving. Ours must block in `GetMessage`.
2. **Double-click to pin does nothing.** The window class sets no `CS_DBLCLKS` (22659-22665; a search for it in the whole file finds nothing), so `WM_LBUTTONDBLCLK` at 22534 never arrives.
3. **Pin does not survive.** `LoadSettings` clears it at 3355-3356, straight after reading the saved value at 3096.
4. **The one-second z-order reclaim would defeat our volume popup.** The comment at 4076-4079 says the volume OSD is exempt, but no OSD class is in the list at 4087-4098. An uncovered class overlapping an eighth of the pill gets raised over (4138-4173).
5. **The alert nudge does nothing when flush to the top.** It starts at -6 and is clamped to `max(0, nudge)` at 11804.
6. **"Settled" means 0.01 px.** See 1629 and 23336. With the Smooth collapse constants the slow pole is about 6.4 per second, so a 150 px collapse keeps rendering for about 1.5 s after it looks finished.
7. **Language detection reads the regional format,** `GetUserDefaultLocaleName` at 2011, while its comment says UI language. Use `GetUserPreferredUILanguages`.
8. **Most of the menu is hard-coded English** (11233-11289). "12 languages" covers only the 35 rows.
9. **Animations freeze during modal loops.** `TrackPopupMenu` at 11371 and drag and drop run on the render thread, whose own loop stops until they return.
10. **Geometric layers everywhere.** Many `PushLayer` masks (15103, 15804, 16090 and others) are expensive and mostly unnecessary on a flat design.

## 2. Rendering decision

### The options

| | A. Resize the bar window | B. Separate island window, ULW | C. Separate window, DirectComposition |
|---|---|---|---|
| Reservation | Awkward: window rect no longer matches the reserved strip | Bar unchanged | Bar unchanged |
| Click-through | Per-pixel, automatic | Per-pixel, automatic | Must be built by hand, see below |
| Animation repaint | Risks repainting 1920 px wide | Island region only | Compositor can animate retained content |
| New GPU dependencies | None | None, with a software target | D3D11, DXGI, 32-bit Intel driver |
| Device-lost handling | No | No with a software target | Yes |
| Proven here | No | Yes, OLD does this | No |

**B is the recommendation.**

**Why not C.** The island is idle most of the time, where every option costs nothing. Frames happen only during a 300 ms expansion, a tab change or the small visualiser, so C's saving applies to very little time and costs tens of megabytes of driver all the time.

C also loses the best property of layered windows. On a layered window updated with per-pixel alpha, fully transparent pixels are click-through without code. That fits a notch, rounded corners and bubble gaps exactly. A non-layered window would need `SetWindowRgn` per frame, because `HTTRANSPARENT` is documented to fall through only to windows of the same thread, so it would not reach an app underneath.

The same limitation means OLD's `WM_NCHITTEST` (21993) cannot be relied on over other apps. We do not need it: with no shadow, the visible pixels are the hit area.

**Why not A.** Beyond the mismatch with the app bar rect, the bar would hold a 1920 x 260 surface (2 MB) to show 475 x 230 of content, and every bubble would share a dirty rect with the island.

### GDI+ versus Direct2D

| Path at 125% scale, expanded island about 475x230 px (estimates) | CPU per frame | One core at 60 fps | Added memory |
|---|---|---|---|
| Today: GDI+, full 1920 px bar, new DIB every paint | 4-8 ms | 25-50% | none |
| GDI+, island region only | 4-7 ms | 25-40% | under 1 MB |
| D2D DC target, software | 1.5-4 ms | 10-25% | 8-15 MB working set |
| D2D DC target, default (may use the GPU, with readback) | 1-3 ms | 6-18% plus GPU | 30-60 MB if the driver loads |
| DirectComposition with swap chain | under 1 ms | under 6% plus GPU | 40-80 MB |

Those are worst-case frames, and they last about 0.3 s per expansion. The i5-1235U has 12 logical processors, so 25% of one core is about 2% of the machine, and only briefly. On speed alone GDI+ would nearly do.

**DirectWrite wins on text, not speed:**

- The user wants 12 languages, including Hindi, Chinese, Japanese and Korean. DirectWrite gives shaping and font fallback. GDI+ is unreliable at both, and media titles can be in any script.
- The handoff notes already complain about wide word gaps in GDI+ text.
- OLD's dashboards, media views and alerts are all written against D2D. Porting them to GDI+ would be a rewrite instead of a port, which matters for the other three specs.

Use grayscale text antialiasing, since ClearType is not valid on a target with transparency.

### Measured today

- Bar process: **35.5 MB working set, 10.1 MB private, 6 threads, 5.9 s CPU in 2 h 34 min**, about 0.064% of one core. That is the baseline to protect.
- For scale, `parallax-wallpaper` has 50.5 MB private.
- Display: 1920x1080 at 60 Hz, Iris Xe. Anything above 60 fps is pointless here, so OLD's high-refresh code (spinning at 23701-23706, `TargetFPS`) is dropped.
- OLD is disabled, so its footprint could not be measured without changing state.

Estimate after the rebuild, before weather or audio capture: **50-60 MB working set, 18-25 MB private**, mostly shared system DLL pages. Worth saying plainly: D2D, DWrite and WinRT make the bar larger than it is today, though still one process.

### Why the 1920 px repaint is wasteful

`Paint` (BAR 639-821) clears and redraws everything on every frame of the pill animation:

- 1920x40 at 125% is 76,800 px, while a pill is about 12,000 px, so **84% of each frame is redrawing things that did not change.**
- `Present` creates and destroys a DIB section on every paint (603-631), 307 KB each, which means kernel objects and page faults 60 times a second.
- `Paint` rebuilds fonts and string formats every time (649-658), and re-measures and re-draws every text item and icon.
- `StepPill` can paint twice in a tick (1387, 1396).

The memory copy itself is cheap (about 30 microseconds for 160 KB). The waste is the redrawing and the allocation, not the blit. So the fix is retained surfaces and dirty rects, not fewer pixels on screen. A 1920 px window is fine once only its changed parts are drawn.

### 32-bit check

- **Compiled today, no files written:** `clang++ -fsyntax-only` with BUILD's defines, for both `i686-w64-mingw32` and `x86_64-w64-mingw32`, including `d3d11.h`, `dxgi1_3.h`, `d2d1_3.h`, `dwrite_3.h`, `dcomp.h`, `wincodec.h`, `wrl/client.h`, `winrt/base.h`, `winrt/Windows.Foundation.h`, `winrt/Windows.Media.Control.h`. Both exit 0.
- **Libraries present** for i686: `libd2d1.a`, `libdwrite.a`, `libdcomp.a`, `libd3d11.a`, `libdxgi.a`, `libdwmapi.a`, `libwindowscodecs.a`, `libruntimeobject.a`. Headers are in the shared `Compiler\include`.
- **Proof it links and loads:** OLD's DLL is in `Engine\Mods\32` at 981,504 bytes. It uses D2D, DWrite and C++/WinRT media controls (includes 620-633, options at line 9).
- **Needed changes:** add the libraries to BUILD line 75 **and** to `@compilerOptions` at BAR line 8, because the Windhawk UI compiles from the latter. They already disagree: BUILD links `-luuid` and the header does not list it.
- Add `#undef GetCurrentTime` after `windows.h` for WinRT (SPLIT line 23). Remove `using namespace Gdiplus` (BAR 34) when GDI+ goes, since its `Rect`, `Size` and `Color` will collide.
- Not measured: compile time with WinRT headers. Allow a `-Arch 32` switch for development.

### The design

```
Bar window (HWND_TOPMOST, app bar)            class WindhawkTopBar, keep this name
   full width x strip height, retained DIB
   draws: left bubble, right bubble, bell, calendar
Island window (owned by the bar)              class WindhawkTopBarIsland, must differ
   stage: largest island size, retained DIB
   draws: centre bubble in every state
UI thread: both windows, all drawing, all module state
Frame clock thread: waits on DWM, posts one message per frame, only while animating
```

**Owned, not just topmost.** An owned window always sits above its owner, so the z-order needs no upkeep. Hiding the owner does not hide owned popups, so hide both explicitly.

**The class name must differ.** SPLIT 77, BRIDGE 82 and `start-menu-pop` 191, 252, 273 all use `FindWindowW(L"WindhawkTopBar")`. Anchor properties stay on the bar.

**Stage size.** Lookup was the tallest case and is dropped (comment at 3978). So at most 712 x 184 DIP plus shoulders, about 905 x 230 px at 125%, which is 0.83 MB instead of OLD's 1.76 MB. The final figure comes from the largest size any module declares.

**Shrink when settled.** Once the spring stops, present with a smaller size, source and destination so the window is only the collapsed notch. Expand to the stage on the first frame of growth. `UpdateLayeredWindow` changes position, size and pixels together (4060-4062), so there is no seam. This also means no large invisible window sits on screen for DWM to composite or for other tools to find.

**Frame clock.** A small thread waits on an event when idle. While animating it loops: `DwmFlush`, then post `WM_APP_FRAME` unless one is already pending.

- Posted messages still arrive during `TrackPopupMenu` and `DoDragDrop`, which fixes OLD's freeze.
- The UI thread never blocks, which matters because BRIDGE waits on it from Explorer's taskbar thread.
- If `DwmFlush` fails or returns instantly, for example on the lock screen, sleep 16 ms so it cannot spin. OLD only checks success at 23682.
- Time with `QueryPerformanceCounter`, clamping dt to 50 ms as at 23260. `GetTickCount64` at BAR 1379 has about 15.6 ms resolution, which causes visible jitter.

**Springs.** Keep OLD's form with a 2 ms step. One fixed set of constants, replacing the style multipliers. Starting values, to tune by eye:

| Motion | Stiffness | Damping | Damping ratio |
|---|---|---|---|
| Expand | 380 | 35 | 0.90 |
| Collapse | 260 | 32 | 0.99 |
| Buttons | 320 | 34 | 0.95 |
| Tabs (OLD 15482-15483) | 250 | 31 | 0.98 |

Stop at under 0.25 px and under 5 px per second, not 0.01.

**Cheaper drawing:**

- Fill the shape, clip content with an axis-aligned rect, and inset content so corners never touch it. Avoid geometric layers.
- While the shape animates, draw expanded content once to a compatible bitmap target and blit it, fading as at 15141.
- Decode art on a worker with WIC; create the D2D bitmap on the UI thread.
- Geometries belong to the factory and can be shared between both targets. Brushes and bitmaps belong to each target.

## 3. The notch, in detail

### What the old mod draws

- **Notch style** sets radius to 16 x scale (12758-12759). `CreateNotchGeometry` (13031-13061) draws a flat top with square top corners and two convex bottom corners.
- **`BorderMergedMode`** is only a position change. It appears at 3935, 4009, 4183, 11803, 11893, 22003 and 23007 and never in shape code. So the shape stays a stadium, leaving wedges of wallpaper in the top corners. It touches the edge without reading as attached.
- Both modes set `flushTop`, placing the window at the top of the work area with `pillTop` 0 (4054-4055).

### Corrections needed

1. **Anchor to `rcMonitor.top`, not `rcWork.top`.** OLD uses the work rect (3934, 4023). Our bar reserves the strip, so `rcWork.top` is the strip's bottom, and a copied notch would hang below the bar. This is the largest trap.
2. **Add concave shoulders.** Convex bottom corners alone look like a rounded tab. What makes a MacBook notch read as part of the screen edge is the inverse fillet flowing into the top edge. OLD lacks this. The stage needs `rs` of spare width each side.
3. **Do not stroke the top edge.** OLD's border is a closed figure (13147-13149, 13082) and would draw a line along y=0. Use an open path with no top segment, faded in only as it expands, so a dark overlay stays separate from dark apps below. No stroke when collapsed.
4. **Drop the hover scale and shadow.** Scale (11881, 12753-12755) grows around the centre, which disturbs the flush top. The shadow (12857-12889) contradicts the flat look.
5. **Make the fill opaque.** A translucent fill shows wallpaper through it and reads as a floating panel. Opaque `#101010`, Start's panel colour without the alpha, is the proposal, and the bell and calendar rely on it to hide behind. Pure black would blend further into a laptop bezel; compare by eye.

```cpp
// Notch outline. L,R = body edges, B = bottom, r = bottom radius, rs = shoulder radius.
sink->BeginFigure({L - rs, 0}, D2D1_FIGURE_BEGIN_FILLED);
sink->AddArc(D2D1::ArcSegment({L, rs}, {rs, rs}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,         D2D1_ARC_SIZE_SMALL));
sink->AddLine({L, B - r});
sink->AddArc(D2D1::ArcSegment({L + r, B}, {r, r}, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
sink->AddLine({R - r, B});
sink->AddArc(D2D1::ArcSegment({R, B - r}, {r, r}, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
sink->AddLine({R, rs});
sink->AddArc(D2D1::ArcSegment({R + rs, 0}, {rs, rs}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,     D2D1_ARC_SIZE_SMALL));
sink->EndFigure(D2D1_FIGURE_END_CLOSED);
```

### How it fits with three bubbles

Sizes in DIP, starting points to judge by eye. Strip height stays 32 (BAR 74).

```
y=0  ================ top edge of screen ===========================
         .---------.                 \___________/                 .---------.
         | profile |  gap (wallpaper,   notch      gap (wallpaper, | tray    |
         | app     |  click-through)   attached    click-through)  | status  |
         '---------'                                               '---------'
y=32 ---------------- maximised apps start here ---------------------------------
```

| | Left and right bubbles | Centre notch |
|---|---|---|
| Top | y = 4, floating | y = 0, attached |
| Height | 24 | 30, leaving 2 above maximised apps (try 32 too) |
| Corners | All four rounded | Shoulders `rs` = 6; bottom `r` = 10 |
| Fill | `#DC101010` | Opaque `#101010` |
| Edge | `#30FFFFFF` hairline, as Start | None when collapsed |
| Screen margin | 8 | Centred |

The contrast does the work: the side bubbles float and the centre one does not. When expanded, the top stays pinned at y=0, the width and height springs run independently, and `r` eases from 10 to about 18, so it only ever grows downward and outward. Apps do not resize, because nothing touches the app bar.

Centre text on the notch's own height, which is 1 DIP higher than the bubbles' centre, or use the 32 height and accept the flush bottom.

With the bubbles separate, the empty gaps are transparent, so wallpaper shows and desktop clicks work through them. The strip remains reserved, so maximised windows stop at y=32.

Replacing the pill drawing in `Paint` (731-766) also removes its effect on `leftLimit` (792).

## 4. Bell and calendar running outward

**Feasible, moderate work, low risk.**

### Which window draws them

They travel along the strip, so draw them in the **bar window**. The island sits above the bar because it is owned by it, so the buttons start hidden behind the opaque notch and emerge from under its edge as they run. That occlusion needs no code. An island-window version would need a stage the width of the screen. Two tiny extra windows would also work and could move without redrawing, but they add z-order and hit-test plumbing for no benefit; the bar version also allows sub-pixel motion.

### Where they end up

```
collapsed:  [ left bubble ]              [notch]              [ right bubble ]
expanded:   [ left bubble ](o) ......[ island      ]...... (o)[ right bubble ]
                          bell                          calendar
```

- **Bell:** immediately right of the left bubble, `leftBubble.right + 4`.
- **Calendar:** immediately left of the right bubble, `rightBubble.left - 4 - size`.
- Round buttons about 28 DIP, same fill as the bubbles.

This keeps them near the side groups, not at the far screen edges where they would need to overlap the bubbles.

**Reserve room so nothing reflows.** Cap the left bubble at `(W - islandExpandedW)/2 - margin - gap - button - gap`. At 1536 DIP wide, that is about 530 DIP, so very long titles cut off a little earlier than today. The left limit is currently only `pillX - 12` (BAR 792). Tray icons can also grow, so clamp each target so it never ends behind the island. If there is no room at all, leave that button hidden and log it.

### Animation

- A single value per button runs on the button spring, in step with the island's target.
- `x = lerp(parked, target, t)`, where parked is behind the collapsed notch.
- Do not draw at all below `t = 0.01`.
- `target` is re-read each frame, so a title change during the motion is chased smoothly.
- Hit areas switch on at `t > 0.6`, as BAR 759 does at 0.5.

**Frame cost:** clear and draw two small rects in the retained strip, then one present with a dirty rect that is the union of both. That union spans up to about 1000x40 px, which is a 30 microsecond copy, so it is simplest to use one rect. Well under 1 ms a frame (estimate). Island and bar present one after the other, so at worst they differ by one frame, while the buttons are mostly hidden anyway.

### The hard part: hover across the gap

The gap between the island and each button is transparent, so messages go to the desktop and the bar sees `WM_MOUSELEAVE`. Without care, reaching for the bell collapses the island and the bell runs away from the pointer.

**Hover zone while expanded:** the island's rect, plus the strip band from bell's left edge to calendar's right edge.

**Tracking:**

- Collapsed: purely event-driven, `WM_MOUSEMOVE` on the notch's pixels. No polling and no hook.
- Expanded: a 50 ms timer tests `GetCursorPos` against the zone. It exists only while not collapsed. OLD polls at about 60 Hz always (23002-23021).
- Leave grace of 250 ms before collapsing.
- Inside the island's rect also check `WindowFromPoint`, so a popup or panel covering the island counts as leaving.

Considered instead: paint the band at alpha 1 so the bar receives mouse messages. It works, but blocks desktop clicks there and still needs a grace timer, so polling is simpler.

**Separate target for the buttons:** `wingsOut = islandExpanded || ownPanelOpen`. If the bell opens its panel, the pointer leaves and the island collapses, but the bell stays out so the same button can close the panel. It also keeps `g_anchorX` meaningful, because panels open centred under the button that was clicked (BAR 1464, 952-963).

## 5. Letting the volume popup cover the pill

This covers only the shell's side. Whether the popup can be moved and restyled is someone else's question, and memory lists it as unverified.

**Z-order: never raise ourselves while it is up.**

- Do not port OLD's reclaim (see section 1, item 4).
- Change BAR 842: `PlaceBar` passes `HWND_TOPMOST` on every call, including on `ABN_POSCHANGED`. Pass it only on creation, after `TaskbarCreated`, and when returning from fullscreen. Otherwise add `SWP_NOZORDER`.
- `UpdateLayeredWindow` does not change z-order, so animating frames are safe.
- Whatever shows later, topmost, is above us. That needs a real test before relying on it.

**Tell the popup where to go.** Publish the collapsed notch's rect as properties on the bar: `TopBarCentreLeft`, `Top`, `Right`, `Bottom`. Use the collapsed rect, not the current one. This is the same mechanism the bar already uses for `TopBarAnchorX`, read cross-process at SPLIT 85-90.

**A "covered" state, `Island::SetCovered(bool)`:**

- Fade the island window's constant alpha to 0 over about 80 ms, using `UpdateLayeredWindowIndirect` with only the blend changed, so there is no redraw.
- Why fade rather than rely on overlap: the popup's rounded corners will not match the notch's shoulders, so rims would show. A fade is the clean answer unless the popup is strictly larger.
- Queue alerts, block hover expansion, and pin the spring at the collapsed size.
- If the island is already expanded, leave it. The popup sits above it, and the pointer is probably there.
- Fade back when the popup goes.

**Signal.** Preferred: `EVENT_OBJECT_SHOW`/`HIDE` for the popup's own window, which gives exact timing. Fallback: the volume or brightness change event plus a 3 s timer. Add the hook only after the window is identified.

**Still unknown:** the popup's window class and process on build 26300. `HardwareConfirmatorHost` in Explorer is the old lead. The volume researcher or a read-only inspection during Phase 0 should settle this. `Volume` and `Brightness` also leave the alert list (OLD 20894-20899).

## 6. Splitting `topbar-core.cpp`

Windhawk needs one `.wh.cpp`, so this stays a **unity build**: separate files for people and agents, concatenated by BUILD. No separate compilation.

### Layout

```
src/topbar/
  manifest.txt            order of files
  mod-header.txt          ==WindhawkMod== block; @compilerOptions is generated
  readme.md
  scaffold.inc            the tool-mod boilerplate, vendored
  core/
    includes.inc          windows.h first, then d2d, dwrite, winrt, #undef GetCurrentTime
    types.inc             shared structs, IModule, Services
    anim.inc              Spring, FrameClock
    surface.inc           LayeredSurface: retained DIB, DC target, present
    icons.inc             SVG to ID2D1Geometry, reusing PathReader and AddArc (BAR 182-345)
    text.inc              text formats, layout cache, font and size setting
    settings.inc, loc.inc, input.inc
  bar/
    placement.inc         app bar, fullscreen, monitors (BAR 825-890)
    panels.inc            native panels (BAR 892-1170)
    tray.inc              (BAR 1172-1272)
    profile.inc           (BAR 1274-1310)
    bubbles.inc           left and right, buttons
    window.inc            WndProc, thread (BAR 1421-1696)
  island/
    state.inc             state machine and alert queue
    window.inc            island window, drawing frame
    notch.inc
  modules/<name>/         module.inc, settings.yaml, strings.tsv
```

**BUILD changes:**

- Read `manifest.txt` and write `#line 1 "core/anim.inc"` before each file, so compiler errors name the real file and line.
- **Vendor the scaffold.** BUILD lines 54-60 extract it from OLD's source at build time, so removing the old mod would break the build.
- Merge each module's `settings.yaml` into the `==WindhawkModSettings==` block, and each `strings.tsv` into the `Loc` table. Files per module mean agents working in parallel do not collide.
- Generate `@compilerOptions` from one list, shared with the clang call.
- Keep version and serial in one place. They are currently at BUILD line 2, BAR line 5 and `install-bar.ps1` line 18.
- Add `-Arch 32` for speed, and `-CheckOnly` for syntax-only checks, which touch nothing.

**First step is a pure move.** Split with no behaviour change, and confirm the concatenated output compiles and is logically the same as before.

### Module interface

Everything runs on the UI thread except a module's own workers. Workers publish with `Services::Post`.

```cpp
struct Size2 { float w, h; };

enum class View { Pill, Alert, Dashboard };

struct DrawCtx {
    ID2D1RenderTarget* rt;
    D2D1_RECT_F rect;      // DIP, already scaled
    float alpha;           // content fade
    double now, dt;
    View view;
};

struct PointerEvent {
    enum Kind { Move, Down, Up, Leave } kind;
    D2D1_POINT_2F pt;      // relative to rect
    bool left;
};

class IModule {
public:
    virtual ~IModule() = default;
    virtual const wchar_t* Id() const = 0;
    virtual void Start(Services&) {}
    virtual void Stop() {}
    virtual void OnSettings(const Settings&) {}

    // Dashboards: appear in the scroll order if HasDashboard().
    virtual bool HasDashboard() const { return false; }
    virtual int  DashboardOrder() const { return 100; }
    // Collapsed: only the highest PillPriority() above 0 shows, otherwise the clock.
    virtual int  PillPriority() const { return 0; }

    virtual Size2 Measure(View) = 0;
    virtual void Draw(const DrawCtx&) = 0;
    virtual bool OnPointer(const PointerEvent&, View) { return false; }  // true: handled
    virtual void OnClickAlert() {}                                        // tap actions

    virtual bool Animating(View) const { return false; }  // keeps the frame clock alive
    virtual void OnVisibility(View, bool shown) {}        // start or stop costly sampling
};

class Services {
public:
    // Alerts: replaces the scattered active/expiresAt fields (OLD 22929-22967).
    void ShowAlert(IModule*, int priority, int ms);
    void ClearAlert(IModule*);

    void Invalidate();                       // repaint once
    void WakeAnimation();                    // re-check Animating()
    void Post(std::function<void()>);        // from any thread to the UI thread

    const Settings& Settings() const;        // UI thread only
    const wchar_t* Loc(const wchar_t*);
    TextService& Text();
    ImageCache& Images();
    void SetTimer(IModule*, UINT ms, std::function<void()>);
    void OpenPanel(PanelKind, int anchorX);
    void PersistInt(const wchar_t* key, int value);  // Wh_SetIntValue
};
```

A static table in the manifest order lists the modules. Each worker's data reaches the UI thread as a posted snapshot, so module state has no locks. Settings have one writer, the UI thread: `WhTool_ModSettingsChanged` only posts a message (the reason is at OLD 655-660, 3400-3408).

This must be reconciled with the media, alerts and dashboards specs, which did not exist when this was written. Freeze it at the end of Phase 3.

### State machine

```
Hidden  <-  hotkey or fullscreen  ->  any state
Collapsed  (clock or best pill)
   -> Alert:    alert queued
   -> Split:    two alerts, or alert + live pill
   -> Expanded: hover after dwell, or pinned
Alert/Split -> Collapsed: expiry (one timer at the next deadline, no polling)
Expanded -> Collapsed: leaves zone + 250 ms grace, unless pinned
Flags: covered (section 5), pinned, wingsOut
```

A pure function `Compute(inputs) -> {kinds[2], width, height, wingsOut}` runs only when an input changes. This keeps OLD's good idea of deriving state, without its per-frame loop.

**Behaviour changes from OLD, for the user to accept:**

- Alert arriving while expanded: OLD snaps the dashboard shut, because the alert takes over (23166, 23198). Proposal: while hovering, stay expanded and queue the alert.
- Hover dwell of about 120 ms. The notch sits above browser tabs and title bars, so brushing past would otherwise throw an overlay over them. OLD has none.

### Other shell items

- **Scroll:** port 22541-22576 and `StepIdleTab`. Add `WM_MOUSEWHEEL` to the island; the bar has no wheel handling. A non-activating window receives the wheel only with "Scroll inactive windows when I hover" on (the Windows default). Without that, it would need a hook, which is not worth it.
- **Right-click:** one menu for all bubbles, in `input.inc`. Port the `SetForegroundWindow` plus `WM_NULL` pattern (11369, 11374), and **also restore the previous foreground window afterwards**, or the user's app stays without keyboard focus. OLD lacks this. Tray icons keep their own right-click (BAR 1478-1484).
- **Hotkey:** `RegisterHotKey` on the bar window on the UI thread. Hide means hide both windows and release the reservation. A fullscreen override follows 22598-22608. `Ctrl+Alt+D` fails with an error if the old mod is re-enabled; log it.
- **Fullscreen:** keep ours. `ABN_FULLSCREENAPP` plus foreground events (BAR 846-871, 1507-1512) are event-driven, better than OLD's polling. Add the island's class to `IsShellWindow` (BAR 522-528). Unverified by hand against a real fullscreen app, and F11 without a foreground change depends on `ABN_FULLSCREENAPP` arriving.
- **Monitors:** port 2700-2726. **Follow-mouse is a conflict with an app bar**: moving the reservation reflows every maximised window whenever the pointer crosses a monitor. Proposal: reservation and bubbles stay on the chosen monitor, and in follow mode only the island moves, using a 250 ms cursor timer that exists only in that mode. Needs a user decision; only matters with two screens.
- **Languages:** port `Loc` (2068-2127) with the table generated from `strings.tsv`, and route every label through it, menus included. English fallback stays.
- **Clock:** port 2022-2066. BAR hard-codes its pattern at 734-735. With seconds on, arm a 1 s timer instead of `ArmClock`'s minute, and use `WidestDigitForm` (OLD 11720) so the notch does not twitch.
- **Settings:** a short YAML plus module fragments, keeping the override rule at 3137-3146 for right-click choices.

## 7. Review of the bar's known bugs

Severity reflects impact on this rebuild. "Verified" means read in the code, not reproduced by running.

### First open of panels

The cause: the bar **pushes** the mode to a host that may not exist yet, the host sizes its clip on open, and so the bar closes and reopens, showing the full panel for 1-2 s.

| # | Sev. | Problem | Lines | Fix |
|---|---|---|---|---|
| 1 | High | **The panel timer can run forever.** `g_panelCold` resets `g_panelTicks` to 26 whenever it reaches 1, so the 16 ms timer never ends unless the panel takes focus. If Win+N is ignored, for example because an elevated window has focus, it runs at 60 Hz indefinitely. | 999-1001, 1023 | Add a 5 s deadline. |
| 2 | High | **Cold is misdetected.** `g_notificationPanel` is set only by a foreground event, so the first click after every bar start counts as cold, even with a warm host, causing a needless close and reopen. | 1045, 1328 | Goes away with the next fix. |
| 3 | High | **Push versus pull.** Set `TopBarPanelMode` and a tick on the bar before Win+N, as at 1467-1468. SPLIT reads them in `Attach` (179-201) and in its visibility handler (187-199), the same way Quick Settings reads its anchor (85-90). `kAttach` already fires at window creation (265, 280). | 1047, SPLIT 194-196 | Untested: named elements may not exist yet at that moment, which would need a retry on load. |
| 4 | Med | **Hiding the first cycle,** which the handoff lists as not tried. SPLIT already hooks `SetWindowRgn` (116) in the host, so it could apply an empty region during a cold open, since these panels paint only inside their region. | SPLIT 116-129 | Untested. Fallback if #3 is not enough. |
| 5 | Med | **Sequences are not re-entrant.** A click during `kReopenTimer` is not guarded, and the timer's own `TogglePanel` (1557) can then close what was just opened. | 1029-1070, 1530-1558 | One `PanelController` with explicit states, keeping only the latest wish. |
| 6 | Med | **Profile can stick or do nothing.** If `WM_PROFILE_DONE` never comes (Start crashes, `start-menu-pop` off), `g_profileOpen` stays true and the next click sends a bare Win key. If the Start host has no window yet, `armed` is false and the click silently does nothing. | 1290-1310, 1494-1502 | 3 s timeout; if not armed, warm the host and retry once. |

### Focus

| # | Sev. | Problem | Lines | Fix |
|---|---|---|---|---|
| 7 | Med | Injected Win+N and Win+A combine with any modifier the user is holding, and can land oddly while Search is foreground (handoff). `StepQuickClose` injects on a timer, unrelated to any click. | 892-908, 1146-1170 | Wait while a modifier is held. Skip injection during fullscreen. |
| 8 | Low | `SetForegroundWindow` back to the previous app can fail (traps memory). Its result is ignored. | 1168 | Log it. Hand-test app switching, still unverified. |
| 9 | Rule | The island must never take focus, as nothing in it needs the keyboard now that Lookup is gone. Keep `MA_NOACTIVATE` (1453) on both windows. Only the context menu takes the foreground, and must give it back. | | |
| 10 | Low | Click fires on release only, so pressing elsewhere and releasing on an icon triggers it. Track the pressed item with capture and handle `WM_CAPTURECHANGED`. | 1455-1477 | |

### Timers and event load

| # | Sev. | Problem | Lines | Fix |
|---|---|---|---|---|
| 11 | High | **The title hook is expensive.** `EVENT_OBJECT_NAMECHANGE` is hooked system-wide, so every accessible name change on the desktop wakes the thread. For the foreground window, each change then does `OpenProcess`, reads version info from disk, and repaints the whole bar. A terminal title spinner would cost real CPU and block the UI thread on I/O. | 1341-1342, 1659, 530-541, 478-520 | Re-hook for the foreground process only, on each foreground change. Coalesce to 100 ms. Cache app names by path. |
| 12 | High | **Animation uses `SetTimer` and tick counts.** 15 ms timer, 15.6 ms clock, DIB per paint, up to two paints per tick. | 1375, 1379, 1387, 1396 | Frame clock and surfaces, section 2. |
| 13 | Med | **Explorer waits on us.** BRIDGE uses a blocking send with a 200 ms timeout on Explorer's taskbar thread for each tray update, and our handler paints synchronously, so a slow bar slows the taskbar thread. | BRIDGE 109-110, BAR 1241 | Convert the icon in the handler, because its handle is valid only then (1192), then schedule the paint. Reduce the timeout to 50 ms. |
| 14 | Med | **UI thread blocking sends** of 200-300 ms per window, plus `OpenProcess` per window, on each click, would hitch an animation. | 992, 1301, 918-926 | Timeout 50 ms, cache host windows. |
| 15 | Low | Hover index is a position in a list whose contents shift when tray icons change or the pill's buttons appear. | 675, 720, 759, 1359-1366 | Use stable item ids. |
| 16 | Low | Tracking and hover are not reset when the window hides for fullscreen, so the pill may come back expanded. | 866, 1433-1452 | Reset on hide. |
| 17 | Low | `InternetGetConnectedState` runs on the UI thread on each network change. | 412-415, 1591 | Fine for now. Watch for stalls. |

### Hook lifetime

**The current hooks are correct. The risks are in what comes next.**

- The scaffold's main thread **exits right after init**: `WhTool_ModInit` runs at OLD 23916, then the entry point is replaced with `ExitThread` (23856-23858, 23928). So anything thread-bound created directly in `WhTool_ModInit` dies with it: hooks, `RegisterHotKey`, windows. BAR is safe because 1701 only creates a thread.
- BAR's win-event hooks are set on `BarThread` (1657-1660), which pumps, and are removed at 1676-1677. Correct.
- BRIDGE keeps its thread asleep on purpose (134-136). SPLIT pumps (284-293). Both correct.
- BRIDGE gives up silently if the taskbar is not found within 2 minutes (128-147). Add a log line.

Rules for all modules:

1. `WhTool_ModSettingsChanged` runs on another thread: post only. Never register anything there.
2. **No low-level hook on the UI thread.** A `WH_KEYBOARD_LL` or `WH_MOUSE_LL` callback holds up input for the whole desktop, and Windows silently removes a slow one. Give it its own thread with a bare pump (OLD 20974, 21070), and do no work in the callback, only post (see OLD's comment at 20930-20938).
3. Worker threads that set a hook or a window-bound registration must live until unload, asleep on the stop event.
4. Install expensive hooks only while needed, as OLD does at 21065-21068.

### Build and teardown

| # | Sev. | Problem | Lines | Fix |
|---|---|---|---|---|
| 18 | High | The build depends on the old mod's source file. | BUILD 54-60 | Vendor it. |
| 19 | Low | `g_hwnd` is not cleared in `WM_DESTROY`; `RegisterClassW` is never undone; line 1642 returns without cleanup. Harmless, as the scaffold calls `ExitProcess` (24015). | 1612-1617, 1633, 1642 | Tidy during the move. |
| 20 | Low | Killing the bar process, as install does, skips `ABM_REMOVE`, so the reservation can linger until Explorer notices. | 883-890 | Close it through `WM_CLOSE` where possible. |

## 8. Phases

Effort is focused engineer-days. An agent session is faster at typing, but each visible step still needs an install, a UAC prompt and the user's eyes, and the user's notes say limits run out fast. **User test rounds are the real constraint**, so they are listed.

The source folder is **not under version control**. Run `git init` in `src` before Phase 1 if the user agrees. Parallel agents on one folder would otherwise overwrite each other, and worktrees need a repo.

```
P0 probes
 |
P1 split (no behaviour change)
 |                                   P7 panel first-open  (parallel, any time after P1)
P2 surfaces, frame clock, text
 |
P3 bar on D2D, three bubbles
 |
P4 island window, state machine, notch  <-- interface frozen here
 |
 +-- P5a bell/calendar run        +-- P6a media module    (media spec)
 +-- P5b covered state            +-- P6b alert modules   (alerts spec)
 +-- P5c menu, hotkey, pin        +-- P6c dashboards      (dashboards spec)
 +-- P5d monitor, DPI, fullscreen
 +-- P5e clock, font, languages
 +-- P5f settings page
 |
P8 integrate, measure, soak
```

| Phase | Content | Effort | Depends on | User test rounds |
|---|---|---|---|---|
| **P0** | Headless timing exe with no window, comparing GDI+, D2D software and D2D default on a mock 475x230 frame, with working set. Identify the volume popup's window. Check z-order assumptions. | 0.5-1 | none | 0-1 |
| **P1** | Vendor scaffold, manifest, `#line`, YAML and strings merge, single version. Pure move. | 0.5-1 | git | 0, compile comparison only |
| **P2** | `LayeredSurface`, frame clock, springs, text service, SVG to geometry, Settings, `Loc`. Ship by moving only the bar's clock text to prove it. Fix #11 and #12. | 2-3 | P0, P1 | 1 |
| **P3** | Bar drawn in D2D with dirty rects. Three bubbles. GDI+ removed. Avatar and tray bitmaps moved over. Fix #13-16. | 2-3 | P2 | 1-2 |
| **P4** | Island window, stage, shrink when settled, state machine, alert queue, notch, hover and zone logic, tabs and scrolling with **stub modules**. Interface frozen at the end. | 3-4 | P3 | 2-3 |
| **P5a** | Bell and calendar run, width caps, `wingsOut`. | 1-1.5 | P4 | 1-2 |
| **P5b** | Covered state, published rect, z-order rule. | 0.5-1 | P4, popup identified | 1 |
| **P5c** | Menu, hotkey, pin, focus restore. | 1 | P4 | 1 |
| **P5d** | Monitor choice, follow-mouse, DPI changes, fullscreen. | 1-1.5 | P4 | 1, mostly fullscreen |
| **P5e** | Clock options, font, text size, `Loc` everywhere. Hindi and CJK checks. | 1 | P4 | 1 |
| **P5f** | Settings page merge and live reload. | 0.5 | P4 | 1 |
| **P6** | Feature modules per the other specs. | see those specs | P4 | see those specs |
| **P7** | Panel controller, pull model in SPLIT, cold-open hiding. Fix #1-6. Touches `panels.inc` and SPLIT only. | 1-2 | P1 | 2, needs host restarts |
| **P8** | Integration, memory and CPU against the 35.5/10.1 MB baseline, overnight soak, cleanup. | 1-2 | all | 1-2 |

**Shell total: about 15-22 engineer-days, and about 13-18 user test rounds**, before any feature module. P0-P4 is strictly one agent at a time, since each changes the foundation. That is half the effort, so parallelism helps less than it appears. It pays off only after P4.

**Rules for parallel work:**

- Each agent owns its folder and its fragment files.
- `core/` and `types.inc` change only through one owner.
- Agents may build with `-CheckOnly` freely.
- **Only one integrator installs, and only after asking,** because the user types during tests and keystrokes can land in Search.

**Main risks:**

1. Restyling and moving the native popup, which is unverified and outside this spec, though the covered state is useful either way.
2. D2D working set could exceed the estimate if the default target loads the Intel driver. P0 answers this, and the software target is the fallback.
3. Touching the panel host has caused crashes before, so P7 carries the most crash risk; keep the existing reopen path until the pull model has been proven.
4. Text-heavy dashboards may need the bitmap cache sooner than planned.

## 9. Questions for the user

1. **Centre fill:** opaque `#101010` for the notch (proposed), with `#DC101010` on the side bubbles? A translucent notch would show the bell and calendar behind it and wouldn't read as attached.
2. **Hover dwell:** about 120 ms before expanding (proposed), or instant as in the old mod?
3. **Alert while expanded:** stay open and queue the alert (proposed), or snap shut as the old mod does?
4. **Hide hotkey:** release the reserved strip so apps reclaim it (proposed), or keep it empty?
5. **Follow-mouse:** only the island moves, the bar and reservation stay (proposed)? Moving the reservation reflows every maximised window at each monitor crossing.
6. **Title length:** long window titles cut off a bit earlier, to leave room for the bell. Acceptable?
7. **Version control:** `git init` in `src` before work starts?
