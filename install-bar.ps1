# Installs the bar 4.0, panel-split 1.10 and tray-bridge 1.4 (start-menu-pop is unchanged) without restarting the Windhawk service, because that
# restart has crashed Explorer. Run elevated, then restart the bar process unelevated with:
#   windhawk.exe -tool-mod "local@top-bar"
$s = $PSScriptRoot
$out = "$s\install-bar-result.txt"
try {
  $base = 'HKLM:\SOFTWARE\Windhawk\Engine\Mods'
  $now = [int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
  function Install($id, $ver, $serial, $arches) {
    $dll = "$($id)_$($ver)_$serial.dll"
    foreach ($a in $arches) { Copy-Item "$s\build\$a\$dll" "C:\ProgramData\Windhawk\Engine\Mods\$a\$dll" -Force }
    Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force
    Set-ItemProperty "$base\$id" -Name LibraryFileName -Value $dll -Type String
    Set-ItemProperty "$base\$id" -Name Version -Value $ver -Type String
    Set-ItemProperty "$base\$id" -Name SettingsChangeTime -Value $now -Type DWord
  }
  # windhawk.exe is 32-bit, so the bar needs both architectures.
  # Order: the Explorer-side mods first, then the bar. Restarting their hosts is left to you (see topbarlerts\INSTALL.txt).
  Install 'local@panel-split' '1.10' '400011' @('64')
  Install 'local@tray-bridge' '1.4' '500005' @('64')
  Install 'local@top-bar' '4.0' '300040' @('32', '64')
  "ok" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
