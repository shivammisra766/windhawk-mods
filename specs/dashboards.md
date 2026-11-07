# Expanded dashboards and shelf: port spec

Scope: the pages shown when the island expands (idle, calendar, weather, hardware, focus timer, shelf) and Dropshelf-style drops across the bar.

"Old" line numbers refer to `C:\ProgramData\Windhawk\ModsSource\dynamic-island-for-windows.wh.cpp`. "Bar" line numbers refer to `D:\backups\windhawk-2026-11-06\src\topbar-core.cpp` as of 2026-11-07.

Dropped, per the brief: Quick Lookup, lyrics, game overlay. Decided by the user: one fixed look with no theme options; the island expands on hover as an overlay over apps, and dashboards draw in that overlay.

Evidence labels:
- **Measured**: run on this machine on 2026-11-07. The probe was a headless 32-bit exe built with Windhawk's clang for `i686-w64-mingw32`.
- **Read**: taken from the source.
- **Not tested**: reasoning or API knowledge only. Each needs a hand test, because real drags need real input.

---

## 1. Findings that change the plan

1. **The old tray keeps no content, only references.** Text is the one exception. `FileTrayItem` (old 1456-1464) has no image, link or virtual-file support. Dropshelf parity is new work, not a port.
2. **Old default shows only 3 rows.** `VisibleRowCapacity` (old 796-800) with height 184 gives `(184-16-46+6)/36 = 3`. Everything else collapses to "+N" and cannot be reached (old 14176-14186). A shelf holding many items needs scrolling.
3. **There is no timer dashboard in the old mod.** The timer starts only from the right-click menu (old 11236-11244) and shows as a 260x54 activity pill (old 20830-20833, 19417-19505). A page is new design.
4. **There is no battery on the old idle view.** The time page (old 14931-15079) shows clock, date and a privacy badge. Battery exists only as an alert (old 20420-20485). Battery on the idle page is new, and the bar already has the data.
5. **`wininet.h` and `winhttp.h` cannot be included together** (Measured: `INTERNET_SCHEME` typedef redefinition). The bar includes wininet at bar line 18, so weather must use WinINet. The old `HttpGet` cannot be pasted in.
6. **First PDH open blocks for about 480 ms** (Measured). It must not run on the bar thread, or the overlay will freeze as it opens. After that, sampling is cheap (section 6).
7. **Old mod storage is writable by every process.** `icacls` on `C:\ProgramData\Windhawk\Engine\ModsWritable\mod-storage` shows `Everyone:(F)` and `ALL APPLICATION PACKAGES:(F)` (Measured). Dropped text can hold passwords, so stored content must not go there. It also breaks the user's rule that data lives on D:.
8. **Text-row drag in the old code looks destructive.** For a text row, `s_trayPressPath` is empty (old 22108-22109), so `StartFileTrayDrag(L"")` runs. `SHParseDisplayName(L"")` probably resolves to the Desktop (Not tested), so the drag would carry the Desktop folder. Afterwards `GetFileAttributesW(L"")` fails, so with an effect set, `RemoveFileTrayItemByPath(L"")` (old 21781-21783) removes **every text row**, because all have empty paths. Do not port this path. Rows must be addressed by item id.
9. **Conflict to raise with the user.** The handoff notes say the user "rejected a drawn calendar" for the calendar button. This spec keeps the bar's calendar button opening the native panel, and adds the month grid only as a read-only overlay page, as the brief asks.

---

## 2. Shared page framework

### Where pages draw
The bar is an app bar that reserves its 32 px height (bar 838-842, `kBarH` at bar 74). Growing it would push windows, so the overlay must be a **separate window**:

- `WS_POPUP`, `WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`, never registered as an app bar.
- Created on the same `BarThread` (bar 1622), and painted through the existing `Present()` (bar 600-633).
- Destroyed, or at least its DIB freed, on collapse. `Present()` already allocates and frees a DIB per paint, so a hidden overlay holds no pixels.

The window, hover logic and open/close animation belong to the island spec. This spec only needs a function `DrawPage(Graphics&, RectF, int page)` and the rules below.

### Geometry
Page rect is 380x184 logical px (old 672-673), multiplied by `Scale()` (bar 176-178). `sizeScale`, `settingsOpacity_` and `textScale` are all dropped.

### Fixed look: colour constants
Replace `material_` (old 12722-12739), `calendarAccent` and album-art accent with constants in one block.

| Name | Value | Source |
|---|---|---|
| `kPanelBg` | `#F4101010`, to confirm by eye | Quick Settings panel in handoff |
| `kCard` | white, alpha 22 | `raised` 0.085 (old 12729) |
| `kCardHover` | white, alpha 38 | `raisedStrong` 0.150 (old 12730) |
| `kHairline` | white, alpha 26 | `hairline` 0.100 (old 12731) |
| `kFg` | exists at bar 91, 228 grey | |
| `kMuted` | `kFg` at about 0.6 alpha | old uses 0.55-0.75 per item |
| `kAccent` | `#E4E4E4`, one constant | Monochrome, matching the Quick Settings on-state |
| `kAmber` | RGB 255,176,33 | old 14202 |
| `kRed` | RGB 255,89,82 | old 14199 |

If `kAccent` is light, anything drawn on it must use dark text. The old code assumes light text on the accent (old 13361-13366), so the today marker needs inverting.

Not available on a layered window here: blur. Hence the high-alpha background.

### Fonts (Read: old 12270-12299)
| Old format | Size, weight | GDI+ equivalent |
|---|---|---|
| `textFormat_` | 13.5 semibold | `Segoe UI Semibold` |
| `smallTextFormat_` | 11 regular | `Segoe UI Variable Text` as the bar uses (fallback `Segoe UI`) |
| `boldTextFormat_` | 12 bold | `Segoe UI`, `FontStyleBold` |
| `hugeTextFormat_` | 42 bold | same |
| `calDayLargeFormat_` | 63 bold | same |
| `calGridFormat_` | 14.4 bold | same, or 12-13 if digits crowd |
| `timeDashboardFormat_` | 56 bold | same |
| `dateDashboardFormat_` | 16 semibold | `Segoe UI Semibold` |

Traps (Not tested unless stated):
- GDI+ offers only regular and bold on a family, so semibold needs the static `Segoe UI Semibold`.
- GDI+ has no colour emoji. Use vector weather icons only; do not port the emoji in old 13155-13166.
- GDI+ glyph advances are wider than DirectWrite, and the handoff already reports wide word gaps. Allow 5-10% more width everywhere, and use `StringTrimmingEllipsisCharacter` where the old code used `D2D1_DRAW_TEXT_OPTIONS_CLIP`.
- Cache `Font` objects while the overlay is open. Creating them each paint, as `Paint()` does at bar 649-652, costs little at 1 Hz but adds up during animation.

