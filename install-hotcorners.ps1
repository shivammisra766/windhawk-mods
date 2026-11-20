# Installs local@hot-corners as a new Windhawk tool mod. Run elevated.
# Undo: Windhawk > Hot corners > Disable (or delete HKLM\SOFTWARE\Windhawk\Engine\Mods\local@hot-corners).
$s = $PSScriptRoot
$out = "$s\install-hotcorners-result.txt"
try {
  reg export "HKLM\SOFTWARE\Windhawk" "D:\backups\windhawk-2026-11-06\windhawk-hklm-before-hotcorners.reg" /y | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "reg export failed ($LASTEXITCODE)" }
  $id = 'local@hot-corners'; $ver = '1.0'; $serial = '900001'; $dll = "$($id)_$($ver)_$serial.dll"
  foreach ($a in '32', '64') { Copy-Item "$s\build\$a\$dll" "C:\ProgramData\Windhawk\Engine\Mods\$a\$dll" -Force }
  Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force
  $key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$id"
  New-Item "$key\Settings" -Force | Out-Null
  Set-ItemProperty "$key\Settings" -Name topLeft -Value 'taskView' -Type String
  Set-ItemProperty "$key\Settings" -Name topRight -Value 'notifications' -Type String
  Set-ItemProperty "$key\Settings" -Name bottomLeft -Value 'start' -Type String
  Set-ItemProperty "$key\Settings" -Name bottomRight -Value 'desktop' -Type String
  Set-ItemProperty "$key\Settings" -Name dwellMs -Value 250 -Type DWord
  Set-ItemProperty $key -Name Include -Value 'windhawk.exe' -Type String
  Set-ItemProperty $key -Name Exclude -Value '' -Type String
  Set-ItemProperty $key -Name Architecture -Value '' -Type String
  Set-ItemProperty $key -Name Version -Value $ver -Type String
  Set-ItemProperty $key -Name LoggingEnabled -Value 1 -Type DWord
  Set-ItemProperty $key -Name Disabled -Value 0 -Type DWord
  Set-ItemProperty $key -Name SettingsChangeTime -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
  Set-ItemProperty $key -Name LibraryFileName -Value $dll -Type String
  Start-Sleep -Seconds 5
  $p = @(Get-CimInstance Win32_Process -Filter "Name='windhawk.exe'" | Where-Object { $_.CommandLine -match 'local@hot-corners' })
  "ok: installed $dll, tool process running: $($p.Count)" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
