param([string]$Payload=$PSScriptRoot,[switch]$NoStart)
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
if($env:PROCESSOR_ARCHITECTURE -ne 'ARM64'){throw 'This runtime requires Windows ARM64'}
$null=Get-PnpDevice -InstanceId 'ACPI\HMRW7861\0' -ErrorAction Stop
if([version](Get-PnpDeviceProperty -InstanceId 'ACPI\HMRW7861\0' -KeyName DEVPKEY_Device_DriverVersion).Data -lt [version]'26.11.5.39'){throw 'Install driver 26.11.5.39 or newer first'}
if(!$PSBoundParameters.ContainsKey('Payload') -and (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'runtime\mihomo.exe'))){$Payload=Join-Path $PSScriptRoot 'runtime'}

# A supplied offline payload is preferred. Otherwise fetch only pinned upstream releases.
if(!(Test-Path -LiteralPath (Join-Path $Payload 'mihomo.exe'))){
 $Payload=Join-Path ([IO.Path]::GetTempPath()) ('WoA-NetRuntime-'+[Guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $Payload|Out-Null
 Invoke-WebRequest -UseBasicParsing 'https://github.com/MetaCubeX/mihomo/releases/download/v1.19.33/mihomo-windows-arm64-v1.19.33.zip' -OutFile "$Payload\engine.zip"
 if((Get-FileHash "$Payload\engine.zip").Hash -ne 'A2A26869A14634C49F3FBCECC593B498ED8591FBDD7616B24475B3D60B148DA3'){throw 'Upstream engine archive digest mismatch'}
 Expand-Archive "$Payload\engine.zip" "$Payload\engine";Copy-Item -LiteralPath "$Payload\engine\mihomo-windows-arm64.exe" -Destination "$Payload\mihomo.exe"
 Invoke-WebRequest -UseBasicParsing 'https://www.wintun.net/builds/wintun-0.14.1.zip' -OutFile "$Payload\wintun.zip";Expand-Archive "$Payload\wintun.zip" "$Payload\driver"
 Copy-Item -LiteralPath "$Payload\driver\wintun\bin\arm64\wintun.dll" -Destination "$Payload\wintun.dll";Copy-Item -LiteralPath "$Payload\driver\wintun\LICENSE.txt" -Destination "$Payload\Wintun-LICENSE"
 Invoke-WebRequest -UseBasicParsing 'https://raw.githubusercontent.com/MetaCubeX/mihomo/v1.19.33/LICENSE' -OutFile "$Payload\Mihomo-LICENSE"
}
$base=Join-Path $env:ProgramData 'WoA-Network';$runtime=Join-Path $base 'runtime';$task='WoA-DualSta'
foreach($item in @(@('mihomo.exe','6F6E471B2FEA55C54C0D98FC4B559FB06627456ED50FEBAF7A5572508B225529'),@('wintun.dll','F7BA89005544BE9D85231A9E0D5F23B2D15B3311667E2DAD0DEBD344918A3F80'))){if((Get-FileHash -LiteralPath (Join-Path $Payload $item[0]) -Algorithm SHA256).Hash -ne $item[1]){throw ('Runtime digest mismatch: '+$item[0])}}
$sig=Get-AuthenticodeSignature -LiteralPath (Join-Path $Payload 'wintun.dll');if($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'O=WireGuard LLC'){throw 'Untrusted Wintun runtime'}
$foreign=Get-Process -ErrorAction SilentlyContinue|Where-Object {$_.ProcessName -match '^(mihomo|clash|sing-box|openvpn|wireguard|tailscale)' -and $_.Path -ne (Join-Path $runtime 'mihomo.exe')};if($foreign){throw 'Another VPN/traffic manager is active; do not take over its routing'}
Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
$watch=[Diagnostics.Stopwatch]::StartNew();while(Get-Process -Name mihomo -ErrorAction SilentlyContinue|Where-Object Path -eq (Join-Path $runtime 'mihomo.exe')){if($watch.Elapsed.TotalSeconds -gt 10){throw 'Existing traffic engine did not stop'};Start-Sleep -Milliseconds 200}
New-Item -ItemType Directory -Path $runtime -Force|Out-Null
$acl=[Security.AccessControl.DirectorySecurity]::new();$acl.SetAccessRuleProtection($true,$false);foreach($sid in 'S-1-5-18','S-1-5-32-544'){$id=[Security.Principal.SecurityIdentifier]::new($sid);$acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($id,'FullControl','ContainerInherit,ObjectInherit','None','Allow'))};Set-Acl -LiteralPath $runtime -AclObject $acl
foreach($name in 'mihomo.exe','wintun.dll','Mihomo-LICENSE','Wintun-LICENSE'){$from=[IO.Path]::GetFullPath((Join-Path $Payload $name));$to=[IO.Path]::GetFullPath((Join-Path $runtime $name));if($from -ne $to){Copy-Item -LiteralPath $from -Destination $to -Force}}
foreach($name in 'dual-sta.ps1','hotspot.ps1','install-network.ps1'){$from=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot $name));$to=[IO.Path]::GetFullPath((Join-Path $base $name));if($from -ne $to){Copy-Item -LiteralPath $from -Destination $to -Force}}
$baseAcl=[Security.AccessControl.DirectorySecurity]::new();$baseAcl.SetAccessRuleProtection($true,$false);$baseAcl.SetOwner([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'));foreach($sid in 'S-1-5-18','S-1-5-32-544'){$baseAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new($sid),'FullControl','ContainerInherit,ObjectInherit','None','Allow'))};$baseAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new('S-1-5-32-545'),'ReadAndExecute','ContainerInherit,ObjectInherit','None','Allow'));Set-Acl -LiteralPath $base -AclObject $baseAcl
$v=& (Join-Path $runtime 'mihomo.exe') -v;if($LASTEXITCODE -ne 0 -or ($v -join "`n") -notmatch 'v1\.19\.33'){throw 'Unexpected traffic engine version'}
$a=New-ScheduledTaskAction -Execute powershell.exe -Argument ('-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "'+$base+'\dual-sta.ps1" -Action Run');$p=New-ScheduledTaskPrincipal -UserId SYSTEM -LogonType ServiceAccount -RunLevel Highest;$s=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName $task -Action $a -Principal $p -Settings $s -Trigger (New-ScheduledTaskTrigger -AtStartup) -Force|Out-Null
[IO.File]::WriteAllText((Join-Path $runtime 'cleanup.pending'),'1')
if(!$NoStart){[IO.File]::WriteAllText((Join-Path $runtime 'enabled'),'1');Start-ScheduledTask -TaskName $task}
'INSTALLED_GLOBAL_IPV4_IPV6_TCP_UDP_NO_SYSTEM_PROXY_NO_REBOOT'
