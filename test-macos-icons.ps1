<#
test-macos-icons.ps1 - guarded trial of the macOS app-icon theme. RUN ELEVATED.

  1. reg export of HKLM\SOFTWARE\Windhawk\Engine\Mods (safety copy).
  2. themePaths[0] = <ThemeDir>\<ThemeIni> (default: theme-minimal.ini = VS Code,
     Mattermost, VLC only), SettingsChangeTime bumped.
  3. Restarts Explorer (Windows' AutoRestartShell brings it back; if it does not,
     it is started de-elevated with runas /trustlevel).
  4. Waits 30 s, counts explorer.exe crashes (Application Error 1000) from
     restart + 4 s on.
  5. If more than 1: puts themePaths[0] back to its previous value, bumps
     SettingsChangeTime, waits 30 s and logs whether the crashes stopped.
Does not run ie4uinit (user-level; run it yourself afterwards).
Log: test-macos-icons-result.txt next to this script.
#>
param(
    [string]$ThemeDir = 'D:\backups\windhawk-2026-11-06\icon-theme-macos',
    [string]$ThemeIni = 'theme-minimal.ini',
    [int]$WaitSeconds = 30
)
$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot 'test-macos-icons-result.txt'
function Log([string]$m) { $line = "{0:HH:mm:ss} {1}" -f (Get-Date), $m; Write-Host $line; Add-Content -LiteralPath $log -Value $line -Encoding UTF8 }

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run elevated.' }
$themePath = Join-Path $ThemeDir $ThemeIni
if (-not (Test-Path $themePath)) { throw "missing $themePath" }
if (Select-String -LiteralPath $themePath -Pattern '^[^;]*\\explorer\.exe=' -Quiet) { throw "$ThemeIni redirects explorer.exe; refusing." }

Log "=== test start: $themePath"
Log ("entries: " + ((Get-Content $themePath | Where-Object { $_ -match '=' -and $_ -notmatch '^;' }) -join ' | '))

$backup = "D:\backups\windhawk-2026-11-06\icons-test-backup-$(Get-Date -Format yyyyMMdd-HHmmss).reg"
& reg.exe export 'HKLM\SOFTWARE\Windhawk\Engine\Mods' $backup /y | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'reg export failed; nothing changed.' }
Log "backup: $backup"

$hklm   = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
$modKey = $hklm.OpenSubKey('SOFTWARE\Windhawk\Engine\Mods\icon-resource-redirect', $true)
$setKey = $modKey.OpenSubKey('Settings', $true)
$slot   = 'themePaths[0]'                         # .NET only: [] are wildcards in PowerShell
$old    = [string]$setKey.GetValue($slot, '')
function Bump { $modKey.SetValue('SettingsChangeTime', [int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds(), 'DWord') }
function Count-Crashes([datetime]$since) {
    $ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = 'Application Error'; Id = 1000; StartTime = $since } -ErrorAction SilentlyContinue |
        Where-Object { $_.Properties.Count -gt 0 -and [string]$_.Properties[0].Value -ieq 'explorer.exe' })
    foreach ($e in $ev) { Log ("  crash {0:HH:mm:ss}  module {1}  code {2}" -f $e.TimeCreated, $e.Properties[3].Value, $e.Properties[6].Value) }
    return $ev.Count
}
function Wait-Explorer([int]$timeout) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $timeout) {
        $p = Get-Process explorer -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq (Get-Process -Id $PID).SessionId }
        if ($p) { return $p }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

Log "old $slot = '$old'"
$setKey.SetValue($slot, $themePath, 'String'); Bump
Log "set $slot, bumped SettingsChangeTime"

Log 'restarting Explorer'
Get-Process explorer -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
$restart = Get-Date
if (-not (Wait-Explorer 15)) {
    Log 'Explorer did not auto-restart; starting it de-elevated'
    & runas.exe /trustlevel:0x20000 "$env:SystemRoot\explorer.exe"
    if (-not (Wait-Explorer 15)) { Log 'WARNING: Explorer still not running' }
}
Log "explorer pid(s): $((Get-Process explorer -ErrorAction SilentlyContinue).Id -join ',')"

Start-Sleep -Seconds $WaitSeconds
$crashes = Count-Crashes ($restart.AddSeconds(4))
Log "explorer crashes since restart+4s: $crashes"

if ($crashes -gt 1) {
    Log "FAIL - rolling back $slot to '$old'"
    $setKey.SetValue($slot, $old, 'String'); Bump
    $rb = Get-Date
    if (-not (Wait-Explorer 15)) { & runas.exe /trustlevel:0x20000 "$env:SystemRoot\explorer.exe" }
    Start-Sleep -Seconds $WaitSeconds
    $after = Count-Crashes ($rb.AddSeconds(4))
    Log "after rollback: $after crash(es) in $WaitSeconds s; explorer pid(s): $((Get-Process explorer -ErrorAction SilentlyContinue).Id -join ',')"
} else {
    Log "PASS - $slot stays '$themePath'. Next: run ie4uinit.exe -show as your user."
}
Log '=== test end'
