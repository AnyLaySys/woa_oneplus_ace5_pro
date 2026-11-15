$ErrorActionPreference='Stop'
Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;using Microsoft.Win32.SafeHandles;
public static class CpuStateApi{
[DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]public static extern SafeFileHandle CreateFile(string n,uint a,uint s,IntPtr sa,uint c,uint f,IntPtr t);
[DllImport("kernel32.dll",SetLastError=true)]public static extern bool DeviceIoControl(SafeFileHandle h,uint c,byte[] i,uint ni,byte[] o,uint no,out uint used,IntPtr ov);
}
'@
$h=[CpuStateApi]::CreateFile('\\.\Sm8750CpuPower',2147483648,3,[IntPtr]::Zero,3,0,[IntPtr]::Zero)
if($h.IsInvalid){throw [ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error())}
try{
 $b=New-Object byte[] 80;$used=[uint32]0
 if(![CpuStateApi]::DeviceIoControl($h,0x226000,$null,0,$b,$b.Length,[ref]$used,[IntPtr]::Zero)){throw [ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error())}
 if($used -ne 80 -or [BitConverter]::ToUInt32($b,0) -ne 0x50434d53 -or [BitConverter]::ToUInt32($b,4) -ne 3){throw 'Unexpected CPU driver report'}
 $flags=[BitConverter]::ToUInt32($b,12);$domains=@()
 foreach($d in 0..1){$domains+=@([ordered]@{domain=$d;ceiling=[BitConverter]::ToUInt32($b,28+4*$d);target=[BitConverter]::ToUInt32($b,36+4*$d);applied=[BitConverter]::ToUInt32($b,44+4*$d);status=('{0:X8}' -f [BitConverter]::ToUInt32($b,52+4*$d));busyPercent=[BitConverter]::ToUInt32($b,64+4*$d);votedMHz=[BitConverter]::ToUInt32($b,72+4*$d)/1000})}
 [ordered]@{time=(Get-Date).ToString('o');control=[bool]($flags-band 2);fault=[bool]($flags-band 4);registered=[BitConverter]::ToUInt32($b,16);retention=[bool][BitConverter]::ToUInt32($b,60);domains=$domains}|ConvertTo-Json -Depth 4
}finally{$h.Dispose()}
