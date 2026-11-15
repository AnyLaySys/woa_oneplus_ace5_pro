param([string]$Bin=$PSScriptRoot,[string]$Evidence,[int]$Seconds=1800)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
if(!$Evidence -or $Seconds -lt 1 -or $Seconds -gt 3600){throw 'Evidence and duration (1..3600 seconds) required'}
New-Item -ItemType Directory -Path $Evidence -Force | Out-Null
$stats=Join-Path $Bin 'gpu-statistics.exe'
function Invoke-Workload([string]$Name,[string]$Arguments='') {
    $parameters=@{FilePath=(Join-Path $Bin "$Name.exe");WorkingDirectory=$Bin;WindowStyle='Hidden';PassThru=$true;
        RedirectStandardOutput="$Evidence/last-$Name.jsonl";RedirectStandardError="$Evidence/last-$Name.err"}
    if($Arguments){$parameters.ArgumentList=$Arguments}
    $process=Start-Process @parameters
    $null=$process.Handle
    if(!$process.WaitForExit(120000)){$process.Kill();throw "$Name timed out"}
    if($process.ExitCode -ne 0){throw "$Name exited with $($process.ExitCode)"}
    @(Get-Content "$Evidence/last-$Name.jsonl")
}
$info=(& $stats power | ConvertFrom-Json)
if($LASTEXITCODE -ne 0 -or !$info.levels){throw 'Power query failed'}
$maximum=($info.levels|Measure-Object khz -Maximum).Maximum
$before=@(Get-ChildItem C:/Windows/Minidump -ErrorAction SilentlyContinue | ForEach-Object Name)
$watch=[Diagnostics.Stopwatch]::StartNew();$round=0;$failure=$null
try {
    & $stats power-set $maximum $maximum | Set-Content "$Evidence/start-power.json"
    if($LASTEXITCODE -ne 0){throw 'Maximum frequency vote failed'}
    while($watch.Elapsed.TotalSeconds -lt $Seconds) {
        $bench=@(Invoke-Workload 'gpu-bench')
        $power=& $stats power
        if($LASTEXITCODE -ne 0 -or ($power|ConvertFrom-Json).status -ne '00000000'){throw 'GMU error'}
        if($round%10 -eq 0) {
            Invoke-Workload 'execution-test' | Out-Null
            Invoke-Workload 'd3d-test' 'native-shared' | Out-Null
            $new=@(Get-ChildItem C:/Windows/Minidump -ErrorAction SilentlyContinue | Where-Object {$_.Name -notin $before})
            if($new.Count){throw 'New kernel dump detected'}
        }
        $round++
        @{ElapsedSeconds=$watch.Elapsed.TotalSeconds;Round=$round;Power=($power|ConvertFrom-Json);Benchmark=$bench} |
            ConvertTo-Json -Depth 6 -Compress | Add-Content "$Evidence/samples.jsonl"
        @{Running=$true;Seconds=$watch.Elapsed.TotalSeconds;Rounds=$round} | ConvertTo-Json -Compress | Set-Content "$Evidence/status.json"
    }
} catch {$failure=$_.Exception.Message} finally {
    & $stats power-set 0 0 | Set-Content "$Evidence/final-power.json"
}
@{Running=$false;Passed=(!$failure -and $watch.Elapsed.TotalSeconds -ge $Seconds);Seconds=$watch.Elapsed.TotalSeconds;Rounds=$round;Error=$failure} |
    ConvertTo-Json -Compress | Set-Content "$Evidence/status.json"
if($failure){throw $failure}
