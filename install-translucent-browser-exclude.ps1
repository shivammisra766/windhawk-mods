# Excludes browsers and Chromium/Electron apps from local@translucent-windows: its GetSysColor hook hands them a dark
# palette (WindowText 240,240,240), which Chromium uses for autofilled field text on its light-blue highlight, so the
# text becomes invisible. Only processes started afterwards are affected. Run elevated.
# Undo: reg import D:\backups\windhawk-2026-11-06\windhawk-hklm-before-translucent-browsers.reg
$out = "$PSScriptRoot\install-translucent-browser-exclude-result.txt"
try {
  reg export "HKLM\SOFTWARE\Windhawk" "D:\backups\windhawk-2026-11-06\windhawk-hklm-before-translucent-browsers.reg" /y | Out-Null
  $key = 'HKLM:\SOFTWARE\Windhawk\Engine\Mods\local@translucent-windows'
  $cur = (Get-ItemProperty $key).Exclude
  $add = 'msedge.exe|msedgewebview2.exe|chrome.exe|brave.exe|firefox.exe|opera.exe|vivaldi.exe|Code.exe|claude.exe|electron.exe|Discord.exe|slack.exe|Spotify.exe|WhatsApp*.exe'
  $parts = @($cur -split '\|' | Where-Object { $_ }) + @($add -split '\|')
  $new = ($parts | Select-Object -Unique) -join '|'
  Set-ItemProperty $key -Name Exclude -Value $new -Type String
  Set-ItemProperty $key -Name SettingsChangeTime -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
  "ok: $new" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
