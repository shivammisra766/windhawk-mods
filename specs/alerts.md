# Alerts, toast suppression and the native volume/brightness popup

Research only; nothing was changed, run or shown on screen. Checked 2026-11-07 on Windows 11 10.0.26300 (shell binaries report file version 10.0.26100.9818).

Line references:
- `OLD:n` is `C:\ProgramData\Windhawk\ModsSource\dynamic-island-for-windows.wh.cpp`
- `BAR:n` is `D:\backups\windhawk-2026-11-06\src\topbar-core.cpp`
- `STYLER:n` is `windows-11-taskbar-styler.wh.cpp`
- `NC:n` is `windows-11-notification-center-styler.wh.cpp`

Evidence labels:
- **VERIFIED**: read on this machine (source, binary strings, public PDB symbols, read-only registry or window listing).
- **UPSTREAM**: read in another project's published source, not run here.
- **INFERRED**: a reasoned conclusion from the evidence.
- **UNTESTED**: needs a run on this machine.

## 1. The answers that affect planning

1. **Popup exactly over the centre bubble: achievable in principle, not yet proven on this machine.**
   - The registry setting cannot do it. It offers only three fixed positions.
   - A private function does return the popup's full rectangle: `HardwareConfirmatorHost::GetPositionRect` in `Windows.Internal.HardwareConfirmator.dll`, loaded in explorer.exe. Hooking it gives direct control of X, Y, width and height (VERIFIED signature, UNTESTED hook).
   - Shape and colour are ordinary styler rules.
   - The weak part is the moment of change. The native popup has its own show and hide animation and arrives a frame or two out of step with the bar. Expect "very close", not a true morph, until tested.
   - Section 5 has the fallback ladder.
2. **Toasts hidden, island only: achievable, with real losses.**
   - Best route: move the toast window off screen from inside ShellExperienceHost, the method used by the published `notifications-placement` mod.
   - Lost: reply boxes, toast buttons, images, and long-lived alarm, call and reminder toasts. There is also no deep link. Tapping opens the app, not the specific chat.
   - The listener API cannot tell an alarm or call from an ordinary toast, so a missed alarm or call is the top risk (section 4).
3. **Progress alert tracks nothing.** No code in the old mod ever writes a real value (section 3.8). Nothing exists to port, so the user needs to decide what it should mean.
4. **DND off has no documented API, and the working COM class is not registered for 32-bit.** It needs a helper in a 64-bit mod. Method signature and vtable order are unverified (section 3.4).
5. **USB alert is much narrower than its name.** It only sees drive letters and COM ports. It does not name the device, and it mislabels anything else as Bluetooth (section 3.9).
6. **Clipboard alert leaks secrets.** It displays copied text without checking the formats password managers use to opt out (section 3.7). Fix this in the rebuild.

## 2. Alert engine: old behaviour and proposed rules

### What the old mod does

- There is **no queue**. Each kind has one slot in `SharedState`, and a new event overwrites its slot.
- `ChooseActivities` (OLD:20867-20923) builds a fixed priority list every frame. `kinds[0]` is the primary pill and `kinds[1]` is a second pill beside it (OLD:22980-22988).
- Fixed order: Lookup > Clipboard > Caps > Device > Bluetooth > DND > Volume > Brightness > Notification > Battery > Progress > Timer > Media > Idle.
- Timers keep running while an alert is hidden behind a higher one. A 4 s notification behind a 2.5 s clipboard alert gets about 1.5 s on screen.
- Several notifications found in one poll overwrite each other, and only the last is shown (OLD:5770-5842). This was tolerable while Windows toasts still appeared. **It is not acceptable once toasts are hidden.**
- Expiry is cleared at OLD:22940-22967. `doNotDisturb` and `bluetoothDevice` are not cleared there and rely on the `now` check at OLD:20886-20892.
- Surfacing: a -6 px nudge spring when the primary kind changes (OLD:22992-22997).
- Hover on a transient alert expands nothing special. It only keeps the island awake (OLD:23056-23091).
- Tap handling exists only for Notification and Clipboard, and it only dismisses them (OLD:22365-22385). **Every tap action in the agreed map is new work.**

### Proposed rules

Use one small queue on the bar's UI thread, with one alert visible at a time. No side-by-side second pill, because the bar's centre bubble is one shape.

| Tier | Alerts | Rule |
|---|---|---|
| 1 Immediate | Caps | Replaces whatever is showing. The interrupted alert resumes with its remaining time. Same-kind events restart in place. |
| 2 Queued, keeps full time | Notification | FIFO, up to 5, then a "+N more" line. Never dropped silently, because the toast is gone. |
| 3 Queued, coalesced | Bluetooth, Device, Battery, DND | One slot per kind, latest wins, FIFO between kinds. |
| 4 Low | Clipboard | Shown only if nothing else is showing. Dropped rather than queued, since a late copy alert is noise. A notification replaces it. |
| Persistent, not queued | Timer, Progress | Show in the resting pill when no transient alert is up. |
| Overlay | Mic and camera dots | Always drawn on the bubble, never compete. |

- The native volume or brightness popup covers the bubble (section 5). While it is up, the queue pauses and timers stop.
- Hover pauses the running alert's timer. The pause is essential for notifications, since reading is the only chance.
- This reverses the old order, which put Clipboard above Notification. It is my recommendation, because a routine copy should not delay a message.

### Durations

| Alert | Old (line) | Proposed |
|---|---|---|
| Caps | 2.5 s (OLD:21922) | 2.5 s, stays while hovered |
| Battery | 4 s (OLD:8508) | 4 s; low 6 s |
| Bluetooth | 4 s (OLD:6167, 6247) | 4 s |
| DND | 3 s (OLD:5645) | 3 s |
| Notification | 4 s (OLD:5788) | **7 s**, as the toast no longer exists; Windows' default toast time is longer than 4 s |
| Clipboard | 2.5 s (OLD:9439) | 2.5 s |
| Device | 3 s (OLD:21955) | 3 s |
| Timer finished | 6 s (OLD:22933) | 6 s |

### Layout

The old mod has one card per alert, with no separate expanded form. Sizes at scale 1 are in `ActivityForKind` (OLD:20788-20865).

