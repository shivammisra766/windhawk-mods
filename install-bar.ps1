# Installs the bar and start-menu-pop without restarting the Windhawk service, because that
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
  Install 'local@top-bar' '3.0' '300030' @('32', '64')
  Install 'local@start-menu-pop' '2.1' '200015' @('64')
  # Windows restarts these by itself. Stopping both also sets up a true first open to test.
  'StartMenuExperienceHost', 'ShellExperienceHost' | ForEach-Object { Stop-Process -Name $_ -Force -ErrorAction SilentlyContinue }
  "ok" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
