# panel-split 1.15: the notification panel goes back to its native place as soon as it closes. Run elevated.
# Undo: reg import D:\backups\windhawk-2026-11-06\windhawk-hklm-before-split-1.15.reg (Windows restarts the hosts).
$s = $PSScriptRoot
$backup = "D:\backups\windhawk-2026-11-06\windhawk-hklm-before-split-1.15.reg"
if (-not (Test-Path $backup)) {  # Keep the first one: it is the state before 1.15.
  reg export "HKLM\SOFTWARE\Windhawk" $backup /y | Out-Null
  if ($LASTEXITCODE -ne 0) { "ERROR: reg export failed" | Out-File "$s\install-split-result.txt"; exit 1 }
}
Set-ItemProperty 'HKLM:\SOFTWARE\Windhawk\Engine\Mods\local@panel-split' -Name LoggingEnabled -Value 1 -Type DWord
& "$s\install-split.ps1" -ver '1.15' -serial '400019'
