$s = $PSScriptRoot
$out = "$s\install-zoom-result.txt"
try {
  $id = 'local@edge-fullscreen-zoom'; $ver = '1.2'; $serial = '500003'; $dll = "$($id)_$($ver)_$serial.dll"
  $key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$id"
  Copy-Item "$s\build\64\$dll" "C:\ProgramData\Windhawk\Engine\Mods\64\$dll" -Force
  Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force
  New-Item $key -Force | Out-Null
  New-Item "$key\Settings" -Force | Out-Null
  Set-ItemProperty $key -Name LibraryFileName -Value $dll -Type String
  Set-ItemProperty $key -Name Disabled -Value 0 -Type DWord
  Set-ItemProperty $key -Name Include -Value 'msedge.exe' -Type String
  Set-ItemProperty $key -Name Exclude -Value '' -Type String
  Set-ItemProperty $key -Name Architecture -Value 'x86-64' -Type String
  Set-ItemProperty $key -Name Version -Value $ver -Type String
  Set-ItemProperty "$key\Settings" -Name duration -Value 360 -Type DWord
  Set-ItemProperty $key -Name LoggingEnabled -Value 1 -Type DWord
  Set-ItemProperty $key -Name SettingsChangeTime -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
  "ok" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