- **Collapsed** below means the card that pops out of the pill.
- **Expanded** is new: on hover the card grows downward as agreed, showing untruncated text and a one-word hint of the tap action.
- Common card shape (OLD:19507-19637): rounded badge at left, inset 14, side `height-16`, radius `side*0.35`, text from `badge.right+14`.
- Drop the countdown bar (`statusCountdownProgress`, OLD:19187-19200), which was off by default and is an option.
- Drop the `settingsOpacity_` and accent variants. **One fixed look, no theme options**: Start's dark flat colours, with green and red kept only as status dots.

**Dependency on the shell spec:** the bar's layered window is only 40 px tall (`kBarH` 32 at 125%, BAR:74, BAR:832), and cards are 42 to 58 DIP tall. Cards need a taller transparent window with correct hit testing, or a separate popup window. The same problem exists for hover expansion, so it should be solved once.

## 3. Each alert

### 3.1 Caps Lock

- **Lines:** hook OLD:20925-21018; handler OLD:21911-21926; draw OLD:19507-19569; card 180x42.
- **Detection:** `WH_KEYBOARD_LL` on a dedicated thread. On key-up of `VK_CAPITAL` or `VK_NUMLOCK` it reads `GetKeyState` and posts to the window. Event driven.
- **Threading:** the hook only posts. The comment at OLD:20932-20941 explains why it takes no lock: it runs on the system-wide input path.
- **Edge cases to keep:**
  - Post only, no work in the callback.
  - Install the hook only while wanted.
  - The thread must keep pumping messages (OLD:20991-21010).
  - **Bar trap:** a hook dies with its thread, so keep that thread alive until unload (`windhawk-mod-traps`).
- **Also keep:** Num Lock event (OLD:19523-19531). The user listed Caps only, so confirm whether Num Lock stays. Recommendation: keep; it is one branch.
- **Alternative worth testing:** Raw Input with `RIDEV_INPUTSINK` avoids sitting on everyone's keystrokes, but it delivers a message for every key to the bar. The LL hook only posts for two keys, and it is proven here. Keep the hook.
- **Tap (Caps turns off):** send `VK_CAPITAL` down and up with `SendInput` from the bar (the pattern already exists at BAR:892-908). Show the action only when Caps is on.
  - **Trap:** UIPI silently blocks `SendInput` when an elevated window is foreground. Re-read `GetKeyState` afterwards. If still on, show nothing misleading.
- **32-bit:** works; LL hooks are not injected. No extra libraries, no permission. `GetKeyState` state comes from the hook's posted lParam, and this path shipped in the same host.

### 3.2 Battery

- **Lines:** OLD:8473-8515; poll every 15 s OLD:22911-22914; draw OLD:20420-20485; pulse OLD:12776-12781; card 290x52.
- **Detection:** old mod polls `GetSystemPowerStatus`. **The bar is already better:** `RegisterPowerSettingNotification` for percent and AC/DC (BAR:93, BAR:1654-1655), handled at BAR:1514-1518. Hang the alert off that; no polling.
- **Triggers to keep:** AC state change; dropping through 20% and 10% on battery; first read sets a baseline with no alert (OLD:8486-8491).
- **Fix:** old test is `percent == 20 || == 10` (OLD:8497). A sleep or a fast drain can skip the exact value. Use "crossed below", from previous to current.
- **Edge cases:**
  - `ACLineStatus==1` means plugged in, not charging. A full battery reads "Power Connected", which is what the label says (OLD:20466), so keep that wording.
  - 255 percent is treated as 100 (OLD:8480).
  - No battery flag 128 is handled at BAR:404.
  - Time-left text is only shown if known (OLD:20472-20478).
- **Pulse:** the red pulsing border runs continuously (OLD:12776-12781). Use a short pulse at most, to stay event driven, or a static red, which also fits the flat look.
- **Tap:** `ms-settings:batterysaver`.
- **32-bit:** works, already running in the bar. No extra libraries.

### 3.3 Bluetooth connect, disconnect, battery level

- **Lines:** watcher OLD:6256-6337; connect OLD:6132-6207; disconnect OLD:6209-6254; classify OLD:5898-5953; GATT battery OLD:5959-6001; classic battery OLD:6009-6083; router OLD:6085-6115; draw OLD:19738 on; card 280x54.
- **Detection:** two `DeviceWatcher`s on the "Connected" selectors, classic and BLE (OLD:6315-6320). Added means connect, Removed means disconnect. Event driven, on WinRT threadpool threads.
- **Battery:** tried in this order:
  1. Undocumented devnode property `{104ea319-6ee2-4701-bd47-8ddbf425bbe5},2` through SetupAPI, matching the address inside the instance ID (OLD:6012-6083). 0xFF means unknown.
  2. BLE GATT 0x180F / 0x2A19 uncached (OLD:5959-6001).
- **Edge cases that must not be lost:**
  - Ignore devices reported before `EnumerationCompleted`, or every boot pops alerts (OLD:6276-6298).
  - Cache the name and category by ID, because Removed carries only the ID (OLD:6127-6130, 6225-6237).
  - Retry battery 6 times at 1.5 s, since it appears late (OLD:6183-6206), cancelled by a generation counter (OLD:6188-6196).
  - Remember last known battery for the disconnect card (OLD:6107-6112, 6246).
  - Skip empty names (OLD:6137).
- **Bugs not to copy:**
  - The first battery read blocks the watcher callback before the alert shows (OLD:6168-6170). Show first, then fill in.
  - The retry is a detached thread with `Sleep` that ignores the stop event (OLD:6184-6205), which is an unload risk. Use one worker waiting on the stop event.
  - No de-duplication: a dual-mode device can appear on both watchers. Debounce by name for about 1 s.
  - The keep-alive loop wakes every second (OLD:6325); wait on the stop event only.
