<#
Builds the macOS-style icon theme from the downloaded WhiteSur SVGs.
Needs no admin rights and changes no system state: it only reads the SVGs and
the original app files (as data), and writes into -OutDir.

  1. SVG -> 512 px PNG with Edge headless (CSP blocks all script; the SVG is drawn as <img>,
     which never runs script anyway; throwaway profile in -OutDir\_edge).
  2. PNG -> ICO (16/24/32/48/64/128/256) with IconTools.cs.
  3. ICO -> resource-only DLL per redirected Win32 file, carrying icon groups
     with the same ids/names as the original's groups, so icon-resource-redirect
     finds them. Everything else falls back to the original file.
  4. theme.ini ([redirections]) for icon-resource-redirect's "Theme paths".

Usage: powershell -ExecutionPolicy Bypass -File build-macos-icons.ps1
#>
param(
    [string]$SvgDir = 'D:\backups\windhawk-2026-11-06\third-party\macos-icons\whitesur',
    [string]$OutDir = 'D:\backups\windhawk-2026-11-06\icon-theme-macos',
    [string]$Edge   = "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
)
$ErrorActionPreference = 'Stop'
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$map   = Import-PowerShellDataFile (Join-Path $here 'icon-map.psd1')
$sizes = [int[]](16, 20, 24, 32, 40, 48, 64, 96, 128, 256)

if ($OutDir.StartsWith((Split-Path $SvgDir -Parent), 'OrdinalIgnoreCase')) { throw 'OutDir must be outside the third-party folder' }
foreach ($d in 'png', 'ico', 'dll', '_render', '_edge') { New-Item -ItemType Directory -Force (Join-Path $OutDir $d) | Out-Null }

Add-Type -Path (Join-Path $here 'IconTools.cs') -ReferencedAssemblies System.Drawing

# --- 1. render ------------------------------------------------------------
$svgs = @($map.Win32.Svg) + @($map.Packaged.Svg) | Sort-Object -Unique
foreach ($s in $svgs) {
    $svg = Join-Path $SvgDir "$s.svg"
    $png = Join-Path $OutDir "png\$s.png"
    if (-not (Test-Path $svg)) { Write-Warning "missing $svg"; continue }
    if ((Test-Path $png) -and (Get-Item $png).LastWriteTime -gt (Get-Item $svg).LastWriteTime) { continue }
    $html = Join-Path $OutDir "_render\$s.html"
    $uri  = ([Uri]$svg).AbsoluteUri
    Set-Content -LiteralPath $html -Encoding Ascii -Value "<!doctype html><html><head><meta http-equiv=`"Content-Security-Policy`" content=`"default-src 'none'; img-src file:; style-src 'unsafe-inline'`"><style>html,body{margin:0;padding:0;background:transparent;overflow:hidden}img{display:block;width:512px;height:512px}</style></head><body><img src=`"$uri`"></body></html>"
    $edgeArgs = @('--headless', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
        '--disable-extensions', '--hide-scrollbars',
        "--user-data-dir=$(Join-Path $OutDir '_edge')", '--force-device-scale-factor=1',
        '--default-background-color=00000000', '--window-size=512,512',
        "--screenshot=$png", ([Uri]$html).AbsoluteUri)
    $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    & $Edge @edgeArgs 2>&1 | Out-Null    # Edge logs to stderr; that is not an error
    $ErrorActionPreference = $prev
    if (-not (Test-Path $png)) { Write-Warning "render failed: $s" } else { "rendered $s" }
}

# --- 2. ico ---------------------------------------------------------------
foreach ($s in $svgs) {
    $png = Join-Path $OutDir "png\$s.png"
    if (Test-Path $png) { [MacIconTools]::WriteIco($png, $sizes, (Join-Path $OutDir "ico\$s.ico")) }
}

# --- 3. stub PE + per-target DLLs ----------------------------------------
$stub = Join-Path $OutDir '_render\stub.dll'
if (-not (Test-Path $stub)) {
    Add-Type -TypeDefinition 'namespace MacIconStub { public static class Empty { } }' -OutputAssembly $stub -OutputType Library
}

function Resolve-Original([string]$path) {
    $exp = [Environment]::ExpandEnvironmentVariables($path)
    if ($exp -match '[*?]') { return Get-ChildItem -Path $exp -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName }
    if (Test-Path -LiteralPath $exp) { return $exp }
    return $null
}