### Page order and navigation
Order: Idle, Calendar, Weather, Hardware, Timer, Shelf. All are always on, so `ActiveTabCount` (old 1906-1913) and the index helpers (old 1929-1947) collapse to a constant enum. If the media spec adds a page, it goes first, and the enum keeps this safe.

Wheel: port old 22541-22576 nearly verbatim. It must keep:
- Accumulated delta, so touchpads work.
- Reset after 220 ms or a direction change.
- Cooldown of 85 ms (notched) or 200 ms (precise), with at most one notch pending.
- Clamped ends with no wrap (old 1949-1961), so inertia cannot twitch the end pages.

The overlay needs `WM_MOUSEWHEEL`. `WS_EX_NOACTIVATE` windows receive it only when under the cursor, which is the default "scroll inactive windows" setting (Not tested).

Swipe: old 22338-22350, dominant axis over 40 px within 500 ms. Optional. It must not fire on a shelf row press, which is a drag.

Reset to first page on collapse, except during a drag (old 15273-15275).

### Page dots
Port old 15608-15642, about 25 lines, and all of it maps directly to `FillEllipse` and a rounded-rect path.
- Column at `right - 10`, spacing 9, radius 2.5, centred vertically.
- Resting dots at alpha 0.22, fading under the indicator; indicator at 0.90 and stretching with speed.

6 pages use 45 px, which fits. Rows and cards must stay left of `right - 16`; the old layouts already do.

### Page transition
Old 15472-15606 is a spring (stiffness 250, damping 31, travel 34 px, scale to 0.965, fade power 1.4) that repaints live page content every frame. In GDI+ that is too slow for per-pixel-alpha full-rect repaints.

Port it as: render outgoing and incoming pages once to two cached `Bitmap`s, then animate only the blit, position and alpha. Drive it from a 16 ms `SetTimer` that is killed when settled, as `kAnimTimer` does (bar 44, 1523). Skip the scale step if it looks soft. Effort is about half the size of the old code.

Check actual frame time at 150% scaling. 380x184 at 1.5x is 570x276, about 157k pixels, plus `UpdateLayeredWindow`. It should be well within budget, but this is Not tested.

### Spending nothing when hidden
The rule for every page: **no timer, thread, query or fetch exists unless that page is on screen**, except the timer's own one-shot.

Add one function, `OnPageShown(page)` / `OnPageHidden(page)`, fired by the overlay on page change, open and collapse. All of the below hang off it. The bar remains event driven when collapsed: the only timer running is `kClockTimer` (bar 43), as today.

For contrast, the old mod is a render loop that polls battery every 15 s (old 22911-22914), system stats every 1 s **even when hidden** (old 23155-23164, only GPU and net are gated), and weather forever (old 6811-6997).

---

## 3. Idle page

**Old lines:** page 14931-15079; time format 2022-2043; date format 2048-2066; battery snapshot 8473-8515; battery drawing 20420-20485. Privacy badge 14998-15075 and the collapsed strip 15145-15227 with `IdleStripLayout` (old 826-849) belong to the island spec; the collapsed pill already exists in the bar.

**Data:**
- `GetLocalTime`, `GetTimeFormatEx` with `TIME_NOSECONDS`, `GetDateFormatEx` with `dddd, MMMM d`.
- Battery: `g_battery`, `g_charging` in `ReadBattery` (bar 402-410), updated by `WM_POWERBROADCAST` (bar 1514-1518) via registered power settings (bar 1654-1655). Already event driven, no polling. Add `BatteryLifeTime` to `ReadBattery`, read once on page show.

**Refresh:** `kClockTimer` already fires on the minute (`ArmClock`, bar 1521) and `WM_TIMECHANGE` is handled (bar 1587). Call the overlay repaint from those. No seconds, because the fixed look has none, so no extra timer.

**Layout (380x184):**
- Time: 56 bold, centred, box `cy-44..cy+20` (old 14986).
- Date: 16 semibold, muted, box `cy+22..cy+46` (old 14991).
- Battery line (new): centred at about `bottom - 24`, icon height 14 plus "82%" and "3h 12m left" in 11 regular. Reuse the bar's icon table `I_batfull` through `I_batwarn` and `I_bolt` (`build-bar.ps1` line 7), through `DrawIcon` (bar 360-380).
- Remaining-time rule from old 20472-20478: show only if not charging and not `BATTERY_LIFE_UNKNOWN`.
- Low colour rule from old 8504 and 20453-20459: red at 20% or below while discharging, otherwise `kFg`. Charging shows the bolt.
- Dots on the right.

**Interactions:** wheel only. Clicking battery opens `ms-settings:batterysaver` if the island spec wants it; the handoff lists it.

**Port effort:** direct. Drop `clockAccentGlow` (old 14937-14957), `dateFirst` (old 14969-14984), `FormatIslandTime`'s 24-hour override branch and all settings.

**Must keep:**
- The bar's handling of `BatteryLifePercent == 255` as unknown (bar 405) is better than the old mod's mapping to 100 (old 8480). Keep the bar's, and hide the battery line when `g_battery < 0`.
- Battery alerts at exactly 20 and 10 on a falling edge, and on charge state change (old 8493-8499), belong to the alert spec, but the falling-edge logic must not be lost.

---

## 4. Calendar page

**Old lines:** page 13187-13384; helpers `GetDaysInMonth` 13168-13175 and `GetDayOfWeek` 13177-13183 (both pure, copy as is).

**Data:** `GetLocalTime`; `GetDateFormatEx` (`MMMM`, `dddd`); `GetLocaleInfoEx(LOCALE_IFIRSTDAYOFWEEK)`; `GetCalendarInfoEx(CAL_SSHORTESTDAYNAME1+)`. No thread. All locale calls are cached and rebuilt only on date change (old 13211-13252). In the bar, also invalidate on `WM_SETTINGCHANGE`.

**Refresh:** none. Repaint on the minute timer only if the date changed; midnight is covered by `ArmClock`.

**Layout:**
- Hero card: x 22-115, y 16 to bottom-20, radius 12.
- In the hero: month 12 bold in accent (top+6..+22), year 11 muted (top+21..+36), day figure 63 bold (top+34..bottom-26), weekday 12 bold muted (bottom-24..-6).
- Grid: starts x 134, top 16, column 28, header 18.
- Hairline at `gridTop + headerH + 3`.
- Dates start at `gridTop + headerH + 7`, end at bottom-14.
- Row height derived: `(170-41)/rows` gives 25.8 for 5 rows and 21.5 for 6.

