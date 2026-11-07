param([ValidateSet('both','32','64')][string]$Arch = 'both', [switch]$CheckOnly)
# Builds local@traffic-lights for 32- and 64-bit processes (it loads into every app, so both are needed).
$s = $PSScriptRoot
$id = 'local@traffic-lights'; $ver = '1.0'; $serial = '600010'
$utf8 = New-Object Text.UTF8Encoding($false)
$t = [IO.File]::ReadAllText("$s\$id.wh.cpp", $utf8)
$m = [regex]::Match($t, '(?m)^// @compilerOptions (.*)$')
if (-not $m.Success) { throw 'no @compilerOptions line' }
$libs = $m.Groups[1].Value.Trim() -split '\s+'
$comp = 'C:\Program Files\Windhawk\Compiler'
$archs = @(@{ dir = '32'; target = 'i686-w64-mingw32' }, @{ dir = '64'; target = 'x86_64-w64-mingw32' }) | Where-Object { $Arch -eq 'both' -or $Arch -eq $_.dir }
$failed = $false
foreach ($a in $archs) {
  $outDir = "$s\build\$($a.dir)"
  New-Item -ItemType Directory -Force $outDir | Out-Null
  $lib = "C:\Program Files\Windhawk\Engine\1.7.3\$($a.dir)\windhawk.lib"
  $dll = "$outDir\$($id)_$($ver)_$serial.dll"
  if (-not $CheckOnly -and (Test-Path $dll)) { Remove-Item $dll }
  $cargs = @('-std=c++23','-O2','-Wall','-Wno-unused-function','-DUNICODE','-D_UNICODE','-DWINVER=0x0A00','-D_WIN32_WINNT=0x0A00','-D_WIN32_IE=0x0A00','-DNTDDI_VERSION=0x0A000008','-D__USE_MINGW_ANSI_STDIO=0','-DWH_MOD',
    ('"-DWH_MOD_ID=L\"' + $id + '\""'), ('"-DWH_MOD_VERSION=L\"' + $ver + '\""'),
    $(if ($CheckOnly) { '' } else { '-shared "' + $lib + '"' }),
    '-x','c++','-','-include','windhawk_api.h','-target',$a.target)
  if ($CheckOnly) { $cargs += '-fsyntax-only' }
  else { $cargs += @('-Wl,--export-all-symbols', '-o', ('"' + $dll + '"')) + $libs }
  $psi = New-Object Diagnostics.ProcessStartInfo
  $psi.FileName = "$comp\bin\clang++.exe"; $psi.Arguments = ($cargs -join ' '); $psi.WorkingDirectory = $comp
  $psi.UseShellExecute = $false; $psi.RedirectStandardInput = $true; $psi.RedirectStandardError = $true; $psi.RedirectStandardOutput = $true
  $p = [Diagnostics.Process]::Start($psi)
  $errTask = $p.StandardError.ReadToEndAsync(); $outTask = $p.StandardOutput.ReadToEndAsync()
  $bytes = $utf8.GetBytes($t); $p.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length); $p.StandardInput.Close()
  $p.WaitForExit()
  "$($a.dir)-bit clang exit: $($p.ExitCode)"
  if ($p.ExitCode -ne 0) { $failed = $true }
  $e = $errTask.Result; if ($e) { $e.Substring(0, [Math]::Min(8000, $e.Length)) }
  if ($CheckOnly) { 'check only, nothing written' } elseif (Test-Path $dll) { "built: $([math]::Round((Get-Item $dll).Length/1KB)) KB" } else { 'no dll produced' }
}
if ($failed) { exit 1 }
