# Media: port spec for the top bar

Old source: `C:\ProgramData\Windhawk\ModsSource\dynamic-island-for-windows.wh.cpp` v1.4.0 (24,016 lines, D2D). Bare line numbers below refer to this file.
Target: `D:\backups\windhawk-2026-11-06\src\topbar-core.cpp` v2.1 (1,717 lines, GDI+). These lines are written `core:N`.

Scope: player, source switching, auto-expand and blocklist, clock in the pill, album-art tint.
Dropped by the user: lyrics, and all theme and backdrop options. There is **one fixed look, but the album-art tint stays**.

Evidence tags:
- **[R]** read in the source
- **[D]** checked on disk or from a live process
- **[C]** type-checked with the Windhawk compiler (`clang++ -fsyntax-only`, i686 target; nothing was written)
- **[K]** my knowledge of the API, not tested here
- **[E]** estimate, not measured

Nothing in this spec was run. No WinRT or audio call has been executed for this work.

---

## 1. Conclusions

1. **It works from 32-bit as far as can be checked without running it.** See section 2. A runtime spike is still step 0.
2. **Do not port the old engine's shape.** While music plays, even collapsed, the old mod does all of this:
   - renders at 60 fps (23433-23458)
   - holds a 1 ms timer (22750-22762)
   - polls WASAPI 62 times a second (8393)
   - takes a mutex about 750 times a second (6636 → 6571)
   - polls SMTC every 1.5 s (5576)

   The bar's value is that nothing polls (core:11-12), and this design would destroy that.
3. **The old mod uses no SMTC events at all.** There are zero matches for `MediaPropertiesChanged`, `PlaybackInfoChanged`, `TimelinePropertiesChanged`, `SessionsChanged` or `CurrentSessionChanged` [R]. An event-driven engine is therefore new, unproven code, and all of the old mod's battle-tested logic assumes a 1.5 s poll. **This is the largest risk.**
4. **The default "waveform" is not a spectrum.** It is 6-7 bars of RMS history over about 37 ms of audio. The FFT only runs for the Orb and Plasma styles (1718, 2897-2898). With one fixed look, the FFT can go.
5. **A full-bar repaint cannot be the animation path.** `Paint()` rebuilds fonts, hit lists and every string (core:639-821), and `Present` allocates a new DIB each call (core:603-631). Anything animated needs its own small surface with a partial present.

---

## 2. 32-bit feasibility and linking

| Check | Result | Tag |
|---|---|---|
| `windhawk.exe` architecture | PE machine `0x14C` (i386) | D |
| `Windows.Media.MediaControl.dll`, `AudioSes.dll`, `MMDevAPI.dll`, `WinTypes.dll` in SysWOW64 | All present. MediaControl is PE32 i386 | D |
| `libruntimeobject.a` for i686 | Present in `Compiler\i686-w64-mingw32\lib` | D |
| `winrt/Windows.Media.Control.h` | Present | D |
| 32-bit build of the old mod | `Engine\Mods\32\dynamic-island-for-windows_1.4.0_530752.dll`, 981 KB, same WinRT and WASAPI code | D |
| `<gdiplus.h>` + C++/WinRT + WASAPI + WIC in one i686 unit, including `auto_revoke` on all five SMTC events, `wait_for`, `Controls()`, `PlaybackRate`, `StartTime`, `MaxSeekTime` | **Type-checks clean** | C |
| Header parse cost | About 5.7 s for syntax-only of those headers, on top of the current build | D |
| `IAudioMeterInformation` | **Forward-declared only** in this compiler's `endpointvolume.h` (line 91). Needs a local `MIDL_INTERFACE("C02216F6-8C67-4B5B-9D00-D008E73E0064")` with `__CRT_UUID_DECL`. That form type-checks | C |
| Tool process integrity | Tool-mod processes appear to be medium: a medium shell can read their image path. So a `ShellExecuteEx` launch is not elevated | D, inferred |

**Link additions:** `-lruntimeobject -lwindowscodecs -loleaut32`. `-lole32`, `-luuid` and `-lgdiplus` are already linked. `-lwinmm` is not needed because there is no `timeBeginPeriod`.

**Change the list in both places:** `core:8` (`@compilerOptions`) and the hard-coded list at `build-bar.ps1:75`. Editing only one will build locally and fail in the other path.

**Threading rule:** `BarThread` is STA (core:1624). `.get()` on a WinRT async must never run there, because it blocks the UI thread and is illegal on STA [K]. All WinRT work goes on one MTA thread.

**Live baseline, the running `local@top-bar` process (PID 3544)** [D]: working set 35.5 MB, private 10.1 MB, 6 threads, 6 s CPU. The island mod is not running at the moment, so there is no live figure for it.

---

## 3. Target architecture

One new thread, one new window, no new timers while idle.

```
MediaThread (MTA)                         BarThread (STA, existing)
  owns manager + session + revokers        owns all GDI+ objects
  WaitForMultipleObjects(stop, wake, cmd)   WM_MEDIA_CHANGED -> Paint() if a visible field changed
  reads SMTC, decodes art to raw PBGRA      panel window: own DIB, own timer, only while open
  command queue: skip/toggle/seek/switch
        | mutex + shared_ptr<const Snapshot>, then PostMessage
```

