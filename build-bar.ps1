param([ValidateSet('both','32','64')][string]$Arch = 'both', [switch]$CheckOnly)
$s = $PSScriptRoot
# Version and serial live on this line only; the header, the DLL name and the defines take them from here.
$id = 'local@top-bar'; $ver = '4.8'; $serial = '300048'
$utf8 = New-Object Text.UTF8Encoding($false)
# One library list, used for the generated @compilerOptions and for the clang call.
$libs = @('-lgdi32','-luser32','-lshell32','-ladvapi32','-lole32','-lversion','-lwininet','-liphlpapi','-luuid','-ld2d1','-ldwrite','-lwindowscodecs','-ldwmapi','-lruntimeobject','-loleaut32','-lsetupapi','-lpdh','-lwbemuuid')

# Unity build: topbar\manifest.txt lists the files, each preceded by a #line so errors name the real file.
$src = "$s\topbar"
$pieces = New-Object Collections.ArrayList
foreach ($f in (Get-Content "$src\manifest.txt" | Where-Object { $_.Trim() })) {
  [void]$pieces.Add("#line 1 `"$($f.Replace([string][char]92, '/'))`"`n" + [IO.File]::ReadAllText("$src\$f", $utf8))
}
$header = [IO.File]::ReadAllText("$src\mod-header.txt", $utf8)
$header = $header -replace '(?m)^// @version .*$', "// @version         $ver"
$header = $header -replace '(?m)^// @compilerOptions .*$', "// @compilerOptions $($libs -join ' ')"
$core = $header + ($pieces -join "`n")

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

# Strings: topbar\strings.tsv (first column the key, then one column per language, English first) becomes the Loc table.
function CppStr($v) { 'L"' + $v.Replace('\', '\\').Replace('"', '\"') + '"' }
$rows = New-Object Collections.ArrayList
$langs = $null
foreach ($line in [IO.File]::ReadAllLines("$src\strings.tsv", $utf8)) {
  if (-not $line.Trim()) { continue }
  if ($line.StartsWith('#')) { $langs = $line.Substring(1).Trim() -split "`t" | Select-Object -Skip 1; continue }
  $c = $line -split "`t"
  [void]$rows.Add(@{ key = $c[0]; text = @(for ($i = 1; $i -le $langs.Count; $i++) { if ($i -lt $c.Count) { $c[$i] } else { '' } }) })
}
if (-not $langs -or $langs[0] -ne 'en') { throw 'strings.tsv needs a header line starting with # key<TAB>en' }
$n = $langs.Count
$loc = "constexpr int kLocLangCount = $n;`r`nconst wchar_t* const kLocLangs[kLocLangCount] = { $(($langs | ForEach-Object { CppStr $_ }) -join ', ') };`r`n"
$loc += "struct LocEntry { const wchar_t* key; const wchar_t* text[kLocLangCount]; };`r`nconst LocEntry kLoc[] = {`r`n"
foreach ($r in $rows) { $loc += "    { $(CppStr $r.key), { $(($r.text | ForEach-Object { CppStr $_ }) -join ', ') } },`r`n" }
$loc += '};'
if ($core.IndexOf('//@@STRINGS@@') -lt 0) { throw 'strings marker not found' }
$core = $core.Replace('//@@STRINGS@@', $loc)

# Windhawk's standard tool-mod scaffold, vendored in topbar\scaffold.inc.
$scaffold = "#line 1 `"scaffold.inc`"`n" + [IO.File]::ReadAllText("$src\scaffold.inc", $utf8)
if ($scaffold -notmatch 'WhTool_ModInit\(\)' -or $scaffold -notmatch 'Wh_ModAfterInit') { throw 'scaffold looks wrong' }
$t = $core + "`r`n" + $scaffold
[IO.File]::WriteAllText("$s\$id.wh.cpp", $t, $utf8)

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