- **Also drops out:** the volume-endpoint and lyric-related lines (OLD:6141-6145, 6215-6219) are not needed here.
- **Icons:** Segoe glyphs by category (OLD:19702-19736). GDI+ needs Segoe Fluent or MDL2 as a font, or the bar's SVG table.
- **Tap:** `ms-settings:bluetooth`.
- **32-bit:** class registrations exist in `WOW6432Node` with DLLs in SysWOW64 (VERIFIED). The old mod has a 32-bit build installed (`Engine\Mods\32\dynamic-island-for-windows_1.4.0_530752.dll`, VERIFIED).
- **Link:** `-lruntimeobject -lsetupapi` (OLD:9). Includes per OLD:635-644. No capability prompt for an unpackaged process per the old mod's use.
- **Cost:** C++/WinRT is the bar's largest footprint jump. The old DLL is 981 KB against the bar's 238 KB. Most of that is rendering, so the increase is smaller, but measure.

### 3.4 Do Not Disturb

- **Lines:** WNF OLD:5583-5687; subscribe at start OLD:23757; draw OLD:19639-19700; card 220x42.
- **Detection:** undocumented WNF state `0x0D83063EA3BF1C75` through `RtlSubscribeWnfStateChangeNotification` in ntdll, and `NtQueryWnfStateData` for the initial value. Event driven; the callback runs on an ntdll thread. It matches `WNF_SHEL_QUIETHOURS_ACTIVE_PROFILE_CHANGED` as published in a Microsoft Q&A answer (Windows 10 tested there).
- **Values:** 0 off, 1 priority only, 2 alarms only (Windows 10 source; UNTESTED on this build).
- **Edge cases to keep:**
  - Swallow the first callback, which only reports the current value (OLD:5633-5637).
  - Store atomically, alert only on change (OLD:5639-5648).
  - Unsubscribe on unload (OLD:5676-5687).
- **Note:** non-zero also comes from automatic rules such as games and fullscreen, so alerts can fire on those. The bar is hidden in fullscreen, so the alert should be skipped there.
- **Tap (DND off) is the hard part:**
  - No documented API found. A forum post says the same, and PowerToys only has an open request for a shortcut.
  - `QuietHours.dll` holds `IQuietHoursSettings` with `get_UserSelectedProfile` / `put_UserSelectedProfile` and the string `Microsoft.QuietHoursProfile.Unrestricted` (VERIFIED from public PDB and binary).
  - CLSID `{f53321fa-34f8-4b7f-b9a3-361877cb94cf}` is registered with an AppID in the 64-bit view, and **absent from `WOW6432Node`**. `QuietHours.dll` is absent from SysWOW64 (VERIFIED). IID `{6bff4732-81ec-4ffb-ae67-b6c1bc29631f}` came from memory, but its bytes do occur next to the CLSID in the DLL.
  - **Plan:** run the call in a 64-bit mod (`local@tray-bridge` in explorer), triggered by a registered message from the bar.
  - **UNTESTED:** vtable order, parameter type (HSTRING vs LPCWSTR), whether non-system callers are allowed, whether automatic rules accept it.
  - A second candidate is `Windows.Internal.UI.Notifications.QuietHoursSettingsInterop` in wpnapps.dll, which **is** registered for 32-bit (VERIFIED). Its interface is unknown.
  - **Avoid:** writing the WNF state directly (would not reach the stored setting; UNTESTED), and the published registry-blob method, which needs a restart of WpnUserService.
- **Fallback:** open `ms-settings:quietmomentshome`, and say so on the card. The exact Settings link needs a check.
- **Settings string found:** `SystemSettings_Notifications_DoNotDisturb_Toggle` in SettingsHandlers_Notifications.dll, a third route but heavier.
- **32-bit:** detection works (ntdll exports exist under WOW64; callback typedef is `NTAPI`, OLD:5584), no libraries. Toggle does not.

### 3.5 Notifications with app icons

- **Lines:** listener OLD:5689-5894; thread start OLD:23754; card OLD:19272-19344, 360x58.
- **A second, cruder route** exists at OLD:9268-9435: `HSHELL_WINDOWCREATED` plus UI Automation scraping of toast windows, called from OLD:22639-22644. **Do not port it.** It guesses from class names, needs a worker per window, and hiding toasts changes what it sees.
- **Detection:** `UserNotificationListener` with `GetNotificationsAsync(Toast)` **polled every 1 s** (OLD:5754-5875), comparing IDs. MTA worker thread. This is the heaviest thing in the alerts area, and needs replacing (section 4.3).
- **Edge cases that must not be lost:**
  - **30 s delay after process start**, because early init can permanently bind to a dead proxy (OLD:5702-5724).
  - Retry loop for access not granted, 3 s (OLD:5726-5747).
  - First poll only records existing IDs (OLD:5763-5768).
  - E_NOTIMPL and REGDB_E_CLASSNOTREG mean the subsystem is not ready, retried quietly (OLD:5845-5849, 5858-5862).
  - Per-item try/catch (OLD:5771, 5844-5852).
- **DND:** `notificationRespectDnD && g_isDnDActive` skips the item (OLD:5782). **This is wrong once toasts are hidden:** DND's priority list and alarms would be dropped although Windows would have shown them. See 4.4.
- **Console filter:** display name containing `powershell`, `pwsh` or `cmd.exe` (OLD:5794-5799). Keep. Add `windows terminal`? Only if asked. The old toast-window filter at OLD:9280 also lists `conhost.exe`, so add that string.
- **App identity and icon:** `AppInfo().DisplayInfo()`, `GetLogo({32,32})`, decoded by WIC (OLD:5789-5810), then `FindAppIconByName` (OLD:5812-5814, helper around OLD:4876). Icon cache at OLD:4752. Bar needs a GDI+ decode of the stream, which is simple.
- **Text:** first two lines of `ToastGeneric` (OLD:5816-5826). Falls back to app name (OLD:5828-5830). Truncates at 120 (OLD:5834-5837). Marquee at OLD:19338; for the bar, wrap in the expanded card instead, which needs no animation.
- **Old mod quirk:** the red badge dot on the icon (OLD:19295-19307). Decorative; drop unless wanted.
- **Tap (opens its app):**
  - `AppInfo().AppUserModelId()`, then `ShellExecute` on `shell:AppsFolder\<AUMID>`, or `IApplicationActivationManager` for packaged apps. Both work from 32-bit. Local notification keys show both kinds exist here: `5319275A.WhatsAppDesktop_cv1g1gvanyjgm!App` and `com.squirrel.Discord.Discord` (VERIFIED).
  - **Lost:** the toast's launch argument is not exposed by the listener. WhatsApp opens, but not at the chat.
  - **Optional, risky:** UI Automation Invoke on the real toast element would keep the deep link. UNTESTED; it is one reason to keep the toast alive off screen.
  - Then `listener.RemoveNotification(id)` so the bell does not stay lit. Open to choice: Windows' own click does remove it.
