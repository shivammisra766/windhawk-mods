# Dock bell: shows only the native notification-center button (bell) in the dock's tray area, as the
# dock's rightmost tile. Edits windows-11-taskbar-styler settings only (applied live by the styler,
# no Explorer restart here). Run elevated. Backs the key up with reg export first.
# Undo (elevated). reg import alone would keep the new slots 56-60, so delete Settings first:
#   reg delete "HKLM\SOFTWARE\Windhawk\Engine\Mods\windows-11-taskbar-styler\Settings" /f
#   reg import <the taskbar-styler-before-shell-*.reg this writes next to it>
#   Set-ItemProperty 'HKLM:\SOFTWARE\Windhawk\Engine\Mods\windows-11-taskbar-styler' SettingsChangeTime ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
# (the import restores the old SettingsChangeTime; the bump makes the styler notice).
# Explorer once crash-looped after styler edits: have recover-4.0.ps1 or the backup ready before running.
$s = $PSScriptRoot
$out = "$s\install-shell-result.txt"
try {
  $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
  if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'run elevated' }

  $mod = 'SOFTWARE\Windhawk\Engine\Mods\windows-11-taskbar-styler'
  $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
  $backup = "$s\taskbar-styler-before-shell-$stamp.reg"
  & reg.exe export "HKLM\$mod" $backup /y | Out-Null
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path $backup)) { throw 'reg export failed, nothing changed' }

  $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("$mod\Settings", $true)
  if (-not $key) { throw 'styler Settings key not found' }
  # The new rules go after the last existing slot; refuse if the layout is not the one these edits were written for.
  if ($key.GetValue('controlStyles[55].target') -ne 'Grid#OverflowRootGrid > Border' -or $null -ne $key.GetValue('controlStyles[56].target')) {
    throw 'unexpected controlStyles layout (slot 55/56), nothing changed'
  }
  if ($key.GetValue('controlStyles[42].target') -ne 'SystemTray.SystemTrayFrame' -or
      $key.GetValue('controlStyles[2].target') -ne 'StackPanel#SystemTrayFrameGrid, Grid#SystemTrayFrameGrid') {
    throw 'unexpected controlStyles layout (slot 2/42), nothing changed'
  }
  function V($name, $value) { $key.SetValue($name, $value, [Microsoft.Win32.RegistryValueKind]::String) }

  # Shelf: square right end, the tray segment continues it.
  V 'controlStyles[1].styles[2]' 'CornerRadius=16,0,0,16'

  # Tray frame: visible again, same height and offset as the dock's RootGrid (slot 3).
  V 'controlStyles[42].styles[0]' 'Visibility=Visible'
  V 'controlStyles[42].styles[1]' 'Height=66'
  V 'controlStyles[42].styles[2]' 'Margin=0,30,0,-16'

  # Tray frame grid: the shelf's colour and right end, flush with the shelf (which overhangs its frame by 12).
  V 'controlStyles[2].styles[0]' 'Background:=<WindhawkBlur BlurAmount="0" TintColor="#CC171718"/>'
  V 'controlStyles[2].styles[1]' 'BorderBrush=Transparent'
  V 'controlStyles[2].styles[2]' 'CornerRadius=0,16,16,0'
  V 'controlStyles[2].styles[3]' 'Margin=12,0,0,0'
  V 'controlStyles[2].styles[4]' 'Visibility=Visible'
  V 'controlStyles[2].styles[5]' 'Height=66'
  V 'controlStyles[2].styles[6]' 'BorderThickness=0'
  V 'controlStyles[2].styles[7]' 'Padding=0,0,8,0'

  # Everything in the tray except the notification-center button stays hidden (the top bar shows it).
  V 'controlStyles[56].target' 'SystemTray.Stack#NotifyIconStack, SystemTray.Stack#NonActivatableStack, SystemTray.NotificationAreaIcons#NotificationAreaIcons, SystemTray.Stack#SecondaryClockStack'
  V 'controlStyles[56].styles[0]' 'Visibility=Collapsed'
  V 'controlStyles[57].target' 'SystemTray.OmniButton#ControlCenterButton'
  V 'controlStyles[57].styles[0]' 'Visibility=Collapsed'
  # The clock in the notification-center button: the island is the clock.
  V 'controlStyles[58].target' 'SystemTray.DateTimeIconContent'
  V 'controlStyles[58].styles[0]' 'Visibility=Collapsed'
  # The bell glyph (the theme collapses it): back, as a dock tile (slot 45's size, colour and radius).
  V 'controlStyles[59].target' 'SystemTray.SystemTrayFrame > StackPanel#SystemTrayFrameGrid > SystemTray.OmniButton#NotificationCenterButton > Grid > ContentPresenter#ContentPresenter > ItemsPresenter > StackPanel > ContentPresenter > SystemTray.IconView#SystemTrayIcon > Grid#ContainerGrid > Grid#ContentGrid > SystemTray.TextIconContent > Grid#ContainerGrid, SystemTray.SystemTrayFrame > Grid#SystemTrayFrameGrid > SystemTray.OmniButton#NotificationCenterButton > Grid > ContentPresenter#ContentPresenter > ItemsPresenter > StackPanel > ContentPresenter > SystemTray.IconView#SystemTrayIcon > Grid#ContainerGrid > Grid#ContentGrid > SystemTray.TextIconContent > Grid#ContainerGrid'
  V 'controlStyles[59].styles[0]' 'Visibility=Visible'
  V 'controlStyles[59].styles[1]' 'Width=55'
  V 'controlStyles[59].styles[2]' 'Height=58'
  V 'controlStyles[59].styles[3]' 'Margin=2.5,4,2.5,4'
  V 'controlStyles[59].styles[4]' 'Background=#1F1F1F'
  V 'controlStyles[59].styles[5]' 'CornerRadius=12.5'
  V 'controlStyles[59].styles[6]' 'BorderThickness=0'
  # No native hover plate around the tile.
  V 'controlStyles[60].target' 'SystemTray.OmniButton#NotificationCenterButton > Grid > Border#BackgroundBorder'
  V 'controlStyles[60].styles[0]' 'Background:=Transparent'
  V 'controlStyles[60].styles[1]' 'BorderThickness=0'
  $key.Close()

  Set-ItemProperty "HKLM:\$mod" -Name SettingsChangeTime -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
  "ok, backup: $backup" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
