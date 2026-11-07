# Elevated crash test for local@translucent-windows 1.8.2.2 in explorer.exe.
#   1. runs install-translucent.ps1 (reg export backup first, then the mod key only)
#   2. restarts Explorer, waits 30 s, counts explorer.exe crashes (Application log, Id 1000) from restart+4 s
#      (the forced kill itself can log one CoreMessaging crash, hence "more than 1")
#   3. more than 1 crash: deletes the mod key outright (DeleteSubKeyTree, not Disabled), restarts Explorer,
#      waits 30 s and logs the crash count again.
# Log: test-translucent-result.txt next to this script. The mod is included in explorer.exe only, so it is
# never loaded into this PowerShell and removing it cannot kill this script.
$s = $PSScriptRoot
$log = "$s\test-translucent-result.txt"
$modKey = 'SOFTWARE\Windhawk\Engine\Mods\local@translucent-windows'
"test-translucent $(Get-Date -Format 's')" | Out-File $log

function Log($m) { "$(Get-Date -Format 'HH:mm:ss') $m" | Out-File $log -Append }

function Restart-Explorer {
  Get-Process explorer -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  $t = Get-Date
  # Winlogon restarts the shell on its own (AutoRestartShell). Fallback: start it un-elevated.
  for ($i = 0; $i -lt 10 -and -not (Get-Process explorer -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 500 }
  if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) {
    Log 'explorer did not come back on its own, starting it with a basic-user token'
    & runas.exe /trustlevel:0x20000 explorer.exe | Out-Null
  }
  return $t
}

function Get-ExplorerCrashes($since) {
  @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $since } -ErrorAction SilentlyContinue |
    Where-Object { $_.Message -match 'Faulting application name: explorer\.exe' })
}

function Log-Crashes($crashes) {
  foreach ($c in $crashes) {
    $m = $c.Message
    $mod = [regex]::Match($m, 'Faulting module name: ([^,]+)').Groups[1].Value
    $code = [regex]::Match($m, 'Exception code: (\S+)').Groups[1].Value
    $off = [regex]::Match($m, 'Fault offset: (\S+)').Groups[1].Value
    Log ("   crash {0:HH:mm:ss} {1} {2} {3}" -f $c.TimeCreated, $mod, $code, $off)
  }
}

function Log-ModLoaded {
  # Evidence for the report: is the mod DLL actually inside the running Explorer(s)?
  foreach ($p in Get-Process explorer -ErrorAction SilentlyContinue) {
    try {
      $m = @($p.Modules | Where-Object { $_.ModuleName -match 'translucent' } | ForEach-Object { $_.ModuleName })
      Log ("   explorer pid {0}: translucent loaded = {1} {2}" -f $p.Id, ($m.Count -gt 0), ($m -join ' '))
    } catch { Log "   explorer pid $($p.Id): modules not readable ($($_.Exception.Message))" }
  }
}

try {
  $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
  if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'run elevated' }

  # 1. Install.
  & "$s\install-translucent.ps1"
  $res = (Get-Content "$s\install-translucent-result.txt" -Raw).Trim()
  Log "install: $res"
  if ($res -notmatch '^ok') { throw 'install failed, nothing restarted' }

  # 2. Restart Explorer and watch it.
  $t1 = Restart-Explorer
  Log 'explorer restarted with the mod, waiting 30 s'
  Start-Sleep -Seconds 30
  $c1 = Get-ExplorerCrashes $t1.AddSeconds(4)
  Log "crashes since restart+4s: $($c1.Count)"
  Log-Crashes $c1
  Log-ModLoaded

  # 3. Remove the mod if Explorer is crashing.
  if ($c1.Count -gt 1) {
    Log 'more than 1 crash: deleting the mod key'
    [Microsoft.Win32.Registry]::LocalMachine.DeleteSubKeyTree($modKey, $false)
    $gone = $null -eq [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($modKey)
    Log "mod key deleted: $gone"
    $t2 = Restart-Explorer
    Log 'explorer restarted without the mod, waiting 30 s'
    Start-Sleep -Seconds 30
    $c2 = Get-ExplorerCrashes $t2.AddSeconds(4)
    Log "crashes since second restart+4s: $($c2.Count)"
    Log-Crashes $c2
    Log-ModLoaded
    Log 'RESULT: mod removed'
  } else {
    Log 'RESULT: mod kept (0-1 crashes)'
  }
} catch { Log "ERROR: $($_.Exception.Message)" }
