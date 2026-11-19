# Bar 4.11: keeps the bar on top through Show Desktop (Win+D / three-finger swipe down). Run elevated.
# Undo: reg import D:\backups\windhawk-2026-11-06\windhawk-hklm-before-bar-4.11.reg, then restart the bar.
$s = $PSScriptRoot
$out = "$s\install-bar-4.11-result.txt"
$log = New-Object System.Collections.Generic.List[string]
try {
  reg export "HKLM\SOFTWARE\Windhawk" "D:\backups\windhawk-2026-11-06\windhawk-hklm-before-bar-4.11.reg" /y | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "reg export failed ($LASTEXITCODE)" }
  $id = 'local@top-bar'; $ver = '4.11'; $serial = '300051'
  $base = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$id"
  $dll = "$($id)_$($ver)_$serial.dll"
  foreach ($a in '32', '64') { $to = "C:\ProgramData\Windhawk\Engine\Mods\$a\$dll"; if (-not (Test-Path $to)) { Copy-Item "$s\build\$a\$dll" $to -Force } }
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
