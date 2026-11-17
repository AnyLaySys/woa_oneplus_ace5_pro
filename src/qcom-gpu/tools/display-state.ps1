param([ValidateRange(1,3600)][int]$Samples=1,[ValidateRange(20,60000)][int]$IntervalMs=250)
$ErrorActionPreference='Stop'
if(-not ('QcomDisplayState' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class QcomDisplayState {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
 static extern IntPtr CreateFile(string name,uint access,uint share,IntPtr security,uint creation,uint flags,IntPtr template);
 [DllImport("kernel32.dll",SetLastError=true)]
 static extern bool DeviceIoControl(IntPtr file,uint code,IntPtr input,uint inputBytes,[Out] uint[] output,uint outputBytes,out uint returned,IntPtr overlapped);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr file);
 public static uint[] Read() {
  IntPtr file=CreateFile(@"\\.\QcomGpu",0x80000000,3,IntPtr.Zero,3,0,IntPtr.Zero);
  if(file==new IntPtr(-1))throw new Win32Exception(Marshal.GetLastWin32Error());
  try {
   uint[] data=new uint[24];uint returned;
   if(!DeviceIoControl(file,0x226058,IntPtr.Zero,0,data,96,out returned,IntPtr.Zero))throw new Win32Exception(Marshal.GetLastWin32Error());
   if(returned!=96 || data[0]!=1 || data[1]!=96)throw new InvalidOperationException("Unsupported display state report");
   return data;
  } finally {CloseHandle(file);}
 }
}
'@
}
for($i=0;$i -lt $Samples;$i++) {
 $r=[QcomDisplayState]::Read()
 [pscustomobject]@{
  Time=Get-Date -Format o
  Enabled=!!($r[2] -band 1)
  Stopping=!!($r[2] -band 2)
  Status=('0x{0:X8}' -f $r[3])
  State=$r[4];Queued=$r[5];Applied=$r[6]
  PendingFence=$r[7];Completed=$r[8];Submitted=$r[9]
  Line=$r[10];Event=('0x{0:X8}' -f $r[11]);Done=('0x{0:X8}' -f $r[12])
  Frame=$r[13];InitialFrame=$r[14];Flushed=!!$r[15]
  FailureState=$r[16];FailureLine=$r[17]
  FailureEvent=('0x{0:X8}' -f $r[18]);FailureDone=('0x{0:X8}' -f $r[19])
  FailureFrame=$r[20];FailureFlushed=!!$r[21];Pitch=$r[22];PeriodMs=$r[23]/10000
 }
 if($i+1 -lt $Samples){Start-Sleep -Milliseconds $IntervalMs}
}
