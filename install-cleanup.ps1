# Post-investigation cleanup. Run elevated. No installs, no Explorer/Windhawk restarts.
#  1. Backs up HKLM\SOFTWARE\Windhawk and WER LocalDumps to .reg files.
#  2. Deletes WER LocalDumps\explorer.exe (the full-dump trap from the crash-loop investigation).
#  3. Deletes old Explorer dumps: CrashDumps\explorer.exe.*.dmp and the CrashDumps\full folder.
#  4. Sets LoggingEnabled=0 + bumps SettingsChangeTime for the four local mods.
# Result is written to install-cleanup-result.txt next to this script.
#
# Undo:
#  - Full-dump key and mod logging:
#      reg import D:\backups\windhawk-2026-11-06\localdumps-before-cleanup.reg
#      reg import D:\backups\windhawk-2026-11-06\windhawk-hklm-before-cleanup.reg
#    (elevated). The Windhawk import restores LoggingEnabled=1 but also the OLD SettingsChangeTime,
#    so running mods will not notice; bump SettingsChangeTime on each mod key (or restart Windhawk).
#  - The deleted .dmp files and the full\ folder cannot be restored (they were only crash evidence).
$ErrorActionPreference = 'Stop'
$backup = 'D:\backups\windhawk-2026-11-06'
$out = "$PSScriptRoot\install-cleanup-result.txt"
$log = New-Object System.Collections.Generic.List[string]
function Add-Log([string]$line) { $log.Add($line); Write-Host $line }
try {
  # 1. Backups first; abort if either export fails.
  reg export "HKLM\SOFTWARE\Windhawk" "$backup\windhawk-hklm-before-cleanup.reg" /y | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "reg export Windhawk failed ($LASTEXITCODE)" }
  reg export "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" "$backup\localdumps-before-cleanup.reg" /y | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "reg export LocalDumps failed ($LASTEXITCODE)" }
  Add-Log "backups: windhawk-hklm-before-cleanup.reg, localdumps-before-cleanup.reg"

  # 2. Remove the LocalDumps\explorer.exe key (RegistryKey method, no wildcard handling).
  $ldPath = 'SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps'
  $ld = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($ldPath, $true)
  if ($null -eq $ld) { Add-Log "LocalDumps key not present" }
  else {
    if ($ld.GetSubKeyNames() -contains 'explorer.exe') {
      $ld.DeleteSubKeyTree('explorer.exe')
      Add-Log "deleted HKLM\$ldPath\explorer.exe"
    } else { Add-Log "LocalDumps\explorer.exe already absent" }
    $ld.Close()
  }

  # 3. Old Explorer crash dumps: only these exact paths.
  $cd = 'C:\Users\shiva\AppData\Local\CrashDumps'
  $full = Join-Path $cd 'full'
  $files = @(Get-ChildItem -LiteralPath $cd -File -Force | Where-Object { $_.Name -like 'explorer.exe.*.dmp' })
  $fullItems = @()
  if (Test-Path -LiteralPath $full) { $fullItems = @(Get-ChildItem -LiteralPath $full -File -Force -Recurse) }
  $all = @($files) + @($fullItems)
  $bytes = ($all | Measure-Object -Property Length -Sum).Sum
  if ($null -eq $bytes) { $bytes = 0 }
  Add-Log ("to delete: {0} files, {1:N1} MB" -f $all.Count, ($bytes / 1MB))
  foreach ($f in $all) { Add-Log ("  {0}  {1:N1} MB" -f $f.FullName, ($f.Length / 1MB)) }
  foreach ($f in $files) { Remove-Item -LiteralPath $f.FullName -Force }
  if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force; Add-Log "removed folder $full" }
  Add-Log ("freed {0:N1} MB" -f ($bytes / 1MB))

  # 4. Mod logging off, with SettingsChangeTime bump so running mods reload it.
  $now = [int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
  foreach ($id in 'local@tray-bridge', 'local@edge-fullscreen-zoom', 'local@dark-shutdown-dialog', 'local@dark-nsis-installers') {
    $key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$id"
    if (-not (Test-Path -LiteralPath $key)) { Add-Log "$id : key missing, skipped"; continue }
    $before = (Get-ItemProperty -LiteralPath $key).LoggingEnabled
    Set-ItemProperty -LiteralPath $key -Name LoggingEnabled -Value 0 -Type DWord
    Set-ItemProperty -LiteralPath $key -Name SettingsChangeTime -Value $now -Type DWord
    Add-Log "$id : LoggingEnabled $before -> 0, SettingsChangeTime=$now"
  }
  Add-Log "ok"
} catch { Add-Log "ERROR: $($_.Exception.Message)" }
$log | Out-File $out
$nonInteractive = [Environment]::GetCommandLineArgs() | Where-Object { $_ -like '-NonI*' }
if ([Environment]::UserInteractive -and -not $nonInteractive) { Read-Host 'Press Enter to close' | Out-Null }
