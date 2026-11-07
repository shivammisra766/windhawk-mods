param([string]$ver, [string]$serial)
# Explorer doesn't pick up a new mod build until it starts again, and Windows restarts it.
$s = $PSScriptRoot
$out = "$s\install-bridge-result.txt"
try {
  $id = 'local@tray-bridge'; $dll = "$($id)_$($ver)_$serial.dll"
  $key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$id"
  Copy-Item "$s\build\64\$dll" "C:\ProgramData\Windhawk\Engine\Mods\64\$dll" -Force
  Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force
  Set-ItemProperty $key -Name LibraryFileName -Value $dll -Type String
  Set-ItemProperty $key -Name Version -Value $ver -Type String
  Set-ItemProperty $key -Name SettingsChangeTime -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
  Stop-Process -Name explorer -Force
  "ok" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
