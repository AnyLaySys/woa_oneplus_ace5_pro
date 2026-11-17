param([ValidateRange(1,3600)][int]$Samples=1,[ValidateRange(20,60000)][int]$IntervalMs=250)
$ErrorActionPreference='Stop'
if(-not ('QcomFaultState' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class QcomFaultState {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
 static extern IntPtr CreateFile(string name,uint access,uint share,IntPtr security,uint creation,uint flags,IntPtr template);
 [DllImport("kernel32.dll",SetLastError=true)]
 static extern bool DeviceIoControl(IntPtr file,uint code,IntPtr input,uint inputBytes,[Out] uint[] output,uint outputBytes,out uint returned,IntPtr overlapped);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr file);
 public static uint[] Read() {
  IntPtr file=CreateFile(@"\\.\QcomGpu",0x80000000,3,IntPtr.Zero,3,0,IntPtr.Zero);
  if(file==new IntPtr(-1))throw new Win32Exception(Marshal.GetLastWin32Error());
  try {
   uint[] data=new uint[16];uint returned;
   if(!DeviceIoControl(file,0x22605C,IntPtr.Zero,0,data,64,out returned,IntPtr.Zero))throw new Win32Exception(Marshal.GetLastWin32Error());
   if(returned!=64 || data[0]!=1 || data[1]!=64)throw new InvalidOperationException("Unsupported fault report");
   return data;
  } finally {CloseHandle(file);}
 }
}
'@
}
for($i=0;$i -lt $Samples;$i++) {
 $r=[QcomFaultState]::Read()
 [pscustomobject]@{
  Time=Get-Date -Format o
  Source=$r[2];Bits=('0x{0:X8}' -f $r[3])
  Syndrome0=('0x{0:X8}' -f $r[4]);Syndrome1=('0x{0:X8}' -f $r[5])
  FaultAddress=$(if($r[2] -gt 4){'0x{0:X8}{1:X8}' -f $r[7],$r[6]}else{$null})
  GlobalSyndrome2=$(if($r[2] -eq 4){'0x{0:X8}' -f $r[6]}else{$null})
  Submitted=$r[8];Completed=$r[9];WriteIndex=$r[10];ReadIndex=$r[11]
  GmuStatus=('0x{0:X8}' -f $r[12]);PowerStatus=('0x{0:X8}' -f $r[13])
  ResetStage=$r[14];ResetStatus=('0x{0:X8}' -f $r[15])
 }
 if($i+1 -lt $Samples){Start-Sleep -Milliseconds $IntervalMs}
}
