param([Parameter(Mandatory=$true)][string]$Package)
$ErrorActionPreference='Stop'
$inf=Join-Path $Package 'qcom_npu.inf'
$computeInf=Join-Path $Package 'qcom_npu_mcdm.inf'
foreach($name in @('qcom_npu.inf','qcom_npu.sys','qcom_npu.cat','CDSP\cdsp.mdt','CDSP\cdsp_dtb.mdt')) {
    if(-not (Test-Path (Join-Path $Package $name))) { throw "Missing package file: $name" }
}
# Match the hardware IDs: Windows generates ROOT\COMPUTEACCELERATOR\...,
# which is different from the driver's ROOT\QCOMNPU hardware ID.
$compute=@(Get-PnpDevice -PresentOnly -Class ComputeAccelerator -ErrorAction SilentlyContinue | Where-Object {
    (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_HardwareIds).Data -contains 'ROOT\QCOMNPU'
})
if($compute.Count -gt 1) { throw 'Multiple Qualcomm NPU compute devices exist.' }
if(Test-Path $computeInf) {
    foreach($name in @('qcom_npu_mcdm.sys','qcom_npu_mcdm.cat','qcom_npu_ep.dll')) {
        if(-not(Test-Path (Join-Path $Package $name))) { throw "Missing package file: $name" }
    }
    # Release the miniport's imports before updating the transport module.
    foreach($device in $compute) {
        # PnPUtil reports "not connected" when disabling an already disabled
        # root adapter (for example, after an interrupted driver update).
        if([uint32]$device.Problem -eq 22) { continue }
        & pnputil /disable-device $device.InstanceId
        if($LASTEXITCODE -notin @(0,3010)) { Disable-PnpDevice -InstanceId $device.InstanceId -Confirm:$false }
    }
}
& pnputil /add-driver $inf /install
if($LASTEXITCODE -notin @(0,3010)) { throw "PnPUtil failed: $LASTEXITCODE" }
$sourceHash=(Get-FileHash (Join-Path $Package 'qcom_npu.sys') -Algorithm SHA256).Hash
$drivers=@(Get-ChildItem "$env:SystemRoot\System32\DriverStore\FileRepository\qcom_npu.inf_arm64_*\qcom_npu.sys" |
    Where-Object { (Get-FileHash $_.FullName -Algorithm SHA256).Hash -eq $sourceHash })
