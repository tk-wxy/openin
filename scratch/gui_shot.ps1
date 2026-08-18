# gui_shot.ps1 — 验证画布可见性 + 抓取主窗口渲染(PS5.1 / System.Drawing)
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File gui_shot.ps1 [-ScrollTicks N]
param([int]$ScrollTicks = 0)
Add-Type -AssemblyName System.Drawing
$gref = [System.Drawing.Bitmap].Assembly.Location
Add-Type -ReferencedAssemblies $gref @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Text;
using System.Runtime.InteropServices;
public class GShot {
  public delegate bool CW(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, CW cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
   [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
   [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  public struct RECT { public int L,T,R,B; }
  static string Cls(IntPtr h){ var s=new StringBuilder(80); GetClassNameW(h,s,80); return s.ToString().TrimEnd('\0'); }
  public static string Verify(IntPtr main){
    IntPtr panel=IntPtr.Zero, canvas=IntPtr.Zero;
    EnumChildWindows(main,(h,l)=>{ if(panel==IntPtr.Zero && Cls(h)=="openin_list") panel=h; return true; },IntPtr.Zero);
    EnumChildWindows(panel,(h,l)=>{ if(canvas==IntPtr.Zero && Cls(h)=="openin_list") canvas=h; return true; },IntPtr.Zero);
    return "panelVis="+IsWindowVisible(panel)+" canvasVis="+IsWindowVisible(canvas);
  }
  public static void MakeDpiAware(){ SetProcessDPIAware(); }
  public static void Shot(IntPtr h, string path){
    RECT r; GetWindowRect(h,out r);
    int w=r.R-r.L, ht=r.B-r.T;
    using(Bitmap bmp=new Bitmap(w,ht)){
      using(Graphics g=Graphics.FromImage(bmp)){
        IntPtr dc=g.GetHdc();
        PrintWindow(h,dc,0);
        g.ReleaseHdc(dc);
      }
      bmp.Save(path,ImageFormat.Png);
      bmp.Save(System.IO.Path.ChangeExtension(path,".jpg"),ImageFormat.Jpeg);
      Console.WriteLine("size="+w+"x"+ht+" Analyze="+Analyze(bmp));
    }
  }
  static string Analyze(Bitmap bmp){
    // 背景基准 = 四角平均;统计显著偏离像素占比;分三带(顶部按钮/中部列表/底部状态)
    System.Drawing.Color[] corners={bmp.GetPixel(2,2),bmp.GetPixel(bmp.Width-3,2),bmp.GetPixel(2,bmp.Height-3),bmp.GetPixel(bmp.Width-3,bmp.Height-3)};
    int br=0,bg=0,bb=0; foreach(var c in corners){ br+=c.R; bg+=c.G; bb+=c.B; }
    br/=4; bg/=4; bb/=4;
    int W=bmp.Width,H=bmp.Height;
    int[] bands=new int[4]; // 0=全部,1=顶部1/4,2=中部,3=底部1/4
    long total=0;
    for(int y=0;y<H;y+=2)
      for(int x=0;x<W;x+=2){
        var c=bmp.GetPixel(x,y);
        int diff=Math.Abs(c.R-br)+Math.Abs(c.G-bg)+Math.Abs(c.B-bb);
        if(diff>40){ total++; int zone=y<H/4?1:(y<3*H/4?2:3); bands[zone]++; }
      }
    string fmt0="all="+(100.0*total/(H*W/4)).ToString("F1")+"%";
    string fmt1="top="+(100.0*bands[1]/(H/4*(W)/2)).ToString("F1")+"%";
    string fmt2="mid="+(100.0*bands[2]/(H/2*(W)/2)).ToString("F1")+"%";
    string fmt3="bot="+(100.0*bands[3]/(H/4*(W)/2)).ToString("F1")+"%";
    return "bg=("+br+","+bg+","+bb+") "+fmt0+" "+fmt1+" "+fmt2+" "+fmt3;
  }
}
'@
[void][GShot]::MakeDpiAware()
$p = Start-Process -FilePath "$PSScriptRoot\..\openin.exe" -PassThru
Start-Sleep 4
$proc = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
if(-not $proc){ Write-Output "DEAD"; exit }
$main = $proc.MainWindowHandle
Write-Output ([GShot]::Verify($main))
if($ScrollTicks -gt 0){
  for($i=0;$i -lt $ScrollTicks;$i++){ [void][GShot]::PostMessageW($main, 0x115, [IntPtr]1, [IntPtr]0); Start-Sleep -Milliseconds 120 }
  Start-Sleep -Milliseconds 200
  Write-Output ("scrolled x"+$ScrollTicks)
}
[GShot]::Shot($main, "$PSScriptRoot\shot.png")
Stop-Process -Id $p.Id -Force -Confirm:$false
