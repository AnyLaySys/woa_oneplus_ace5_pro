param([string]$Root='C:\Users\sui\d3d10-20261109',[string]$Mode='present',[switch]$RequireWddm)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
if((Get-Process -Id $PID).SessionId -eq 0){throw 'The presentation test requires an interactive console session.'}
if($RequireWddm){$env:A830_REQUIRE_WDDM='true'}
# This process is the visible presentation test; the scheduling helper stays hidden.
$process=Start-Process "$Root\d3d-test.exe" -ArgumentList $Mode -WorkingDirectory $Root -WindowStyle Normal -PassThru -RedirectStandardOutput "$Root\present.out" -RedirectStandardError "$Root\present.err"
$null=$process.Handle
Start-Sleep -Seconds 4
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type 'using System.Runtime.InteropServices;public static class A830ConsoleDpi{[DllImport("user32.dll")]public static extern bool SetProcessDPIAware();}'
[A830ConsoleDpi]::SetProcessDPIAware() | Out-Null
$bounds=[Windows.Forms.Screen]::PrimaryScreen.Bounds
$image=New-Object Drawing.Bitmap $bounds.Width,$bounds.Height
$graphics=[Drawing.Graphics]::FromImage($image)
$graphics.CopyFromScreen($bounds.Location,[Drawing.Point]::Empty,$bounds.Size)
$image.Save("$Root\present.png",[Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose();$image.Dispose()
if(!$process.WaitForExit(25000)){$process.Kill();throw 'The presentation test exceeded its deadline.'}
@{SessionId=(Get-Process -Id $PID).SessionId;ExitCode=$process.ExitCode;Output=[string](Get-Content "$Root\present.out" -Raw);Error=[string](Get-Content "$Root\present.err" -Raw)} | ConvertTo-Json -Compress | Set-Content "$Root\present-status.json"
exit $process.ExitCode
