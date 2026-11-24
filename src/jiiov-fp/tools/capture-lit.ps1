param(
    [Parameter(Mandatory=$true)][string]$PackagePath,
    [int]$CenterFromBottom=260,
    [int]$Diameter=195,
    [switch]$ExtractFeature,
    [switch]$ReuseSession,
    [switch]$AllowRotation
)
$ErrorActionPreference='Stop'
if ($ReuseSession -and -not $ExtractFeature) { throw 'ReuseSession requires ExtractFeature' }
$PackagePath=(Resolve-Path $PackagePath).Path
foreach ($file in 'jiiov_fp_check.exe','uff_spi.elf','uff_jv.elf') {
    if (-not (Test-Path (Join-Path $PackagePath $file))) { throw "Missing $file" }
}
if ($CenterFromBottom -lt 1 -or $CenterFromBottom -gt 2000 -or $Diameter -lt 32 -or $Diameter -gt 600) {
    throw 'Invalid illumination geometry'
}
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type @'
using System.Runtime.InteropServices;
public static class JfpCaptureDesktop {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    private static extern bool EnumDisplaySettings(string device, int mode, System.IntPtr settings);
    public static int Rotation() {
        System.IntPtr settings=Marshal.AllocHGlobal(220);
        try {
            Marshal.Copy(new byte[220],0,settings,220);
            Marshal.WriteInt16(settings,68,220);
            return EnumDisplaySettings(null,-1,settings) ? Marshal.ReadInt32(settings,84) : -1;
        } finally { Marshal.FreeHGlobal(settings); }
    }
}
'@
[JfpCaptureDesktop]::SetProcessDPIAware() | Out-Null
$bounds=[Windows.Forms.Screen]::PrimaryScreen.Bounds
function Update-Geometry {
    $script:bounds=[Windows.Forms.Screen]::PrimaryScreen.Bounds
    $script:rotation=[JfpCaptureDesktop]::Rotation()
    $scale=[Math]::Min($bounds.Width,$bounds.Height)/1264.0
    $script:size=[int]($Diameter*$scale)
    $offset=$CenterFromBottom*$scale
    $cx=$bounds.Width/2.0
    $cy=$bounds.Height-$offset
    if ($bounds.Width -gt $bounds.Height) {
        if ($rotation -eq 1) { $cx=$bounds.Width-$offset }
        elseif ($rotation -eq 3) { $cx=$offset }
        else { throw 'Display rotation is unknown; use portrait orientation for the test' }
        $cy=$bounds.Height/2.0
    } elseif ($rotation -eq 2) { $cy=$offset }
    $script:x=[int]($cx-$size/2.0)
    $script:y=[int]($cy-$size/2.0)
    if ($x -lt 0 -or $y -lt 0 -or $x+$size -gt $bounds.Width -or $y+$size -gt $bounds.Height) {
        throw 'Circle is outside the display'
    }
}
Update-Geometry
$form=[Windows.Forms.Form]::new()
$form.FormBorderStyle='None'
$form.StartPosition='Manual'
$form.Bounds=$bounds
$form.BackColor=[Drawing.Color]::Black
$form.TopMost=$true
$form.KeyPreview=$true
$text=[Windows.Forms.Label]::new()
$text.Text="指纹采图测试（尚未录入 Windows Hello）`n按住下方白圈即可测试；Esc 退出。"
if (-not $AllowRotation -and $bounds.Width -gt $bounds.Height) {
    $text.Text="请先将手机竖屏，白圈会自动显示。`n本次测试尚未录入 Windows Hello；Esc 退出。"
}
$text.ForeColor=[Drawing.Color]::White
$text.Font=[Drawing.Font]::new('Microsoft YaHei',20)
$text.AutoSize=$false
$text.SetBounds(20,40,$bounds.Width-40,200)
$form.Controls.Add($text)
$state=@{ Process=$null; Delay=$null; Started=$null; Tests=0; Closing=$false }
$timer=[Windows.Forms.Timer]::new()
$timer.Interval=100
$form.Add_Paint({ param($sender,$event)
    if (-not $AllowRotation -and $bounds.Width -gt $bounds.Height) { return }
    $event.Graphics.FillEllipse([Drawing.Brushes]::White,$x,$y,$size,$size)
})
$form.Add_MouseDown({ param($sender,$event)
    if (-not $AllowRotation -and $bounds.Width -gt $bounds.Height) { return }
    if ($state.Process -or $state.Delay) { return }
    $dx=$event.X-($x+$size/2); $dy=$event.Y-($y+$size/2)
    if ($dx*$dx+$dy*$dy -gt $size*$size/4) { return }
    $state.Delay=[DateTime]::UtcNow.AddMilliseconds(600)
    $text.Text="请继续按住白圈，正在采图……`n本次测试只检查采图状态，不保存指纹模板。"
})
$timer.Add_Tick({
    $current=[Windows.Forms.Screen]::PrimaryScreen.Bounds
    if ($current.Width -ne $bounds.Width -or $current.Height -ne $bounds.Height -or [JfpCaptureDesktop]::Rotation() -ne $rotation) {
        try {
            Update-Geometry
            $form.Bounds=$bounds
            $text.Width=$bounds.Width-40
            if (-not $state.Process -and -not $state.Delay) {
                $text.Text=if (-not $AllowRotation -and $bounds.Width -gt $bounds.Height) {
                    "请先将手机竖屏，白圈会自动显示。`n本次测试尚未录入 Windows Hello；Esc 退出。"
                } else { "指纹采图测试（尚未录入 Windows Hello）`n按住下方白圈即可测试；Esc 退出。" }
            }
            $form.Invalidate()
        } catch { $text.Text=$_.Exception.Message; $state.Delay=$null; return }
    }
    if ($state.Delay -and [DateTime]::UtcNow -ge $state.Delay) {
        $state.Delay=$null
        $state.Tests++
        $log=Join-Path $PackagePath ('capture-lit-'+$state.Tests+'.log')
        $errors=Join-Path $PackagePath ('capture-lit-'+$state.Tests+'.err')
        $command=if ($ReuseSession) { 'session-feature' } elseif ($ExtractFeature) { 'sample-feature' } else { 'capture' }
        $arguments=$command+' "'+(Join-Path $PackagePath 'uff_spi.elf')+'" "'+(Join-Path $PackagePath 'uff_jv.elf')+'"'
        try {
            $state.Process=Start-Process (Join-Path $PackagePath 'jiiov_fp_check.exe') -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError $errors
            $state.Started=[DateTime]::UtcNow
        } catch { $text.Text="无法启动采图：$($_.Exception.Message)`nEsc 退出。" }
    }
    if ($state.Process -and $state.Process.HasExited) {
        $log=Join-Path $PackagePath ('capture-lit-'+$state.Tests+'.log')
        $results=Get-Content $log -Raw
        $outcome=if ($results -match 'TeeStatus=00000000') {
            if ($ReuseSession) { '连续采图和算法调用已通过，样本质量待核对。' }
            elseif ($ExtractFeature) { '采图和算法调用已通过，样本质量待核对。' } else { '采图已通过。' }
        } elseif ($results -match 'CaptureResult=00000000') { '已有采图通过，完整测试尚未通过。' }
        else { '本次采图尚未通过，测试结果已保存。' }
        $text.Text="测试已结束，可以松手。`n$outcome`n再次按白圈可重试；Esc 退出。"
        $state.Process.Dispose()
        $state.Process=$null
        if ($state.Closing) { $form.Close() }
    }
    if ($state.Process -and ([DateTime]::UtcNow-$state.Started).TotalSeconds -gt 30) {
        $text.Text="驱动仍在等待响应，请松手。`n程序会保留现场，响应结束后可退出。"
    }
})
$form.Add_KeyDown({ param($sender,$event)
    if ($event.KeyCode -eq 'Escape') { $form.Close() }
})
$form.Add_FormClosing({ param($sender,$event)
    if ($state.Process -and -not $state.Process.HasExited) {
        $event.Cancel=$true
        $state.Closing=$true
        $text.Text="可以松手；等待本次驱动请求结束后退出。"
    }
})
$form.Add_Shown({
    @{ Width=$bounds.Width; Height=$bounds.Height; CenterX=$x+$size/2; CenterY=$y+$size/2;
        Diameter=$size; Rotation=$rotation; ProcessId=$PID; Ready=$true } | ConvertTo-Json |
        Set-Content (Join-Path $PackagePath 'capture-ui-ready.json') -Encoding UTF8
})
$brightness=$null
$method=$null
try {
    $brightness=Get-CimInstance -Namespace root/WMI -ClassName WmiMonitorBrightness -ErrorAction SilentlyContinue | Select-Object -First 1
    $method=Get-CimInstance -Namespace root/WMI -ClassName WmiMonitorBrightnessMethods -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($brightness -and $method) {
        Invoke-CimMethod -InputObject $method -MethodName WmiSetBrightness -Arguments @{Timeout=[uint32]0;Brightness=[byte]100} | Out-Null
    }
    $timer.Start()
    [Windows.Forms.Application]::Run($form)
} finally {
    $timer.Stop()
    $timer.Dispose()
    $form.Dispose()
    if ($brightness -and $method) {
        Invoke-CimMethod -InputObject $method -MethodName WmiSetBrightness -Arguments @{Timeout=[uint32]0;Brightness=[byte]$brightness.CurrentBrightness} -ErrorAction SilentlyContinue | Out-Null
    }
}
