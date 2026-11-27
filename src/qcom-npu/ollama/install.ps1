param([string]$Package=$PSScriptRoot)
$ErrorActionPreference='Stop'
$destination=Join-Path $env:LOCALAPPDATA 'Programs\Ollama'
foreach($name in @('ollama.exe','ollama app.exe','lib\ollama\llama-server.exe')) {
    if(!(Test-Path (Join-Path $Package $name))) {throw "Missing Ollama file: $name"}
}
if(Get-ScheduledTask -TaskName 'AnyLaySys-Ollama' -ErrorAction SilentlyContinue) {
    Stop-ScheduledTask -TaskName 'AnyLaySys-Ollama'
    Unregister-ScheduledTask -TaskName 'AnyLaySys-Ollama' -Confirm:$false
}
foreach($name in @('ollama app','ollama','llama-server')) {
    $processes=@(Get-Process $name -ErrorAction SilentlyContinue | Where-Object {$_.Path -like "$destination\*"})
    foreach($process in $processes) {
        Stop-Process -InputObject $process -Force -ErrorAction SilentlyContinue
        if(!$process.WaitForExit(10000)){throw "Ollama process did not exit: $($process.Id)"}
    }
}
New-Item $destination -ItemType Directory -Force | Out-Null
if([IO.Path]::GetFullPath($Package).TrimEnd('\') -ne $destination) {Copy-Item (Join-Path $Package '*') $destination -Recurse -Force}
Remove-Item (Join-Path $destination 'start.ps1') -Force -ErrorAction SilentlyContinue
$oldPath=[Environment]::GetEnvironmentVariable('Path','User')
if(($oldPath -split ';' | ForEach-Object {$_.TrimEnd('\')}) -notcontains $destination) {[Environment]::SetEnvironmentVariable('Path',($oldPath.TrimEnd(';')+';'+$destination).TrimStart(';'),'User')}
$env:Path=$destination+';'+$env:Path
$frontend=Join-Path $destination 'ollama app.exe'
Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name Ollama -ErrorAction SilentlyContinue
$shell=New-Object -ComObject WScript.Shell
foreach($folder in @([Environment]::GetFolderPath('Desktop'),[Environment]::GetFolderPath('Programs'),[Environment]::GetFolderPath('Startup'),(Join-Path $destination 'lib'))) {
    Remove-Item (Join-Path $folder 'Ollama NPU.lnk') -Force -ErrorAction SilentlyContinue
    $link=$shell.CreateShortcut((Join-Path $folder 'Ollama.lnk'))
    $link.TargetPath=$frontend;$link.WorkingDirectory=$destination;$link.Save()
}
$action=New-ScheduledTaskAction -Execute $frontend -Argument hidden -WorkingDirectory $destination
$principal=New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited
$settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName 'AnyLaySys-OllamaLaunch' -Action $action -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName 'AnyLaySys-OllamaLaunch' 
for($i=0;$i -lt 120;$i++) {try {$version=Invoke-RestMethod 'http://127.0.0.1:11434/api/version' -TimeoutSec 1;if($version.version){break}}catch{};Start-Sleep -Milliseconds 500}
Unregister-ScheduledTask -TaskName 'AnyLaySys-OllamaLaunch' -Confirm:$false
if(!$version.version) {throw 'Ollama did not start.'}
Write-Output "Installed: $destination"
