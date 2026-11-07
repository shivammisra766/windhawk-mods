# Installs local@translucent-windows 1.8.2.2 (plain #C4000000 tint, no blur) for explorer.exe only.
# In explorer.exe the mod now touches only File Explorer folder windows (CabinetWClass and their
# children); the taskbar, desktop, tray flyouts and XAML islands are left alone.
# Installs ONLY the mod: the File Explorer styler rules are already in place, the Settings styler part
# is skipped for now. Run elevated, after build-translucent.ps1. It restarts nothing: the mod loads
# into Explorer the next time it starts (test-translucent.ps1 does the restart and the crash check).
#
# Undo (elevated):  [Microsoft.Win32.Registry]::LocalMachine.DeleteSubKeyTree('SOFTWARE\Windhawk\Engine\Mods\local@translucent-windows', $false)
#                   then restart Explorer. Or: reg import <mods-before-translucent-*.reg> written below.
$s = $PSScriptRoot
$out = "$s\install-translucent-result.txt"
try {
  $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
  if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'run elevated' }

  $id = 'local@translucent-windows'; $ver = '1.8.2.2'; $serial = '600002'
  $dll = "$($id)_$($ver)_$serial.dll"
  if (-not (Test-Path "$s\build\64\$dll")) { throw "missing build\64\$dll, run build-translucent.ps1 first" }

  # 1. Backup of every mod key, before anything is written.
  $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
  $backup = "$s\mods-before-translucent-$stamp.reg"
  & reg.exe export 'HKLM\SOFTWARE\Windhawk\Engine\Mods' $backup /y | Out-Null
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path $backup)) { throw 'reg export failed, nothing changed' }

  $hklm = [Microsoft.Win32.Registry]::LocalMachine
  $S = [Microsoft.Win32.RegistryValueKind]::String
  $D = [Microsoft.Win32.RegistryValueKind]::DWord
  $now = [int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()

  # 2. DLL (explorer.exe is 64-bit) and source. An installed serial is never overwritten.
  $to = "C:\ProgramData\Windhawk\Engine\Mods\64\$dll"
  if (-not (Test-Path $to)) { Copy-Item "$s\build\64\$dll" $to -Force }
  Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force

  # 3. Mod key, same layout as the installed explorer mods. A leftover Settings subkey is replaced.
  $base = "SOFTWARE\Windhawk\Engine\Mods\$id"
  $old = $hklm.OpenSubKey("$base\Settings")
  if ($old) { $old.Close(); $hklm.DeleteSubKeyTree("$base\Settings", $false) }
  $k = $hklm.CreateSubKey($base)
  $k.SetValue('LibraryFileName', $dll, $S)
  $k.SetValue('Disabled', 0, $D)
  $k.SetValue('Include', 'explorer.exe', $S)
  $k.SetValue('Exclude', '', $S)
  $k.SetValue('Architecture', 'x86-64', $S)
  $k.SetValue('Version', $ver, $S)
  $set = $k.CreateSubKey('Settings')
  $set.SetValue('RenderingMod.ThemeBackground', 1, $D)       # paints Win32 backgrounds as alpha-0 black and fixes text alpha
  $set.SetValue('RenderingMod.SysColors', 0, $D)             # never: SetSysColors is system-wide and outlives the mod
  $set.SetValue('RenderingMod.AccentColorControls', 0, $D)
  $set.SetValue('BackgroundEffects.type', 'transparent', $S) # local option: ACCENT_ENABLE_TRANSPARENTGRADIENT, AccentFlags 2
  $set.SetValue('BackgroundEffects.AccentBlurBehind', 'C4000000', $S)  # ARGB, Terminal's black at 77%
  $set.SetValue('FlyoutsEffects', 0, $D)                     # menus/tooltips stay with win32-ui-modernizer
  $set.SetValue('RuledPrograms[0].target', '', $S)           # no per-process rules (upstream default has a Notepad rule)
  $set.Close()
  $k.SetValue('SettingsChangeTime', $now, $D)
  $k.Close()

  "ok, backup: $backup" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