**Interactions:** none. Read-only. No month navigation in the old code, and none proposed.

**Port effort:** about 90 lines of GDI+ from 198. All drawing is rects, ellipses and text. Replace `DrawTextW` with `DrawString` and centred `StringFormat`.

**Must keep:**
- Row height computed from rows. A fixed 26 clipped the sixth row (comment at old 13311-13314).
- `rowCount = (startDayIdx + monthDays + 6) / 7`.
- Marker radius from the cell, `min(colW,rowH)/2 - 1.5` (old 13346).
- Month and year on separate lines, because long month names clip (old 13254-13257).
- No `towupper` on month names (old 13216-13218).
- Weekday initials from the same locale, with S M T W T F S fallback (old 13238-13248). `CAL_SSHORTESTDAYNAME1` is Monday, hence `(weekday+6)%7`.
- Locale first day counts 0 = Monday, hence `(v+1)%7` (old 13227-13233).
- Only today uses accent; weekends recede at 0.62 (old 13367-13372); headers uniform at 0.55.

---

## 5. Weather page

**Old lines:** fetch and parse 6807-6999; `HttpGetEx` 6698-6759 (use its logic; not `HttpGet` 6641-6681, which has no timeouts or status); `PercentEncodeUtf8` 6765-6805; code map 11608-11631; page 13679-13869; wind arrows 13391-13401; icons 13409-13534; metric glyphs 13545-13677.

**API (Measured):**

| Request | Result |
|---|---|
| `https://wttr.in/London?format=j1` | 200, 39,310 bytes, 1.3 s |
| `https://wttr.in/London?format=j2` | 200, 2,981 bytes, with `current_condition`, `temp_F`, `FeelsLikeF`, `humidity`, `windspeedMiles`, `winddir16Point`, `weatherCode`, `nearest_area`, `areaName` |
| `https://wttr.in/Qqqzzzxyxy12?format=j1` | **HTTP 500**, plain text "location not found: ..." |

**Use `format=j2`.** It is 13 times smaller. London returned `areaName` "Strand", so show the configured city name when set, as old 6956-6957 does.

**Client:** WinINet, already linked, see finding 5. `InternetOpenW`, `InternetOpenUrlW` with `INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI`, then `HttpQueryInfoW(HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER)`. Set connect, send and receive timeouts to about 10 s. WinINet's DNS stage may ignore timeouts (Not tested), which is one more reason for a worker thread.

**Threading:** one short-lived `CreateThread` per fetch. It parses into a heap struct and `PostMessage`s `WM_APP+n` with the pointer to the bar thread, which adopts it and repaints. Guard against a second fetch with a flag. If the window has gone, free the struct. Exit needs no join: `Wh_ModUninit` ends with `ExitProcess(0)` (old 24015). Close the handle right after creating.

**When to fetch (the rule that spends nothing):**
- Only in `OnPageShown(Weather)`, if the last success is over 15 minutes old (old 6980), or none.
- While the page stays open: a single one-shot `SetTimer` for the next due time. Killed on hide.
- Failure backoff: 60 s, 2 min, 5 min, 15 min (old 6974-6986). Reset on city change (old 6993-6996) and on `WM_REFRESH_NET` when connected. This matters: wttr.in is a community service and rate limiting is among the failures.
- On resume from sleep (old 21962-21971): mark stale only. Do not zero the timestamp as the old code does; that blanks the page via the 1-hour check at old 15118.

**Persistence:** last good reading in `<shelfRoot>\..\weather.txt` as `key=value`, so the page shows data instantly rather than `--` for over a second. Show readings up to 1 hour old normally (old 15118). Past that, show them dimmed with their age, rather than "Locating...".

**Settings:** city text, Fahrenheit on. These are not theme options. The source has no settings block yet, and `WhTool_ModSettingsChanged` is empty (bar 1705). Either add a `==WindhawkModSettings==` block with `Wh_GetStringSetting`, or use constants `kWeatherCity`, `kFahrenheit`. Constants are simpler and match "fixed", but changing the city means a rebuild. Recommend the settings block, since the city is data. Changing it must wake a fetch.

**Parsing:** port old 6854-6929 with the string-find approach; a JSON parser is not worth it here.
- `"temp_F":` has key length 9, which the code hardcodes at old 6895. Fine for C or F, but keep the two equal if another key is added.
- `areaName` is unescaped, so non-ASCII names work via UTF-8 decode; JSON `\u` escapes would show raw. Only used without a city override.

**Layout:**
- Hero: x 26-188. City 11 regular at 0.55 (y 26-44).
- Icon 44 px centred at (x+22, y 82). Temperature 42 bold at x+52, box ±32.
- Description 120-162, wrapping. Size tiers by length: over 58 chars 9.8, over 44 10.5, over 32 11.5, over 22 12.5, else 13.5 (old 13743-13747).
- Divider at x 200, inset 34 from top and bottom, 1 px.
- Metric cards: x 214 to right-22, height 40, gap 7, stack centred, radius 10. Glyph 14 at (left+17, top+14). Label 11 at top+4..+20; value 13.5 at top+19..bottom-3. Text starts at left+30.
- Cards: Wind `"12 mph ↘"`, Feels Like `"45°"`, Humidity `"90%"`.

**Port effort:**
- Parse and encode are portable as is, once `HttpGet` is swapped.
- `WeatherVisualFromCode` and `WindDirToArrow`: copy as is.
- Icons and glyphs: mechanical. D2D ellipse, line and rounded-rect map to GDI+, about 130 lines total. The bolt and droplet geometry (old 13433-13452, 13655-13673) map to `GraphicsPath`.
- Better option: build icon outlines once into cached `GraphicsPath`s, as `PartPath` does (bar 347-354). Only glyphs 0, 1, 2 (wind, thermometer, droplet) and 3-8 (hardware) are needed. Drop 9 (gauge).
- Check the arrows ↓↙←↖↑↗→↘ render in Segoe UI through GDI+ (Not tested).

**Must keep:**
- Commit only if a real reading parsed: `code != 0 || !desc.empty()` (old 6938). Otherwise wttr.in error pages become a confident "0 degrees" (comment at old 6931-6935). With a 500 now visible, require status 200 as well.
- Keep previous data on failure.
- Percent-encode the city as UTF-8, and pass through an already encoded value (old 6770-6779). The check is strict `%XX`.
- Trim trailing spaces in the description: the API really returns `"Clear "` (Measured; old 6920).
- Wind arrows show flow direction, not bearing: wind from NE points SW (comment at old 13386-13390).
- Wind unit must follow the same setting as temperature (old 6926, 13816).
- Settings read under lock (old 6815-6818): moot if the worker receives a copy of city and unit at start. Do that.
- The 3-second start delay (old 6809) is not needed because nothing fetches at startup.