**Wake policy, replacing the fixed 1.5 s at 5576:**

| State | Wakes |
|---|---|
| No sessions | `SessionsChanged` only, plus a 60 s health check |
| Paused sessions | Events, plus 30 s fallback |
| Playing, panel closed | Events, plus 5 s fallback. Reads status and title only, not the timeline |
| Playing, panel open | Events, plus 1.5 s (same as the old mod, so its jitter guard works unchanged) |

- Event handlers only `SetEvent`. Coalesce bursts for about 100 ms before reading.
- The session the handlers are attached to changes on source switch. Revoke and re-attach using `auto_revoke`.
- The fallback intervals exist because event reliability per player is unproven. If the spike shows events are reliable, lengthen them. If not, the fallback is the safety net. Do not remove it on assumption.

**Every async gets a timeout.** Use `op.wait_for(2s)`, then `Cancel()` on timeout. `.get()` at 5286/5296/4962/4974 has no timeout, so a hung player would block the media thread forever. `wait_for` compiles [C].

**Shutdown:** the scaffold calls `WhTool_ModUninit`, then `ExitProcess(0)` (24014-24015). Unload crashes are therefore not the main worry, but `core:1707-1716` must also signal and join the media thread. The 2 s cap on waits keeps this inside the 3 s budget.

---

## 4. Feature: media player

### 4.1 Line ranges

| Part | Lines |
|---|---|
| Layout constants | 671-741 (`MediaLayout`; lyrics constants at 675-677 and 743+ are dropped) |
| Snapshot structs | 1262-1318 |
| Media thread | 5267-5581 |
| Art bytes and decode | 4955-4978, 4232-4433 |
| Source icon | 4545-4600, 4676-4697, 4763-4815, 4837-4874 |
| Seek and transport | 10866-10894, 10899-10920, 10932-10977 |
| Open the player | 10979-11119 |
| Hit tests | 2134-2191, 2211-2276, 2353-2366 |
| Paint | 15685-16243 (media parts: 15840-15907, 15931-16049, 16086-16173, 16175-16242) |
| Controls | 17326-17400, 17411-17537, 17539-17643, 17648-17758 |
| Art draw | 18193-18207, 18299-18310, 18905-18952 |
| Marquee | 19086-19134 |
| Input | 22042-22069 (cursor), 22081-22152 (down), 22154-22215 (move), 22270-22301 (leave, capture), 22303-22528 (up), 22534-22539 (pin) |
| Size decision | 23198-23215 |
| Repaint rules | 23399-23458, 23511-23559 |

### 4.2 APIs, threading, event vs polling

- **APIs:** `GlobalSystemMediaTransportControlsSessionManager::RequestAsync`, `GetSessions`, `GetCurrentSession`; `session.TryGetMediaPropertiesAsync` (Title, Artist, AlbumTitle, Thumbnail), `GetPlaybackInfo`, `GetTimelineProperties`, `TrySkipPrevious/Next/TogglePlayPauseAsync`, `TryChangePlaybackPositionAsync`, `SourceAppUserModelId`.
- **Art:** `IRandomAccessStreamReference.OpenReadAsync`, `DataReader`, then WIC to PBGRA.
- **Old threading:** one MTA polling thread, plus **one new detached thread per button press and per live seek** (10867, 10933), each calling `RequestAsync` again.
- **New threading:** a single command queue on MediaThread, using the manager it already holds. Coalesce seeks so only the newest target is sent. The old version can spawn about 7 threads a second during a drag (22179), and those seeks can arrive out of order.

### 4.3 Collapsed layout

The old pill is 150×44 (20794-20795), art 32 px with 6 px pad (15730-15733), bars 28×20 at `right-42..right-14` (16111). **None of this fits the bar**, which is 32 tall with a 24 px pill (core:74, 79).

The proposal is to extend the existing centre pill instead of adding a second one. In design px, ×`Scale()`:

```
[bell] [art 18px] [ date, time ] [bars 18x14] [calendar]
        <- 26 slot ->            <- 26 slot ->
```

- Both slots are always the same width, so the clock stays exactly centred, which is the pill's existing rule (core:731, 752).
- Width: `pillW = clockW + 2*12 + 2*26*m + 2*grow`, where `m` is a new 0..1 "media present" value on the same easing as `g_pillT` (core:738-741, 1378-1386). `kAnimTimer` only runs while something moves.
- Bell and calendar are anchored to the pill's outer edges (core:764-765), so they land outside the slots without changes.
- The left limit follows `pillX` (core:792), so the window title re-ellipsizes automatically.
- **The right group has no limit** (core:683-729). The pill grows about 32 physical px each side at the current 125% scale (`Scale()` assumed 1.25 from the 1536×864 logical bounds). This can touch tray icons if there are many. Test with a full tray.

| State | Art slot | Bars slot |
|---|---|---|
| No session | Width 0 | Width 0 |
| Paused | Art | Flat row of dots, alpha 0.5 (16115-16125) |
| Playing | Art | **Static** accent bars, no timer (user decision, section 5.3) |
| No art | Accent at 0.24 alpha, plus source icon or badge (18922-18934) | As above |

