param([string]$ver, [string]$serial)
# Windows restarts Start's host by itself, and it loads the new DLL.
$s = $PSScriptRoot
$out = "$s\install-pop-result.txt"
try {
  $id = 'local@start-menu-pop'; $dll = "$($id)_$($ver)_$serial.dll"
  $key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$id"
  Copy-Item "$s\build\64\$dll" "C:\ProgramData\Windhawk\Engine\Mods\64\$dll" -Force
  Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force
  Set-ItemProperty $key -Name LibraryFileName -Value $dll -Type String
  Set-ItemProperty $key -Name Version -Value $ver -Type String
  Set-ItemProperty $key -Name SettingsChangeTime -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
  Stop-Process -Name StartMenuExperienceHost -Force -ErrorAction SilentlyContinue
  "ok" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
