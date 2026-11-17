param([ValidateRange(1,3600)][int]$Samples=1,[ValidateRange(20,60000)][int]$IntervalMs=250)
$ErrorActionPreference='Stop'
if(-not ('QcomGpuPower' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class QcomGpuPower {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
 static extern IntPtr CreateFile(string name,uint access,uint share,IntPtr security,uint creation,uint flags,IntPtr template);
 [DllImport("kernel32.dll",SetLastError=true)]
 static extern bool DeviceIoControl(IntPtr file,uint code,IntPtr input,uint inputBytes,[Out] uint[] output,uint outputBytes,out uint returned,IntPtr overlapped);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr file);
 public static uint[] Read() {
  IntPtr file=CreateFile(@"\\.\QcomGpu",0x80000000,3,IntPtr.Zero,3,0,IntPtr.Zero);
  if(file==new IntPtr(-1))throw new Win32Exception(Marshal.GetLastWin32Error());
  try {
   uint[] data=new uint[9];uint returned;
   if(!DeviceIoControl(file,0x226054,IntPtr.Zero,0,data,36,out returned,IntPtr.Zero))throw new Win32Exception(Marshal.GetLastWin32Error());
   if(returned!=36 || data[0]!=1 || data[1]!=36)throw new InvalidOperationException("Unsupported GPU power report");
   return data;
  } finally {CloseHandle(file);}
 }
}
'@
}
for($i=0;$i -lt $Samples;$i++) {
 $r=[QcomGpuPower]::Read()
 [pscustomobject]@{
  Time=Get-Date -Format o
  Online=!!($r[2] -band 1)
  Fault=!!($r[2] -band 4)
  MHz=$r[3]/1000
  CeilingMHz=$r[4]/1000
  BusyPercent=$r[5]
  TemperatureC=$(if($r[2] -band 2){[BitConverter]::ToInt32([BitConverter]::GetBytes($r[6]),0)/10}else{$null})
  CoolingBand=$r[7]
  PowerStatus=('0x{0:X8}' -f $r[8])
 }
 if($i+1 -lt $Samples){Start-Sleep -Milliseconds $IntervalMs}
}