### 4.4 Expanded layout

Use a separate layered popup, not a resized bar. The bar is an appbar (core:873), so resizing it would shift every window on screen. Style: `WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST`, 380×184, centred under the pill at `bar bottom + kPanelGap` (core:83). Open with position and alpha through `UpdateLayeredWindow`, **without re-rendering**, reusing `kPanelDrop`/`kPanelDropMs` (core:85-86).

All in design px from the panel's top-left, reusing `MediaLayout`:

| Element | Geometry | Source |
|---|---|---|
| Art | x24 y20, 64×64, radius 16 | 706-708, 15793-15794 |
| Source badge on art | 21 px circle at art `right-24..-3`, `bottom-22..-3` | 18937-18945 |
| Bars | x324..356, y42..62 (32×20) | 15856-15861 |
| Text column | x106..308 | 15863-15864 |
| Title | y34-54, falls back to "Unknown" | 15867-15869 |
| Artist | y54-74, muted 0.80 | 15876-15879 |
| Album, only if present | y68-84, muted 0.70 | 15882-15887 |
| Source line | y70 (y85 with album), 14 tall, "Playing from X" / "Paused in X" | 15906-15907, 17161-17162 |
| Scrubber bar | x72..308, y114, half-height 2.5 (3.5 dragging) | 679-691, 15964-15992 |
| Thumb | Radius 4.5 (6.5 dragging), white fill, accent ring 1.6, halo ×2.2 at 0.22 alpha while dragging | 15998-16025 |
| Time labels | Left x24..64 right-aligned; right x316..356 "-m:ss" | 15949-15978 |
| Prev, play, next | y148, cx ±64; radius 16, 22, 16; hit pad 6 | 696-703, 16032-16038 |
| Button look | White fill 0.04/0.09 (nav), 0.08/0.16 (play), rest/hover; accent when pressed; press scale 0.87; icon opacity 0.62/0.92, 0.88/1.0 | 17553-17575 |
| Source dock | Section 6 | |

Dropped from layout: lyrics button (16040-16042), immersive view, tab pagination (16081).

**Coordination item:** other specs may want tabs. If so, this panel should be a host shared with those features, and the swipe and wheel code (22339-22351, 22541-22576) comes back.

### 4.5 States and interactions

**Open and close.** The old default is expand on hover (197, 1203). In the bar, hovering the pill already grows it for bell and calendar (core:1439), so a plain hover would fight that.

- Proposed: click on the art or bars slot toggles the panel. Optionally open after a 300 ms hover dwell on those slots only.
- A `WS_EX_NOACTIVATE` window never receives focus loss, so **close by pointer leave with about 300 ms grace**, spanning the bar, the 8 px gap and the panel. `TME_LEAVE` fires per window, so one small one-shot timer has to check the cursor.
- Double-click pins it open (22534-22539), persisted with `Wh_SetIntValue`. Needs `CS_DBLCLKS` on the panel class.
- Hide while fullscreen (23199; core:640 stops bar painting but must also hide the panel).

**Input order on press:** dock, then transport, then scrubber (22124-22149).

| Input | Behaviour | Source |
|---|---|---|
| Press on a button | `SetCapture`, press animation. Command fires on release | 22133-22139, 22494-22500 |
| Press on bar | Starts scrub, `SetCapture` | 22141-22149 |
| Drag | Fraction keeps tracking off the bar | 22169-22191, 2265-2276 |
| Release from scrub | Seek, then write position locally at once | 22305-22323 |
| Release over bar, no drag | Swallowed, must not open the player | 22503-22509 |
| Capture lost | Clears scrub, pressed and hover | 22289-22301 |
| Leave | Clears hover and pressed slot | 22270-22276 |
| Hover over dock and prev overlap | Dock wins | 22205 |
| Over controls | Hand cursor | 22042-22069 |
| Click art or text | Focus the player | 22512-22516 |
| Right-click | Source menu | 11209-11221 |

Deliberate change: in the old mod a click on *any* empty area of the expanded player opens the app (22512-22516). Limit that to the art and text.

**Scrubbing during a live drag.** The old mod seeks every 150 ms while dragging. For players that stutter when seeking, commit on release is smoother. Keep live seek only through the coalescing queue, and check Spotify in the spike.

**Scroll:** only changes tabs in the old mod (22573), so it has no media function to port.

### 4.6 Timeline

Position only matters while the panel is open, so read it only then.

- Extrapolate from `LastUpdatedTime` if age is under 10 s (5312-5321).
- Render-side extrapolation between polls with `GetTickCount64` (15935-15938).
- Keep the jitter guard exactly (5323-5361). It holds a backward jump until a second reading confirms it, because Spotify refreshes `LastUpdatedTime` without moving `Position`.
- It interacts with the optimistic write at 22319-22320. A stale reading after a seek is caught by the guard instead of snapping the thumb back. Port them as a pair.

### 4.7 Reuse vs porting

