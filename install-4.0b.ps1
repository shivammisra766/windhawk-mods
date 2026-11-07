# Bar 4.0 and panel-split only. tray-bridge stays at 1.3, because 1.4 or the popup styler edits
# put Explorer into a crash loop. Nothing here runs in Explorer.
$s = $PSScriptRoot
$out = "$s\install-4.0b-result.txt"
try {
  $base = 'HKLM:\SOFTWARE\Windhawk\Engine\Mods'
  $now = [int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
  foreach ($m in @(@('local@top-bar', '4.0', '300040'), @('local@panel-split', '1.10', '400011'))) {
    $dll = "$($m[0])_$($m[1])_$($m[2]).dll"
    Set-ItemProperty "$base\$($m[0])" -Name LibraryFileName -Value $dll -Type String
    Set-ItemProperty "$base\$($m[0])" -Name Version -Value $m[1] -Type String
    Set-ItemProperty "$base\$($m[0])" -Name SettingsChangeTime -Value $now -Type DWord
  }
  'ShellExperienceHost', 'ShellHost' | ForEach-Object { Stop-Process -Name $_ -Force -ErrorAction SilentlyContinue }
  "ok" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
