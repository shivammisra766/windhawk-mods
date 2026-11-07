$s = $PSScriptRoot
$id = 'local@top-bar'; $ver = '2.1'; $serial = '300021'
$utf8 = New-Object Text.UTF8Encoding($false)
$core = [IO.File]::ReadAllText("$s\topbar-core.cpp", $utf8)

# Turn the SVGs in icons\ (the same sets Seelen's react-icons names come from) into a C++ table.
$names = 'wifi','offline','bluetooth','settings','bell','calendar','batfull','bathigh','batmed','batlow','batwarn','bolt'
$enum = New-Object Collections.ArrayList
$defs = New-Object Collections.ArrayList
$rx = [Text.RegularExpressions.RegexOptions]::Singleline
foreach ($n in $names) {
  $svg = [IO.File]::ReadAllText("$s\icons\$n.svg", $utf8)
  $root = [regex]::Match($svg, '<svg\b[^>]*>', $rx).Value
  $vb = [regex]::Match($root, 'viewBox="([^"]+)"').Groups[1].Value -split '\s+'
  $rootStroke = $root -match '\bstroke="(?!none)'
  $rootFillNone = $root -match '\bfill="none"'
  $rootWidth = [regex]::Match($root, 'stroke-width="([\d\.]+)"').Groups[1].Value
  $parts = New-Object Collections.ArrayList
  foreach ($m in [regex]::Matches($svg, '<(path|circle)\b[^>]*>', $rx)) {
    $el = $m.Value
    $strokeNone = $el -match '\bstroke="none"'
    $fillNone = $rootFillNone -or ($el -match '\bfill="none"') -or ($el -match 'fill:\s*none')
    $hasStroke = (-not $strokeNone) -and ($rootStroke -or ($el -match '\bstroke="(?!none)') -or ($el -match '\bstroke:'))
    if ($fillNone -and -not $hasStroke) { continue }
    $width = 0.0
    if ($hasStroke) {
      $w = [regex]::Match($el, 'stroke-width[=:]"?\s*([\d\.]+)').Groups[1].Value
      if (-not $w) { $w = $rootWidth }
      if (-not $w) { throw "no stroke width in $n" }
      $width = [double]$w
    }
    if ($m.Groups[1].Value -eq 'circle') {
      $cx = [double][regex]::Match($el, '\bcx="([^"]+)"').Groups[1].Value
      $cy = [double][regex]::Match($el, '\bcy="([^"]+)"').Groups[1].Value
      $r = [double][regex]::Match($el, '\br="([^"]+)"').Groups[1].Value
      $d = "M$($cx - $r) $cy a$r $r 0 1 0 $(2 * $r) 0 a$r $r 0 1 0 $(-2 * $r) 0 z"
    } else {
      $d = [regex]::Match($el, '\bd="([^"]+)"').Groups[1].Value
    }
    if (-not $d) { throw "no path data in $n" }
    $fill = if ($fillNone) { 'false' } else { 'true' }
    [void]$parts.Add(('{{{0}, {1}f, "{2}"}}' -f $fill, $width.ToString('0.0##', [Globalization.CultureInfo]::InvariantCulture), ($d -replace '\s+', ' ')))
  }
  if ($parts.Count -eq 0 -or $parts.Count -gt 8) { throw "bad part count $($parts.Count) in $n" }
  [void]$enum.Add("I_$n")
  [void]$defs.Add(('    {{{0}.0f, {1}.0f, {2}, {{{3}}}}},' -f $vb[2], $vb[3], $parts.Count, ($parts -join ', ')))
  "$n parts=$($parts.Count)"
}
$table = "enum IconId { $($enum -join ', ') };`r`nstruct IconPart { bool fill; float stroke; const char* d; };`r`nstruct IconDef { float w; float h; int count; IconPart parts[8]; };`r`nconst IconDef kIcons[] = {`r`n$($defs -join "`r`n")`r`n};"
if ($core.IndexOf('//@@ICONS@@') -lt 0) { throw 'icon marker not found' }
$core = $core.Replace('//@@ICONS@@', $table)

# Windhawk's standard tool-mod scaffold, taken from the island mod, which uses it unchanged.
$island = [IO.File]::ReadAllText('C:\ProgramData\Windhawk\ModsSource\dynamic-island-for-windows.wh.cpp', $utf8)
$marker = 'Windhawk tool mod implementation'
$at = $island.IndexOf($marker)
if ($at -lt 0) { throw 'scaffold not found' }
$lineStart = $island.LastIndexOf("`n", $at) + 1
$scaffold = $island.Substring($lineStart)
if ($scaffold -notmatch 'WhTool_ModInit\(\)' -or $scaffold -notmatch 'Wh_ModAfterInit') { throw 'scaffold looks wrong' }
$t = $core + "`r`n" + $scaffold
[IO.File]::WriteAllText("$s\$id.wh.cpp", $t, $utf8)

# windhawk.exe is 32-bit, so tool mods load from Mods\32. Build both, like the working tool mods.
$comp = 'C:\Program Files\Windhawk\Compiler'
foreach ($arch in @(@{ dir = '32'; target = 'i686-w64-mingw32' }, @{ dir = '64'; target = 'x86_64-w64-mingw32' })) {
  $outDir = "$s\build\$($arch.dir)"
  New-Item -ItemType Directory -Force $outDir | Out-Null
  $lib = "C:\Program Files\Windhawk\Engine\1.7.3\$($arch.dir)\windhawk.lib"
  $dll = "$outDir\$($id)_$($ver)_$serial.dll"
  if (Test-Path $dll) { Remove-Item $dll }
  $cargs = @('-std=c++23','-O2','-shared','-DUNICODE','-D_UNICODE','-DWINVER=0x0A00','-D_WIN32_WINNT=0x0A00','-D_WIN32_IE=0x0A00','-DNTDDI_VERSION=0x0A000008','-D__USE_MINGW_ANSI_STDIO=0','-DWH_MOD',
    ('"-DWH_MOD_ID=L\"' + $id + '\""'), ('"-DWH_MOD_VERSION=L\"' + $ver + '\""'), ('"' + $lib + '"'),
    '-x','c++','-','-include','windhawk_api.h','-target',$arch.target,'-Wl,--export-all-symbols','-o',('"' + $dll + '"'),
    '-lgdiplus','-lgdi32','-luser32','-lshell32','-ladvapi32','-lole32','-lversion','-lwininet','-liphlpapi','-luuid') -join ' '
  $psi = New-Object Diagnostics.ProcessStartInfo
  $psi.FileName = "$comp\bin\clang++.exe"; $psi.Arguments = $cargs; $psi.WorkingDirectory = $comp
  $psi.UseShellExecute = $false; $psi.RedirectStandardInput = $true; $psi.RedirectStandardError = $true; $psi.RedirectStandardOutput = $true
  $p = [Diagnostics.Process]::Start($psi)
  $errTask = $p.StandardError.ReadToEndAsync(); $outTask = $p.StandardOutput.ReadToEndAsync()
  $bytes = $utf8.GetBytes($t); $p.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length); $p.StandardInput.Close()
  $p.WaitForExit()
  "$($arch.dir)-bit clang exit: $($p.ExitCode)"
  $e = $errTask.Result; if ($e) { $e.Substring(0, [Math]::Min(3500, $e.Length)) }
  if (Test-Path $dll) { "built: $([math]::Round((Get-Item $dll).Length/1KB)) KB" } else { 'no dll produced' }
}
