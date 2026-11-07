# Installs the bar 4.2 and panel-split 1.12. Tray-bridge stays at 1.3: 1.4 or the popup styler edits crashed Explorer. Nothing else changes without restarting the Windhawk service, because that
# restart has crashed Explorer. Run elevated, then restart the bar process unelevated with:
#   windhawk.exe -tool-mod "local@top-bar"
$s = $PSScriptRoot
$out = "$s\install-bar-result.txt"
try {
  $base = 'HKLM:\SOFTWARE\Windhawk\Engine\Mods'
  $now = [int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
  function Install($id, $ver, $serial, $arches) {
    $dll = "$($id)_$($ver)_$serial.dll"
    foreach ($a in $arches) { $to = "C:\ProgramData\Windhawk\Engine\Mods\$a\$dll"; if (-not (Test-Path $to)) { Copy-Item "$s\build\$a\$dll" $to -Force } }  # An installed serial is locked and never changes.
    Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force
    Set-ItemProperty "$base\$id" -Name LibraryFileName -Value $dll -Type String
    Set-ItemProperty "$base\$id" -Name Version -Value $ver -Type String
    Set-ItemProperty "$base\$id" -Name SettingsChangeTime -Value $now -Type DWord
  }
  # windhawk.exe is 32-bit, so the bar needs both architectures.
  # Order: the Explorer-side mods first, then the bar. Restarting their hosts is left to you (see topbarlerts\INSTALL.txt).
  Install 'local@panel-split' '1.12' '400013' @('64')
  Install 'local@top-bar' '4.2' '300042' @('32', '64')
  'ShellExperienceHost', 'ShellHost' | ForEach-Object { Stop-Process -Name $_ -Force -ErrorAction SilentlyContinue }
  "ok" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