Get-ChildItem (Join-Path $OutDir 'dll') -File | Remove-Item -Force   # no stale redirect DLLs
Get-ChildItem (Join-Path $OutDir 'ico') -Filter '*.redirect.ico' -File | Remove-Item -Force
$std = [int[]](16, 20, 24, 32, 40, 48, 64, 96, 128, 256)   # Windows' usual set at 100-200% DPI
$lines = [System.Collections.Generic.List[object]]::new()
$report = foreach ($t in $map.Win32) {
    $orig = Resolve-Original $t.Path
    $png  = Join-Path $OutDir "png\$($t.Svg).png"
    if (-not $orig) { [pscustomobject]@{ Name = $t.Name; Status = 'not installed, skipped'; Groups = ''; Sizes = '' }; continue }
    if (-not (Test-Path $png)) { [pscustomobject]@{ Name = $t.Name; Status = 'no icon'; Groups = ''; Sizes = '' }; continue }
    $safe = ($t.Name -replace '[^A-Za-z0-9]+', '-').Trim('-').ToLower()

    if ($orig -like '*.ico') {
        # .ico originals are redirected to an .ico, never to a DLL: LoadImage(LR_LOADFROMFILE)
        # and PrivateExtractIcons(index 0) then both read a real icon file.
        $out = Join-Path $OutDir "ico\$safe.redirect.ico"
        [MacIconTools]::WriteIco($png, $std, $out)
        $ok = [MacIconTools]::TestExtract($out, 0, 32) -ge 1
        $lines.Add([pscustomobject]@{ Name = $t.Name; Line = "$($t.Path)=.\ico\$safe.redirect.ico" })
        [pscustomobject]@{ Name = $t.Name; Status = $(if ($ok) { 'ok (ico)' } else { 'SELF-TEST FAILED' }); Groups = '-'; Sizes = ($std -join ',') }
        continue
    }

    $names = [MacIconTools]::GetIconGroupNames($orig)
    if (-not $names -or $names.Count -eq 0) { [pscustomobject]@{ Name = $t.Name; Status = 'original has no icon groups'; Groups = ''; Sizes = '' }; continue }
    $groups = [System.Collections.Generic.List[string]]::new()
    foreach ($i in $t.Indexes) {
        if ($i -ge 0) { if ($i -lt $names.Count) { $groups.Add($names[$i]) } } else { $groups.Add("#$(-$i)") }
    }
    $groups = @($groups | Select-Object -Unique)
    # Mirror each original group: same id/name, same language, and at least every size
    # the original has (LR_EXACTSIZEONLY misses make the mod return NULL on purpose).
    $langs = @(); $sizes = [System.Collections.Generic.HashSet[int]]::new([int[]]$std)
    $keep = @()
    foreach ($g in $groups) {
        $info = [MacIconTools]::GetGroupInfo($orig, $g)
        if (-not $info) { Write-Warning "$($t.Name): group $g not in original, dropped"; continue }
        $keep += $g; $langs += $info[0]
        foreach ($s in $info[1..($info.Count - 1)]) { [void]$sizes.Add([Math]::Min(256, $s)) }
    }
    if (-not $keep) { [pscustomobject]@{ Name = $t.Name; Status = 'no matching groups'; Groups = ''; Sizes = '' }; continue }
    $sizeArr = [int[]]($sizes | Sort-Object)
    $dll = Join-Path $OutDir "dll\$safe.dll"
    [MacIconTools]::WriteResourceDll($stub, $png, $sizeArr, [string[]]$keep, [int[]]$langs, $dll)

    # self-test: every group must be extractable, and its size list must cover the original's
    $ok = $true
    foreach ($g in $keep) {
        if ($g -like '#*' -and [MacIconTools]::TestExtract($dll, -[int]$g.Substring(1), 32) -lt 1) { $ok = $false }
        $mine = [MacIconTools]::GetGroupInfo($dll, $g); $theirs = [MacIconTools]::GetGroupInfo($orig, $g)
        if (-not $mine) { $ok = $false; continue }
        foreach ($s in $theirs[1..($theirs.Count - 1)]) { if (($mine[1..($mine.Count - 1)]) -notcontains [Math]::Min(256, $s)) { $ok = $false } }
        if ($mine[0] -ne $theirs[0]) { $ok = $false }
    }
    if ([MacIconTools]::TestExtract($dll, 0, 256) -lt 1) { $ok = $false }
    $lines.Add([pscustomobject]@{ Name = $t.Name; Line = "$($t.Path)=.\dll\$safe.dll" })
    [pscustomobject]@{ Name = $t.Name; Status = $(if ($ok) { 'ok' } else { 'SELF-TEST FAILED' }); Groups = (($keep | ForEach-Object -Begin { $k = 0 } -Process { "$_@$($langs[$k])"; $k++ }) -join ','); Sizes = ($sizeArr -join ',') }
}
# Shortcut-only icons (LnkIcons) just need their .ico.
foreach ($l in $map.LnkIcons) { if (-not (Test-Path (Join-Path $OutDir "png\$($l.Svg).png"))) { Write-Warning "no icon for $($l.Name)" } }

function Write-Theme([string]$file, $entries) {
    $ini = @('; Generated by build-macos-icons.ps1. Icons: WhiteSur icon theme (vinceliuice), GPL-3.0.', '[redirections]') + @($entries | ForEach-Object Line)
    [IO.File]::WriteAllLines((Join-Path $OutDir $file), [string[]]$ini, [Text.Encoding]::ASCII)
}
Write-Theme 'theme.ini' $lines
Write-Theme 'theme-minimal.ini' @($lines | Where-Object { $map.Minimal -contains $_.Name })
Copy-Item (Join-Path $SvgDir 'LICENSE.txt') (Join-Path $OutDir 'LICENSE-WhiteSur-GPL-3.0.txt') -Force
Remove-Item -Recurse -Force (Join-Path $OutDir '_edge') -ErrorAction SilentlyContinue
$report | Format-Table -AutoSize | Out-String -Width 200
