# Icon map for the macOS-style app icons (WhiteSur icon theme, GPL-3.0).
# Svg = file name (no extension) under third-party\macos-icons\whitesur.
#
# Win32: resource redirection through icon-resource-redirect (theme.ini in our
#   own theme folder). Path = original file as Windows asks for it (env vars and
#   * ? patterns allowed, matched case-insensitively). Indexes = icon indexes
#   (0 = the app icon) and/or negative resource ids that shortcuts reference.
#   The redirect DLL only carries those groups, so every other icon in the file
#   (file-type icons etc.) falls back to the original.
#
# Packaged: UWP/MSIX apps the mod cannot reach (their tiles come from PNGs in
#   WindowsApps). The install script makes new shortcuts to shell:AppsFolder\<AUMID>
#   with our .ico; nothing existing is changed.
@{
    Win32 = @(
        @{ Name = 'Edge';           Svg = 'microsoft-edge';      Path = '%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe';        Indexes = @(0) }
        @{ Name = 'Edge (pin .ico)';Svg = 'microsoft-edge';      Path = '%LOCALAPPDATA%\Microsoft\Edge\User Data\*\Edge Profile.ico';       Indexes = @(0) }
        @{ Name = 'Brave';          Svg = 'brave';               Path = '%ProgramFiles%\BraveSoftware\Brave-Browser\Application\brave.exe'; Indexes = @(0) }
        @{ Name = 'Chrome';         Svg = 'google-chrome';       Path = '%ProgramFiles%\Google\Chrome\Application\chrome.exe';              Indexes = @(0) }
        @{ Name = 'VS Code';        Svg = 'visual-studio-code';  Path = '%LOCALAPPDATA%\Programs\Microsoft VS Code\Code.exe';               Indexes = @(0) }
        @{ Name = 'Mattermost';     Svg = 'mattermost';          Path = '%ProgramFiles%\Mattermost\Mattermost.exe';                         Indexes = @(0) }
        @{ Name = 'Discord';        Svg = 'discord';             Path = '%LOCALAPPDATA%\Discord\app-*\Discord.exe';                         Indexes = @(0) }
        @{ Name = 'Discord stub';   Svg = 'discord';             Path = '%LOCALAPPDATA%\Discord\Discord.exe';                               Indexes = @(0) }
        @{ Name = 'Discord (.ico)'; Svg = 'discord';             Path = '%LOCALAPPDATA%\Discord\app.ico';                                   Indexes = @(0) }
        @{ Name = 'Docker Desktop'; Svg = 'docker';              Path = '%ProgramFiles%\Docker\Docker\Docker Desktop.exe';                  Indexes = @(0) }
        @{ Name = 'Postman';        Svg = 'postman';             Path = '%LOCALAPPDATA%\Postman\*Postman.exe';                              Indexes = @(0) }
        @{ Name = 'MongoDB Compass';Svg = 'mongodb-compass';     Path = '%LOCALAPPDATA%\MongoDBCompass\*MongoDBCompass.exe';                Indexes = @(0) }
        @{ Name = 'Steam';          Svg = 'steam';               Path = '%ProgramFiles(x86)%\Steam\steam.exe';                              Indexes = @(0) }
        @{ Name = 'VLC';            Svg = 'vlc';                 Path = '%ProgramFiles%\VideoLAN\VLC\vlc.exe';                              Indexes = @(0) }
        @{ Name = 'Free Download Manager'; Svg = 'freedownloadmanager'; Path = '%LOCALAPPDATA%\Programs\Softdeluxe\Free Download Manager\fdm.exe'; Indexes = @(0) }
        @{ Name = 'Google Drive';   Svg = 'google-drive';        Path = '%ProgramFiles%\Google\Drive File Stream\*\GoogleDriveFS.exe';     Indexes = @(0, 1) }
        @{ Name = '7-Zip';          Svg = 'file-roller';         Path = '%ProgramFiles%\7-Zip\7zFM.exe';                                    Indexes = @(0) }
        @{ Name = 'Word';           Svg = 'ms-word';             Path = '%ProgramFiles%\Microsoft Office\root\Office16\WINWORD.EXE';        Indexes = @(0) }
        @{ Name = 'Word (Start)';   Svg = 'ms-word';             Path = '%ProgramFiles%\Microsoft Office\Root\VFS\Windows\Installer\{90160000-000F-0000-1000-0000000FF1CE}\wordicon.exe'; Indexes = @(0) }
        @{ Name = 'Excel';          Svg = 'ms-excel';            Path = '%ProgramFiles%\Microsoft Office\root\Office16\EXCEL.EXE';          Indexes = @(0) }
        @{ Name = 'Excel (Start)';  Svg = 'ms-excel';            Path = '%ProgramFiles%\Microsoft Office\Root\VFS\Windows\Installer\{90160000-000F-0000-1000-0000000FF1CE}\xlicons.exe'; Indexes = @(0) }
        @{ Name = 'PowerPoint';     Svg = 'ms-powerpoint';       Path = '%ProgramFiles%\Microsoft Office\root\Office16\POWERPNT.EXE';       Indexes = @(0) }
        @{ Name = 'PowerPoint (Start)'; Svg = 'ms-powerpoint';   Path = '%ProgramFiles%\Microsoft Office\Root\VFS\Windows\Installer\{90160000-000F-0000-1000-0000000FF1CE}\pptico.exe'; Indexes = @(0) }
        @{ Name = 'OneNote';        Svg = 'ms-onenote';          Path = '%ProgramFiles%\Microsoft Office\root\Office16\ONENOTE.EXE';        Indexes = @(0) }
        @{ Name = 'OneNote (Start)';Svg = 'ms-onenote';          Path = '%ProgramFiles%\Microsoft Office\Root\VFS\Windows\Installer\{90160000-000F-0000-1000-0000000FF1CE}\joticon.exe'; Indexes = @(0) }
        @{ Name = 'Python';         Svg = 'python';              Path = '%LOCALAPPDATA%\Programs\Python\Python311\python.exe';              Indexes = @(0) }
        @{ Name = 'Python (w)';     Svg = 'python';              Path = '%LOCALAPPDATA%\Programs\Python\Python311\pythonw.exe';             Indexes = @(0) }
        @{ Name = 'Task Manager';   Svg = 'utilities-system-monitor'; Path = '%SystemRoot%\System32\Taskmgr.exe';                           Indexes = @(0, -30651) }
        @{ Name = 'Registry Editor';Svg = 'regedit';             Path = '%SystemRoot%\regedit.exe';                                         Indexes = @(0, -100) }
        @{ Name = 'Remote Desktop'; Svg = 'remmina';             Path = '%SystemRoot%\System32\mstsc.exe';                                  Indexes = @(0) }
        @{ Name = 'Character Map';  Svg = 'accessories-character-map'; Path = '%SystemRoot%\System32\charmap.exe';                          Indexes = @(0) }
        @{ Name = 'Command Prompt'; Svg = 'terminal';            Path = '%SystemRoot%\System32\cmd.exe';                                    Indexes = @(0) }
    )
    # Never redirect explorer.exe itself: it is the shell's own module, so every
    # icon/string/menu explorer loads from itself would go through the redirect
    # (2026-11-08: explorer crash loop with explorer.exe in theme.ini).
    # File Explorer gets its icon through its shortcuts instead (IconLocation,
    # originals backed up by the install script).
    LnkIcons = @(
        @{ Name = 'File Explorer'; Svg = 'file-manager'; Match = 'File Explorer.lnk'; IconLocation = '%windir%\explorer.exe,0' }
    )
    # Small, low-risk subset for the first test (theme-minimal.ini): apps that are
    # not running inside explorer and not shell modules.
    Minimal = @('VS Code', 'Mattermost', 'VLC')
    Packaged = @(
        @{ Name = 'Microsoft Store';  Svg = 'softwarecenter';       Aumid = 'Microsoft.WindowsStore_8wekyb3d8bbwe!App' }
        @{ Name = 'Terminal';         Svg = 'terminal';             Aumid = 'Microsoft.WindowsTerminal_8wekyb3d8bbwe!App' }
        @{ Name = 'Settings';         Svg = 'preferences-system';   Aumid = 'windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel' }
        @{ Name = 'Calculator';       Svg = 'calc';                 Aumid = 'Microsoft.WindowsCalculator_8wekyb3d8bbwe!App' }
        @{ Name = 'Notepad';          Svg = 'text-editor';          Aumid = 'Microsoft.WindowsNotepad_8wekyb3d8bbwe!App' }
        @{ Name = 'Photos';           Svg = 'shotwell';             Aumid = 'Microsoft.Windows.Photos_8wekyb3d8bbwe!App' }
        @{ Name = 'Paint';            Svg = 'gnome-paint';          Aumid = 'Microsoft.Paint_8wekyb3d8bbwe!App' }
        @{ Name = 'Camera';           Svg = 'accessories-camera';   Aumid = 'Microsoft.WindowsCamera_8wekyb3d8bbwe!App' }
        @{ Name = 'Outlook';          Svg = 'ms-outlook';           Aumid = 'Microsoft.OutlookForWindows_8wekyb3d8bbwe!Microsoft.OutlookforWindows' }
        @{ Name = 'Claude';           Svg = 'claude';               Aumid = 'Claude_pzs8sxrjxfjjc!Claude' }
        @{ Name = 'ChatGPT';          Svg = 'openai';               Aumid = 'OpenAI.Codex_2p2nqsd0c76g0!App' }
        @{ Name = 'Telegram Desktop'; Svg = 'telegram';             Aumid = 'TelegramMessengerLLP.TelegramDesktop_t4vj0pshhgkwm!Telegram.TelegramDesktop.Store' }
        @{ Name = 'Windows Security'; Svg = 'preferences-security'; Aumid = 'Microsoft.SecHealthUI_8wekyb3d8bbwe!SecHealthUI' }
        @{ Name = 'Windows Backup';   Svg = 'deja-dup';             Aumid = 'MicrosoftWindows.Client.CBS_cw5n1h2txyewy!WindowsBackup' }
        @{ Name = 'Get Help';         Svg = 'help';                 Aumid = 'Microsoft.GetHelp_8wekyb3d8bbwe!App' }
    )
}