---

## 6. Hardware monitor page

**Old lines:** sampling 8517-8610 and 8817-8898; visibility gate 23143-23164; colours 14197-14205; page 14207-14342; glyphs 13578-13628.

### Sources

| Metric | API | Old lines | Cost |
|---|---|---|---|
| CPU | `GetSystemTimes` delta | 8870-8890 | trivial |
| RAM | `GlobalMemoryStatusEx` | 8837-8843 | trivial |
| Disk | `GetDiskFreeSpaceExW(L"C:\\")` | 8845-8852 | low |
| GPU | PDH `\GPU Engine(*)\Utilization Percentage` | 8524-8561 | see below |
| Net | PDH `\Network Interface(*)\Bytes Sent/sec`, `Received/sec` | 8563-8610 | see below |

### 32-bit: confirmed working (Measured)
Both `C:\Windows\SysWOW64\typeperf.exe` and the WOW64 probe (`ptr=4 wow64=1`) read all three of GPU Engine, Network Interface and Processor Information.

| Probe | First open | Each sample | Items | Private memory |
|---|---|---|---|---|
| GPU `*engtype_3D` | **481 ms** (first PDH use in the process) | 0.6 ms, max 0.7 | 111 | +3.9 MB, **not returned by `PdhCloseQuery`** |
| GPU `*` | 0.5 ms (PDH already loaded) | 0.7 ms, max 0.8 | 351 | under 100 KB |
| Network | 18.6 ms | 1.7 ms, max 1.9 | 1 | +112 KB |
| GPU `*engtype_3D` again | 0.7 ms | 0.6 ms | 111 | about 192 KB |

Process CPU per sample was below `GetProcessTimes` resolution in all cases.

Conclusions:
- **Filter the GPU wildcard to `*engtype_3D`** in the counter path. It gives a third of the instances, and the old code filters by string afterwards anyway (old 8553). This also works around having to walk all engines.
- **Load `pdh.dll` by `LoadLibrary` the first time the page is shown, on a short worker thread**, then post back. Sessions that never open this page pay nothing. Close nothing on hide except stopping the timer: closing returns almost no memory, and reopening is trivial, but keeping the query means the next open has an immediate delta.
- Later samples are about 2.5 ms total, so they can run on the bar thread at 1 Hz. If real hardware shows spikes, move them to the same worker.

**Linking:** with `LoadLibrary`, no new link flag. `pdh.h` and `pdhmsg.h` exist under the compiler include dir and `libpdh.a` exists for i686 (Measured). If static linking is preferred, add `-lpdh` in **both** places: `@compilerOptions` (bar line 8) and `build-bar.ps1` line 75. They are maintained separately.

### Refresh and spending nothing
- `OnPageShown(Hardware)`: start a 1000 ms `SetTimer`. Sample immediately for the cheap metrics. Show GPU and net as `--` until two samples exist.
- `OnPageHidden`: kill the timer. Nothing else runs.
- Disk: once per page show, then every 30 s. It does not need 1 Hz.
- Repaint only if a rendered string or bar width changed. The old code does the equivalent to avoid unnecessary paints (old 23140-23142).

### Layout
- Title 12 bold at x 24, y 16-32. Fixed look allows dropping this title if the grid needs room.
- 2x3 grid: pad 24, column gap 9, column width `(380-48-9)/2 = 161.5`, row height 38, gap 6, top 38. Bottom is 164 of 184.
- Card radius 9. Reading order: CPU, NET UP / RAM, NET DOWN / GPU, DISK.
- Inside each card: glyph 15 at (left+16, top+15); label 11 at top+2..+15, text at left+30, right-9; value 13.5 at top+15..+30, or down to bottom-4 when there is no bar; bar track bottom-6..-3.5, radius 1.25, fill drawn only over 0.5 px.
- Value text: `%d%%`, `%.1f / %.1f GB`, `%.1f Mbps`. Show `--` for unavailable GPU.
- Network cards have **no bar**, deliberately: throughput has no natural maximum (old 14248-14249).

### Load colours (Read: old 14197-14205)
Fraction 0.90 or over is red; 0.75 or over is amber; else accent. Applied to both glyph and bar. Port exactly. In the old code `DrawMetricGlyph` tint uses alpha 0.85 and fill 0.95. Non-bar cards always use accent.

### Bugs to fix rather than port
- **GPU failure is shown as 0%.** `GetGpuUsage` returns 0 on failure (old 8538, 8560), while the page handles `< 0` as `--` (old 14227). Return -1.
- **First reading is junk.** Rate counters need two samples spaced apart, but `InitGpuQuery` collects and `GetGpuUsage` collects again immediately (old 8531, 8540). Same for net (old 8572, 8583). Skip the first read.
- **`CStatus` is never checked** (old 8552-8556, 8594-8599). Skip items without `PDH_CSTATUS_VALID_DATA` or `PDH_CSTATUS_NEW_DATA`. 0 bad items in the probe, but instances come and go with processes.
- **`PdhCloseQuery` never called** anywhere in the old source. Close on exit.
- **GPU sum across adapters.** The probe saw two LUIDs (`0xE626`, `0xF190`). Summing all and clamping (old 8551-8557) can overstate on a multi-adapter machine, and this laptop has one physical iGPU. Better: sum per `luid_..._eng_N` key, then take the max, which is how Task Manager appears to behave (Not tested against Task Manager). Takes 15 lines.
- **Loopback filter only** (old 8596). The probe saw a single adapter, Realtek, but with Docker running, `vEthernet` instances may appear and double count traffic (Not tested). Exclude names containing `vEthernet` and `isatap`/`Teredo`.
- **Disk label.** Only `C:` is sampled, labelled DISK, yet the user's data lives on D:. Keep C: as the primary; decide whether a second value is wanted.
- **CPU value differs from Task Manager** on this hybrid chip. `GetSystemTimes` is time-based, while Task Manager uses Processor Utility. Reasonable to keep, but expect it to differ.

### Must keep
- `kernel` time includes idle, so total is `kernel + user` (old 8881). 
- Skip the first delta (`kernelPrev != 0`, old 8883). And **reset the stored values on page show**, new here, otherwise the first number is an average since the page was last open.
- `memoryLoad` and used/total are derived differently; the fraction uses used/total (old 14237-14239), with a guard for total near zero.
- The label sits above the value, not on one line (comment at old 14291-14295).

---

## 7. Focus timer page

