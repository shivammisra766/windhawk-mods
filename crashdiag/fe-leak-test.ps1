# Opens N File Explorer windows in the main Explorer, browses a few folders in each, closes them,
# and logs the shell Explorer's private memory. Run as the signed-in user (not from a sandboxed shell).
param([int]$Windows = 6, [string]$Out = 'D:\backups\windhawk-2026-11-06\fe-leak-test.txt', [string]$Tag = '')
Add-Type -Namespace X -Name W -MemberDefinition '[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);'
$log = New-Object System.Collections.Generic.List[string]
function Shell { Get-Process explorer | Sort-Object StartTime | Select-Object -First 1 }
function Snap($t) { $p = Shell; $n = @(Get-Process explorer).Count; $log.Add(("{0} {1} {2,-10} priv {3:N1} MB handles {4} threads {5} explorers {6}" -f (Get-Date -Format HH:mm:ss), $Tag, $t, ($p.PrivateMemorySize64 / 1MB), $p.HandleCount, $p.Threads.Count, $n)) }
$sh = New-Object -ComObject Shell.Application
$home_ = 'shell:::{f874310e-b6b7-47dc-bc84-b9e6b38f5903}'
$targets = $home_, "$env:USERPROFILE\Downloads", 'C:\Windows\System32', "$env:USERPROFILE\Pictures", $home_
Snap 'before'
for ($w = 1; $w -le $Windows; $w++) {
  $have = @($sh.Windows() | ForEach-Object { $_.HWND })
  Start-Process explorer.exe $home_
  $new = $null
  for ($t = 0; $t -lt 50 -and -not $new; $t++) { Start-Sleep -Milliseconds 200; $new = $sh.Windows() | Where-Object { $have -notcontains $_.HWND } | Select-Object -First 1 }
  if (-not $new) { $log.Add("window $w did not register"); continue }
  Start-Sleep 3
  foreach ($tg in $targets) { try { $new.Navigate2($tg) } catch {}; Start-Sleep 3 }
  [void][X.W]::PostMessage([IntPtr]$new.HWND, 0x10, [IntPtr]0, [IntPtr]0)
  Start-Sleep 2
  Snap "window $w"
}
Start-Sleep 30
Snap 'after 30s'
$log | Out-File $Out -Append
