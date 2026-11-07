# Island feature parity: plan

Details live in `shell.md`, `media.md`, `alerts.md` and `dashboards.md`. Estimates come from those specs and nothing below has been built or timed.

## Decisions

From the user:
- Rebuild in the bar, one process. Three bubbles: left, centre island, right.
- One fixed dark look, Start's colours. No theme, shape, animation-style or blur options. Album tint stays.
- Notch feel when collapsed. On expansion, bell runs to the left and calendar to the right, animated.
- Hover expands over apps; apps don't resize.
- Volume and brightness: native popups placed over the centre bubble.
- Notifications: island only, ALL toasts hidden, including alarms and calls. A fail-safe restores toasts if the bar dies or can't read notifications.
- Collapsed waveform: slow animation, 10-15 fps, partial repaint, only while music plays. Expanded: full speed.
- Progress dropped, since the old one tracks nothing. Game overlay, lyrics and Quick Lookup dropped.
- Local git in `src`, nothing pushed.

Defaults, unless the user overrules them:
- Fill: opaque `#101010` centre, `#DC101010` side bubbles.
- Hover waits about 120 ms before expanding.
- An alert while expanded queues instead of snapping shut.
- Hide hotkey releases the reserved strip.
- Follow-mouse moves only the island.
- Long titles cut off slightly earlier, to make room for the bell.
- Waveform driver: peak meter first (no capture stream); media panel opens on click/hover as the shell defines, not on track resume; track text uses ellipsis first; only the play/pause morph animates.
- Toasts return in fullscreen, while the bar is hidden.
- No Num Lock alert. Privacy dot tap does nothing. Device alerts cover drives and COM ports only, as the old one does.
- Tapping a notification also removes it from the bell list.
- Clipboard hides content that apps mark private, which password managers do. The old mod displayed it.
- Calendar: bell-side button keeps opening the native calendar; the month grid is a read-only dashboard page.

## Phases

| Phase | Content | Depends on | Test rounds with the user |
|---|---|---|---|
| P0 | Probes: GDI+ vs Direct2D timing and memory; identify the volume popup's window; log-only popup hook | none | 1 (a volume key press) |
| P1 | `git init`, split `topbar-core.cpp` into modules, no behaviour change | P0 | 0 |
| P2 | Rendering foundation: surfaces, frame clock, springs, text, settings, strings | P1 | 1 |
| P3 | Bar redrawn in Direct2D, three bubbles, GDI+ removed | P2 | 1-2 |
| P4 | Island window, state machine, alert queue, notch, hover, scrolling with stub modules. Interfaces frozen | P3 | 2-3 |
| P5 | Parallel: bell and calendar run; covered state for the popup; menu, hotkey, pin; monitor, DPI, fullscreen; clock, font, languages; settings page | P4 | 5-7 |
| P6a | Media: player, sources, auto-expand, tint, waveform | P4 | see media.md |
| P6b | Alerts: Caps, battery, Bluetooth, DND, notifications and toast hiding, clipboard, privacy dots, devices, tap actions; native popup | P4 | see alerts.md |
| P6c | Dashboards: idle, calendar, hardware, weather, timer, then shelf, then Dropshelf | P4 | see dashboards.md |
| P7 | Panels: cold-open hiding for first bell, plus a safer model | P1, any time | 2 |
| P8 | Integrate, measure memory and CPU, overnight soak, cleanup | all | 1-2 |

Effort: shell 15-22 engineer-days, media 5-8 sessions, alerts 8-11 sessions, dashboards 8-10 days. P0-P4 is one agent at a time, since each changes the foundation. Parallel agents pay off from P5 on.

## Rules for agents

- Each agent owns its module folder and works on its own git branch.
- Shared core changes go through one owner.
- Agents may compile freely. They never install, send input, open UI, restart processes or change the registry.
- One integrator (the main session) installs, and asks the user first, because the user types during tests.
- Avoid `Translation=` in styler rules, and keep hook threads alive: see `windhawk-mod-traps` in memory.

## Risks

1. Popup over the bubble is unproven. Exact placement needs a hook in Explorer, new ground for Windhawk mods. Fallback: top centre just below the bubble. Set `PositionIndex=3` regardless, so a failed hook lands there.
2. Hiding all toasts loses Reply, Snooze and Answer buttons, and alarms or calls appear only as island alerts.
3. Turning DND off from a tap has no documented API and needs a 64-bit helper. If it fails, the tap opens settings.
4. Moving rendering to Direct2D could raise memory; P0 measures it. Software target is the fallback.
5. Drag and drop needs hand testing across apps. Word, Outlook and browsers are likely to surprise.
6. Touching the panel host has caused crashes before. Keep the existing path until the new one is proven.
7. User test rounds, more than coding, set the pace.

## Stopping points that still deliver value

- After P3: three bubbles.
- After P4: notch and working island shell.
- After P6a: media.
- After dashboards step 6: read-only dashboards.
- After dashboards step 8: old file tray matched.
