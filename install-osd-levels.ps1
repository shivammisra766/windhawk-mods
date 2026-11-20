# Volume/brightness fix: tray-bridge 1.12 (hooks the native popup from the start instead of giving up after
# 10 minutes) and bar 4.12 (slider follows volume steps live, brightness read in a short burst). Run elevated.
# No Explorer restart: Windhawk reloads tray-bridge in place when LibraryFileName changes.
# Undo: reg import D:\backups\windhawk-2026-11-06\windhawk-hklm-before-osd-levels.reg, then restart the bar.
$s = $PSScriptRoot
$out = "$s\install-osd-levels-result.txt"
$log = New-Object System.Collections.Generic.List[string]
try {
  reg export "HKLM\SOFTWARE\Windhawk" "D:\backups\windhawk-2026-11-06\windhawk-hklm-before-osd-levels.reg" /y | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "reg export failed ($LASTEXITCODE)" }

  # tray-bridge (explorer.exe, 64-bit)
  $id = 'local@tray-bridge'; $ver = '1.12'; $serial = '500013'; $dll = "$($id)_$($ver)_$serial.dll"
  $key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$id"
  Copy-Item "$s\build\64\$dll" "C:\ProgramData\Windhawk\Engine\Mods\64\$dll" -Force
  Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force
  Set-ItemProperty $key -Name LoggingEnabled -Value 1 -Type DWord
  Set-ItemProperty $key -Name LibraryFileName -Value $dll -Type String
  Set-ItemProperty $key -Name Version -Value $ver -Type String
  Set-ItemProperty $key -Name SettingsChangeTime -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
  $log.Add("installed $dll")

  # bar (tool process, 32 and 64-bit)
  $id = 'local@top-bar'; $ver = '4.12'; $serial = '300052'; $dll = "$($id)_$($ver)_$serial.dll"
  $base = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$id"
  foreach ($a in '32', '64') { Copy-Item "$s\build\$a\$dll" "C:\ProgramData\Windhawk\Engine\Mods\$a\$dll" -Force }
  Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force
  Set-ItemProperty $base -Name LibraryFileName -Value $dll -Type String
  Set-ItemProperty $base -Name Version -Value $ver -Type String
  Set-ItemProperty $base -Name SettingsChangeTime -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
  $log.Add("installed $dll")
  $bar = @(Get-CimInstance Win32_Process -Filter "Name='windhawk.exe'" | Where-Object { $_.CommandLine -match '-tool-mod\s+"?local@top-bar"?\s*$' })
  foreach ($p in $bar) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Seconds 2
  $user = (Get-CimInstance Win32_ComputerSystem).UserName
  $task = 'WindhawkTopBarLaunch'
  Register-ScheduledTask -TaskName $task -Force `
    -Action (New-ScheduledTaskAction -Execute 'C:\Program Files\Windhawk\windhawk.exe' -Argument '-tool-mod "local@top-bar"') `
    -Principal (New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited) | Out-Null
  Start-ScheduledTask -TaskName $task
  Start-Sleep -Seconds 3
  Unregister-ScheduledTask -TaskName $task -Confirm:$false
  $now2 = @(Get-CimInstance Win32_Process -Filter "Name='windhawk.exe'" | Where-Object { $_.CommandLine -match 'local@top-bar' })
  $log.Add("bar: stopped $($bar.Count), running now $($now2.Count)")
  $log.Add("ok")
} catch { $log.Add("ERROR: $($_.Exception.Message)") }
$log | Out-File $out
