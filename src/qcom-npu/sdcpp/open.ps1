$ErrorActionPreference='Stop'
$url='http://127.0.0.1:1234'
try {$ready=Invoke-RestMethod "$url/sdcpp/v1/capabilities" -TimeoutSec 1}catch{}
if(!$ready){
    Start-Process (Join-Path $PSScriptRoot 'start-server.bat') -WorkingDirectory $PSScriptRoot -WindowStyle Minimized
    for($i=0;$i -lt 120;$i++){
        try {$ready=Invoke-RestMethod "$url/sdcpp/v1/capabilities" -TimeoutSec 1;if($ready){break}}catch{}
        Start-Sleep -Milliseconds 500
    }
}
if(!$ready){throw 'Stable Diffusion server did not start.'}
$browser=Join-Path ${env:ProgramFiles(x86)} 'Microsoft\Edge\Application\msedge.exe'
$profile=Join-Path $env:LOCALAPPDATA 'StableDiffusionCpp\Browser'
Start-Process $browser -ArgumentList ('--app='+$url+' --user-data-dir="'+$profile+'" --disable-gpu --no-first-run --start-maximized') -WindowStyle Normal
