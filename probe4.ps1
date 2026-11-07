$s = 'C:\Users\shiva\AppData\Local\Temp\claude\dd6418d2605d4fa5\ae7d919f-98d3-467e-8ca0-aae07bf2d6fb\scratchpad'
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices; using System.Collections.Generic; using System.Diagnostics;
public static class W6 {
 [StructLayout(LayoutKind.Sequential)] public struct RECT{public int L,T,R,B;}
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr FindWindowEx(IntPtr p,IntPtr a,string c,string t);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h,int a,out int v,int s);
 [DllImport("user32.dll",SetLastError=true)] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int cx,int cy,uint f);
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern void keybd_event(byte v,byte s,uint f,UIntPtr e);
 public static int Cloak(IntPtr h){ int v=-1; DwmGetWindowAttribute(h,14,out v,4); return v; }
 public static string Rect(IntPtr h){ RECT r; GetWindowRect(h,out r); return r.L+","+r.T+","+r.R+","+r.B; }
 public static string Proc(IntPtr h){ uint p; GetWindowThreadProcessId(h,out p); try{ return Process.GetProcessById((int)p).ProcessName; }catch{ return "?"; } }
 public static string Fg(){ return Proc(GetForegroundWindow()); }
 public static string Cls(IntPtr h){ var c=new StringBuilder(128); GetClassName(h,c,128); return c.ToString(); }
 public static List<IntPtr> ShellVisible(){ var o=new List<IntPtr>(); IntPtr a=IntPtr.Zero; while((a=FindWindowEx(IntPtr.Zero,a,null,null))!=IntPtr.Zero){ var n=Proc(a); if((n=="ShellHost"||n=="ShellExperienceHost")&&IsWindowVisible(a)&&Cloak(a)==0){ RECT r; GetWindowRect(a,out r); if(r.R-r.L>50) o.Add(a);} } return o; }
}
'@
[void][W6]::SetProcessDPIAware()
function Save-Right($n){ $b=New-Object Drawing.Bitmap 520,1080; $g=[Drawing.Graphics]::FromImage($b); $g.CopyFromScreen(1400,0,0,0,$b.Size); $g.Dispose(); $d=New-Object Drawing.Bitmap 390,810; $g2=[Drawing.Graphics]::FromImage($d); $g2.DrawImage($b,0,0,390,810); $g2.Dispose(); $d.Save("$s\$n",[Drawing.Imaging.ImageFormat]::Png); $d.Dispose(); $b.Dispose() }
function Win-Key($v){ [W6]::keybd_event(0x5B,0,0,[UIntPtr]::Zero); [W6]::keybd_event($v,0,0,[UIntPtr]::Zero); [W6]::keybd_event($v,0,2,[UIntPtr]::Zero); [W6]::keybd_event(0x5B,0,2,[UIntPtr]::Zero) }
function Press-Esc(){ if([W6]::Fg() -match 'ShellHost|ShellExperienceHost'){ [W6]::keybd_event(0x1B,0,0,[UIntPtr]::Zero); [W6]::keybd_event(0x1B,0,2,[UIntPtr]::Zero); Start-Sleep -Milliseconds 600 } }

"foreground at start: " + [W6]::Fg()
"shell windows visible at start: " + (([W6]::ShellVisible() | ForEach-Object { [W6]::Cls($_) + ' ' + [W6]::Rect($_) }) -join ' ; ')
Win-Key 0x41  # Win+A
Start-Sleep -Milliseconds 1400
"foreground after Win+A: " + [W6]::Fg()
$list=[W6]::ShellVisible()
foreach($h in $list){ "visible: $([W6]::Proc($h)) [$([W6]::Cls($h))] rect=$([W6]::Rect($h))" }
$qs=$list | Where-Object { [W6]::Cls($_) -eq 'ControlCenterWindow' } | Select-Object -First 1
if(-not $qs){ "ControlCenterWindow not among visible windows"; Save-Right 'q-none.png'; Press-Esc; return }
Save-Right 'q-native.png'
$r=New-Object W6+RECT; [void][W6]::GetWindowRect($qs,[ref]$r)
$ok=[W6]::SetWindowPos($qs,[IntPtr]::Zero,$r.L,($r.T-224),0,0,0x0015)
"move result=$ok err=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
Start-Sleep -Milliseconds 800
"after move: rect=$([W6]::Rect($qs)) cloak=$([W6]::Cloak($qs)) foreground=$([W6]::Fg())"
Save-Right 'q-moved.png'
Press-Esc
Start-Sleep -Milliseconds 500
"--- second open: does Windows restore its own position? ---"
Win-Key 0x41; Start-Sleep -Milliseconds 1300
$list2=[W6]::ShellVisible(); $q2=$list2 | Where-Object { [W6]::Cls($_) -eq 'ControlCenterWindow' } | Select-Object -First 1
if($q2){ "reopened: same hwnd=$($q2 -eq $qs) rect=$([W6]::Rect($q2))" } else { "reopen: not visible" }
Press-Esc
"foreground at end: " + [W6]::Fg()