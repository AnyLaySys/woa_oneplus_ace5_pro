param([Parameter(Mandatory=$true)][string]$Package)
$ErrorActionPreference='Stop'
$file=Join-Path $Package 'qcom_npu_ep.msix'
if(!(Test-Path $file)){throw 'The Windows ML provider package is missing.'}
$name='AnyLaySys-NpuEp-'+[Guid]::NewGuid().ToString('N')
$script=Join-Path $env:TEMP ($name+'.ps1')
[IO.File]::WriteAllText($script,'param([string]$File); $ErrorActionPreference="Stop"; Add-AppxPackage -Path $File',[Text.UTF8Encoding]::new($true))
try {
    $action=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$script`" `"$file`""
    $principal=New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited
    $settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::FromMinutes(2)) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    Register-ScheduledTask -TaskName $name -Action $action -Principal $principal -Settings $settings | Out-Null
    Start-ScheduledTask $name
    for($i=0;$i -lt 120;$i++) {
        Start-Sleep -Milliseconds 500
        $info=Get-ScheduledTaskInfo $name
        if((Get-ScheduledTask $name).State -eq 'Ready' -and $info.LastRunTime.Year -gt 2000 -and $info.LastTaskResult -notin @(267009,267011)){break}
    }
    if((Get-ScheduledTask $name).State -eq 'Running'){throw 'Windows ML package installation is still running.'}
    if($info.LastTaskResult){throw "Windows ML package installation failed: $($info.LastTaskResult)"}
    $installed=Get-AppxPackage 'AnyLaySys.QcomNpu.ExecutionProvider'
    if(!$installed){throw 'The Windows ML provider is not registered for this user.'}
    Write-Output "Windows ML provider: $($installed.PackageFullName)"
} finally {
    if(Get-ScheduledTask $name -ErrorAction SilentlyContinue){Stop-ScheduledTask $name -ErrorAction SilentlyContinue;Unregister-ScheduledTask $name -Confirm:$false}
    Remove-Item $script -Force -ErrorAction SilentlyContinue
}