| Code | Verdict |
|---|---|
| `SelectActiveMediaSession`, `CollectMediaSources`, `ChooseMediaSourceAumid` (5006-5265) | Nearly verbatim |
| Timeline, VLC title, art-retention logic inside 5267-5581 | Same logic, restructured into a read function. Fix the bugs in section 10 |
| `ReadWinRtStreamBytes` (4955-4978) | Verbatim, with `wait_for` |
| `DecodeImageBytesToPixels` (4232-4433) | Nearly verbatim. Add `IWICBitmapScaler`, replace `D2D1_COLOR_F` |
| Transport logic (10932-10977) | Logic verbatim, threading replaced |
| `MediaLayout`, hit tests (2211-2276, 2389-2450) | Verbatim, minus `MediaPlayerZonePoint` (immersive only) |
| Name and badge helpers (4639-4674, 5058-5068) | Verbatim. Fix substring matches (section 10) |
| `OpenRelevantApp` (10986-11119) | Verbatim with bug fix |
| Seqlock geometry (926-933, 2134-2191) | **Delete.** It exists because rendering and input ran across threads while the pill scaled. The panel is fixed-size and single-threaded |
| Play/pause shapes (17650-17693) | Nearly verbatim (`Point2F` to `PointF`). Draw with `FillPolygon` and a round-join `Pen` |
| Skip icons (17411-17537) | **Rewrite.** They rely on D2D geometry combine. GDI+ `Region` exclude is not anti-aliased [K]. Use two gapped triangles, or add SVGs to the existing icon pipeline (`build-bar.ps1:7`, core:360) |
| Marquee (19086-19134) | **Rewrite.** Pre-render text to a PARGB bitmap once and blit with an offset. Speeds 42/30/28/26 px/s, gap 38. **Start with ellipsis** (core:657-658) and add it later if wanted |
| Accent gradient track (12971-12993) | Rewrite with `LinearGradientBrush` (a few lines) |
| Everything using `ID2D1Layer`, including rounded art (18209-18232, 18905-18952) | Rewrite. `SetClip` in GDI+ is aliased (existing case at core:775-779). Use `TextureBrush` + `FillPath` for anti-aliased corners, and pre-compose once per art |
| Flip, record drop, title smoke, swap marquee, ripple (18234-18292, 18294-18526, 17920+, 18143+, 19159-19176) | **Drop.** Per-frame rotation and skew in software GDI+ would be the single most expensive thing in the bar. Snap, or do one ULW alpha pulse |

**GDI+ note:** `Bitmap(w, h, stride, PixelFormat32bppPARGB, bytes)` wraps WIC's PBGRA buffer directly and does not copy (same pattern as core:616). Keep the shared pointer alive as long as the `Bitmap`.

---

## 5. Feature: waveform

### 5.1 Line ranges

Thread 8338-8472. Gate 1717, 23235-23239. Chunking and envelope 6349-6376, 6570-6638. Paint 18541-18599, 19136-19185. FFT 6382-6548 (Orb and Plasma only). Dropped styles: 1009-1014, 2887-2898, 18602-18839.

### 5.2 How the old one works

- WASAPI shared-mode loopback on the default render endpoint: `eRender, eConsole`, 200 ms buffer (8358, 8368-8371). **No event callback:** it polls every 16 ms (8393).
- RMS of each 64-frame chunk, multiplied by 4, into a 48-entry ring. Attack 0.7, release 0.85 (6578-6585, 6601, 6636).
- At 48 kHz one chunk is 1.33 ms, so the 48-entry ring holds about 64 ms. The comment at 19147 claims a 4-sample step is "approx 40ms". It is really about 5 ms.
- 6 bars collapsed (28 px), 7 expanded (32 px). Gap 2.5, height `max(3, amp*0.86*H)`, opacity `0.45+0.5*amp`, accent colour (19136-19185).
- Capture opens only when the media pill is visible and playing (23235-23239). When idle the thread still wakes four times a second (8345).

It captures all system audio, not just the shown player, so a notification sound moves the bars.

### 5.3 Decision: how to drive the bars

User preference is near-zero idle, with the waveform only while expanded. This means **the collapsed pill shows static bars**. That is a visible difference from the old mod. If it is unacceptable, the fallback is 10-15 fps with a partial present on only the slot's rect.

| Option | How | Cost | Look |
|---|---|---|---|
| **A: peak meter (recommended first)** | `IAudioMeterInformation::GetPeakValue` on the default endpoint, about 30 Hz from the panel timer, pushed into a history ring. No thread, no capture, no audio buffers | One COM call per frame, nothing while closed | Bars scroll over about 230 ms. Slower and calmer than the old 37 ms window, so it will look different |
| B: loopback RMS (old look) | Port 8338-8472 and 6570-6638 | Thread plus capture stream, only while open | Identical to the old default |
| C: loopback + FFT | Add 6382-6548 unchanged. No D2D in it. About 14.5 KB of tables, about 94 FFTs a second at 48 kHz | Slightly more than B | A true spectrum, the only option that literally is one. Band-to-bar mapping must be written, since the old Bars style never used bands |

Option A needs the local interface declaration from section 2. Caveats, all [K] and to verify in the spike: reading may depend on endpoint volume, and some devices may behave differently, so check it at low volume and over Bluetooth.

