# Installs local@dark-nsis-installers (32 and 64-bit, Include *; unloads itself outside NSIS processes). Run elevated.
# Undo: set HKLM\SOFTWARE\Windhawk\Engine\Mods\local@dark-nsis-installers Disabled=1, or reg import the backup below.
$s = $PSScriptRoot
$out = "$s\install-nsis-result.txt"
try {
  reg export "HKLM\SOFTWARE\Windhawk" "D:\backups\windhawk-2026-11-06\windhawk-hklm-before-dark-nsis.reg" /y | Out-Null
  $id = 'local@dark-nsis-installers'; $ver = '1.0'; $serial = '710001'; $dll = "$($id)_$($ver)_$serial.dll"
  foreach ($a in '32', '64') { Copy-Item "$s\build\$a\$dll" "C:\ProgramData\Windhawk\Engine\Mods\$a\$dll" -Force }
  Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force
  $key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$id"
  New-Item -Path $key -Force | Out-Null
  New-Item -Path "$key\Settings" -Force | Out-Null
  Set-ItemProperty $key -Name Disabled -Value 0 -Type DWord
  Set-ItemProperty $key -Name Include -Value '*' -Type String
  Set-ItemProperty $key -Name Exclude -Value '' -Type String
  Set-ItemProperty $key -Name LibraryFileName -Value $dll -Type String
  Set-ItemProperty $key -Name Version -Value $ver -Type String
  Set-ItemProperty $key -Name LoggingEnabled -Value 1 -Type DWord
  Set-ItemProperty $key -Name SettingsChangeTime -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
  "ok" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
