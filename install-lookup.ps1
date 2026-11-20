# Installs the radial-window-switcher build with Lookup (three-finger tap) and the Hindi OCR pack. Run elevated.
# Undo: reg import D:\backups\windhawk-2026-11-06\windhawk-hklm-before-lookup.reg (the previous DLL 685937 is left in place).
$s = $PSScriptRoot
$out = "$s\install-lookup-result.txt"
$log = @()
try {
  reg export "HKLM\SOFTWARE\Windhawk" "D:\backups\windhawk-2026-11-06\windhawk-hklm-before-lookup.reg" /y | Out-Null
  $id = 'radial-window-switcher'; $ver = '1.0.0'; $serial = '685940'; $dll = "$($id)_$($ver)_$serial.dll"
  Copy-Item "$s\build\64\$dll" "C:\ProgramData\Windhawk\Engine\Mods\64\$dll" -Force
  Copy-Item "$s\build\32\$dll" "C:\ProgramData\Windhawk\Engine\Mods\32\$dll" -Force
  Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force
  $key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$id"
  Set-ItemProperty "$key\Settings" -Name lookupTap -Value 1 -Type DWord
  Set-ItemProperty $key -Name LibraryFileName -Value $dll -Type String
  Set-ItemProperty $key -Name SettingsChangeTime -Value ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
  $log += "mod: ok"
} catch { $log += "mod ERROR: $($_.Exception.Message)" }
try {
  $cap = Get-WindowsCapability -Online -Name 'Language.OCR~~~hi-IN*' | Select-Object -First 1
  if ($cap -and $cap.State -ne 'Installed') { Add-WindowsCapability -Online -Name $cap.Name | Out-Null }
  $cap = Get-WindowsCapability -Online -Name 'Language.OCR~~~hi-IN*' | Select-Object -First 1
  $log += "hindi ocr: $($cap.Name) $($cap.State)"
} catch { $log += "hindi ocr ERROR: $($_.Exception.Message)" }
$log | Out-File $out
