<#
install-macos-icons.ps1 - macOS-style (WhiteSur) app icons on top of the macOSx
icon theme. RUN ELEVATED. Undo: uninstall-macos-icons.ps1.

What it does
  1. Backs up HKLM\SOFTWARE\Windhawk\Engine\Mods (reg export) and writes
     state.json (every value it changes, with the old value) into
     D:\backups\windhawk-2026-11-06\icons-backup-<time>\.
  2. Adds our theme folder (theme.ini + resource DLLs built by
     macos-icons\build-macos-icons.ps1) to icon-resource-redirect's
     Settings\themePaths[n]. The macOSx iconTheme stays as it is (system icons,
     folders, drives, generic file types).
  3. Grants read on the theme folder to AppContainer processes (ALL APPLICATION
     PACKAGES / ALL RESTRICTED APP PACKAGES) so Start/Search hosts can load it.
  4. Bumps SettingsChangeTime so running processes reload the settings.
  5. Packaged (UWP/MSIX) apps, which the mod cannot reach: creates NEW shortcuts
     (shell:AppsFolder\<AUMID>, our .ico) in -ShortcutDir for you to pin. With
     -AddToStartMenu they are also copied to Start Menu\Programs\macOS Icons.
     No existing shortcut is touched unless -LnkFallback is given.
  6. File Explorer shortcuts get the Finder icon via IconLocation (explorer.exe is
     never redirected). -LnkFallback: also sets IconLocation on existing Start/desktop/dock
     shortcuts of mapped Win32 apps (originals saved in lnk-backup.csv). Not
     needed when the mod works; it is a belt-and-braces option.

Never touches Program Files, WindowsApps or any app binary. Restarts nothing.
Afterwards: refresh the icon cache (ie4uinit.exe -show as your user), and for
Explorer/dock/Start to drop cached icons, sign out or restart Explorer yourself.
#>
param(
    [string]$ThemeDir    = 'D:\backups\windhawk-2026-11-06\icon-theme-macos',
    [string]$ThemeIni    = 'theme.ini',   # or theme-minimal.ini; themePaths[n] gets the .ini path
    [string]$ShortcutDir = 'D:\backups\windhawk-2026-11-06\macos-shortcuts',
    [string]$MapFile     = (Join-Path $PSScriptRoot 'macos-icons\icon-map.psd1'),
    [switch]$AddToStartMenu,
    [switch]$GroupWithApp,      # set System.AppUserModel.ID on packaged-app shortcuts
    [switch]$LnkFallback,
    [switch]$RefreshIconCache   # runs ie4uinit.exe -show (no restart)
)
$ErrorActionPreference = 'Stop'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run elevated (Start-Process powershell -Verb RunAs).' }
$ThemePath = Join-Path $ThemeDir $ThemeIni
if (-not (Test-Path $ThemePath)) { throw "No $ThemeIni in $ThemeDir - run macos-icons\build-macos-icons.ps1 first." }
if (Select-String -LiteralPath $ThemePath -Pattern '^[^;]*\\explorer\.exe=' -Quiet) { throw 'theme redirects explorer.exe - that crash-looped Explorer on 2026-11-08; rebuild.' }
$map = Import-PowerShellDataFile $MapFile

$modKeyPath = 'SOFTWARE\Windhawk\Engine\Mods\icon-resource-redirect'
$hklm  = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
$modKey = $hklm.OpenSubKey($modKeyPath, $true)
if (-not $modKey) { throw 'icon-resource-redirect is not installed.' }
$setKey = $modKey.OpenSubKey('Settings', $true)
if (-not $setKey) { $setKey = $modKey.CreateSubKey('Settings') }

# --- 1. backup ------------------------------------------------------------
$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
$backup = "D:\backups\windhawk-2026-11-06\icons-backup-$stamp"
New-Item -ItemType Directory -Force $backup | Out-Null
& reg.exe export 'HKLM\SOFTWARE\Windhawk\Engine\Mods' (Join-Path $backup 'windhawk-mods.reg') /y | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'reg export failed; nothing changed.' }
$state = [ordered]@{
    Created = (Get-Date).ToString('o'); ThemeDir = $ThemeDir
    RegistryValues = @(); Shortcuts = @(); LnkBackup = $null; AclGranted = $false
}
function Save-State { $state | ConvertTo-Json -Depth 5 | Set-Content -Encoding Ascii (Join-Path $backup 'state.json') }
Save-State

# --- 2. themePaths[n] -----------------------------------------------------
$names = $setKey.GetValueNames()
$existing = @($names | Where-Object { $_ -match '^themePaths\[(\d+)\]$' } | ForEach-Object {
    [pscustomobject]@{ Name = $_; Index = [int]$Matches[1]; Value = [string]$setKey.GetValue($_) } } | Sort-Object Index)