- **32-bit and permission:** wpnapps.dll exists in SysWOW64 and is registered in `WOW6432Node` (VERIFIED). **Consent is on here**: `ConsentStore\userNotificationListener` Value=Allow in HKCU and HKLM (VERIFIED). A user can switch it off; then show a one-time note and do not hide toasts (4.5).
- **Link:** `-lruntimeobject`. Includes OLD:625-633.

### 3.6 Privacy dots: mic and camera

- **Lines:** registry reading OLD:8983-9133; update OLD:9142-9152; poll 2 s OLD:22919-22922; draw OLD:12995-13029; label OLD:14998 on; layout reservation OLD:832.
- **Detection:** `CapabilityAccessManager\ConsentStore\{microphone,webcam}`, HKCU then HKLM, including the `NonPackaged` subtree. Polled on the render thread.
- **Replace polling:** `RegNotifyChangeKeyValue` with `bWatchSubtree=TRUE` on the two keys in HKCU and HKLM, waited on a worker. Event driven, no wake-ups when idle. UNTESTED that every Windows path writes through here, but the values do live in these keys.
- **Edge cases that must not be lost:**
  - An entry is in use only if `LastUsedTimeStop==0` **and** `LastUsedTimeStart!=0`, both REG_QWORD, type and size checked. The older test left the mic dot on permanently (OLD:9014-9059).
  - `KEY_WOW64_64KEY` is needed for HKLM from 32-bit (OLD:9067, 9112, 9118). **This matters directly: without it the 32-bit bar reads the wrong HKLM view.**
  - `CleanActiveAppName` for package names and `#`-separated paths (OLD:8983-9012).
- **Bugs not to copy:** only one dot draws; camera hides mic (OLD:13010-13028). Dots force continuous rendering while active (OLD:23461-23463), which is a CPU drain. The dots are static, so paint once on change.
- **Layout:** 8 px dot, right side of the bubble, vertically centred; hidden on tall cards (`height>55`, OLD:12998). Orange mic, green camera (OLD:1422-1423), fixed colours. Show both side by side. Hover: "Zoom is using your microphone" (OLD:15027-15030).
- **Tap:** not in the agreed map. Recommendation: nothing, or `ms-settings:privacy-microphone`. Ask.
- **32-bit:** works. `-ladvapi32` is already linked (BAR:8).

### 3.7 Clipboard

- **Lines:** `CaptureClipboard` OLD:9437-9495; text OLD:9154-9171; image OLD:9178-9266; trigger OLD:21976-21980; listener registration OLD:21794; draw OLD:19202-19270; card 340x56.
- **Detection:** `AddClipboardFormatListener`, `WM_CLIPBOARDUPDATE` on the window thread. Event driven.
- **Edge cases to keep:**
  - Fall back to foreground window when there is no owner (OLD:9440-9443).
  - Ignore explorer, windhawk and untitled owners for the icon (OLD:9448, helper OLD:4817-4835).
  - Flatten newlines, cap 96 chars (OLD:9469-9475).
  - CF_BITMAP synthesised from DIB; must not delete the clipboard's bitmap; fill alpha (OLD:9175-9177, 9216-9217, 9250-9254).
- **Must fix:**
  - **Secrets:** check `ExcludeClipboardContentFromMonitorProcessing` and `CanIncludeInClipboardHistory` (value 0) with `IsClipboardFormatAvailable`, and show "Copied" with no text. Password managers set these. The old mod ignores them and paints passwords on the top of the screen.
  - **`OpenClipboard` has no retry** (OLD:9156, 9179), and the owner may still hold it. Retry a few times over a short timer, not with `Sleep` on the UI thread.
  - **Blocks the window thread** while decoding large images. In a 32-bit process with limited address space, a big screenshot synthesised to CF_BITMAP can fail: check result and show "Image copied" without the thumbnail.
  - **Self-trigger:** the bar's own file shelf writes (OLD:21544) must set a skip flag.
  - **Dedupe:** apps fire several updates per copy; coalesce within 100 ms.
- **Tap (clipboard history):** there is no URI. Use `SendWinKey('V')`, already at BAR:892-908. The bar is `WS_EX_NOACTIVATE`, so the paste target keeps focus. `EnableClipboardHistory`=1 here (VERIFIED). If 0, Win+V only offers to turn it on, which is fine. Same UIPI caveat with an elevated foreground window. **Memory rule: the user types during tests; ask first.**
- **32-bit:** works, no extra libraries. Thumbnail goes to a GDI+ bitmap directly.

### 3.8 Progress: what it really tracks

**Nothing.** VERIFIED by a search of the whole file:

- `UpdateProgressSnapshot` reads mod storage `Wh_GetIntValue(L"ProgressPercent", -1)` four times a second (OLD:9135-9140, 22915-22918).
- The only write in the file is setting it back to -1 (OLD:11132).
- No other code feeds it. The setting's description "for downloads or file copies" (OLD:389-390) is not implemented. The value could be changed only by something writing the mod's storage by hand.
- UI: a ring around the pill (OLD:12772-12774, 20496-20525), text "Progress N%" (OLD:20487-20494).

**The old feature must not be ported as it is.** Options to put to the user:

| Option | How | Cost | Catch |
|---|---|---|---|
| A. Taskbar progress | No public read API for `ITaskbarList3::SetProgressValue`. Would need an explorer hook on the taskbar's private progress code. | High | Fragile between builds; depends on Taskbar.View internals. UNTESTED. Would cover browser downloads and Explorer copies, which is what the label promises. |
| B. Explorer copy only | Watch file operation dialogs (`OperationStatusWindow`) | Medium | Scraping, one case only |
| C. Timer only | Drop Progress; the agreed tap "timer stops" is really the Focus Timer alert | None | No download ring |

Recommendation: **C now, A later only if wanted.**

