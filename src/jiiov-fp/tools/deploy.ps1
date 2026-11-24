param(
    [Parameter(Mandatory=$true)][string]$PackagePath,
    [string]$InstanceId='ROOT\DEVGEN\JIIOV_FP'
)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$PackagePath=(Resolve-Path $PackagePath).Path
$inf=Join-Path $PackagePath 'jiiov_fp.inf'
$sys=Join-Path $PackagePath 'jiiov_fp.sys'
$version=(Get-Item $sys).VersionInfo.FileVersion
$expectedHash=(Get-FileHash $sys -Algorithm SHA256).Hash
& pnputil /add-driver $inf /install
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { throw 'Driver package installation failed' }
$installed=(Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
if ($installed -ne $version) { throw "PnP version $installed does not match package $version" }
$store=Join-Path $env:SystemRoot 'System32\DriverStore\FileRepository'
$matches=@(Get-ChildItem "$store\jiiov_fp.inf_arm64_*\jiiov_fp.sys" | Where-Object {
    $_.VersionInfo.FileVersion -eq $version -and
    (Get-FileHash $_.FullName -Algorithm SHA256).Hash -eq $expectedHash
})
if ($matches.Count -ne 1) { throw 'Cannot identify the installed driver binary uniquely' }
$newPath='\SystemRoot'+$matches[0].FullName.Substring($env:SystemRoot.Length)
$oldPath=(Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\jiiov_fp).ImagePath
if ($oldPath -ne $newPath) {
    # Updating the INF can leave this PnP driver's service pointing at the previous package.
    $backup=Join-Path $PackagePath ('service-path-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'.txt')
    $oldPath | Set-Content $backup
    & pnputil /disable-device $InstanceId
    if ($LASTEXITCODE -ne 0) { throw 'Could not disable the fingerprint device for reload' }
    try {
        & sc.exe config jiiov_fp binPath= $newPath
        if ($LASTEXITCODE -ne 0) { throw 'Could not update the fingerprint service binary' }
        & pnputil /enable-device $InstanceId
        if ($LASTEXITCODE -ne 0) { throw 'Could not enable the fingerprint device' }
    } catch {
        & sc.exe config jiiov_fp binPath= $oldPath
        & pnputil /enable-device $InstanceId
        throw
    }
} else {
    & pnputil /restart-device $InstanceId
    if ($LASTEXITCODE -ne 0) { throw 'Could not restart the fingerprint device' }
}
$active=Get-CimInstance Win32_SystemDriver -Filter "Name='jiiov_fp'"
if ($active.State -ne 'Running' -or (Get-PnpDevice -InstanceId $InstanceId).Status -ne 'OK') {
    throw 'Fingerprint driver did not start successfully'
}
$activeVersion=(Get-Item $active.PathName).VersionInfo.FileVersion
$activeHash=(Get-FileHash $active.PathName -Algorithm SHA256).Hash
if ($activeVersion -ne $version -or $activeHash -ne $expectedHash) {
    throw 'Active driver does not match the installed package'
}
Write-Output "LoadedVersion=$activeVersion"
Write-Output "LoadedHash=$activeHash"
Write-Output "LoadedPath=$($active.PathName)"
