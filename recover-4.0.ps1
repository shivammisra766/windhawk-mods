$out = "$PSScriptRoot\recover-result.txt"
function Log($t) { "$(Get-Date -Format HH:mm:ss) $t" | Out-File $out -Append }
Remove-Item $out -ErrorAction SilentlyContinue
$mod = 'SOFTWARE\Windhawk\Engine\Mods\windows-11-taskbar-styler'
$key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("$mod\Settings", $true)
$removed = 0
foreach ($n in @($key.GetValueNames())) {
  if ($n -match '^controlStyles\[(\d+)\]\.target$' -and $key.GetValue($n) -in 'Grid#ConfirmatorMainGrid', 'Grid#VolumeConfirmator', 'Grid#BrightnessConfirmator') {
    $i = $Matches[1]
    foreach ($v in @($key.GetValueNames())) { if ($v.StartsWith("controlStyles[$i].")) { $key.DeleteValue($v, $false) } }
    $removed++
  }
}
$key.Close()
Log "removed rules: $removed"
Set-ItemProperty "HKLM:\$mod" -Name SettingsChangeTime -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
$t = Get-Date
Start-Sleep -Seconds 30
$c = @(Get-WinEvent -FilterHashtable @{LogName='Application';Id=1000;StartTime=$t.AddSeconds(8);ProviderName='Application Error'} -ErrorAction SilentlyContinue | Where-Object { $_.Message -match 'Explorer\.EXE' }).Count
Log "explorer crashes in 22 s after removal: $c"
if ($c -gt 0) {
  reg import "D:\backups\windhawk-2026-11-06\windhawk-hklm-before-4.0.reg" 2>$null
  Set-ItemProperty "HKLM:\$mod" -Name SettingsChangeTime -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
  Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
  Log 'restored full backup'
}
Log 'done'
