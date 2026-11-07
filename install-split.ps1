param([string]$ver, [string]$serial)
$s = $PSScriptRoot
$out = "$s\install-split-result.txt"
try {
  $id = 'local@panel-split'; $dll = "$($id)_$($ver)_$serial.dll"
  $key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$id"
  Copy-Item "$s\build\64\$dll" "C:\ProgramData\Windhawk\Engine\Mods\64\$dll" -Force
  Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force
  Set-ItemProperty $key -Name LibraryFileName -Value $dll -Type String
  Set-ItemProperty $key -Name Version -Value $ver -Type String
  Set-ItemProperty $key -Name SettingsChangeTime -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
  # Windows restarts this host by itself, and it loads the new DLL. No service restart,
  # because restarting the Windhawk service has crashed Explorer.
  Stop-Process -Name ShellExperienceHost -Force -ErrorAction SilentlyContinue
  Stop-Process -Name ShellHost -Force -ErrorAction SilentlyContinue
  "ok" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
