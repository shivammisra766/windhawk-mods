param([string]$ver = '1.0', [string]$serial = '600010', [switch]$NoExplorerTabShift)
# Installs local@traffic-lights (build first with build-lights.ps1). Run elevated.
# Backs up HKLM\SOFTWARE\Windhawk\Engine\Mods with reg export before writing anything.
# Restarts nothing: apps started afterwards get the circles at once; Windhawk loads new mods into
# running processes on its own, and anything that misses them gets them on its next start
# (Explorer windows after the next Explorer restart, which is left to you).
# Undo (elevated):
#   reg delete "HKLM\SOFTWARE\Windhawk\Engine\Mods\local@traffic-lights" /f
#   then, if the Explorer tab shift was added: reg import <the backup this writes>, and bump
#   SettingsChangeTime of windows-11-file-explorer-styler (the import restores the old one).
# -NoExplorerTabShift skips the File Explorer styler rule that moves the tabs right of the circles.
$s = $PSScriptRoot
$out = "$s\install-traffic-lights-result.txt"
try {
  $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
  if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'run elevated' }

  $id = 'local@traffic-lights'; $dll = "$($id)_$($ver)_$serial.dll"
  foreach ($a in '32', '64') { if (-not (Test-Path "$s\build\$a\$dll")) { throw "missing build\$a\$dll, run build-lights.ps1" } }

  $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
  $backup = "D:\backups\windhawk-2026-11-06\windhawk-hklm-before-traffic-lights-$stamp.reg"
  & reg.exe export 'HKLM\SOFTWARE\Windhawk\Engine\Mods' $backup /y | Out-Null
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path $backup)) { throw 'reg export failed, nothing changed' }

  foreach ($a in '32', '64') { Copy-Item "$s\build\$a\$dll" "C:\ProgramData\Windhawk\Engine\Mods\$a\$dll" -Force }
  Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force

  # Same layout as the other local mods. Include/Exclude mirror the source header.
  $exclude = 'dwm.exe|windhawk.exe|LockApp.exe|LogonUI.exe|ShellExperienceHost.exe|ShellHost.exe|StartMenuExperienceHost.exe|SearchHost.exe|TextInputHost.exe'
  $key = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey("SOFTWARE\Windhawk\Engine\Mods\$id")
  $set = $key.CreateSubKey('Settings')
  # Settings first, while the mod is still disabled, so no process starts it with empty values.
  $key.SetValue('Disabled', 1, 'DWord')
  $set.SetValue('size', 12, 'DWord')
  $set.SetValue('gap', 8, 'DWord')
  $set.SetValue('leftMargin', 9, 'DWord')
  $set.SetValue('drawTitle', 1, 'DWord')
  $set.SetValue('standardFrames', 'draw', 'String')
  $set.SetValue('customFrames', 'left', 'String')
  $set.SetValue('customHeight', 0, 'DWord')
  $set.SetValue('rightSideProcesses', '', 'String')
  $set.SetValue('skipProcesses', '', 'String')
  $set.Close()
  $key.SetValue('LibraryFileName', $dll, 'String')
  $key.SetValue('Include', '*', 'String')
  $key.SetValue('Exclude', $exclude, 'String')
  $key.SetValue('Architecture', '', 'String')
  $key.SetValue('Version', $ver, 'String')
  $key.SetValue('SettingsChangeTime', [int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds(), 'DWord')
  $key.SetValue('Disabled', 0, 'DWord')
  $key.Close()

  $note = ''
  if (-not $NoExplorerTabShift) {
    # File Explorer draws its tabs from the top-left corner; move them right of the circles
    # (9 + 3*12 + 2*8 = 61 px, plus a gap). Reuses its slot if run again.
    $mod = 'SOFTWARE\Windhawk\Engine\Mods\windows-11-file-explorer-styler'
    $sk = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("$mod\Settings", $true)
    if (-not $sk) { $note = ' (explorer styler not found, tabs not shifted)' }
    else {
      $target = 'FileExplorerExtensions.FileExplorerTabControl'
      $found = $null; $next = 0
      foreach ($n in $sk.GetValueNames()) {
        if ($n -match '^controlStyles\[(\d+)\]\.target$') {
          if ([int]$Matches[1] -ge $next) { $next = [int]$Matches[1] + 1 }
          if ($sk.GetValue($n) -eq $target) { $found = [int]$Matches[1] }
        }
      }
      if ($null -eq $found) { $found = $next }
      $sk.SetValue("controlStyles[$found].target", $target, 'String')
      $sk.SetValue("controlStyles[$found].styles[0]", 'Margin=72,0,0,0', 'String')
      $sk.Close()
      Set-ItemProperty "HKLM:\$mod" -Name SettingsChangeTime -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
      $note = " (explorer tabs shifted, styler slot $found)"
    }
  }
  "ok, backup: $backup$note" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