if ($existing | Where-Object { $_.Value -ieq $ThemePath -or $_.Value -ieq $ThemeDir }) {
    Write-Host 'Theme path already configured.'
} else {
    # Use the first empty slot; the mod stops reading at the first empty entry.
    $slot = $existing | Where-Object { -not $_.Value } | Select-Object -First 1
    $idx  = if ($slot) { $slot.Index } else { ($existing | Measure-Object Index -Maximum).Maximum + 1 }
    if ($null -eq $idx) { $idx = 0 }
    # Every slot before ours must be non-empty or the mod never reaches ours.
    $gap = $existing | Where-Object { $_.Index -lt $idx -and -not $_.Value }
    if ($gap) { throw "themePaths has an empty slot before index $idx; fix manually." }
    $name = "themePaths[$idx]"
    $old  = $setKey.GetValue($name, $null)
    $state.RegistryValues += [ordered]@{ Name = $name; Existed = ($null -ne $old); Old = $old }
    Save-State
    $setKey.SetValue($name, $ThemePath, 'String')
    Write-Host "Set $name = $ThemePath"
}

# --- 3. ACL for AppContainer readers ---------------------------------------
& icacls.exe $ThemeDir /grant '*S-1-15-2-1:(OI)(CI)RX' '*S-1-15-2-2:(OI)(CI)RX' | Out-Null
if ($LASTEXITCODE -eq 0) { $state.AclGranted = $true; Save-State } else { Write-Warning 'icacls grant failed; Start/Search hosts may not see the icons.' }

