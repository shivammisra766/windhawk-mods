# Local build of the repo mod translucent-windows (1.8.2) as local@translucent-windows, with an added
# no-blur "Plain tint" background effect (ACCENT_ENABLE_TRANSPARENTGRADIENT, AccentFlags 2) for the
# user's Terminal look (#C4000000). Upstream source kept as translucent-windows.orig.wh.cpp for diffs.
# Builds both archs (the mod's @include is *); install-translucent.ps1 restricts it to 64-bit hosts first.
$s = $PSScriptRoot
$id = 'local@translucent-windows'; $ver = '1.8.2.2'; $serial = '600002'
$comp = 'C:\Program Files\Windhawk\Compiler'
$utf8 = New-Object Text.UTF8Encoding($false)
$t = [IO.File]::ReadAllText("$s\$id.wh.cpp", $utf8)
# The mod's @compilerOptions.
$libs = '-ldwmapi -luxtheme -lcomctl32 -lgdi32 -ld2d1 -lmsimg32 -lshcore -lversion -ffp-exception-behavior=maytrap'
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
