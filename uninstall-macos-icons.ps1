<#
uninstall-macos-icons.ps1 - undo install-macos-icons.ps1. RUN ELEVATED.
Reads state.json from the given (default: newest) icons-backup-* folder and:
  - restores every registry value it changed (themePaths[n] back to its old
    value or removed; SettingsChangeTime is bumped again, not rolled back, so
    running processes reload),
  - restores IconLocation on shortcuts changed by -LnkFallback (lnk-backup.csv),
  - deletes only the shortcuts the installer created,
  - removes the AppContainer read grant from the theme folder.
The theme folder and the backups are left in place. Restarts nothing.
Full fallback: reg import <backup>\windhawk-mods.reg (restores the whole Mods key
as it was before install; then bump SettingsChangeTime).
#>
param([string]$BackupDir)
$ErrorActionPreference = 'Stop'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run elevated (Start-Process powershell -Verb RunAs).' }
if (-not $BackupDir) {
    $BackupDir = Get-ChildItem 'D:\backups\windhawk-2026-11-06' -Directory -Filter 'icons-backup-*' |
        Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
}
$stateFile = Join-Path $BackupDir 'state.json'
if (-not (Test-Path $stateFile)) { throw "No state.json in '$BackupDir'." }
$state = Get-Content -Raw $stateFile | ConvertFrom-Json
Write-Host "Undoing from $BackupDir"

$hklm   = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
$modKey = $hklm.OpenSubKey('SOFTWARE\Windhawk\Engine\Mods\icon-resource-redirect', $true)
if (-not $modKey) { throw 'icon-resource-redirect key not found.' }
$setKey = $modKey.OpenSubKey('Settings', $true)

# Registry values (bracketed names: .NET methods only, PowerShell treats [] as wildcards).
foreach ($v in $state.RegistryValues) {
    if ($v.Name -like '..\*') { continue }   # SettingsChangeTime: handled below
    if ($v.Existed) { $setKey.SetValue($v.Name, [string]$v.Old, 'String'); Write-Host "restored $($v.Name) = '$($v.Old)'" }
    else { $setKey.DeleteValue($v.Name, $false); Write-Host "removed $($v.Name)" }
}

# Shortcut IconLocations.
$wsh = New-Object -ComObject WScript.Shell
if ($state.LnkBackup -and (Test-Path $state.LnkBackup)) {
    foreach ($r in Import-Csv $state.LnkBackup) {
        if (-not (Test-Path -LiteralPath $r.Path)) { Write-Warning "gone: $($r.Path)"; continue }
        $l = $wsh.CreateShortcut($r.Path); $l.IconLocation = $r.IconLocation; $l.Save()
        Write-Host "restored icon: $($r.Path)"
    }
}

# Shortcuts we created.
foreach ($p in @($state.Shortcuts)) {
    if ($p -and (Test-Path -LiteralPath $p)) { Remove-Item -LiteralPath $p -Force; Write-Host "deleted $p" }
}
$startDir = Join-Path ([Environment]::GetFolderPath('Programs')) 'macOS Icons'
if ((Test-Path $startDir) -and -not (Get-ChildItem $startDir -Force)) { Remove-Item $startDir }

if ($state.AclGranted -and (Test-Path $state.ThemeDir)) {
    & icacls.exe $state.ThemeDir /remove:g '*S-1-15-2-1' '*S-1-15-2-2' | Out-Null
}

$modKey.SetValue('SettingsChangeTime', [int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds(), 'DWord')
Write-Host 'Done. Refresh with ie4uinit.exe -show (as your user); Explorer/dock/Start drop cached icons on the next Explorer restart or sign-out.'
