# Installs the radial-window-switcher build with Lookup (three-finger tap). Run elevated.
# Undo: reg import D:\backups\windhawk-2026-11-06\windhawk-hklm-before-lookup-<serial>.reg (older DLLs are left in place;
# the first Lookup install's backup is windhawk-hklm-before-lookup.reg). Windows has no Hindi OCR pack, so none is installed.
$s = $PSScriptRoot
$out = "$s\install-lookup-result.txt"
$log = @()
try {
  $id = 'radial-window-switcher'; $ver = '1.0.0'; $serial = '685941'; $dll = "$($id)_$($ver)_$serial.dll"
  reg export "HKLM\SOFTWARE\Windhawk" "D:\backups\windhawk-2026-11-06\windhawk-hklm-before-lookup-$serial.reg" /y | Out-Null
  if (-not (Test-Path "D:\backups\windhawk-2026-11-06\windhawk-hklm-before-lookup-$serial.reg")) { throw "registry backup failed" }
  Copy-Item "$s\build\64\$dll" "C:\ProgramData\Windhawk\Engine\Mods\64\$dll" -Force
  Copy-Item "$s\build\32\$dll" "C:\ProgramData\Windhawk\Engine\Mods\32\$dll" -Force
  Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force
  $key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$id"
  Set-ItemProperty "$key\Settings" -Name lookupTap -Value 1 -Type DWord
  Set-ItemProperty $key -Name LibraryFileName -Value $dll -Type String
  Set-ItemProperty $key -Name SettingsChangeTime -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
  $log += "mod: ok $dll"
} catch { $log += "mod ERROR: $($_.Exception.Message)" }
$log | Out-File $out
