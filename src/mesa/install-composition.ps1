param(
    [string]$Source=$PSScriptRoot,
    [Parameter(Mandatory=$true)][string]$DriverDirectory,
    [string]$ExpectedDriverVersion='26.11.20.60',
    [switch]$Uninstall
)
$ErrorActionPreference='Stop'
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
if(-not ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
    throw 'Run PowerShell as administrator.'
}
$DriverDirectory=(Resolve-Path -LiteralPath $DriverDirectory).Path
$Source=(Resolve-Path -LiteralPath $Source).Path
if($identity.User.Value -ne 'S-1-5-18'){
    # DriverStore grants SYSTEM write access. A temporary task performs the
    # same checked transaction without changing directory ownership or ACLs.
    $tag=[Guid]::NewGuid().ToString('N')
    $task='A830-Composition-'+$tag
    $log=Join-Path ([IO.Path]::GetTempPath()) ($task+'.log')
    $errorLog=$log+'.error'
    function Quote([string]$Value){"'"+$Value.Replace("'","''")+"'"}
    $command='& '+(Quote $PSCommandPath)+' -Source '+(Quote $Source)+' -DriverDirectory '+(Quote $DriverDirectory)+' -ExpectedDriverVersion '+(Quote $ExpectedDriverVersion)
    if($Uninstall){$command+=' -Uninstall'}
    $body='$ErrorActionPreference="Stop";try{'+$command+'|Out-String|Set-Content -LiteralPath '+(Quote $log)+' -Encoding UTF8;exit 0}catch{$_|Out-String|Set-Content -LiteralPath '+(Quote $errorLog)+' -Encoding UTF8;exit 1}'
    $encoded=[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($body))
    $action=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -ExecutionPolicy Bypass -EncodedCommand '+$encoded)
    $principal=New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $settings=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 2)
    try{
        Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings|Out-Null
        Start-ScheduledTask -TaskName $task
        $deadline=(Get-Date).AddMinutes(2)
        do{
            Start-Sleep -Milliseconds 500
            $info=Get-ScheduledTaskInfo -TaskName $task
            $running=(Get-ScheduledTask -TaskName $task).State -eq 'Running'
            if((Get-Date) -gt $deadline){throw 'Composition installation task timed out.'}
        }while($running -or $info.LastRunTime.Year -lt 2000)
        if($info.LastTaskResult){
            $message=if(Test-Path -LiteralPath $errorLog){[string](Get-Content -LiteralPath $errorLog -Raw)}else{'Composition installation task failed: '+$info.LastTaskResult}
            throw $message
        }
        Get-Content -LiteralPath $log -Raw
    }finally{
        Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
        Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $log,$errorLog -ErrorAction SilentlyContinue
    }
    return
}
$state=Join-Path $env:ProgramData 'AnyLaySys\Composition'
$record=Join-Path $state 'state.json'
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash}
if($Uninstall){
    $saved=Get-Content -LiteralPath $record -Raw|ConvertFrom-Json
    foreach($file in $saved.Files){
        if((Hash $file.Backup) -ne $file.OriginalHash){throw "Backup checksum mismatch: $($file.Backup)"}
        if((Hash $file.Target) -ne $file.InstalledHash){throw "File changed after installation: $($file.Target)"}
    }
    foreach($file in $saved.Files){
        $removed=$file.Target+'.removed-A830Composition'
        if(Test-Path -LiteralPath $removed){throw "Restore destination already exists: $removed"}
        Move-Item -LiteralPath $file.Target -Destination $removed
        try{Move-Item -LiteralPath $file.Backup -Destination $file.Target}
        catch{Move-Item -LiteralPath $removed -Destination $file.Target;throw}
        Remove-Item -LiteralPath $removed -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $record
    'Restored the previous composition runtime. Restart affected applications.'
    return
}
$driver=Get-CimInstance Win32_PnPSignedDriver|Where-Object {$_.DeviceClass -eq 'DISPLAY' -and $_.DeviceName -eq 'Qualcomm Adreno 830'}
if(!$driver -or $driver.DriverVersion -ne $ExpectedDriverVersion){throw 'The display driver changed since inspection.'}
if(Test-Path -LiteralPath $record){throw 'A composition runtime installation already exists.'}
$names=@('qcom_dxgi_arm64.dll','qcom_dxgi_x64.dll','vulkan_freedreno.dll','vulkan_freedreno_x64.dll')
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
$files=@()
foreach($name in $names){
    $payload=Join-Path $Source $name
    $target=Join-Path $(if($name -like 'qcom_dxgi_*'){Join-Path $env:windir 'System32'}else{$DriverDirectory}) $name
    $data=[IO.File]::ReadAllBytes($payload)
    $offset=[BitConverter]::ToInt32($data,60)
    $machine=[BitConverter]::ToUInt16($data,$offset+4)
    $expected=if($name -like '*x64*'){0x8664}else{0xAA64}
    if($machine -ne $expected){throw "Unexpected architecture: $name"}
    $backup=$target+'.before-A830Composition-'+$stamp
    if(Test-Path -LiteralPath $backup){throw "Backup already exists: $backup"}
    $files+=[pscustomobject]@{Source=$payload;Target=$target;Backup=$backup;OriginalHash=Hash $target;InstalledHash=Hash $payload}
}
New-Item -ItemType Directory -Path $state -Force|Out-Null
[ordered]@{Revision='A830-Composition-1';DriverVersion=$driver.DriverVersion;Status='Preparing';Files=$files}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath $record -Encoding UTF8
$changed=New-Object Collections.Generic.List[object]
try{
    foreach($file in $files){
        # Rename preserves loaded mappings and the original file's ACL.
        Move-Item -LiteralPath $file.Target -Destination $file.Backup
        $changed.Add($file)
        Copy-Item -LiteralPath $file.Source -Destination $file.Target
        if((Hash $file.Target) -ne $file.InstalledHash){throw "Installation checksum mismatch: $($file.Target)"}
    }
}catch{
    $failure=$_
    for($i=$changed.Count-1;$i -ge 0;$i--){
        $file=$changed[$i]
        if(Test-Path -LiteralPath $file.Target){Remove-Item -LiteralPath $file.Target}
        Move-Item -LiteralPath $file.Backup -Destination $file.Target
    }
    Remove-Item -LiteralPath $record
    throw $failure
}
[ordered]@{Revision='A830-Composition-1';DriverVersion=$driver.DriverVersion;Status='Installed';InstalledAt=Get-Date -Format o;Files=$files}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath $record -Encoding UTF8
$files|Select-Object Target,InstalledHash,Backup|ConvertTo-Json
