# Optimization round 2026-11-18. Run elevated. Does not restart Explorer or the Windhawk service.
#  1. src\install-cleanup.ps1 (dump trap + old dumps removed, logging off in 4 mods; it makes its own backups)
#  2. wt\parallax\install-parallax.ps1 (local@parallax-wallpaper-idle replaces parallax-wallpaper; own backup)
#  3. bar 4.9 (32+64) and start-menu-pop 2.5 (64), then the bar process is restarted unelevated.
# Undo for step 3: reg import D:\backups\windhawk-2026-11-06\windhawk-hklm-before-optimize.reg, then restart the bar.
$s = $PSScriptRoot
$out = "$s\install-optimize-result.txt"
$log = New-Object System.Collections.Generic.List[string]
try {
  reg export "HKLM\SOFTWARE\Windhawk" "D:\backups\windhawk-2026-11-06\windhawk-hklm-before-optimize.reg" /y | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "reg export failed ($LASTEXITCODE)" }

  powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File 'D:\backups\windhawk-2026-11-06\src\install-cleanup.ps1' | Out-Null
  $log.Add("cleanup: " + ((Get-Content 'D:\backups\windhawk-2026-11-06\src\install-cleanup-result.txt') -join ' | '))
  powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File 'D:\backups\windhawk-2026-11-06\wt\parallax\install-parallax.ps1' | Out-Null
  $log.Add("parallax: " + (Get-Content 'D:\backups\windhawk-2026-11-06\wt\parallax\install-parallax-result.txt'))

  $base = 'HKLM:\SOFTWARE\Windhawk\Engine\Mods'
  $now = [int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
  function Install($id, $ver, $serial, $arches) {
    $dll = "$($id)_$($ver)_$serial.dll"
    foreach ($a in $arches) { $to = "C:\ProgramData\Windhawk\Engine\Mods\$a\$dll"; if (-not (Test-Path $to)) { Copy-Item "$s\build\$a\$dll" $to -Force } }  # An installed serial is locked.
    Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force
    Set-ItemProperty "$base\$id" -Name LibraryFileName -Value $dll -Type String
    Set-ItemProperty "$base\$id" -Name Version -Value $ver -Type String
    Set-ItemProperty "$base\$id" -Name SettingsChangeTime -Value $now -Type DWord
    $log.Add("installed $dll")
  }
  Install 'local@start-menu-pop' '2.5' '200019' @('64')
  Install 'local@top-bar' '4.9' '300049' @('32', '64')

  # Restart the bar unelevated so it loads 4.9 (a one-shot task with RunLevel Limited gives the user's normal token).
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
