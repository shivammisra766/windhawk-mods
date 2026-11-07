# Module API (frozen at the end of P4)

Source: `topbar/bar/module.inc` (interface, stubs) and `topbar/bar/island.inc` (host). Everything runs on the bar's UI thread; a module's own workers hand results over with `Services::Post`.

- **Register** a module by adding an instance to `g_modules` in `module.inc`. `Start(Services&)` runs once the island exists.
- **Views**: `Pill` (collapsed content: the module with the highest `PillPriority()` above 0, else the clock), `Alert` (shown while the module has a queued alert), `Dashboard` (a page of the expanded island; pages sort by `DashboardOrder()`, mouse wheel switches).
- **Size**: `Measure(view)` returns DIP and is called when state changes, not per frame. The body springs to it (width capped at 712, height at 184). `Draw` gets `DrawCtx.rect` already at that size in pixels, so layout never reflows during the animation; the island clips and fades it. Set `ctx.brush` colour before every use and multiply by `ctx.alpha`.
- **Alerts**: `ShowAlert(module, priority, ms)` (ms 0: until `ClearAlert`). Highest priority first, then oldest. While expanded they queue and show after collapse. Tapping the island in alert state calls `OnClickAlert()`.
- **Animation**: return true from `Animating(view)` to keep the frame clock running; otherwise nothing ticks. `Services::Invalidate()` redraws once, `WakeAnimation()` restarts the clock.
- **Timers**: `Services::SetTimer(module, ms, fn)` (one per module, repeats), `KillTimer`. Use it only while visible (`OnVisibility`).
- **Pointer**: `OnPointer` receives pixels relative to `rect` for Move/Down/Up/Leave of the current view.
- **Panels**: `OpenPanel(Notifications|Calendar, anchorX)` opens the native panel under that x.
- Reserved for later modules: `Services::Images()`, `PersistInt`. Add them in `module.inc` when first needed, then update this file.

Stubs shipped: `ClockModule` (collapsed), `PlaceholderPage` x2 (dashboards), `DemoAlertModule` (right-click the island to raise it; remove when P6b lands).