# --- 4. bump SettingsChangeTime --------------------------------------------
$state.RegistryValues += [ordered]@{ Name = '..\SettingsChangeTime'; Existed = $true; Old = $modKey.GetValue('SettingsChangeTime') }
Save-State
$modKey.SetValue('SettingsChangeTime', [int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds(), 'DWord')

# --- 5. shortcuts for packaged apps ----------------------------------------
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class LnkAumid {
    [StructLayout(LayoutKind.Sequential, Pack = 4)] public struct PROPERTYKEY { public Guid fmtid; public uint pid; }
    [StructLayout(LayoutKind.Explicit, Size = 24)] public struct PROPVARIANT { [FieldOffset(0)] public ushort vt; [FieldOffset(8)] public IntPtr p; }
    [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPropertyStore {
        void GetCount(out uint c); void GetAt(uint i, out PROPERTYKEY k); void GetValue(ref PROPERTYKEY k, out PROPVARIANT v);
        void SetValue(ref PROPERTYKEY k, ref PROPVARIANT v); void Commit();
    }
    [DllImport("shell32", CharSet = CharSet.Unicode, PreserveSig = false)]
    static extern void SHGetPropertyStoreFromParsingName(string path, IntPtr bc, int flags, ref Guid iid, [MarshalAs(UnmanagedType.Interface)] out IPropertyStore ps);
    public static void Set(string lnk, string aumid) {
        Guid iid = typeof(IPropertyStore).GUID; IPropertyStore ps;
        SHGetPropertyStoreFromParsingName(lnk, IntPtr.Zero, 2 /*GPS_READWRITE*/, ref iid, out ps);
        var key = new PROPERTYKEY { fmtid = new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"), pid = 5 };
        var v = new PROPVARIANT { vt = 31 /*VT_LPWSTR*/, p = Marshal.StringToCoTaskMemUni(aumid) };
        try { ps.SetValue(ref key, ref v); ps.Commit(); } finally { Marshal.FreeCoTaskMem(v.p); Marshal.ReleaseComObject(ps); }
    }
}
'@
$wsh = New-Object -ComObject WScript.Shell
New-Item -ItemType Directory -Force $ShortcutDir | Out-Null
$startDir = Join-Path ([Environment]::GetFolderPath('Programs')) 'macOS Icons'
$installed = @{}; Get-StartApps | ForEach-Object { $installed[$_.AppID] = $true }
foreach ($p in $map.Packaged) {
    $ico = Join-Path $ThemeDir "ico\$($p.Svg).ico"
    if (-not $installed.ContainsKey($p.Aumid)) { Write-Host "skip $($p.Name): not installed"; continue }
    if (-not (Test-Path $ico)) { Write-Warning "skip $($p.Name): no icon"; continue }
    $targets = @(Join-Path $ShortcutDir "$($p.Name).lnk")
    if ($AddToStartMenu) { New-Item -ItemType Directory -Force $startDir | Out-Null; $targets += Join-Path $startDir "$($p.Name).lnk" }
    foreach ($lnkPath in $targets) {
        if (Test-Path -LiteralPath $lnkPath) { Write-Host "exists, left alone: $lnkPath"; continue }
        $l = $wsh.CreateShortcut($lnkPath)
        $l.TargetPath = "$env:SystemRoot\explorer.exe"
        $l.Arguments = "shell:AppsFolder\$($p.Aumid)"
        $l.IconLocation = "$ico,0"
        $l.Description = $p.Name
        $l.Save()
        if ($GroupWithApp) { try { [LnkAumid]::Set($lnkPath, $p.Aumid) } catch { Write-Warning "AUMID not set on ${lnkPath}: $_" } }
        $state.Shortcuts += $lnkPath
    }
}
Save-State

# --- 6. shortcut icons -----------------------------------------------------
# Always: LnkIcons (File Explorer - explorer.exe must not be redirected), only on
# shortcuts whose IconLocation is still the stock one. -LnkFallback: also mapped Win32 apps.
$csv = Join-Path $backup 'lnk-backup.csv'
$state.LnkBackup = $csv; Save-State
$lnkDirs = @("$env:APPDATA\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar",
             [Environment]::GetFolderPath('Programs'), [Environment]::GetFolderPath('CommonPrograms'),
             [Environment]::GetFolderPath('DesktopDirectory'), [Environment]::GetFolderPath('CommonDesktopDirectory')) | Select-Object -Unique
$lnkRows = [System.Collections.Generic.List[object]]::new()
foreach ($li in $map.LnkIcons) {
    $ico = Join-Path $ThemeDir "ico\$($li.Svg).ico"
    if (-not (Test-Path $ico)) { Write-Warning "no icon for $($li.Name)"; continue }
    foreach ($d in $lnkDirs) {
        foreach ($f in Get-ChildItem -LiteralPath $d -Filter $li.Match -Recurse -ErrorAction SilentlyContinue) {
            $l = $wsh.CreateShortcut($f.FullName)
            if ($l.IconLocation -ine $li.IconLocation) { Write-Host "left alone (custom icon): $($f.FullName)"; continue }
            $lnkRows.Add([pscustomobject]@{ Path = $f.FullName; IconLocation = $l.IconLocation; LastWriteTimeUtc = $f.LastWriteTimeUtc.ToString('o') })
            $lnkRows | Export-Csv -NoTypeInformation -Encoding UTF8 $csv   # saved before each change
            $l.IconLocation = "$ico,0"; $l.Save()
            Write-Host "icon set: $($f.FullName)"
        }
    }
}
if ($LnkFallback) {
    $exeToIco = @{}
    foreach ($t in $map.Win32) {
        $exp = [Environment]::ExpandEnvironmentVariables($t.Path)
        if ($exp -match '\.exe$') {
            foreach ($f in @(Get-ChildItem -Path $exp -ErrorAction SilentlyContinue)) { $exeToIco[$f.FullName.ToUpper()] = Join-Path $ThemeDir "ico\$($t.Svg).ico" }
        }
    }
    foreach ($d in $lnkDirs) {
        foreach ($f in Get-ChildItem -LiteralPath $d -Filter *.lnk -Recurse -ErrorAction SilentlyContinue) {
            if ($state.Shortcuts -contains $f.FullName) { continue }
            $l = $wsh.CreateShortcut($f.FullName)
            if (-not $l.TargetPath) { continue }
            $key = $l.TargetPath.ToUpper()
            if (-not $exeToIco.ContainsKey($key)) { continue }
            # Shortcuts pointing at a secondary icon (e.g. Sticky Notes -> joticon.exe,5) keep theirs.
            if ($l.IconLocation -match ',(\d+)$' -and [int]$Matches[1] -gt 1) { continue }
            $lnkRows.Add([pscustomobject]@{ Path = $f.FullName; IconLocation = $l.IconLocation; LastWriteTimeUtc = $f.LastWriteTimeUtc.ToString('o') })
            $lnkRows | Export-Csv -NoTypeInformation -Encoding UTF8 $csv
            $l.IconLocation = "$($exeToIco[$key]),0"
            $l.Save()
        }
    }
}
Write-Host "Changed IconLocation on $($lnkRows.Count) shortcut(s); originals in $csv"

if ($RefreshIconCache) { & "$env:SystemRoot\System32\ie4uinit.exe" -show }

Write-Host ''
Write-Host "Done. Backup: $backup"
Write-Host 'Nothing was restarted. To see the new icons:'
Write-Host '  - run  ie4uinit.exe -show  as your normal user (refreshes the shell icon cache);'
Write-Host '  - apps launched from now on pick up the redirect; the dock, Start and Explorer'
Write-Host '    keep cached icons until Explorer restarts or you sign out (your call when).'
Write-Host "  - Packaged apps: pin the shortcuts in $ShortcutDir to the dock in place of the"
Write-Host '    original Store/Terminal pins (Windows does not allow scripted pinning).'