if($drivers.Count -ne 1) { throw 'Cannot identify the exact staged NPU package.' }
$binary=$drivers[0].FullName
# The older DSDT has no NPU node. Register this staged package's service so UEFI
# can find its firmware on the first boot; PnP binds it when the new node appears.
$service=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\qcom_npu' -ErrorAction SilentlyContinue
if(-not $service) {
    & sc.exe create qcom_npu type= kernel start= demand binPath= $binary DisplayName= 'Qualcomm Hexagon NPU'
    if($LASTEXITCODE) { throw "Service registration failed: $LASTEXITCODE" }
} elseif($service.ImagePath -ne $binary -and $service.ImagePath -ne ('\SystemRoot'+$binary.Substring($env:SystemRoot.Length))) {
    & sc.exe config qcom_npu binPath= $binary
    if($LASTEXITCODE) { throw "Service update failed: $LASTEXITCODE" }
}
if(Test-Path $computeInf) {
    $transport=@(Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'ACPI\HMRN8750\*')
    if($transport.Count -ne 1) { throw 'The NPU ACPI transport must be present before installing the compute adapter.' }
    & pnputil /restart-device $transport[0].InstanceId
    if($LASTEXITCODE -notin @(0,3010)) { throw 'Cannot restart the updated NPU transport.' }
    if(!$compute.Count) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
public static class QcomNpuSetup {
    [StructLayout(LayoutKind.Sequential)] struct DeviceInfo {
        public uint Size; public Guid ClassGuid; public uint DevInst; public UIntPtr Reserved;
    }
    [DllImport("setupapi.dll", SetLastError=true)] static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid guid, IntPtr parent);
    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool SetupDiCreateDeviceInfo(IntPtr set, string name, ref Guid guid, string description, IntPtr parent, uint flags, ref DeviceInfo info);
    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool SetupDiSetDeviceRegistryProperty(IntPtr set, ref DeviceInfo info, uint property, byte[] data, uint bytes);
    [DllImport("setupapi.dll", SetLastError=true)] static extern bool SetupDiCallClassInstaller(uint function, IntPtr set, ref DeviceInfo info);
    [DllImport("setupapi.dll")] static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    public static void Create() {
        Guid guid = new Guid("F01A9D53-3FF6-48D2-9F97-C8A7004BE10C");
        IntPtr set = SetupDiCreateDeviceInfoList(ref guid, IntPtr.Zero);
        if(set == new IntPtr(-1)) throw new Win32Exception();
        try {
            DeviceInfo info = new DeviceInfo { Size = (uint)Marshal.SizeOf(typeof(DeviceInfo)) };
            if(!SetupDiCreateDeviceInfo(set, "ComputeAccelerator", ref guid, "Qualcomm Hexagon NPU", IntPtr.Zero, 1, ref info)) throw new Win32Exception();
            byte[] ids = Encoding.Unicode.GetBytes("ROOT\\QCOMNPU\0\0");
            if(!SetupDiSetDeviceRegistryProperty(set, ref info, 1, ids, (uint)ids.Length)) throw new Win32Exception();
            if(!SetupDiCallClassInstaller(0x19, set, ref info)) throw new Win32Exception();
        } finally { SetupDiDestroyDeviceInfoList(set); }
    }
}
'@
        [QcomNpuSetup]::Create()
    }
    & pnputil /add-driver $computeInf /install
    if($LASTEXITCODE -notin @(0,3010)) { throw 'Cannot install the NPU compute miniport.' }
    $compute=@(Get-PnpDevice -PresentOnly -Class ComputeAccelerator | Where-Object {
        (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_HardwareIds).Data -contains 'ROOT\QCOMNPU'
    })
    if($compute.Count -ne 1) { throw 'Cannot identify the installed NPU compute adapter.' }
    & pnputil /enable-device $compute[0].InstanceId
    if($LASTEXITCODE -notin @(0,3010)) { Enable-PnpDevice -InstanceId $compute[0].InstanceId -Confirm:$false }
    $code=[uint32](Get-PnpDevice -InstanceId $compute[0].InstanceId).Problem
    if($code) { throw "NPU compute adapter did not start: PnP problem $code" }
}
# GGML's standard dynamic loader accepts one extra backend DLL here.
$backend=Join-Path (Split-Path $binary) 'GGML\qcom_npu_ggml.dll'
if(Test-Path $backend) {
    $registration='HKLM:\SOFTWARE\AnyLaySys\Npu'
    $old=[Environment]::GetEnvironmentVariable('GGML_BACKEND_PATH','Machine')
    $owned=(Get-ItemProperty $registration -ErrorAction SilentlyContinue).GgmlBackendPath
    if(!$old -or $old -eq $owned -or $old -eq $backend) {
        New-Item $registration -Force | Out-Null
        [Environment]::SetEnvironmentVariable('GGML_BACKEND_PATH',$backend,'Machine')
        Set-ItemProperty $registration -Name GgmlBackendPath -Value $backend
        $env:GGML_BACKEND_PATH=$backend
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NpuEnvironment {
    [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr SendMessageTimeout(IntPtr hwnd, uint msg, UIntPtr wparam, string lparam, uint flags, uint timeout, out UIntPtr result);
}
'@
        $result=[UIntPtr]::Zero
        [void][NpuEnvironment]::SendMessageTimeout([IntPtr]0xffff,0x1a,[UIntPtr]::Zero,'Environment',2,3000,[ref]$result)
    } else { Write-Warning 'Preserved an existing third-party GGML_BACKEND_PATH. NPU selects its own installed backend.' }
}
if(Test-Path (Join-Path $Package 'qcom_npu_ep.msix')) {
    & (Join-Path $PSScriptRoot 'ep\install.ps1') -Package $Package
}
[pscustomobject]@{Driver=$binary;SHA256=$sourceHash;Version=$drivers[0].VersionInfo.FileVersion;Devices=@(Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'ACPI\HMRN8750\*' | Select-Object Status,FriendlyName,InstanceId)} | ConvertTo-Json -Depth 3 -Compress
