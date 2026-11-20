# Local build of the repo mod radial-window-switcher plus a three-finger touchpad swipe (raw HID input).
# Keeps the mod id, so settings stay; a repo update would overwrite it. Original: radial\radial-window-switcher.orig.wh.cpp
$s = $PSScriptRoot
$id = 'radial-window-switcher'; $ver = '1.0.0'; $serial = '685940'
$comp = 'C:\Program Files\Windhawk\Compiler'
$utf8 = New-Object Text.UTF8Encoding($false)
$t = [IO.File]::ReadAllText("$s\$id.wh.cpp", $utf8)
$libs = '-lgdiplus -lgdi32 -ldwmapi -lversion -lshcore -lshell32 -ladvapi32 -lhid -lole32 -loleaut32 -luuid -lwinhttp -lshlwapi -lruntimeobject'
foreach ($a in @(@('64', 'x86_64-w64-mingw32'), @('32', 'i686-w64-mingw32'))) {
  $lib = "C:\Program Files\Windhawk\Engine\1.7.3\$($a[0])\windhawk.lib"
  New-Item -ItemType Directory -Force "$s\build\$($a[0])" | Out-Null
  $dll = "$s\build\$($a[0])\$($id)_$($ver)_$serial.dll"
  if (Test-Path $dll) { Remove-Item $dll }
  $cargs = @('-std=c++23', '-O2', '-shared', '-DUNICODE', '-D_UNICODE', '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00', '-D_WIN32_IE=0x0A00', '-DNTDDI_VERSION=0x0A000008', '-D__USE_MINGW_ANSI_STDIO=0', '-DWH_MOD',
    ('"-DWH_MOD_ID=L\"' + $id + '\""'), ('"-DWH_MOD_VERSION=L\"' + $ver + '\""'), ('"' + $lib + '"'),
    '-x', 'c++', '-', '-include', 'windhawk_api.h', '-target', $a[1], '-Wl,--export-all-symbols', '-o', ('"' + $dll + '"'),
    $libs, '-luser32', '-loleaut32') -join ' '
  $psi = New-Object Diagnostics.ProcessStartInfo
  $psi.FileName = "$comp\bin\clang++.exe"; $psi.Arguments = $cargs; $psi.WorkingDirectory = $comp
  $psi.UseShellExecute = $false; $psi.RedirectStandardInput = $true; $psi.RedirectStandardError = $true; $psi.RedirectStandardOutput = $true
  $p = [Diagnostics.Process]::Start($psi)
  $errTask = $p.StandardError.ReadToEndAsync(); $outTask = $p.StandardOutput.ReadToEndAsync()
  $bytes = $utf8.GetBytes($t); $p.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length); $p.StandardInput.Close()
  $p.WaitForExit()
  "$($a[0])-bit clang exit: $($p.ExitCode)"
  $e = $errTask.Result; if ($e -and $p.ExitCode -ne 0) { $e.Substring(0, [Math]::Min(3000, $e.Length)) }
  if (Test-Path $dll) { "built: $([math]::Round((Get-Item $dll).Length/1KB)) KB" }
}
