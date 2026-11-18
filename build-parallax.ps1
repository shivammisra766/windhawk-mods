param([ValidateSet('both','32','64')][string]$Arch = 'both', [switch]$CheckOnly)
# Builds local@parallax-wallpaper-idle, the idle-efficient fork of parallax-wallpaper 0.9.2.
# The source @id is the bare id (parallax-wallpaper-idle); files and the registry key keep local@.
$s = $PSScriptRoot
$id = 'local@parallax-wallpaper-idle'; $ver = '0.9.2.1'; $serial = '800001'
$utf8 = New-Object Text.UTF8Encoding($false)
$t = [IO.File]::ReadAllText("$s\$id.wh.cpp", $utf8)
if ($t -notmatch '(?m)^// @id\s+parallax-wallpaper-idle\s*$') { throw 'source @id must be parallax-wallpaper-idle' }
if ($t -notmatch "(?m)^// @version\s+$([regex]::Escape($ver))\s*$") { throw "source @version must be $ver" }
# Libraries come from the mod's own @compilerOptions line, as Windhawk would use them.
$opts = [regex]::Match($t, '(?m)^// @compilerOptions\s+(.+?)\s*$').Groups[1].Value
$libs = @($opts -split '\s+' | Where-Object { $_ }) + '-luser32'

# windhawk.exe is 32-bit, so tool mods load from Mods\32. Build both, like the working tool mods.
# -CheckOnly stops at a syntax check and writes no DLL; -Arch 32 or 64 builds one architecture.
$comp = 'C:\Program Files\Windhawk\Compiler'
$archs = @(@{ dir = '32'; target = 'i686-w64-mingw32' }, @{ dir = '64'; target = 'x86_64-w64-mingw32' }) | Where-Object { $Arch -eq 'both' -or $Arch -eq $_.dir }
$failed = $false
foreach ($a in $archs) {
  $outDir = "$s\build\$($a.dir)"
  New-Item -ItemType Directory -Force $outDir | Out-Null
  $lib = "C:\Program Files\Windhawk\Engine\1.7.3\$($a.dir)\windhawk.lib"
  $dll = "$outDir\$($id)_$($ver)_$serial.dll"
  if (-not $CheckOnly -and (Test-Path $dll)) { Remove-Item $dll }
  $cargs = @('-std=c++23','-O2','-DUNICODE','-D_UNICODE','-DWINVER=0x0A00','-D_WIN32_WINNT=0x0A00','-D_WIN32_IE=0x0A00','-DNTDDI_VERSION=0x0A000008','-D__USE_MINGW_ANSI_STDIO=0','-DWH_MOD',
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
  $e = $errTask.Result; if ($e) { $e.Substring(0, [Math]::Min(6000, $e.Length)) }
  if ($CheckOnly) { 'check only, nothing written' } elseif (Test-Path $dll) { "built: $([math]::Round((Get-Item $dll).Length/1KB)) KB" } else { 'no dll produced' }
}
if ($failed) { exit 1 }