**Timer** (it shares this slot and the tap map): `StopFocusTimer` OLD:10832-10840, start OLD:10800, finish OLD:22929-22938, card 260x54, 6 s finished message. Tap stops. Timer logic belongs to the dashboards spec; this spec only covers the tap and tier.

### 3.9 Device and USB alerts: what exists

- **Lines:** OLD:21928-21960; draw OLD:19571-19637; card 240x50.
- **Detection:** `WM_DEVICECHANGE` with `DBT_DEVICEARRIVAL` or `DBT_DEVICEREMOVECOMPLETE`. Event driven, window thread.
- **What it actually sees:** with no `RegisterDeviceNotification` call in the file (VERIFIED), Windows broadcasts only volume and port events to top-level windows. So: USB drives shown as "USB Drive", COM ports shown as "COM Device". **Mice, keyboards, phones, headsets and other USB devices produce nothing.**
- **Bugs:**
  - The `else` branch labels anything else "Bluetooth Device" (OLD:21941-21945).
  - No real device or drive name.
  - `DBTF_NET` and `DBTF_MEDIA` flags are not filtered, so network mapped drives and disc media would count as "USB Drive".
  - `isBluetoothLike` serves no purpose given the separate Bluetooth alert.
- **Proposed:**
  - Filter out `DBTF_NET`.
  - Decode `dbcv_unitmask` to a letter. Add the volume label on arrival: "Samsung T7 (E:)".
  - Decide with the user: adding `RegisterDeviceNotification(DEV_BROADCAST_DEVICEINTERFACE, GUID_DEVINTERFACE_USB_DEVICE)` plus a SetupAPI friendly name would catch all USB devices. It needs debounce, because composite devices fire many times, and hubs and internal devices must be ignored. It would also duplicate drive alerts. Recommendation: drives first, general USB as a second step.
- **Tap:** `ms-settings:usb`.
- **32-bit:** works. No permission. Message structure sizes are the same. `-lsetupapi` only for friendly names. **The bar's window must be top-level and not message-only**; it is.

### 3.10 How the old mod detects volume and brightness (not being rebuilt)

- **Volume:** `IAudioEndpointVolumeCallback::OnNotify` on the default render endpoint, and `IMMNotificationClient` for default device changes; rebinds on the render thread because MMDevice calls are not allowed in callbacks (OLD:8612-8815). Device switching does not surface it (OLD:8791-8792). Only the first step nudges, to avoid bouncing (OLD:8643-8646). Surfaces on any change, whatever the source. Duration 1.8 s.
- **Brightness:** `DeviceIoControl` on `\\.\LCD` with `0x230498` polled every 0.25 s (OLD:8902-8980, 23150-23153). AC/DC swap resets the baseline without an alert (OLD:8967-8970). Retry 30 s on desktops (OLD:8910-8926). **Polling at 4 Hz: good riddance.**
- **Behaviour change the user should expect:** the native popup appears on hardware keys and its own triggers. A slider dragged in Quick Settings or a volume change from an app did show in the old island and may not show in the native one. INFERRED; test.
- **Keep nothing here.** The bar has no need for these if the native popup carries it.

## 4. Decision 1: hide toasts, island only

### 4.1 Options

| Option | Listener still reads? | Verdict |
|---|---|---|
| Global toggle `PushNotifications\ToastEnabled=0` (currently 1, VERIFIED) | Likely no; turns notifications off | **Rejected** |
| DND on permanently | Yes | **Rejected**. Ruins the DND alert and its meaning, shows the state in the shell, and alarms and priority apps still pop. |
| Per-app banners off, `Notifications\Settings\<AUMID>\ShowBanner=0` | Yes, per Microsoft's UI ("still in notification center") | **Workable but poor.** Documented in the UI, and the string is in NotificationController.dll (VERIFIED). 42 app keys exist here, none with ShowBanner (VERIFIED). A new app's first toast still shows. It alters user settings. The bar would have to write registry and watch for new keys. |
| Collapse XAML with a styler rule on `ActionCenter.ToastCenterView` or `Grid#ToastCenterMainGrid` (names at NC:3204, 3214) | Yes | **Partial.** Invisible, but the window still sits there and swallows clicks in its rectangle. Rules for toast views exist in NC:489, NC:724. |
| **Move the toast window off screen in a `SetWindowPos` hook in ShellExperienceHost** | Yes | **Recommended** |

### 4.2 Recommended method

- **Precedent (UPSTREAM):** m417z's published `notifications-placement` mod. Hooks `SetWindowPos` in ShellExperienceHost.exe. It identifies the toast window as class `Windows.UI.Core.CoreWindow`, in process ShellExperienceHost.exe, with a title matching a locale table containing `New notification`. It also hooks `CToastCenterExperienceManager::PositionView` in `twinui.pcshell.dll` from explorer, and it is kept up to date (it mentions a XAML change "~Jul 2026").
- **Local fit (VERIFIED):** `local@panel-split` already runs in ShellExperienceHost and ShellHost, 64-bit, and already hooks `SetWindowPos` for Quick Settings. Add a branch to it. A Notification Center CoreWindow does exist in that process here, and the toast window is created on demand, so it was not present to inspect.
- **Why off screen, not `SW_HIDE`:** keeps the toast's life cycle running: timeout, move to Notification Center, sound. UNTESTED whether hiding would interrupt that, and off screen is the lower risk.
- **Sound:** should continue, as the toast still "plays". UNTESTED. Island-only with a sound is probably what the user wants.
- **Locale:** title table needed. One language here, but Windows display language can change. Add a table anyway from the placement mod, since the bar has 12 languages planned.
- **Flash risk:** placement in the first `SetWindowPos` means no flash if that call is what shows it. Memory notes SEH starts on demand and was slow on first open, so test the first toast after a host restart.

### 4.3 Replacing the 1 s poll

- The toast hook gives a perfect wake-up: post a registered message, for example `Windhawk_TopBar_ToastShown`, to the bar. The bar's worker then calls `GetNotificationsAsync` once.
- A 30 s safety poll covers a missed signal, or watch `%LOCALAPPDATA%\Microsoft\Windows\Notifications\wpndatabase.db-wal` with `ReadDirectoryChangesW`. The file exists here (VERIFIED).
- The listener's `NotificationChanged` event: my recollection, with no source checked, is that it fails for unpackaged apps. The old mod's choice to poll is consistent with that. Try once; do not depend on it.
- **Timing:** the toast window can appear before the listener lists the item, so retry at 100, 300, 800 ms.