If B or C is chosen:
- Use one lock per frame, not one per sample. `PushWaveformSample` locks `g_stateMutex` for every chunk, about 750 times a second (6571). Use a thread-local ring and publish once per packet.
- Start and stop the thread with the panel. Do not keep the 250 ms idle poll (8345).
- Keep the behaviours in section 11.

Paused or stopped: dots row, no timer. Rendered from 15892-15903.

### 5.4 Porting

`DrawWaveform` is about 50 lines, and only `FillRoundedRectangle` needs replacing (`AddRounded` + `FillPath`, core:575). Drop the track-change ripple (19159-19176) and the `specT_` logic in `DrawSpectrum`.

---

## 6. Feature: cross-source switching

### 6.1 Line ranges

- Selection: 1807-1887, 5006-5055, 5070-5081, 5083-5200, 5208-5265
- Dock: 718-726, 734-740, 2382-2450, 17174-17324
- Animated switch: 17039-17134 (drop)
- Menu: 11168-11221, 11246-11251, 11388
- Input: 22123-22131, 22201-22214, 22331-22333, 22472-22486

### 6.2 Selection rules (keep exactly)

Priority order (comment at 5203-5207):

1. **Dock pick:** temporary. Ends when something new starts playing anywhere, or when that app goes away (5228-5241).
2. **Locked app** from the right-click menu: wins over Auto, and ignores other apps starting (5243-5245, 1838-1853). Not persisted across restarts. Keep it that way unless asked.
3. **Auto:** playing beats paused; within each group the newest start wins (5247-5260). If nothing has ever played, returns `""` so Windows decides (5261-5263). `SelectActiveMediaSession` then prefers current-if-playing, then any playing, then current (5041-5054). This is the fix for VLC being ignored while a stale session was "current" (4999-5004).

Details that must survive:
- Sessions with the same AUMID merge, playing if any is playing (5099-5104).
- A source that disappears forgets its history, so reopening it counts as a fresh start (5123-5134).
- If several start within one read, the system's current session counts as newest (5144-5151).
- Stable dock order: new sources go at the end (5154-5166).
- A user switch is not a new track: the 4 s window suppresses auto-expand (5530-5538, 1831).

**Event-driven consequence:** `wasPlaying` and `startedSeq` are computed by comparing against the previous poll. With events, every session needs a `PlaybackInfoChanged` subscription, not just the shown one, or a start in another player will go unnoticed until the fallback tick. **This also means a playing-to-playing start is detected up to 5 s late if events fail for that app.**

### 6.3 Layout and interactions

Dock appears only with 2 or more sources (2391, 17176). Pill x20, centred on y148, height 30, slots 26, gap 2, pad 2, icons 16 (718-723). Widest is 86 px, ending at x106, with prev's hit area overlapping from x104, where the dock wins.

- Up to 3 sources: one slot each. More: first, active, "+N" (2389-2403).
- Accent ring 1.5 px on the active slot. Icons at 50% opacity, 100% when active or hovered (17280). 7 px green dot with a 1.5 px ring when that source is playing (17311-17320).
- Left click: switch (22481), or the popup for "+N" (22476-22479). Clicking the active slot does nothing. No `SetCapture` on press (22127).

The slide and fade animations (0.17 s out, 0.24 s in, 0.30 s ring slide; 729-732, 726, 17225-17233) are the expensive part and are droppable. Use a snap, keeping the 1.5 s hold-timeout idea only if the slide returns. The kept code is the direct path: `RequestMediaSource` (1823-1836).

### 6.4 Menu pitfalls in the bar

- `TrackPopupMenu` needs `SetForegroundWindow` to dismiss correctly (11212), but the bar is `WS_EX_NOACTIVATE`. Taking the foreground also triggers the bar's own foreground hook (core:1313-1321), which can close Quick Settings. Test this.
- `core:525-527` `IsShellWindow` must include the **new panel class**. Otherwise a foreground change to it makes the bar show "Windhawk" as the current app (core:530-533).

### 6.5 Icons

`FindMediaSourceIcon` (4859-4874) runs `EnumWindows`, matching by process name. It retries every 8 polls per source (5187-5194). Reuse `BitmapFromIcon` and `SHGetFileInfoW` already in core (core:464-476) rather than porting `getProcessIcon` and `IconToPixels` (4435-4600). Store apps such as a Store Spotify won't match by exe. Keep the letter badge fallback (17303-17309). Icon lookup belongs on MediaThread, delivered as raw pixels, because `Bitmap` objects live on BarThread.

---

## 7. Feature: auto-expand on track change, plus blocklist

### 7.1 Line ranges

Settings 364-366, 511-513, 2919, 3059-3080. Match 4699-4729. Trigger 5526-5573. Decision 23049-23054, 23056-23066, 23198-23202.

### 7.2 Behaviour

