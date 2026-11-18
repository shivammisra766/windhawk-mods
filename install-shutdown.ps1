# Installs local@dark-shutdown-dialog (Explorer only) and restarts Explorer so it loads. Run elevated.
# Undo: set HKLM\SOFTWARE\Windhawk\Engine\Mods\local@dark-shutdown-dialog Disabled=1 (or reg import the backup below).
$s = $PSScriptRoot
$out = "$s\install-shutdown-result.txt"
try {
  reg export "HKLM\SOFTWARE\Windhawk" "D:\backups\windhawk-2026-11-06\windhawk-hklm-before-dark-shutdown.reg" /y | Out-Null
  $id = 'local@dark-shutdown-dialog'; $ver = '1.0.2'; $serial = '700003'; $dll = "$($id)_$($ver)_$serial.dll"
  Copy-Item "$s\build\64\$dll" "C:\ProgramData\Windhawk\Engine\Mods\64\$dll" -Force
  Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force
  $key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$id"
  New-Item -Path $key -Force | Out-Null
  New-Item -Path "$key\Settings" -Force | Out-Null
  Set-ItemProperty $key -Name Disabled -Value 0 -Type DWord
  Set-ItemProperty $key -Name Include -Value 'explorer.exe' -Type String
  Set-ItemProperty $key -Name Exclude -Value '' -Type String
  Set-ItemProperty $key -Name Architecture -Value 'x86-64' -Type String
  Set-ItemProperty $key -Name LibraryFileName -Value $dll -Type String
  Set-ItemProperty $key -Name Version -Value $ver -Type String
  Set-ItemProperty $key -Name LoggingEnabled -Value 1 -Type DWord
  Set-ItemProperty $key -Name SettingsChangeTime -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
  Stop-Process -Name explorer -Force
  "ok" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
