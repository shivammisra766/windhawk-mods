# Installs bar 4.3 (own volume and brightness slider) and tray-bridge 1.7 (osdOverride=2 hides the native popup and
# tells the bar). Both reload without restarting Explorer or the Windhawk service (that restart has crashed Explorer).
# Run elevated, then restart the bar process unelevated with:
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
  New-Item "$base\local@tray-bridge\Settings" -Force | Out-Null
  Set-ItemProperty "$base\local@tray-bridge\Settings" -Name osdOverride -Value 2 -Type DWord
  Install 'local@tray-bridge' '1.7' '500008' @('64')
  Install 'local@top-bar' '4.3' '300043' @('32', '64')
  "ok" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
