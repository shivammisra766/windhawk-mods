# Turns Memory Integrity (HVCI) back on. Takes effect after a restart. Run elevated.
# It was on until 2026-10-03, when it was turned off for a benchmark that showed no gain.
# Undo: Windows Security > Device security > Core isolation > Memory integrity > Off, then restart.
$out = "$PSScriptRoot\enable-memory-integrity-result.txt"
try {
  $k = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity'
  reg export 'HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard' "D:\backups\deviceguard-before-hvci-on.reg" /y | Out-Null
  if (-not (Test-Path $k)) { New-Item $k -Force | Out-Null }
  Set-ItemProperty $k -Name Enabled -Value 1 -Type DWord
  Set-ItemProperty $k -Name WasEnabledBy -Value 2 -Type DWord
  "ok: Enabled=" + (Get-ItemProperty $k).Enabled | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