**Old lines:** state changes 10800-10840; expiry 22929-22938; menu 11236-11244, 11525-11539; activity drawing 19417-19505; once-per-second repaint 23495-23509. Activity gating 20909-20912.

**State:** `active, running, isBreak, totalSeconds, endsAt, remainingAtPause, justFinished, finishedExpiresAt`. Plain globals on the bar thread; the old mutex is not needed.

**Time source:** `GetTickCount64`, which counts through sleep. The old `steady_clock` (old 1759-1763) has the same intent. Compute remaining from `endsAt` every time, and never count ticks.

**Threading and refresh:** bar thread only.
- Running and visible (page or pill): a timer aligned to the next displayed second, one paint each. Key repaint to the displayed value as old 23495-23509 does, to avoid drift and double paints.
- Running and not visible: **one one-shot `SetTimer` for `endsAt`**, no 1 Hz.
- Paused or stopped: no timer.
- On `WM_POWERBROADCAST` resume (bar 1514): re-evaluate. An overdue timer finishes at once.

**Layout (new, since the old mod has no page):**
- Centre-left: ring, radius 44, stroke 5. Track `kCardHover`. Progress sweeps clockwise from 12 o'clock (old 19450-19451). Use `DrawArc`; there is no need for the polyline in old 19452-19462.
- Inside the ring: `m:ss`, 13.5 semibold (old 19496-19498, using `ceil`). Over an hour: 50 min is the longest preset, so `%d:%02d` is fine.
- Right column: label "Focus Timer", "Break Timer", "Focus Complete", "Break Complete" (old 19488-19490).
- Idle state: three chips, 25 min, 50 min, 5 min break (values from old 11526-11532).
- Active: two buttons, Pause/Resume and Stop.
- Ring colours from old 19465-19470: focus orange (255,148,0), break green (48,212,97), finished red (255,59,46). These are status colours, so they survive the fixed look. Paused uses `kMuted` or the old two-bar glyph (old 19474-19484).

**Collapsed pill:** while `active` or `justFinished`, the island spec should show the compact form, 260x54 (old 20830-20833), per old 19417-19505. Its guard, `height < 24 || width < 140` (old 19418), protects against drawing during morph.

**Interactions:** click chips and buttons, using `g_hits` (bar 135) or a page-local equivalent. Keep a menu out: `TrackPopupMenu` on a `WS_EX_NOACTIVATE` window has foreground issues, as the old code's workaround at 11215-11217 shows.

**Persistence:** none in the old code. Timers die with the process. Keep it that way unless asked.

**Must keep:**
- Pause stores remaining, clamped to 0 or more, and resume recomputes `endsAt` (old 10821-10826).
- `totalSeconds` guard against divide by zero (old 19424).
- Finished state lasts 6 s (old 22933), then clears.
- No sound in the old code. Decide whether a finish needs one: `MessageBeep` adds no dependency. If the pill is collapsed and the user is away, 6 s of red is easy to miss.

---

## 8. Shelf (the file tray)

**Old lines:** layout 783-801; hit tests 2280-2350; state 1456-1464, 1706-1709; store 21128-21204; save 21209-21256; load 21262-21373; add 21380-21437; remove 21439-21450; extract 21452-21476; text 21478-21542; clipboard 21544-21602; drop effect 21604-21609; target 21613-21698; source 21700-21727; data object 21731-21747; drag 21750-21784; create and destroy 21798-21809, 21886-21891; press 22095-22111; hover 22225-22266; click 22408-22469; drawing 14024-14190, buttons 13872-14023.

Do not port `WM_DROPFILES` (old 21812-21884). It is dead code: nothing calls `DragAcceptFiles`, since the OLE target replaced it.

### Data model

```
struct ShelfItem {
    uint32_t id;          // Stable. Never address rows by path (finding 8).
    enum { File, Text, Link, Image, Stored } kind;
    wstring title;        // Row label
    wstring path;         // File: original. Image/Stored: blob. Text: blob if long.
    wstring text;         // Text: body if short. Link: URL.
    uint64_t size;
    int64_t addedAt;
    bool isDir, missing;
    Bitmap* thumb;        // Lazy; 32 px icon or 64 px thumbnail; visible rows only.
};
```

`File` is a reference and the source file is never touched (comment at old 1454-1455). `Stored` is a materialised copy, for virtual files and temp-path files.

### Layout
Port constants from old 783-801: pad 26, list top 46, bottom inset 16, row 30, gap 6, remove button 18, clear button 24 at y 16, paste gap 4.

