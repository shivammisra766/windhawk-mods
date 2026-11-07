# Installs local@translucent-windows 1.8.2.1 (plain #C4000000 tint, no blur) for explorer.exe and SystemSettings.exe,
# and removes the remaining blur layers from the File Explorer and Settings styler themes.
# Run elevated, after build-translucent.ps1. It does not restart anything: the mod loads into Explorer and Settings
# the next time they start (sign out/in, or let Explorer restart on its own). Restarting the Windhawk service has
# crashed Explorer before, so this script never does it.
#
# Undo (elevated): delete the new key, then restore the stylers from the backup this writes:
#   reg delete "HKLM\SOFTWARE\Windhawk\Engine\Mods\local@translucent-windows" /f
#   reg delete "HKLM\SOFTWARE\Windhawk\Engine\Mods\windows-11-settings-styler\Settings" /f
#   reg import <mods-before-translucent-*.reg>      (restores every mod key as it was)
#   then bump SettingsChangeTime on windows-11-file-explorer-styler and windows-11-settings-styler.
# If Explorer crash-loops: Disabled=1 on local@translucent-windows (or the reg import above) from Task Manager > Run.
$s = $PSScriptRoot
$out = "$s\install-translucent-result.txt"
try {
  $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
  if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'run elevated' }

  $id = 'local@translucent-windows'; $ver = '1.8.2.1'; $serial = '600001'
  $dll = "$($id)_$($ver)_$serial.dll"
  foreach ($a in '64', '32') { if (-not (Test-Path "$s\build\$a\$dll")) { throw "missing build\$a\$dll, run build-translucent.ps1 first" } }

  # 1. Backup of every mod key.
  $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
  $backup = "$s\mods-before-translucent-$stamp.reg"
  & reg.exe export 'HKLM\SOFTWARE\Windhawk\Engine\Mods' $backup /y | Out-Null
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path $backup)) { throw 'reg export failed, nothing changed' }

  $hklm = [Microsoft.Win32.Registry]::LocalMachine
  $S = [Microsoft.Win32.RegistryValueKind]::String
  $D = [Microsoft.Win32.RegistryValueKind]::DWord
  $now = [int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()

  # Check the styler layouts these edits were written for before touching anything.
  $fe = $hklm.OpenSubKey('SOFTWARE\Windhawk\Engine\Mods\windows-11-file-explorer-styler\Settings', $true)
  if (-not $fe) { throw 'file explorer styler Settings key not found, nothing changed' }
  if ($fe.GetValue('theme') -ne 'OS26 Liquid Glass' -or
      $fe.GetValue('controlStyles[20].target') -ne 'Microsoft.UI.Xaml.Controls.AppBarButton > Grid@CommonStates' -or
      $null -ne $fe.GetValue('controlStyles[21].target')) { throw 'unexpected file explorer styler layout (theme / slot 20 / 21), nothing changed' }
  $ss = $hklm.OpenSubKey('SOFTWARE\Windhawk\Engine\Mods\windows-11-settings-styler\Settings', $true)
  if (-not $ss) { throw 'settings styler Settings key not found, nothing changed' }
  if ($ss.GetValue('theme') -ne 'Translucent_Settings11' -or $ss.GetValue('controlStyles[0].target') -ne '' -or
      $null -ne $ss.GetValue('controlStyles[1].target')) { throw 'unexpected settings styler layout, nothing changed' }

  # 2. DLLs and source. An installed serial is never overwritten.
  foreach ($a in '64', '32') {
    $to = "C:\ProgramData\Windhawk\Engine\Mods\$a\$dll"
    if (-not (Test-Path $to)) { Copy-Item "$s\build\$a\$dll" $to -Force }
  }
  Copy-Item "$s\$id.wh.cpp" "C:\ProgramData\Windhawk\ModsSource\$id.wh.cpp" -Force

  # 3. Mod key, same layout as the installed mods.
  $k = $hklm.CreateSubKey("SOFTWARE\Windhawk\Engine\Mods\$id")
  $k.SetValue('LibraryFileName', $dll, $S)
  $k.SetValue('Disabled', 0, $D)
  $k.SetValue('Include', 'explorer.exe|SystemSettings.exe', $S)   # widen later, one app at a time
  $k.SetValue('Exclude', '', $S)
  $k.SetValue('Architecture', '', $S)                             # both DLLs installed; the two hosts are 64-bit
  $k.SetValue('Version', $ver, $S)
  $set = $k.CreateSubKey('Settings')
  $set.SetValue('RenderingMod.ThemeBackground', 1, $D)       # needed: paints Win32 backgrounds as alpha-0 black and fixes text alpha
  $set.SetValue('RenderingMod.SysColors', 0, $D)             # never: SetSysColors is system-wide and outlives the mod
  $set.SetValue('RenderingMod.AccentColorControls', 0, $D)
  $set.SetValue('BackgroundEffects.type', 'transparent', $S) # local option: ACCENT_ENABLE_TRANSPARENTGRADIENT, AccentFlags 2
  $set.SetValue('BackgroundEffects.AccentBlurBehind', 'C4000000', $S)  # ARGB, Terminal's black at 77%
  $set.SetValue('FlyoutsEffects', 0, $D)                     # menus/tooltips stay with win32-ui-modernizer
  $set.SetValue('RuledPrograms[0].target', '', $S)           # no per-process rules (upstream default has a Notepad rule)
  $set.Close()
  $k.SetValue('SettingsChangeTime', $now, $D)
  $k.Close()

  # 4. File Explorer styler: the OS26 Liquid Glass blur layers that slots 0-20 don't already cover.
  #    Plain SolidColorBrushes (they composite over the window's tint); slots 0-2 keep their BlurAmount=0 #C4000000.
  #    backgroundTranslucentEffect stays 'default': the mod's WindhawkFileExplorerStylerNoBackgroundEffect atom
  #    makes the styler leave the window backdrop and Win32 painting to the mod anyway.
  function F($n, $v) { $fe.SetValue($n, $v, $S) }
  F 'backgroundTranslucentEffect' 'default'
  F 'controlStyles[20].styles[0]' 'Background@Disabled:=<SolidColorBrush Color="#00000000"/>'
  F 'controlStyles[21].target' 'TabViewItem > Grid#LayoutRoot@CommonStates'
  F 'controlStyles[21].styles[0]' 'Background@Selected:=<SolidColorBrush Color="#25FFFFFF"/>'
  F 'controlStyles[21].styles[1]' 'Background@PointerOverSelected:=<SolidColorBrush Color="#35FFFFFF"/>'
  F 'controlStyles[21].styles[2]' 'Background@Normal:=<SolidColorBrush Color="#15FFFFFF"/>'
  F 'controlStyles[22].target' 'Microsoft.UI.Xaml.Controls.AppBarButton#backButton > Grid@CommonStates, Microsoft.UI.Xaml.Controls.AppBarButton#forwardButton > Grid@CommonStates, Microsoft.UI.Xaml.Controls.AppBarButton#upButton > Grid@CommonStates'
  F 'controlStyles[22].styles[0]' 'Background@Disabled:=<SolidColorBrush Color="#00000000"/>'
  $fe.Close()
  Set-ItemProperty 'HKLM:\SOFTWARE\Windhawk\Engine\Mods\windows-11-file-explorer-styler' -Name SettingsChangeTime -Value $now -Type DWord

  # 5. Settings styler: Translucent_Settings11 stacks ~15 HostBackdrop AcrylicBrush layers (blurred).
  #    One un-blurred tint on the root page, every other layer transparent.
  $tint = 'Background:=<WindhawkBlur BlurAmount="0" TintColor="#C4000000"/>'
  $rules = @(
    @('SystemSettings.View.RootPage > Grid#RootPageGrid', $tint),
    @('Border > Frame > ContentPresenter > SystemSettings.View.RootPage > Grid#RootPageGrid', $tint),
    @('SplitView#RootSplitView > Grid > Grid#ContentRoot > Border > Grid#ContentGrid', 'Background=Transparent'),
    @('Microsoft.UI.Xaml.Controls.NavigationView#PermanentNavigationView > Grid#RootGrid > Grid > SplitView#RootSplitView > Grid@DisplayModeStates > Grid#PaneRoot', 'Background=Transparent'),
    @('Grid#ContentRoot > Border > Grid#ContentGrid > ContentControl#HeaderContent', 'Background=Transparent'),
    @('Frame#PermanentNavRootFrame', 'Background=Transparent'),
    @('Microsoft.UI.Xaml.Controls.NavigationView#PermanentNavigationView > Grid#RootGrid', 'Background=Transparent'),
    @('Microsoft.UI.Xaml.Controls.NavigationView#PermanentNavigationView > Grid#RootGrid > Grid', 'Background=Transparent'),
    @('Grid#ContentRoot', 'Background=Transparent'),
    @('SplitView#RootSplitView > Grid > Grid#ContentRoot', 'Background=Transparent'),
    @('Grid#RootGrid', 'Background=Transparent'),
    @('Grid#AppTitleBar', 'Background=Transparent'),
    @('Border#AppTitleBarBackground', 'Background=Transparent'),
    @('Grid#TitleBar', 'Background=Transparent'),
    @('Border#TitleBarBackground', 'Background=Transparent'),
    @('TextBox#CommandSearchTextBox', 'Background:=<SolidColorBrush Color="#1AFFFFFF"/>')
  )
  for ($i = 0; $i -lt $rules.Count; $i++) {
    $ss.SetValue("controlStyles[$i].target", $rules[$i][0], $S)
    $ss.SetValue("controlStyles[$i].styles[0]", $rules[$i][1], $S)
  }
  $ss.Close()
  Set-ItemProperty 'HKLM:\SOFTWARE\Windhawk\Engine\Mods\windows-11-settings-styler' -Name SettingsChangeTime -Value $now -Type DWord

  "ok, backup: $backup" | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
