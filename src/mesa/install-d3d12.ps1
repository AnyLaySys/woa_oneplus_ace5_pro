param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run PowerShell as administrator.'
}
if ((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').PROCESSOR_ARCHITECTURE -ne 'ARM64') {
    throw 'This runtime requires Windows ARM64.'
}
$system = Join-Path $env:windir 'System32'
$target = Join-Path $system 'd3d12.dll'
$original = Join-Path $system 'qcom_d3d12_system.dll'
$state = Join-Path $env:ProgramData 'AnyLaySys\D3D12'
$record = Join-Path $state 'state.json'
$files = @('qcom_d3d12_arm64.dll', 'qcom_d3d12core_arm64.dll', 'qcom_dxgi_arm64.dll',
    'qcom_d3d12_x64.dll', 'qcom_d3d12core_x64.dll', 'qcom_dxgi_x64.dll')
if (-not $Uninstall) {
    foreach ($name in $files + 'd3d12.dll') {
        if (-not (Test-Path (Join-Path $PSScriptRoot $name))) { throw "Missing $name" }
    }
    if (-not (Test-Path $record)) {
        New-Item -ItemType Directory -Path $state -Force | Out-Null
        $saved = @{ Sddl = (Get-Acl $target).Sddl; Hash = (Get-FileHash $target -Algorithm SHA256).Hash }
        Copy-Item $target $original -Force
        $saved | ConvertTo-Json | Set-Content $record -Encoding UTF8
    }
} elseif (-not (Test-Path $record)) {
    throw 'No saved system runtime.'
}
$saved = Get-Content $record -Raw | ConvertFrom-Json
if ((Get-FileHash $original -Algorithm SHA256).Hash -ne $saved.Hash) { throw 'System runtime backup checksum mismatch.' }
$source = if ($Uninstall) { $original } else { Join-Path $PSScriptRoot 'd3d12.dll' }
if (-not $Uninstall) {
    foreach ($name in $files) { Copy-Item (Join-Path $PSScriptRoot $name) (Join-Path $system $name) -Force }
}
& takeown.exe /F $target /A | Out-Null
if ($LASTEXITCODE) { throw 'Cannot take ownership of d3d12.dll.' }
& icacls.exe $target /grant '*S-1-5-32-544:F' | Out-Null
if ($LASTEXITCODE) { throw 'Cannot update d3d12.dll permissions.' }
try {
    Copy-Item $source $target -Force
} finally {
    $acl = Get-Acl $target
    $acl.SetSecurityDescriptorSddlForm($saved.Sddl)
    Set-Acl $target $acl
}
if ($Uninstall) {
    Remove-Item $record
    Write-Output 'Restored the Windows D3D12 runtime. Restart applications.'
} else {
    Write-Output 'Installed the ARM64 and x64 D3D12 runtime. Restart applications.'
}
