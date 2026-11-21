param([ValidateSet('Status','Start','Stop')][string]$Action='Status',[ValidateSet('Wpa2','Wpa3TransitionMode','Wpa3')][string]$Security='Wpa3TransitionMode',[ValidateSet('Saved','Auto','TwoPointFourGigahertz','FiveGigahertz')][string]$Band='Saved',[string]$InterfaceAlias='WLAN')
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null=[Windows.Networking.Connectivity.NetworkInformation,Windows.Networking.Connectivity,ContentType=WindowsRuntime]
$null=[Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager,Windows.Networking.NetworkOperators,ContentType=WindowsRuntime]
$null=[Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult,Windows.Networking.NetworkOperators,ContentType=WindowsRuntime]
$null=[Windows.Networking.NetworkOperators.TetheringWiFiAuthenticationKind,Windows.Networking.NetworkOperators,ContentType=WindowsRuntime]
function Await-Operation($op){$m=[System.WindowsRuntimeSystemExtensions].GetMethods()|Where-Object {$_.Name -eq 'AsTask' -and $_.IsGenericMethod -and $_.GetGenericArguments().Count -eq 1 -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -like 'IAsyncOperation*'}|Select-Object -First 1;$t=$m.MakeGenericMethod([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult]).Invoke($null,@($op));if(!$t.Wait(30000)){throw 'Hotspot operation timed out'};$r=$t.GetAwaiter().GetResult();if([string]$r.Status -ne 'Success'){throw "Hotspot result: $($r.Status) $($r.AdditionalErrorMessage)"}}
$id=[Guid](Get-NetAdapter -Name $InterfaceAlias -ErrorAction Stop).InterfaceGuid;$p=@([Windows.Networking.Connectivity.NetworkInformation]::GetConnectionProfiles()|Where-Object {$_.NetworkAdapter -and $_.NetworkAdapter.NetworkAdapterId -eq $id})|Select-Object -First 1;if(!$p){throw 'No physical WLAN connection profile'}
$m=[Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager]::CreateFromConnectionProfile($p);$saved=$m.GetCurrentAccessPointConfiguration()
$kind=[Windows.Networking.NetworkOperators.TetheringWiFiAuthenticationKind]::$Security
if($Action -eq 'Start'){
 if([string]$m.TetheringOperationalState -ne 'Off'){throw 'Stop the current hotspot before changing session security'}
 if(!$saved.IsAuthenticationKindSupported($kind)){throw "Windows/driver does not report support for $Security"}
 $null=[Windows.Networking.NetworkOperators.NetworkOperatorTetheringSessionAccessPointConfiguration,Windows.Networking.NetworkOperators,ContentType=WindowsRuntime]
 $session=[Windows.Networking.NetworkOperators.NetworkOperatorTetheringSessionAccessPointConfiguration]::new();$session.Ssid=$saved.Ssid;$session.Passphrase=$saved.Passphrase;$session.Band=if($Band -eq 'Saved'){$saved.Band}else{[Windows.Networking.NetworkOperators.TetheringWiFiBand]::$Band};$session.AuthenticationKind=$kind
 try{Await-Operation ($m.StartTetheringAsync($session))}finally{$session=$null}
}elseif($Action -eq 'Stop' -and [string]$m.TetheringOperationalState -ne 'Off'){Await-Operation ($m.StopTetheringAsync())}
[ordered]@{time=(Get-Date).ToString('o');state=[string]$m.TetheringOperationalState;clients=$m.ClientCount;savedAuthentication=[string]$saved.AuthenticationKind;requestedSession=$(if($Action -eq 'Start'){$Security}else{$null});requestedBand=$(if($Action -eq 'Start'){$Band}else{$null});wpa3Supported=$saved.IsAuthenticationKindSupported([Windows.Networking.NetworkOperators.TetheringWiFiAuthenticationKind]::Wpa3);transitionSupported=$saved.IsAuthenticationKindSupported([Windows.Networking.NetworkOperators.TetheringWiFiAuthenticationKind]::Wpa3TransitionMode)}|ConvertTo-Json -Compress