### 4.4 Using the toast signal to get DND right

"A toast window appeared" means Windows decided to show it. Use that as the gate rather than the old `g_isDnDActive` check:

- Windows' own rules for DND, priority list, alarms, `SuppressPopup` and per-app banner settings then all stay correct, with no logic of ours.
- Notifications that arrive silently, without a banner, do not pop on the island. They stay in the bell.
- This is a design proposal, UNTESTED. If the hook is absent, fall back to the old DND check and show all.

### 4.5 Fail safe: required

If the bar dies, or the listener has no permission, toasts must come back. Otherwise the user silently gets no notifications at all.

- The hook hides a toast only if the bar's window exists and has a property such as `TopBarToastsOk` set, using the same window-property pattern as `TopBarAnchorX/Tick` in panel-split.
- The bar sets it only after the listener has succeeded.
- During the 30 s startup delay (3.5), toasts show normally.
- Fullscreen: the bar hides itself in fullscreen. Decide whether toasts return then. Recommendation: yes, unless Windows' DND rule already blocks them.

### 4.6 What is lost, plainly

| Lost | Impact | Mitigation |
|---|---|---|
| Action buttons and reply boxes | Teams, WhatsApp quick replies | Open bell panel. Memory: agreed, "toast buttons only via the bell's native panel" |
| Deep link on tap | Opens app, not the item | Optional UIA Invoke later |
| Alarm, reminder and **incoming call** toasts that wait for an answer | **Could be missed.** Listener gives no scenario, and the hook sees only a window | **Top risk.** Allow-list by app: Clock, Teams, Phone Link, calendar apps show as native toasts. The hook would need the app, which requires reading the XAML sender name after creation (`TextBlock#SenderName` exists, NC:1620), or the listener's latest item. Needs design and test. |
| Images, hero art, progress bars in toasts | Text only | Accept |
| Long text beyond two lines | Expanded card, or bell | Accept |
| Windows Update, security or sign-in prompts | Reach the island as text, tap opens app | Check by hand |
| Screen reader announcement | Off-screen window may still announce; island has none | Accept; check if needed |
| Windhawk Action Center styling for toast | Irrelevant after hiding | Bell list still styled |

## 5. Decision 2: the native popup over the centre bubble

### 5.1 Verified facts

| Fact | Evidence |
|---|---|
| Code lives in `C:\Windows\System32\Windows.Internal.HardwareConfirmator.dll`, 626,688 bytes, version 26100.9818. Loaded in `explorer.exe` (not twinui, as earlier assumed; twinui.dll only has the name) | `tasklist /m`, strings |
| `Rect HardwareConfirmatorHost::GetPositionRect(Rect const&)`, private member, returns struct. Mangled: `?GetPositionRect@HardwareConfirmatorHost@implementation@HardwareConfirmator@Internal@Windows@winrt@@AEAA?AURect@Foundation@56@AEBU7856@@Z`. Rect is 4 floats. x64 convention: `rcx=this, rdx=hidden result pointer, r8=input rect`, returns `rdx` | Public PDB from msdl |
| Related: `ShowConfirmatorHWND(Rect const&)`, `EnsureFrame(Rect const&)`, `GetDisplayRectAsync()`, `OnFrameSizeChanged`, `ShowVolumeAsync(int)`, `ShowBrightnessAsync(int)`, `HideConfirmator()`, `OnHideAnimationComplete`, `HideConfirmatorWithoutAnimation` | PDB |
| Control: `ConfirmatorHostControl::{ShowVolume, ShowBrightness, AnimateConfirmator(ConfirmatorAnimation), GridSizeChanged, SetConfirmatorMaxWidth, OnPointerWheelChangedHandler, OnSliderValueChanged}` and constants `c_bottomCenterPosition, c_topCenterPosition, c_topLeftPosition` | PDB |
| Window placed through `IXamlExplorerHostWindow` `Position` taking a Rect. The DLL imports no `SetWindowPos`; its only window-related user32 imports are `GetWindowLongPtrW`, `SetWindowLongPtrW`, `MonitorFromRect` | PDB, binary |
| Host window: hidden top-level `XamlExplorerHostIslandWindow` in explorer, last rect 683,793 to 852,864 at the 96-DPI view, about **211x89 px** at this machine's 120 DPI, centred and flush with the bottom. Ex-style `0x8200088` = `NOACTIVATE \| NOREDIRECTIONBITMAP \| TOPMOST \| TOOLWINDOW`. A second such window is fullscreen-sized | Read-only `FindWindowEx` walk. **That this exact window is the popup is INFERRED** from size, location and flags |
| Registry key and values: `HKCU\...\SystemSettings\ConfirmatorPosition\PositionIndex`, **present here = 1**. Read by the DLL and by `SettingsHandlers_Notifications.dll` with options `BottomCenter`, `TopLeft`, `TopCenter`. Web guides give 1 bottom, 2 top-left, 3 top-centre. The setting is in Settings > System > Notifications | Binary strings, reg query. Value mapping from web only |
| XAML: `ms-appx://Windows.UI.PCShell/HWConfirmatorUI/ConfirmatorHostControl.xaml`. Names: `ConfirmatorFrameCanvas`, `ConfirmatorMainGrid`, `VolumeConfirmator`, `BrightnessConfirmator`, `KeyboardBrightnessConfirmator`, `AirplaneModeConfirmator`, `CameraPrivacyConfirmator`, `QuickMuteConfirmator`, `TextConfirmator`, `VolumeIcon`, `volumeSlider`, `volumeLevelText`, `BrightnessIcon`, `brightnessSlider`, `HorizontalTrackRect`, `HorizontalDecreaseRect`. Resources: `ConfirmatorHeight`, `ConfirmatorShadowPadding`, `ConfirmatorPageShadow`, `OverlayCornerRadius` | Strings in `Windows.UI.PCShell.pri` |
| Tree path: `ScrollViewer > ScrollContentPresenter > Border > HWConfirmatorUI.ConfirmatorHostControl > Canvas#ConfirmatorFrameCanvas > Grid > Grid#ConfirmatorMainGrid > Grid#VolumeConfirmator` | STYLER:2467-2495 |
| Taskbar styler is enabled here (v1.10) and runs in explorer. Active theme `OS26_Liquid_Glass_variant_ClearMacDock` **already styles** `ConfirmatorMainGrid`, with custom slots 17, 18 and 41, and a 10 px bottom margin | Registry, STYLER:6849-6856 |
| Sizes and shapes can be styled small: `MinHeight=16`, `CornerRadius=0`, `Width` 132 outer, 90 inner slider, `Padding=0` on parent Grid | STYLER:2467-2499 (`g_themeBottomDensy`) |
| `Shadow:=` removes the shadow, and `Background`, `CornerRadius` are accepted | STYLER:4087-4092 |