- Header: "Shelf" 12 bold at y 18-38. Count `"3 items"` 11 muted, right-aligned before the paste button (old 14050-14062).
- Paste and bin buttons (drawing at old 13872-13987, about 115 lines; simplify to two icons from the existing SVG table by adding SVGs to `icons\` and the `$names` list in `build-bar.ps1` line 7).
- Rows, newest first: radius 9, hover fill, icon 18 at left+8, name at left+32 in 11 regular with ellipsis, size column 66 wide right-aligned, remove button at right-7.
- Size column by kind: `%.1f GB/MB`, `%.0f KB`, `B` (old 14134-14141), a dash for folders (old 14133), `N ch` for text (old 14131), domain for links, `WxH` for images.
- Empty state: dashed rounded rect, "Drop anything here" (old 14071-14097). GDI+: `Pen::SetDashStyle`.
- Drag hover: outline the page in `kAccent` while a drop is over the shelf.

**Scrolling (new, finding 2).** Height 184 fits 3 rows. Options, in order of preference:
1. On this page, the wheel scrolls the list, and at either end the next notch changes page.
2. Grow the overlay on this page: 6 rows gives height about 294. The window is cheap to resize, but the island spec's morph must allow per-page height.

Recommend both: taller only if there are more than 3 items, with wheel scrolling beyond that. Row mapping must include the scroll offset; the old mapping `count - 1 - row` (old 22109, 22431) must become `count - 1 - (row + scroll)`.

**Keep hit testing and drawing on the same constants**, as old 781-782 does. Also keep the slop of 3 px on buttons (old 2313, 2342), and rejection of gaps between rows (old 2295-2299).

### Interactions

| Action | Behaviour |
|---|---|
| Click file, stored, image | Open with default app: `ShellExecuteExW` with `SEE_MASK_FLAG_NO_UI` (old 22459-22463) |
| Click link | Open, **only for `http`, `https`, `mailto`**, otherwise copy |
| Click text | Copy to clipboard (old 22454-22456), new: brief "Copied" flash, since old gives no feedback |
| Press, move past `SM_CXDRAG`/`SM_CYDRAG` | Drag out (old 22256-22265) |
| Remove button | Remove that row. Press on it must not start a drag (old 22104) |
| Bin | Clear. Delete blobs. New: needs a second click within 3 s if anything stored would be deleted, since unlike the old tray this destroys content |
| Paste button | Same pipeline as drop, via `OleGetClipboard` |
| Click empty area | Consumed, no fall-through (old 22466-22468) |

Open-file note: a single click runs `.exe` and `.bat` files, as the old code does. Acceptable because the user put them there, but worth knowing.

Foreground: an app launched from a no-activate window may open behind. Call `AllowSetForegroundWindow(ASFW_ANY)` first (Not tested).

**Text classified as a link must never reach `ShellExecute` unchecked.** A dropped string like `calc.exe` or `C:\x.bat` must not run on a click. That is why the scheme check above is required.

---

## 9. Dropshelf: drops on all three bubbles

### Registering
Bar thread:
1. Replace `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)` at bar 1624 with `OleInitialize(nullptr)`, and `CoUninitialize` at bar 1694 with `OleUninitialize`. `RegisterDragDrop` needs OLE, not only COM. (Measured: `OleInitialize` returns success from an STA thread either way, so order is not fragile.)
2. `RegisterDragDrop(g_hwnd, target)` after window creation (bar 1638-1643), and on the overlay when it is created. One shared target object serves both, with no locking needed because every call arrives on the bar thread.
3. `RevokeDragDrop` on `WM_DESTROY` (bar 1612; old 21887), **before the window goes away**, and on the overlay if it is destroyed on collapse. If the overlay is destroyed in mid-drag, OLE's held reference to a dead window may cause errors, so never destroy during a drag.

A registered window is enough. `DragAcceptFiles` is not needed, and `WS_EX_NOACTIVATE`, `WS_EX_TOPMOST` and `WS_EX_LAYERED` do not block a target. The old mod does this on the same style of window (old 22667-22668, 21805).

### Layered-window hit testing: the main trap
OLE finds the target from the window under the cursor, and a layered window with `ULW_ALPHA` is **not hit** where pixel alpha is 0. (Documented for mouse input; Not tested for this drag path.) Consequences:

- Today `g.Clear(kBarBg)` (bar 645) fills all of the bar at alpha 230, so all of it is a target.
- **If the three bubbles are one window with transparent gaps, a drag over a gap falls to the app below.** Before the next `DragEnter`, the target gets `DragLeave`, and the app below may react. Fix by always painting gaps with alpha 1 (`Color(1,0,0,0)`), invisible by eye. Gaps then also catch clicks. Painting alpha 1 only during a drag does not work, because the bar cannot know a drag has started until it reaches one of the bar's own pixels. Accept the trade: the bar is an app bar that reserves that strip, so no normal window sits beneath the gaps.
- **If the bubbles are three windows**, call `RegisterDragDrop` on each with the same target. A gap then simply is no target.
- The overlay's rounded corners are alpha 0, which is fine.
- The old window relied on `WS_EX_TRANSPARENT` toggling, staying solid only while the left button was down over the pill (old 23325, 23333), and on `WM_NCHITTEST` (old 21984-22012). **Do not add `WS_EX_TRANSPARENT` to the bar**, or `HTTRANSPARENT` regions in a way that would hide it from drags.

### Integrity level (Measured now, must stay true)
OLE drag does not cross integrity levels upward: a drag from a medium process cannot reach an elevated target, and it fails silently. Today `explorer` and `windhawk -tool-mod local@top-bar` are both at medium (0x2000), and so is the launcher `-tray-only`.

Handoff notes say install scripts run elevated and the bar must be relaunched unelevated. **If the bar is ever left running from an elevated install, every drop fails with a "no" cursor and no log.** At startup, check `TokenIntegrityLevel` and `Wh_Log` a clear warning if above medium. Drags from elevated apps to the bar will always be refused; that is Windows behaviour. Autostart after reboot is noted as unverified in the handoff: verify its level too.

### Target behaviour

Implements `IDropTarget`, plus `IDropTargetHelper` (`CLSID_DragDropHelper`, Measured to create in 32-bit) so the source's drag image stays visible over the bar. The old code does not, which is why the ghost vanishes over it. Forward all four calls to the helper, with the HWND in `DragEnter`.

- **`DragEnter`**: use `QueryGetData` only. **Never call `GetData` here**, because for a browser or virtual file it can start a download. Accept if any format in the table below is present, and the drag is not ours (`g_trayInternalDrag`, old 21634).
- **Effect** (port `PickTrayDropEffect`, old 21604-21609): COPY, else LINK, else NONE. Never ask to move, so the source keeps its data. Sources offering only MOVE get refused, as before.
- **Opening the overlay**: on any bubble, after about 120 ms of hover to avoid flashes on a passing drag, switch to the Shelf page and open the overlay. Hold it open while the drag is over either window (old 23027-23031, 21656-21657).
- **Leave and re-enter**: moving from bar to overlay gives `DragLeave` then `DragEnter` on the new window. Collapse only after a **300 ms grace timer** with no re-enter, or the overlay closes under the cursor while crossing the seam. Mouse messages do not arrive during another process's drag, so `TME_LEAVE`-based hover cannot drive this.
- **Hit anywhere counts**: a drop on any bubble or on any part of the overlay stores the item. No per-bubble behaviour. Dropping on a tray icon does not reach the app.
- **Drop**: all reads complete inside `Drop`, because the data object is valid only then. Keep it short: the source app is waiting, and COM calls run on the bar thread. After `Drop`, stay on the Shelf page (old 21679-21685), and collapse by the normal hover rule.
- **Re-entrancy**: while `GetData` is out to another process, the bar's message pump can dispatch `WndProc`. `Paint()` and shelf state must tolerate that. Build the new items in locals and commit after all reads finish.
- `DROPDESCRIPTION` (optional): "Keep on shelf" via `CFSTR_DROPDESCRIPTION`. Small and pleasant, later.

### Formats, in order of preference

Decide in this order. The first match wins, and everything is decided from what is offered, not guessed from the source app.

| # | Format | Result | Notes |
|---|---|---|---|
| 1 | `CF_HDROP` | `File` rows, one per path | Port old 21452-21476. Paths under `GetTempPath` become `Stored` copies, since apps delete those soon. Folders allowed. |
| 2 | `CFSTR_FILEDESCRIPTORW` + `CFSTR_FILECONTENTS`, when the name is **not** `*.url` | `Stored` rows, or `Image` if the extension is an image | Outlook attachments, zip contents, browser image drags. Ask with `TYMED_HGLOBAL \| TYMED_ISTREAM \| TYMED_ISTORAGE` and `lindex = i`. ISTORAGE (Outlook `.msg`) needs `StgCreateStorageEx` and `CopyTo`, so treat as phase 2. |
| 3 | `CFSTR_INETURLW`, then `CFSTR_INETURLA` | `Link` | Title from the descriptor name without `.url`, else the URL. Browsers offer a `.url` descriptor with this, hence the exclusion above. |
| 4 | `CF_UNICODETEXT`, then `CF_TEXT` | `Text`, or `Link` if it is a single line matching `https?://` or `mailto:` with no spaces | Comes before images because Word and Excel offer both text and a picture of it. |
| 5 | `PNG` (registered), then `CF_DIBV5`, then `CF_DIB` | `Image`, saved as PNG | `PNG` and `CF_DIBV5` keep alpha. Convert with GDI+ using `image/png`. |

