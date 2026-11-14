# UI checks for Usage Monitor: open the table, check centering, screenshot, memory.
#   powershell -File tools\uitest.ps1 [-Shot path.png] [-Close]
param([string]$Shot = "$env:TEMP\usage-monitor-shot.png", [switch]$Close)
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class UM {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, IntPtr t);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
$main = [UM]::FindWindow('UsageMonitorMain', [IntPtr]::Zero)
if ($main -eq [IntPtr]::Zero) { 'app not running'; exit 1 }
[UM]::PostMessage($main, 0x0111, [IntPtr]101, [IntPtr]::Zero) | Out-Null   # WM_COMMAND ID_OPEN
Start-Sleep -Milliseconds 800
$t = [UM]::FindWindow('UsageMonitorTable', [IntPtr]::Zero)
if ($t -eq [IntPtr]::Zero) { 'table window not found'; exit 1 }
$r = New-Object UM+RECT; [UM]::GetWindowRect($t, [ref]$r) | Out-Null
$wa = [System.Windows.Forms.Screen]::FromPoint([System.Windows.Forms.Cursor]::Position).WorkingArea
$dx = ($r.L + $r.R) / 2 - ($wa.Left + $wa.Right) / 2
$dy = ($r.T + $r.B) / 2 - ($wa.Top + $wa.Bottom) / 2
"window: $($r.L),$($r.T) - $($r.R),$($r.B)  work area: $wa  center offset: $dx, $dy"
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc(); [UM]::PrintWindow($t, $dc, 2) | Out-Null; $g.ReleaseHdc($dc); $bmp.Save($Shot); $g.Dispose(); $bmp.Dispose()
"screenshot: $Shot"
$p = Get-Process usage-monitor
'open:   Private {0:N1} MB  WorkingSet {1:N1} MB' -f ($p.PrivateMemorySize64/1MB), ($p.WorkingSet64/1MB)
if ($Close) {
  [UM]::PostMessage($t, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE
  Start-Sleep -Milliseconds 800; $p.Refresh()
  'closed: Private {0:N1} MB  WorkingSet {1:N1} MB' -f ($p.PrivateMemorySize64/1MB), ($p.WorkingSet64/1MB)
}