No Windhawk mod that moves this popup was found on the web or in the local mods. The feature is built-in only since 26100.6725-era builds per web sources, so a mod like this appears to be new ground.

### 5.2 Plain answer: can it sit exactly over the bubble?

**Position: yes, in principle, by hooking `GetPositionRect`.** It decides the rectangle, so a pixel-exact result is plausible. Hook is in explorer, so it must be 64-bit: add to `local@tray-bridge`, not the bar. Windhawk can hook by public symbols, as the placement mod does.

**Size: yes, with constraints.**
- `GetPositionRect` returns width and height too, and the styler sets inner dimensions.
- Current bubble: pill height `kPillH` 24 DIP = 30 px; fully round (BAR:79, 743, 749), width depends on the clock text (BAR:741).
- Styler precedents reach heights near this, so 24 DIP looks possible, but `ConfirmatorHeight` could fight it. UNTESTED.
- **Width conflict:** icon + slider + percentage needs about 180 DIP. The clock pill is about 130 to 150 DIP. Either the slider is very short, or the bar widens the bubble as the popup appears. **Recommendation:** widen, using the same animation as the hover growth.
- **Layered height:** a notch-style or larger island would change these numbers. The final geometry comes from the shell spec.

**Shape and colour: yes.** `CornerRadius`, `Background` as solid, `BorderThickness=0`, `Shadow:=`, `Padding=0` on the parent Grid.
- **Colour must be opaque.** Today's pill is opaque black (BAR:747). A semi-transparent popup over a bubble would double up and show. New look is "Start's colours": whatever the final fill, make both exactly the same solid colour.
- Remove the existing theme rules for this grid, slots 17, 18, 41, and check any `WindhawkBlur` is gone.
- **Avoid `Translation=`** in rules (`windhawk-mod-traps`: crash in Windows.UI.Xaml).

**The illusion: this is the uncertain part.**
- The native popup animates itself in and out (`AnimateConfirmator`). A slide or fade over a bubble of identical colour reduces it, since content appears to fade in, but a positional slide would show.
- Options, in order: find an animation switch (the DLL uses `UISettings`, maybe for `AnimationsEnabled`, and the `SPI_GETCLIENTAREAANIMATION` trick works elsewhere per memory, but in explorer it would hit the taskbar too); hook `AnimateConfirmator` to a no-op, which must still return a completed `IAsyncAction`, risky because hide completion may wait on it; or use `HideConfirmatorWithoutAnimation`.
- Bar and popup are separate windows, so widening and appearing are one or two frames apart. At 16 ms it is barely visible. It will not be a true shape morph.
- **Honest expectation:** "the island's content changes to a slider" is a realistic goal. "The island physically becomes it, with no visible seam ever" is not guaranteed.

**Other uncertainties:**
- Are returned Rect values DIP or physical pixels? Telemetry names `positionRectX/Y/Width/Height` do not say. Log first.
- Does `OnFrameSizeChanged` or `GridSizeChanged` re-run the calculation or apply its own? If so, the hook still covers it, unless it applies a separate formula.
- Window rect may include `ConfirmatorShadowPadding`. Then the visible grid is inset, and the rect must be offset. Setting padding to 0 should remove this.
- Clamping: `SetConfirmatorMaxWidth` may limit from display size, and that is fine at these sizes.
- Is the symbol stable? Windows updates can rename it. Windhawk will fail to find it, the hook will not install, and the popup falls back to the registry position. That's a safe failure.
- Transparent parts of the window can swallow clicks for the 2 or so seconds it's up. Small.
- Other popups share the host: airplane mode, keyboard brightness, camera, mic mute, text. All would land on the bubble, which is probably good. The "text" width may vary. Confirm.
- Z-order: the bar reasserting topmost may cover the popup. The old mod's rule was never to do that against shell windows (OLD:4076-4079). BAR has no periodic reassert seen in this reading; keep it so.
- **Fullscreen:** the bar hides, the popup still appears at the top. Fine. Possibly let the original position apply.
- **Multi-monitor:** return the bar's rectangle only if the input display rect is on the bar's monitor; otherwise leave unchanged.

### 5.3 How it would be built

1. **Geometry from the bar.** The bar publishes the bubble's screen rect and a chosen slider width as window properties on its HWND, as with `TopBarAnchorX/Tick`.
2. **Hook in explorer** on `GetPositionRect`: call original, read the bar's props, overwrite the result if the bar is alive and on the same monitor, else leave. Log the original result first.
3. **Module timing:** the DLL is in memory now but may load on demand. Handle both: hook at load via `LoadLibraryExW` hook if not yet loaded.
4. **Rules** in the styler for `ConfirmatorMainGrid`, `VolumeConfirmator`, `BrightnessConfirmator` and the slider.
5. **Bar notified** by a registered message posted from the hook, e.g. `Windhawk_TopBar_Osd` (shown, hidden, kind), hooking `ShowVolumeAsync`, `ShowBrightnessAsync` and `OnHideAnimationComplete` or `HideConfirmator`.
   - **No-hook alternative:** from the 32-bit bar, `SetWinEventHook(EVENT_OBJECT_SHOW..HIDE)` out of context, filtered to explorer's PID and class `XamlExplorerHostIslandWindow`, excluding the fullscreen one. Same pattern as BAR:1657-1660. It also works if the hook breaks. Use this for the bar's own state in any case, as it does not depend on symbols.

