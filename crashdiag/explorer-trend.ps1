# Appends one row of Explorer resource usage to explorer-trend.csv.
# Read-only: only queries process counters. Run hourly by the "ExplorerTrend" scheduled task.
$csv = 'D:\backups\windhawk-2026-11-06\explorer-trend.csv'
Add-Type -Namespace ET -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern uint GetGuiResources(System.IntPtr h, uint f);
[DllImport("kernel32.dll")] public static extern System.IntPtr OpenProcess(uint a, bool i, int pid);
[DllImport("kernel32.dll")] public static extern bool CloseHandle(System.IntPtr h);
'@
if (-not (Test-Path $csv)) {
    'timestamp,pid,start_time,private_mb,ws_mb,handles,gdi,user,threads' | Out-File $csv -Encoding ascii
}
foreach ($p in Get-Process explorer -ErrorAction SilentlyContinue) {
    $h = [ET.U]::OpenProcess(0x1000, $false, $p.Id)   # PROCESS_QUERY_LIMITED_INFORMATION
    $gdi = [ET.U]::GetGuiResources($h, 0); $usr = [ET.U]::GetGuiResources($h, 1)
    [void][ET.U]::CloseHandle($h)
    $row = '{0},{1},{2},{3:F1},{4:F1},{5},{6},{7},{8}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $p.Id,
        $p.StartTime.ToString('yyyy-MM-dd HH:mm:ss'), ($p.PrivateMemorySize64 / 1MB), ($p.WorkingSet64 / 1MB),
        $p.HandleCount, $gdi, $usr, $p.Threads.Count
    Add-Content -Path $csv -Value $row -Encoding ascii
}
