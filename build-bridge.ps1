$s = $PSScriptRoot
$id = 'local@tray-bridge'; $ver = '1.12'; $serial = '500013'
$utf8 = New-Object Text.UTF8Encoding($false)
$t = [IO.File]::ReadAllText("$s\$id.wh.cpp", $utf8)
$comp = 'C:\Program Files\Windhawk\Compiler'
$lib = 'C:\Program Files\Windhawk\Engine\1.7.3\64\windhawk.lib'
New-Item -ItemType Directory -Force "$s\build\64" | Out-Null
$dll = "$s\build\64\$($id)_$($ver)_$serial.dll"
if (Test-Path $dll) { Remove-Item $dll }
$cargs = @('-std=c++23','-O2','-shared','-DUNICODE','-D_UNICODE','-DWINVER=0x0A00','-D_WIN32_WINNT=0x0A00','-D_WIN32_IE=0x0A00','-DNTDDI_VERSION=0x0A000008','-D__USE_MINGW_ANSI_STDIO=0','-DWH_MOD',
  ('"-DWH_MOD_ID=L\"' + $id + '\""'), ('"-DWH_MOD_VERSION=L\"' + $ver + '\""'), ('"' + $lib + '"'),
  '-x','c++','-','-include','windhawk_api.h','-target','x86_64-w64-mingw32','-Wl,--export-all-symbols','-o',('"' + $dll + '"'),
  '-luser32') -join ' '
$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName = "$comp\bin\clang++.exe"; $psi.Arguments = $cargs; $psi.WorkingDirectory = $comp
$psi.UseShellExecute = $false; $psi.RedirectStandardInput = $true; $psi.RedirectStandardError = $true; $psi.RedirectStandardOutput = $true
$p = [Diagnostics.Process]::Start($psi)
$errTask = $p.StandardError.ReadToEndAsync(); $outTask = $p.StandardOutput.ReadToEndAsync()
$bytes = $utf8.GetBytes($t); $p.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length); $p.StandardInput.Close()
$p.WaitForExit()
"bridge clang exit: $($p.ExitCode)"
$e = $errTask.Result; if ($e) { $e.Substring(0, [Math]::Min(3000, $e.Length)) }
if (Test-Path $dll) { "built: $([math]::Round((Get-Item $dll).Length/1KB)) KB" } else { 'no dll produced' }