Not accepted: `CFSTR_SHELLIDLIST` alone (phone over MTP, Control Panel and This PC items), unless a descriptor is also offered. Refuse with no effect.

Notes:
- Query with `tymed` covering HGLOBAL and ISTREAM for every format, not only HGLOBAL as old 21635 does, since some sources answer only with a stream. A small helper reads either into a byte buffer with a size cap.
- **Text**: strip trailing whitespace, one-line preview of 60 chars (old 21479-21517). Old truncates silently at 100,000 chars (old 21486-21489). Instead, write text over 4 KB to a blob and keep the row, so nothing is lost and the index stays small.
- Add optional `HTML Format` retention to Text rows later, so rich text survives. Phase 2.
- **Dedup** (old 21416-21425, 21533-21537): re-dropping promotes the existing row. Path compare is case-insensitive; text and URL exact; images by size and a hash.
- **Limits**: 2 GB address space. Reading a 4K screenshot DIB is 33 MB, fine. Stream to disk in 64 KB chunks rather than buffering. Cap a single stored item at 512 MB with a clear refusal. Skip thumbnails of images over about 64 MP.

### Dragging out

Same `SHCreateDataObject` route for every kind but one. **Measured in 32-bit:** `SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_IDataObject, ...)` gives an empty container that accepts arbitrary `SetData`, round-trips `CF_UNICODETEXT`, `UniformResourceLocatorW`, `PNG`, `FileGroupDescriptorW` and `FileContents`, and supports enumeration. So **no custom `IDataObject` class is needed for single rows**.

**Limit found by the probe:** it ignores `lindex`. Setting `FileContents` at index 0 and then at 1 overwrites: both reads returned the second, and only one is enumerated. So one virtual file per object only. A multi-row drag would need a custom `IDataObject`, about 120 lines. Single-row drag is the old behaviour, so this is fine for now.

| Row kind | Formats offered | Explorer gets | Editor or browser gets |
|---|---|---|---|
| `File` | Shell object via `CreateFileDataObject` (old 21731-21747), as is | the file | the file |
| `Stored` | Same, on the blob path | a copy | the file |
| `Image` | `CreateFileDataObject` on the PNG blob, then `SetData` of `PNG` and `CF_DIB` | PNG copy | picture or upload |
| `Link` | `UniformResourceLocatorW`, `CF_UNICODETEXT`, descriptor + contents of a `.url` | a shortcut | the URL |
| `Text` | `CF_UNICODETEXT`, descriptor + contents of `<preview>.txt` | a `.txt` file | the text |

Adding `SetData` to a shell object (the `Image` row) is Not tested. Fallback: build the container and set `CF_HDROP` by hand.

Do not use `CF_HDROP` for text rows, or editors open it as a file rather than inserting text.

**Source:** `TrayDropSource` (old 21700-21727) as is; ESC cancels, button up drops. Start through `SHDoDragDrop` with `DoDragDrop` as fallback (old 21764-21773). The export exists on this machine (Measured). Allowed effects: `COPY | LINK` (old 21759), never MOVE, so the blob stays.

**Drag image:** `SHDoDragDrop` handles shell objects. For non-file rows, use `IDragSourceHelper::InitializeFromBitmap` with a rendered row, on the same object; the helper creates in 32-bit (Measured), but the image itself is Not tested.

**Must keep:**
- `g_trayInternalDrag` around the drag, so dropping back on the bar does nothing (old 21768-21774, 21634).
- Clear hover state afterwards, since no `WM_MOUSELEAVE` will arrive (old 21776-21778).
- Blocks modally, so keep the overlay open through it, with an `g_shelfDragOut` flag on the hover logic, and re-evaluate hover by cursor position at the end.
- Stale-entry check after a drag if the file has gone (old 21780-21783), addressed by `id`.
- `s_trayPressActive` cleared on leave (old 22280) and on any release (old 22257-22258).
- Copy `dragPath` before starting (old 22262), because the list can change during the modal loop. Same for the row.