### 5.4 One test before committing

A log-only hook in a test copy of tray-bridge: on key press, log input and output rects, and DPI. Then one version returning a fixed rect at the top. That single step resolves DIP vs pixels, hookability and flicker on this build. It needs a volume-key press from the user, and explorer may need a restart for a new mod to load (`windhawk-mod-traps`: **restart Windhawk service only with care**; it has crashed Explorer here). Ask before running.

### 5.5 Fallback ladder

| Level | What | Result |
|---|---|---|
| 0 | Hook plus rules | Popup over the bubble |
| 1 | Hook works but height or width won't shrink | Popup centred on the bubble, slightly larger, like a brief swell |
| 2 | No hook, or hook breaks on a Windows update: `PositionIndex=3` (top centre), rules for look | **Popup just below the bubble**, same colour and shape, looks like a drop from the island. No code in explorer. |
| 3 | Bar draws its own slider and native popup is hidden | User ruled this out, so only if they change their mind |

**Set `PositionIndex=3` even when using level 0**, so a failed hook lands at level 2 and not at the bottom of the screen.

Level 2 detail: top-centre's exact offset from the bar is UNTESTED, particularly whether it respects the bar's reserved space. A styler margin can adjust it.

## 6. Notes for the bar

- **New threads:** WinRT worker for Listener and Bluetooth, MTA, waiting on stop event; keyboard hook thread; registry watcher. Everything else on the UI thread. Keep alerts' `Paint` on demand; animate only while a card moves, using the existing `kAnimTimer` pattern (BAR:44, 1375).
- **Link additions:** `-lruntimeobject -lsetupapi`, plus `-lwindowscodecs` if decoding logos by WIC, though GDI+ `Image::FromStream` is enough.
- **Icons:** icon cache and extraction from OLD:4752-5070 are the reference.
- **Tap hit area:** add an `Action` per alert to the existing hit list (`addHit`, BAR:760, handled at BAR:1458). Tap dismisses the alert too.
- **Taps launching links:** `ShellExecuteW` as at BAR:1108. The bar has no foreground; a window launched from a real click should still come forward. Memory lists foreground problems for Quick Settings, so check.
- **Localisation:** old strings at `Loc(...)`; a few are hard-coded (OLD:19615, 20466). Use the shared table.
- **Fixed look:** no setting for colours, no per-alert toggles unless the settings page spec wants them. Old module toggles are at OLD:2920-2935, 3000-3003.
- **The old mod is disabled, not removed:** `Disabled=1` confirmed. All alerts are gone until these land, and toast hiding must not arrive before the notification card.

## 7. Suggested order and honest effort

"Session" means one focused build-and-test round with the user available. Estimates assume the shell work that gives a taller card area is already done.

| # | Work | Effort | Risk |
|---|---|---|---|
| 1 | Alert engine: queue, tiers, card drawing, tap dispatch | 1 session | Low |
| 2 | Caps, Battery, Device (drives), DND status display | 1 session | Low |
| 3 | Clipboard with secret filter, Privacy dots with registry watcher | 1 session | Low |
| 4 | WinRT worker, Bluetooth with battery | 1 session | Medium: binary size, late battery |
| 5 | Notifications from listener, toasts still visible, tap opens app | 1 session | Medium |
| 6 | **OSD spike: log-only hook, fixed rect** | 0.5 session | **Decides section 5** |
| 7 | OSD geometry, rules, bar widening, tune animation | 1 to 3 sessions | **High**: visual polish is open-ended |
| 8 | Toast hiding with signal, fail safe, allow-list for calls and alarms | 1 to 2 sessions | **High**: can lose real notifications |
| 9 | DND off through 64-bit helper | 0.5 to 1 session | Medium: may end at Settings fallback |
| 10 | Progress, only if option A chosen | 2+ sessions | High, optional |

**Total without Progress: about 8 to 11 sessions.** Items 7 and 8 are where it could overrun. Item 6 can run early, and it is worth doing first if the popup is the part the user cares most about. It is independent of everything else.

Each session needs a user test: toasts, Bluetooth and volume keys cannot be proved by script alone. Memory says limits run out fast, so plan these as separate sittings.

## 8. Questions to ask the user

1. Progress: drop it, or pay for option A?
2. Calls and alarms: which apps must still show native toasts? (Suggested: Clock, Teams, Phone Link.)
3. Should toasts return in fullscreen, while the bar is hidden?
4. Num Lock alert too?
5. Privacy dot tap: nothing or settings page?
6. General USB devices, or only drives?
7. Does tapping a notification also remove it from the bell list?

## Sources

- [notifications-placement mod source](https://raw.githubusercontent.com/ramensoftware/windhawk-mods/main/mods/notifications-placement.wh.cpp)
- [Microsoft Q&A: reading DND state through WNF](https://learn.microsoft.com/en-us/answers/questions/2182613/how-to-programmatically-determine-do-not-disturb-m)
- [Microsoft: notification listener](https://learn.microsoft.com/en-us/windows/apps/develop/notifications/app-notifications/notification-listener)
- [ElevenForum: on-screen indicator position](https://www.elevenforum.com/t/change-position-of-on-screen-indicators-in-windows-11.37291)
- [GeekRewind: on-screen indicator position](https://geekrewind.com/how-to-change-on-screen-indicator-position-in-windows-11/amp/)
- [Windows Latest: on-screen indicator setting](https://www.windowslatest.com/2025/06/20/windows-11-will-let-you-change-on-screen-indicators-position-like-volume-flyout)
- [ElevenForum: per-app banners](https://www.elevenforum.com/t/turn-on-or-off-show-notification-banners-from-apps-in-windows-11.6214/post-118783)
- [ElevenForum: DND toggle scripts](https://www.elevenforum.com/t/focus-assist-do-not-disturb-toggle-scripts-exe.32153/) (search summary only; the page refused a direct fetch)
- [PowerToys issue 51454: DND shortcut request](https://github.com/microsoft/PowerToys/issues/51454)
- Symbols: `msdl.microsoft.com/download/symbols`, `Windows.Internal.HardwareConfirmator.pdb` and `QuietHours.pdb` for this machine's DLLs. Read as text in the scratchpad, then deleted.
