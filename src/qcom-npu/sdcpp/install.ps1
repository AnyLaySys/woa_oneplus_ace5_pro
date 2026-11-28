param([string]$Package=$PSScriptRoot)
$ErrorActionPreference='Stop'
$destination=Join-Path $env:LOCALAPPDATA 'Programs\StableDiffusionCpp'
foreach($name in @('sd-server.exe','ggml-hexagon.dll','open.ps1','models\checkpoints\SDXS-512-Q8_0.gguf')){
    if(!(Test-Path (Join-Path $Package $name))){throw "Missing Stable Diffusion file: $name"}
}
foreach($process in @(Get-Process sd-server -ErrorAction SilentlyContinue | Where-Object {$_.Path -like "$destination\*"})){
    Stop-Process -InputObject $process -Force
    if(!$process.WaitForExit(10000)){throw 'Stable Diffusion server did not exit.'}
}
New-Item $destination -ItemType Directory -Force | Out-Null
if([IO.Path]::GetFullPath($Package).TrimEnd('\') -ne $destination){Copy-Item (Join-Path $Package '*') $destination -Recurse -Force}
$shell=New-Object -ComObject WScript.Shell
foreach($folder in @([Environment]::GetFolderPath('Desktop'),[Environment]::GetFolderPath('Programs'))){
    $link=$shell.CreateShortcut((Join-Path $folder 'Stable Diffusion.lnk'))
    $link.TargetPath=Join-Path $PSHOME 'powershell.exe'
    $link.Arguments='-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "'+(Join-Path $destination 'open.ps1')+'"'
    $link.WorkingDirectory=$destination
    $link.IconLocation=(Join-Path $destination 'sd-server.exe')+',0'
    $link.Save()
}
Write-Output "Installed: $destination"