### 32-bit traps with shell APIs (Not tested unless stated)
- **WOW64 redirection:** paths under `C:\Windows\System32` resolve to `SysWOW64` for a 32-bit process, so attributes, size and icon can be wrong, or the file can look missing. For stat calls only, rewrite to `%windir%\Sysnative\`. Do not hand that path to other apps; for such a row, hand-build `CF_HDROP` with the original path. Rare, but a dropped system file should not show as missing.
- **Icons:** `SHGetFileInfo` in a 32-bit process cannot load 64-bit-only icon handlers, so some types show generic icons. Keep `SHGFI_LARGEICON` (old 21404) and convert with the bar's `BitmapFromIcon` (bar 417). Do not expect shell thumbnails either: decode images directly with GDI+. WebP or HEIC may fail; fall back to the file icon.
- **OLE marshalling between 32-bit bar and 64-bit Explorer:** works via standard marshalling and the old 32-bit tool mod already does this.

### Persistence

**Location (decision for the user):** a folder on D:, proposed `D:\windhawk-data\top-bar\shelf\`, with fallback to `%LOCALAPPDATA%\top-bar\shelf\` if D: is unavailable. Reasons: finding 7, and the user's storage rule. Not `Wh_GetModStoragePath`, which is used by old 21162. Check the ACL on that folder is not wider than the user.

**Format:** versioned UTF-8 lines, tab-separated with the old escape rules (old 21171-21202), written by `WriteFileAtomic` (old 7452-7469).

```
TBSHELF 2
F <id> <addedAt> <path>
T <id> <addedAt> <title> <text or @blobname>
U <id> <addedAt> <title> <url>
I <id> <addedAt> <title> <blobname>
S <id> <addedAt> <title> <blobname> <originalname>
```

Blobs live in `shelf\blobs\<id>.<ext>`. Reject any blob name holding a path separator on load, so an edited index cannot point delete at other files.

Improvement on `WriteFileAtomic`: add `FlushFileBuffers` and `MOVEFILE_WRITE_THROUGH`, since this is now the only copy of user content.

**Load:**
- Read the index on the first shelf show, or at idle after startup if a count badge is wanted. Not at window creation.
- Keep the old rule: **missing files stay** and show dim with a type icon (comment at old 21258-21261, 21343-21347). `SHGFI_USEFILEATTRIBUTES` avoids touching the disk.
- `GetFileAttributesExW` on a dead network or unplugged path can stall for seconds (old comment at 21343-21344 is about this). Do these on a worker, or defer until the page shows, and skip `\\` paths until needed.
- Tolerate unknown lines by skipping them (old 21314-21316, 21355), and bad headers by starting empty (old 21300-21304). **But rename a bad file to `.bad` rather than overwriting it on next save**, since it may hold the only list of content.
- Old 16 MB read cap (old 21288-21291) treats a larger file as unreadable. Long text now lives in blobs, so the index stays small. Keep the cap.
- On load, delete orphan blobs not in the index, only after a successful parse.
- Save after every change (old 21436, 22448), and coalesced is not needed at this rate. Delete the index when empty (old 21236-21238).

**Migration:** none needed. No `filetray` folder exists under the old mod's storage on this machine (Measured).

**Capacity:** old default 10, max 25, oldest dropped silently (old 3055, 21427-21429, 21539-21541). For a shelf that stores content, silent eviction loses data. Proposal: 100 rows and a 2 GB blob budget, then **refuse** the drop with a visible flash rather than evict. No setting needed. Numbers are for the user to confirm; note the handoff listed "25 items" as a gap.

**Not building (from the handoff's gap list):** multiple shelves, naming, shake gesture, hotkey. Mentioned so the omission is a decision.

### Reuse summary

| Part | Old lines | Verdict |
|---|---|---|
| Escape, unescape, atomic write | 21171-21202, 7452-7469 | Copy, small fix |
| Drop effect, source class, data object, drag | 21604-21609, 21700-21784 | Copy; swap `ComPtr` for raw pointers or include `<wrl.h>`; the bar does not use it |
| Target class | 21613-21698 | Rewrite the body; shape stays |
| Text and paste | 21478-21602 | Preview logic copies. Paste becomes `OleGetClipboard` + shared ingest, replacing the two-branch code |
| Hit tests | 2280-2350 | Copy, add scroll offset. Replace `MediaContentPoint` |
| Drawing | 14024-14190, 13872-14023 | Rewrite in GDI+ |
| `PixelsRef`, `IconToPixels` | 4513 and nearby | Replace with `Bitmap*` and `BitmapFromIcon` (bar 417) |
| Mutexes | `g_stateMutex`, `g_ioMutex` | Remove: single thread |
| `WM_DROPFILES` | 21812-21884 | Drop |

---

## 10. Build and link changes

| Need | Change | Where |
|---|---|---|
| OLE, shell objects | `-lole32`, `-lshell32` already present | Nothing |
| `CLSID_DragDropHelper`, `IID_*` | `-luuid` is in `build-bar.ps1` line 75 but **missing from `@compilerOptions` at bar line 8**. A rebuild through the Windhawk UI would fail to link | Add to line 8 |
| GDI+ encoder, PNG | `-lgdiplus` present | Nothing |
| HTTP | `-lwininet` present | Nothing |
| PDH | `LoadLibrary`; else `-lpdh` in both places | Optional |
| Headers | `<ole2.h>`, `<shlobj.h>`, `<shobjidl.h>` compile together for i686 with wininet and pdh (Measured) | Add to top |

Do not include `<winhttp.h>` (finding 5) or `<winsock2.h>` (comment at bar 26-27).

Note the bar builds both 32-bit and 64-bit (`build-bar.ps1` line 66), and only 32-bit runs. All code must compile for both; the probe covered i686 only.

---

## 11. Must-test by hand

These need real drags, so the user has to be present. The handoff asks to request consent before screen tests.

1. Drop from Explorer on each bubble, on a gap, and on the overlay. Cross the seam between bar and overlay without collapse.
2. Drag from: Explorer (file, folder, several), Chrome and Firefox (link, image, selected text), Word or Excel selection, Outlook attachment if available, a zip open in Explorer (virtual file), Snipping Tool or similar temp file.
3. Drag out to: Explorer, Notepad, a browser text field and page, Word, a chat app upload.
4. Refuse cases: drag from an elevated app, phone via MTP.
5. Reboot: shelf restored, bar integrity level medium.
6. Bar stays responsive during a large virtual-file drop.
7. Memory after hardware page opened and closed; expected about +4 MB once, per the probe.

---

## 12. Suggested order and effort

Assumes the overlay window, hover and animation exist from the island spec. Those are not counted here, and everything below depends on them.

| # | Step | Estimate | Risk |
|---|---|---|---|
| 1 | `DrawPage`, colour block, fonts, dots, wheel, `OnPageShown`/`OnPageHidden` | 0.5 day | Low |
| 2 | Idle page | 0.25 day | Low |
| 3 | Calendar page | 0.25 day | Low |
| 4 | Hardware page, including PDH loader and fixes | 0.75 day | Medium: worker, first-open blocking |
| 5 | Weather page, WinINet worker, icons | 1 day | Low to medium |
| 6 | Timer page and pill state | 0.5 day | Low |
| 7 | Page transition with cached bitmaps | 0.5 day | Medium: may need tuning for smoothness |
| 8 | Shelf: files only, drawing, scroll, persistence, drag out, target (old parity) | 1.5 days | Medium |
| 9 | Dropshelf: formats, blobs, text/link/image drag out, helper, bubbles | 2-3 days | **High** |
| 10 | Testing with the user across apps, and fixes | 1-2 days | **High** |

**Total: about 8-10 working days of focused implementation, and roughly 2,000-2,500 lines of new or ported code.** That is against about 2,400 lines covering these features in the old source, because GDI+ drawing is similar in size to the D2D, and the new shelf work offsets what gets dropped.

Honest uncertainties:
- Step 9 and 10 could run longer. Each source app does drags differently, and Word, Outlook and browsers are where surprises come from. Only hand testing finds them.
- GDI+ text may need repeated tuning to look right next to the old DirectWrite design. The handoff already reports wide word gaps. This is not a line item above and could take days of back-and-forth by eye.
- Measured numbers come from one short run on an idle machine.

Stopping points that still deliver value: after step 6 the read-only dashboards are complete; after step 8 the old tray is matched; step 9 is the part that goes beyond the old mod.