- **Trigger:** `isDifferentTrack` = not a user switch, title not empty, playing, and (title differs, artist differs, or paused-to-playing) (5540-5541).
- **Effect:** `titleChangedAt = now`, and the panel opens for 5 s (23054). It does not open in fullscreen (23199). Other activities with higher priority win because of `primary.kind == Media` (23051).
- **Blocklist:** comma-separated, trimmed and lowercased at parse (3061-3075). Each entry is a substring match against `sourceName \n AUMID \n title` (4719-4727). **Only expansion is suppressed, the collapsed pill still updates** (4702-4703). Motivation: short-video feeds caused constant popups (#62).
- Empty blocklist is a fast path (4711-4716). The render-thread race noted at 4705-4708 disappears here if the check runs on BarThread with settings held there.

### 7.3 Port

- Keep the blocklist function and parser verbatim.
- Replace the render-loop timestamp check with: on `WM_MEDIA_CHANGED` with `trackChanged`, if not blocked and not fullscreen, open the panel and arm a one-shot 5 s timer. On the timer, close unless hovered or pinned. If hovered, fall back to pointer-leave close.
- A new track during those 5 s resets the timer.
- Bar has no settings today: no settings block (core:1-9), `WhTool_ModSettingsChanged` is empty (core:1705). Add a `==WindhawkModSettings==` block with two entries: `MediaAutoExpand` and `MediaExpandBlocklist`. Default in the old mod is off (364). The user asked for the feature, so propose on. **Not checked:** whether `install-bar.ps1` handles a settings block.
- Bugs to fix when porting: section 10, items 1-3.

---

## 8. Feature: clock in the collapsed media pill

### 8.1 Line ranges

Settings 367-369, 3039. Layout 851-863. Measure 12092-12115, 11720. Paint 15770-15777, 16094-16100, 16144-16170. Size 23204-23213. Repaint 23465-23477, 23500-23509.

### 8.2 Port

**Almost nothing to port.** The old feature exists because media replaces the idle clock there. In the bar, the clock is already the pill, updated by one aimed timer (core:732-752, 1353-1357). Section 4.3's layout gives this result by construction.

Keep one thing: `WidestDigitForm` (11720), used to measure width so digits don't make the pill wobble. The bar measures the real string (core:737), and the media slots would visibly shift by a pixel each minute. About 15 lines.

Drop: `MeasureMediaPill`, the setting (the clock is always on), the seconds option (the bar uses a fixed `hh:mm tt`, core:734), and the divider. The art already separates things. If wanted, the old values are 1 px wide with a 10 px inset on a 44 px pill, about 6 px at this height.

---

## 9. Feature: album-art accent tint

### 9.1 Line ranges

Sampling 4305-4428. Contrast 2550-2649. Smoothing 12437-12473. Bloom 12924-12953, 13119-13128. Uses 15984-15996, 17271-17273, 17553-17585, 19180. Fallback 1135, 1267.

### 9.2 Algorithm (keep verbatim)

Runs at decode time on MediaThread, stored with the pixels:

1. Skip pixels with alpha under 32, luminance under 36 or over 220, or saturation under 28 (4319-4332).
2. Histogram into 16×16×16 buckets, weighted by `1 + saturation/32` (4334-4341).
3. Take the top 3 by weight, then the one with the highest average saturation (4344-4376). This stops a big beige background beating a smaller vivid colour.
4. Pool the winner with its 26 neighbours, to avoid splitting across bucket edges (4379-4406).
5. Enforce at least 3:1 contrast against the surface by stepping lightness, minimum saturation 0.35 (2617-2649).
6. Black-and-white art, with no qualifying pixel, falls back to `#4cc9f0` (1267, 4424-4428).

**Port:** replace `D2D1_COLOR_F` with a 4-float struct, about 100 lines near verbatim. No D2D logic. The light-theme branch of the contrast function is now dead with one fixed look but costs nothing. Replace `g_settings.pillBgColor` (4414), which is also an unlocked cross-thread read, with a constant: the panel background `kBarBg` (core:90). The bucket array is about 82 KB on the stack, fine on a default 1 MB thread.

**Sample after the downscale** (section 12), and compare a few covers against the old mod. Averaging while downscaling reduces saturation slightly, so winners may shift. If it looks worse, sample before scaling, using a nearest-neighbour stride.

### 9.3 Where the tint shows

Fixed look, so a shorter list than the old mod:

| Surface | Spec |
|---|---|
| Panel background wash | Radial, centred at top-middle, radii `0.75w` × `1.15h`, peak alpha 0.115, fading to 0 (12945-12947, 13124). GDI+: `PathGradientBrush` on an ellipse |
| Scrubber fill | Gradient, accent mixed 30% white to accent (12983-12986) |
| Thumb ring, dock ring, bars, button icons, pressed button fill | Accent (15995-16024, 17271, 19180, 17557-17575) |
| Collapsed pill | Bars only. The pill stays black (core:747) |

**No-art fallback:** `#4cc9f0`, matching 1135. The alternative is the system accent the bar already reads (core:156, 384). Trivial decision, flagged.

**Dropped:** accent modes (989, 2843-2845), bloom slider (3021), auto contour border (13130-13144), surface depth (12896-12922), scrim.

**Transition:** the old lerp has τ = 0.18 s, per frame (12453-12468). Panel closed: snap. Panel open: a few frames at most, because each repaints the cached background. Bloom at 0.115 alpha only has about 29 8-bit levels across the panel, so **check for banding**; GDI+ does not dither [K].

---

## 10. Bugs in the old code: fix, do not port

Inferred from reading. None was reproduced at runtime.

| # | Lines | Problem | Fix |
|---|---|---|---|
| 1 | 5540-5545 | First poll after start: stored title is empty, so anything already playing counts as a new track. **Panel pops up at login** | Skip the trigger on first read |
| 2 | 5485, 5541 | Paused-to-playing counts as a track change, so pressing play opens the panel. The setting text says "new song or video" (366) | Require a title or artist change |
| 3 | 5519-5524, 5562 | Any exception publishes an empty snapshot: pill vanishes, and the next read is a "new track", giving a false auto-expand and an art re-decode. Likely when a player closes mid-read | Keep the last snapshot on error, drop it only after session loss is confirmed |
| 4 | 5285 | `manager` is never reset. If the broker dies, the object stays stale forever | Reset on failure, and retry in the health check |
| 5 | 5549-5555 vs 5479-5505 | **The settle-window refetch is undone.** Same title, position moved forward: `next.art` is overwritten by old art. When text updates before art (the case that window exists for), the wrong cover can stick for the whole track | Hash the thumbnail bytes and replace only when different. Retry at +1.5 s and +3 s |
| 6 | 5466-5469 | No icon found: `EnumWindows` plus a process query for every window, every 1.5 s. It ignores the retry throttle at 5187-5194 | Use the throttle |
| 7 | 10997-11001 | Empty title: `std::search` with an empty needle matches the first titled visible window, so **an unrelated window gets focus**. Also doesn't exclude the bar or panel | Skip the title match when empty, exclude own windows |
| 8 | 10867, 10933 | Detached thread per command, `RequestAsync` each time, possible seek reordering | Command queue |
| 9 | 8358, 8454 | Default device switch is only picked up by the 5 s stall watchdog | If using B or C: `IMMNotificationClient`, or reopen on panel open. For A: re-resolve the endpoint on each panel open |
| 10 | 4289 | Art over 2048 px is rejected, so no art at all | Scale instead |
| 11 | 4644, 4648 | `edge` and `media` substrings mislabel any AUMID containing them ("knowledge") | Match `msedge` and specific tokens |
| 12 | 5033, 10914 | Duplicate AUMIDs: the first session wins, though possibly the paused one | Prefer the playing session within a duplicate group |
| 13 | 5307-5309, 15933-15938, 22316 | `StartTime`, `PlaybackRate`, `Controls()` and seek limits are all ignored. At 2× the thumb runs slow and jumps | Duration = end − start. Multiply extrapolation by rate. Grey out or disable controls from `IsNextEnabled`, `IsPlaybackPositionEnabled` etc [C for API, K for player behaviour] |
| 14 | 15949-15959 | Live stream: `endTicks` is 0, showing "0:00 / -0:00" | Hide scrubber and labels when duration is 0 |

---

## 11. Behaviours that must not be lost

- **VLC with no title:** falls back to the window caption, must end in "VLC media player", and the owning process must really be VLC, so a browser tab named after VLC doesn't match (5378-5428).
- **Spotify timeline jitter guard** (5324-5358).
- **Browser art delay** (5479-5490), corrected per bug 5.
- **No blank flash** when one thumbnail read comes back empty, but never keep old art across a real track change (5507-5516).
- **Source icon retention** across reads (5556-5560).
- **Media keys are a last resort.** If a session exists and refuses, do *not* synthesize a key, because it can double-skip or toggle back. Keys only when no session could be reached at all (10922-10931, 10954-10975).
- **Launch fallback:** path checks, `" (x86)"` retry, abort rather than guess, because guessing triggers broken App Paths (11079-11099), and `shell:AppsFolder` for real AUMIDs (11107-11113).
- **Silent packets** push zero (8414-8420). Silence stays silent regardless of gain (6504). **Stall watchdog:** playing with no packets for 5 s reconnects (8439-8458). Init failure backs off 2.5 s (8380-8386). Float and 16-bit plus `WAVE_FORMAT_EXTENSIBLE` detection (6603-6608). All apply to options B and C only.
- **Pause must stop the bars' animation and repaint once.** The old mod fixed both a frozen frame (23541-23545) and a paused session keeping 60 fps for hours (23412-23422).
- **Bars read `playing`, not session presence:** browsers keep paused sessions alive for hours (23414-23415).
- **Press cancel paths:** 22289-22301, 22270-22276.
- **`endTicks > 0` guard** before any seek (22186, 22315).
- **Mic and camera shift** of the bars slot (16106-16109). If the privacy dots land in this bar through another spec, reconcile the positions.

---

## 12. Memory and CPU

| Item | Old | New | Tag |
|---|---|---|---|
| Art in memory | Full decode, up to 2048²×4 = **16 MB** per cover, shared by refcount | 128² = 64 KB, plus small composites. Scale to about 2× the largest display size (64 × 1.25 × 2 ≈ 160), so 128-160 px | R, E |
| Art decode | Every 1.5 s for the 3 s settle window (bug 5) | Only if the bytes hash differs | R |
| Threads | Media, audio, plus one per command | 1. Audio only under B or C, only while open | R |
| Idle, no media | 1 wake per 1.5 s, 4 wakes per s from audio | **0**, except a 60 s check | R, E |
| Playing, collapsed | 60 fps, 62 wakes/s audio, 1 ms timer | **About 12 wakes a minute**, no repaint until the track changes | R, E |
| Playing, panel open | Same | ≤30 fps of a small dirty rect plus 1 Hz progress | E |
| Process memory | n/a | Baseline 35.5 MB working set, 10.1 MB private [D]. Expect +3-8 MB working set from loading WinRT and WIC, +150-300 KB on the DLL | E |

**Rendering rules that make this possible:**

1. Paint the bar only for visible changes: art, playing state, title, source count. **Never for position.** Compare before posting.
2. The panel keeps a persistent DIB and memory DC, unlike `Present` (core:603-631), and a cached background layer holding wash, art and static text.
3. On each timer tick, restore only the bars' rect from the cache, draw, and call `UpdateLayeredWindowIndirect` with `prcDirty`. Progress redraws at 1 Hz, or only when it moves a pixel.
4. The 30 fps timer exists only while the panel is open and playing. Paused: no timer.
5. Create `Font` and `FontFamily` once. `Paint()` rebuilds them every call (core:649-652). Harmless today, not harmless at 30 fps.
6. No `timeBeginPeriod`. `SetTimer` at 33 ms is good enough for bars, and a 1 ms timer is a system power cost.
7. Free the art composites and the panel DIB on close, to give back the memory.

**Unknown until measured:** GDI+ cost for a 475×230 physical-pixel panel on this i5-1235U. I have no number. Measure in the spike with a `QueryPerformanceCounter` log. Target: under 1 ms per partial frame, and under 1% of one core with the panel open.

---

## 13. Risks, ranked

1. **SMTC event reliability per player** (Spotify, Chromium, Firefox, VLC, Store apps). Nothing here has tested it, and the old code gives no evidence. Mitigated by fallback timers, at the cost of detection lag.
2. **Multi-session subscriptions and re-attachment** around closing players. Stale revokers and callbacks during a player's exit are new failure paths with no precedent in the old code.
3. **`IAudioMeterInformation` behaviour.** It compiles but is untested, and the look differs from the old one. Options B and C are the safe fallback.
4. **Pill hover versus media expand** (section 4.5) is a UX conflict the old mod never had. It needs the user's decision, then a try-out.
5. **Build time and size.** Adds at least 5.7 s per build for both architectures, and every edit recompiles everything since the build pipes one file through stdin. Acceptable, but noticeable.
6. **`IsShellWindow`, foreground hook, Quick Settings interplay** with the popup menu (section 6.4).

---

## 14. Decisions needed from the user

1. Collapsed bars static (recommended) or slow-animated?
2. Waveform driver: A (recommended), B or C?
3. Panel opens on click (recommended), or also on hover dwell?
4. Drop the track-change animations (recommended)? Keep only the play/pause morph.
5. Marquee, or ellipsis first (recommended)?
6. Should a resume-from-pause open the panel? The old mod does. Recommended no.

---

## 15. Implementation order

Each step ends in something the user can test. Lines are new or ported code, estimated.

| # | Step | Lines | Notes |
|---|---|---|---|
| 0 | **Spike:** separate throwaway 32-bit tool mod, log only. Activation, events per player, meter values, panel paint timing | 150 | **Go/no-go for sections 3 and 5.3.** Do not skip |
| 1 | Build flags (both places), MediaThread, snapshot, wake policy, `WM_MEDIA_CHANGED`, shutdown join. Log only | 300 | |
| 2 | Art decode and scale, accent sampling, hash | 150 | Mostly copied |
| 3 | Pill slots, `m` animation, static bars, `WidestDigitForm`. Clock feature finished here | 120 | First visible result |
| 4 | Panel window, placement, open and close, pointer-leave, fullscreen hide, `IsShellWindow`, static render, tint wash | 300 | Largest GDI+ work |
| 5 | Transport, command queue, hit tests, cursor, open player | 200 | |
| 6 | Scrubber, timeline, jitter guard, optimistic seek | 150 | Subtle: test with Spotify |
| 7 | Waveform, partial present | 100 (A) / 250 (B or C) | |
| 8 | Sources: multi-session subscribe, selection, dock, menu, icons | 300 | Mostly copied logic, new event wiring |
| 9 | Auto-expand, blocklist, settings block | 80 | |
| 10 | Polish: play/pause morph, optional marquee | 80 | Optional |

### Honest effort estimate

- **About 1,900-2,200 lines** on a 1,717-line file, so the bar roughly doubles. Roughly 40% is copied logic from the old mod, and the rest is new.
- **About 5-8 focused working sessions** of agent work, assuming the spike is clean, plus the user's testing after each step. A human alone: about 1-2 weeks.
- **The uncertainty is concentrated in steps 0, 4 and 8.** If events prove unreliable, step 8 gets simpler but idle cost rises (5 s poll becomes the primary path). If GDI+ partial frames are too slow, waveform falls back to 10-15 fps.
- **Not a cheap port.** Fewer than 200 of the old mod's drawing lines survive in a recognisable form. The logic moves over cleanly, and the rendering is effectively a rewrite.

### Compliance note

Research was read-only. One no-output compiler check, process listing and file inspection were performed. No input was sent, and no UI, process, registry or mod state was changed. The only file written is this spec.
