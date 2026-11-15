param(
    [string]$Bin = $PSScriptRoot,
    [string]$OutputPath,
    [switch]$Headless
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$env:A830_REQUIRE_WDDM = 'true'
function Invoke-GpuMeasure([string]$Name, [string[]]$Arguments = @()) {
    $exe = Join-Path $Bin "$Name.exe"
    if (!(Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Missing $exe" }
    $lines = @(& $exe @Arguments)
    $exitCode = $LASTEXITCODE
    $records = @($lines | ForEach-Object {
        $line = $_
        try { ConvertFrom-Json -InputObject $line -ErrorAction Stop } catch { @{ text = "$line" } }
    })
    [ordered]@{ exit_code = $exitCode; records = $records }
}
$session = (Get-Process -Id $PID).SessionId
$version = Get-PnpDeviceProperty -InstanceId 'ACPI\HMRG0830\0' -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue
$report = [ordered]@{
    captured_at = (Get-Date).ToString('o')
    computer = $env:COMPUTERNAME
    session = $session
    driver_version = $version.Data
    dxgi_arm64 = Invoke-GpuMeasure 'dxgi-probe-arm64'
    dxgi_x64 = Invoke-GpuMeasure 'dxgi-probe-x64'
    telemetry_before = Invoke-GpuMeasure 'gpu-statistics' @('telemetry')
    desktop = if (!$Headless -and $session) { Invoke-GpuMeasure 'dwm-stat' } else { @{ skipped = 'interactive desktop required' } }
    telemetry_after_desktop = Invoke-GpuMeasure 'gpu-statistics' @('telemetry')
    benchmark = Invoke-GpuMeasure 'gpu-bench'
    telemetry_after_bench = Invoke-GpuMeasure 'gpu-statistics' @('telemetry')
    clients = Invoke-GpuMeasure 'gpu-statistics' @('clients')
}
$json = $report | ConvertTo-Json -Depth 12
if ($OutputPath) { Set-Content -LiteralPath $OutputPath -Value $json -Encoding UTF8 }
$json
