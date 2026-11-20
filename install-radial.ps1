# Installs the local radial-window-switcher build (touchpad swipe). Run elevated.
# Undo: reg import D:\backups\windhawk-2026-11-06\windhawk-hklm-before-radial-touchpad.reg (old DLL 685934 is left in place).
$s = $PSScriptRoot
$out = "$s\install-radial-result.txt"
try {
  reg export "HKLM\SOFTWARE\Windhawk" "D:\backups\windhawk-2026-11-06\windhawk-hklm-before-radial-parity.reg" /y | Out-Null
  $id = 'radial-window-switcher'; $ver = '1.0.0'; $serial = '685937'; $dll = "$($id)_$($ver)_$serial.dll"
  Copy-Item "$s\build\64\$dll" "C:\ProgramData\Windhawk\Engine\Mods\64\$dll" -Force
  Copy-Item "$s\build\32\$dll" "C:\ProgramData\Windhawk\Engine\Mods\32\$dll" -Force
  Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force
  $key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$id"
  Set-ItemProperty "$key\Settings" -Name touchpadGesture -Value 1 -Type DWord
  Set-ItemProperty $key -Name LibraryFileName -Value $dll -Type String
  Set-ItemProperty $key -Name LoggingEnabled -Value 1 -Type DWord
  Set-ItemProperty $key -Name SettingsChangeTime -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
  "ok" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
