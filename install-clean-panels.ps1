# Turns off the glass theme for the panels, leaving native controls in the Start menu's colours,
# and installs the current bar and Start mods. Run elevated, then restart the bar process
# unelevated with: windhawk.exe -tool-mod "local@top-bar"
# To undo the panel look: reg import windhawk-hklm-before-clean-panels.reg
$s = $PSScriptRoot
$out = "$s\install-clean-result.txt"
try {
  reg export "HKLM\SOFTWARE\Windhawk" "D:\backups\windhawk-2026-11-06\windhawk-hklm-before-clean-panels.reg" /y | Out-Null
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
  Install 'local@top-bar' '1.7' '300017' @('32', '64')
  Install 'local@start-menu-pop' '1.9' '200013' @('64')

  $mod = 'SOFTWARE\Windhawk\Engine\Mods\windows-11-notification-center-styler'
  $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("$mod\Settings", $true)
  $key.SetValue('theme', '')
  # These forced one background on tiles and slider thumbs, which only suited the glass theme.
  # With it, an on toggle would look the same as an off one. The rule slots stay, because the
  # styler stops reading at the first missing one.
  foreach ($i in 6, 7, 8, 9, 10) { $key.SetValue("controlStyles[$i].styles[0]", 'Opacity=1') }
  $key.Close()
  Set-ItemProperty "HKLM:\$mod" -Name SettingsChangeTime -Value $now -Type DWord

  # Windows restarts these by itself, and they load the changes.
  'ShellExperienceHost', 'ShellHost', 'StartMenuExperienceHost' | ForEach-Object { Stop-Process -Name $_ -Force -ErrorAction SilentlyContinue }
  "ok" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
