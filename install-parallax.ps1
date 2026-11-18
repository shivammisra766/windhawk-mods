# Installs local@parallax-wallpaper-idle (32 and 64-bit tool mod) with the settings of the installed parallax-wallpaper,
# then disables the original. Does not restart Explorer or the Windhawk service. Run elevated.
# Undo: reg import the backup below (or set local@parallax-wallpaper-idle Disabled=1 and parallax-wallpaper Disabled=0,
# bumping SettingsChangeTime on both).
$s = $PSScriptRoot
$out = "$s\install-parallax-result.txt"
try {
  reg export "HKLM\SOFTWARE\Windhawk" "D:\backups\windhawk-2026-11-06\windhawk-hklm-before-parallax-idle.reg" /y | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "reg export failed ($LASTEXITCODE)" }
  $id = 'local@parallax-wallpaper-idle'; $ver = '0.9.2.1'; $serial = '800001'; $dll = "$($id)_$($ver)_$serial.dll"
  $orig = 'parallax-wallpaper'
  $base = 'SOFTWARE\Windhawk\Engine\Mods'
  $now = [int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
  $hklm = [Microsoft.Win32.Registry]::LocalMachine

  # windhawk.exe is 32-bit, so a tool mod needs both architectures. An installed serial is locked and never changes.
  foreach ($a in '32', '64') {
    $to = "C:\ProgramData\Windhawk\Engine\Mods\$a\$dll"
    if (-not (Test-Path $to)) { Copy-Item "$s\build\$a\$dll" $to -Force }
  }
  Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force

  # Copy every setting of the original with its registry type (thresholdPercent=-1 is a DWORD 0xFFFFFFFF).
  $src = $hklm.OpenSubKey("$base\$orig\Settings")
  if (-not $src) { throw "no settings for $orig" }
  $key = $hklm.CreateSubKey("$base\$id")
  $key.SetValue('Disabled', 1, 'DWord')  # Stays off until the settings are in place.
  $dst = $hklm.CreateSubKey("$base\$id\Settings")
  foreach ($n in $src.GetValueNames()) {
    $dst.SetValue($n, $src.GetValue($n, $null, 'DoNotExpandEnvironmentNames'), $src.GetValueKind($n))
  }
  # New in the fork. A missing value reads as 0 (off), so write the declared default.
  if ($src.GetValueNames() -notcontains 'pauseMaximized') { $dst.SetValue('pauseMaximized', 1, 'DWord') }
  $copied = $src.GetValueNames().Count
  $src.Close(); $dst.Close()

  $key.SetValue('LibraryFileName', $dll, 'String')
  $key.SetValue('Include', 'windhawk.exe', 'String')
  $key.SetValue('Exclude', '', 'String')
  $key.SetValue('Architecture', '', 'String')
  $key.SetValue('Version', $ver, 'String')
  $key.SetValue('LoggingEnabled', 1, 'DWord')
  $key.SetValue('Disabled', 0, 'DWord')
  $key.SetValue('SettingsChangeTime', $now, 'DWord')
  $key.Close()

  $o = $hklm.OpenSubKey("$base\$orig", $true)
  $o.SetValue('Disabled', 1, 'DWord')
  $o.SetValue('SettingsChangeTime', $now, 'DWord')
  $o.Close()

  # Windhawk unloads the original (its tool process exits) and its launcher normally starts the new tool process.
  $exe = 'C:\Program Files\Windhawk\windhawk.exe'
  function ToolProcs($name) {
    @(Get-CimInstance Win32_Process -Filter "Name='windhawk.exe'" | Where-Object { $_.CommandLine -match ('-tool-mod\s+"?' + [regex]::Escape($name) + '"?\s*$') })
  }
  Start-Sleep -Seconds 5
  $stale = ToolProcs $orig
  foreach ($p in $stale) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
  $started = 'by Windhawk'
  if ((ToolProcs $id).Count -eq 0) {
    # Launch unelevated as the signed-in user, like the bar: windhawk.exe -tool-mod "<id>".
    # A one-shot scheduled task with RunLevel Limited gives the normal medium-integrity token from this elevated shell.
    # A second instance would just exit: the tool mod holds a per-id mutex.
    $user = (Get-CimInstance Win32_ComputerSystem).UserName
    $task = 'WindhawkParallaxIdleLaunch'
    $action = New-ScheduledTaskAction -Execute $exe -Argument "-tool-mod `"$id`""
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Force | Out-Null
    Start-ScheduledTask -TaskName $task
    Start-Sleep -Seconds 3
    Unregister-ScheduledTask -TaskName $task -Confirm:$false
    $started = 'by install script'
  }
  $running = (ToolProcs $id).Count
  "ok; settings copied=$copied; original procs stopped=$($stale.Count); new tool started $started; running=$running" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
