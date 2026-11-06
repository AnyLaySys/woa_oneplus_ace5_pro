param([ValidateSet('Start','Stop','Status','Run')][string]$Action='Status',[string]$InterfaceAlias='WLAN')
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
$base=Join-Path $env:ProgramData 'WoA-Network';$runtime=Join-Path $base 'runtime';$config=Join-Path $runtime 'config.json';$state=Join-Path $runtime 'state.json';$enabled=Join-Path $runtime 'enabled';$task='WoA-DualSta';$engine=Join-Path $runtime 'mihomo.exe'
function Api($Method,$Path,$Body){$c=Get-Content -LiteralPath $config -Raw|ConvertFrom-Json;$p=@{UseBasicParsing=$true;Uri=('http://127.0.0.1:10829'+$Path);Method=$Method;Headers=@{Authorization=('Bearer '+$c.secret)};TimeoutSec=5};if($null -ne $Body){$p.Body=$Body;$p.ContentType='application/json'};(Invoke-WebRequest @p).Content|ConvertFrom-Json}
function DiagnosticFiles($Root) {
 $stack=[Collections.Generic.Stack[string]]::new();$stack.Push($Root)
 while($stack.Count){foreach($item in Get-ChildItem -LiteralPath $stack.Pop() -Force -ErrorAction SilentlyContinue){if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){continue};if($item.PSIsContainer){$stack.Push($item.FullName)}else{$item}}}
}
function Clear-OldNetworkDiagnostics {
 $pending=Join-Path $runtime 'cleanup.pending';if(!(Test-Path -LiteralPath $pending)){return}
 $version=(Get-PnpDeviceProperty -InstanceId 'ACPI\HMRW7861\0' -KeyName DEVPKEY_Device_DriverVersion -ErrorAction Stop).Data
 if([version]$version -lt [version]'26.11.5.39'){return} # The previous driver still owns its open trace file.
 $failures=0
 foreach($name in 'wcn7861.log','wcn7861-rddm.bin','wcn7861-cap-basic.bin','wcn7861-cap-ext.bin','wcn7861-cap-ext2.bin'){$p=Join-Path $env:WINDIR $name;if(Test-Path -LiteralPath $p){try{Remove-Item -LiteralPath $p -Force -ErrorAction Stop}catch{$failures++}}}
 $root=[IO.Path]::GetFullPath($env:ProgramData).TrimEnd('\')+'\'
 foreach($d in Get-ChildItem -LiteralPath $root -Directory|Where-Object {$_.Name -like 'WoA-Wifi*' -or $_.Name -like 'WoA-Wireless*' -or $_.Name -eq 'WoA-DualSta'}){
  $path=[IO.Path]::GetFullPath($d.FullName);if($d.Attributes -band [IO.FileAttributes]::ReparsePoint){continue};if(!$path.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)){throw 'Cleanup escaped ProgramData'}
  foreach($f in DiagnosticFiles $path|Where-Object {!($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -and ($_.Extension -in '.log','.jsonl','.etl','.dmp' -or ($_.Extension -eq '.txt' -and $_.Name -notmatch '^(LICENSE|COPYING|AUTHORS)'))}){try{Remove-Item -LiteralPath $f.FullName -Force -ErrorAction Stop}catch{$failures++}}
 }
 foreach($old in Get-ScheduledTask -ErrorAction SilentlyContinue|Where-Object {$_.TaskName -like 'WoA-Wifi*' -or $_.TaskName -like 'WoA-Hotspot-*' -or $_.TaskName -like 'WoA-DualSta-Verify*'}){if($old.State -ne 'Running'){Unregister-ScheduledTask -TaskName $old.TaskName -Confirm:$false -ErrorAction SilentlyContinue}}
 if(!$failures){Remove-Item -LiteralPath $pending -Force}
}
function Status {
 $s=if(Test-Path -LiteralPath $state){Get-Content -LiteralPath $state -Raw|ConvertFrom-Json}else{[pscustomobject]@{mode='Stopped'}}
 $running=(Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue).State -eq 'Running';$s|Add-Member -NotePropertyName running -NotePropertyValue $running -Force;$s|Add-Member -NotePropertyName enabled -NotePropertyValue (Test-Path -LiteralPath $enabled) -Force
 if($running){try{$s|Add-Member -NotePropertyName connections -NotePropertyValue ((Api Get '/connections' $null).connections.Count) -Force}catch{}}
 $s|ConvertTo-Json -Depth 8 -Compress
}
if($Action -eq 'Start'){if(!(Test-Path -LiteralPath $engine)){throw 'Network runtime is not installed'};[IO.File]::WriteAllText($enabled,'1');Start-ScheduledTask -TaskName $task;Start-Sleep -Seconds 2;Status;return}
if($Action -eq 'Stop'){Remove-Item -LiteralPath $enabled -Force -ErrorAction SilentlyContinue;try{$null=Api Patch '/configs' '{"tun":{"enable":false}}'}catch{};Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue;Status;return}
if($Action -eq 'Status'){Status;return}
if(!(Test-Path -LiteralPath $enabled)){return}
Add-Type -TypeDefinition @'
using System;using System.ComponentModel;using System.Runtime.InteropServices;using System.Diagnostics;using System.Net;using System.Net.NetworkInformation;using System.Net.Sockets;using System.Linq;
public sealed class WoANetwork:IDisposable {
 [DllImport("wlanapi.dll")]static extern uint WlanOpenHandle(uint v,IntPtr r,out uint n,out IntPtr h);
 [DllImport("wlanapi.dll")]static extern uint WlanCloseHandle(IntPtr h,IntPtr r);
 [DllImport("wlanapi.dll")]static extern uint WlanSetInterface(IntPtr h,ref Guid g,uint op,uint n,IntPtr p,IntPtr r);
 [DllImport("wlanapi.dll")]static extern uint WlanQueryInterface(IntPtr h,ref Guid g,uint op,IntPtr r,out uint n,out IntPtr p,out uint t);
 [DllImport("wlanapi.dll")]static extern void WlanFreeMemory(IntPtr p);
 [DllImport("kernel32.dll",SetLastError=true)]static extern IntPtr CreateJobObject(IntPtr a,string n);
 [DllImport("kernel32.dll",SetLastError=true)]static extern bool SetInformationJobObject(IntPtr j,int c,ref JobInfo i,uint n);
 [DllImport("kernel32.dll",SetLastError=true)]static extern bool AssignProcessToJobObject(IntPtr j,IntPtr p);
 [DllImport("kernel32.dll")]static extern bool CloseHandle(IntPtr h);
 [StructLayout(LayoutKind.Sequential)]struct Basic {public long a,b;public uint flags;public UIntPtr min,max;public uint count;public UIntPtr affinity;public uint priority,cls;}
 [StructLayout(LayoutKind.Sequential)]struct Io {public ulong a,b,c,d,e,f;}
 [StructLayout(LayoutKind.Sequential)]struct JobInfo {public Basic basic;public Io io;public UIntPtr a,b,c,d;}
 IntPtr handle,job;Guid primary;public Process Engine{get;private set;}public string LastError{get;private set;}
 public WoANetwork(Guid id){primary=id;uint v;uint e=WlanOpenHandle(2,IntPtr.Zero,out v,out handle);if(e!=0)throw new Win32Exception((int)e);job=CreateJobObject(IntPtr.Zero,null);var i=new JobInfo();i.basic.flags=0x2000;if(job==IntPtr.Zero||!SetInformationJobObject(job,9,ref i,(uint)Marshal.SizeOf(i))){Dispose();throw new Win32Exception(Marshal.GetLastWin32Error());}}
 public uint Hold(bool enable){IntPtr p=Marshal.AllocHGlobal(4);try{Marshal.WriteInt32(p,enable?1:0);return WlanSetInterface(handle,ref primary,18,4,p,IntPtr.Zero);}finally{Marshal.FreeHGlobal(p);}}
 byte[] Query(Guid id,uint op){uint n,t;IntPtr p;if(WlanQueryInterface(handle,ref id,op,IntPtr.Zero,out n,out p,out t)!=0)return null;try{if(n>65536)return null;var b=new byte[n];Marshal.Copy(p,b,0,(int)n);return b;}finally{WlanFreeMemory(p);}}
 public uint EnsureHold(){var b=Query(primary,18);return b!=null&&b.Length>=4&&BitConverter.ToInt32(b,0)!=0?0:Hold(true);}
 public sealed class Link {public string Id,Name;public bool IPv4,IPv6;}
 public Link[] Links(){var ids=new System.Collections.Generic.HashSet<Guid>();ids.Add(primary);var b=Query(primary,17);if(b!=null&&b.Length>=8){int n=BitConverter.ToInt32(b,0);if(n>=0&&n<=8&&8L+n*532L<=b.Length)for(int k=0;k<n;k++){var g=new byte[16];Array.Copy(b,8+k*532,g,0,16);ids.Add(new Guid(g));}}
 return NetworkInterface.GetAllNetworkInterfaces().Where(n=>{Guid g;return Guid.TryParse(n.Id,out g)&&ids.Contains(g)&&n.OperationalStatus==OperationalStatus.Up;}).Select(n=>{var l=new Link{Id=n.Id,Name=n.Name};try{var c=Query(new Guid(n.Id),7);if(c==null||c.Length<4||BitConverter.ToInt32(c,0)!=1)return l;var p=n.GetIPProperties();l.IPv4=p.GatewayAddresses.Any(g=>g.Address.AddressFamily==AddressFamily.InterNetwork)&&p.UnicastAddresses.Any(a=>a.Address.AddressFamily==AddressFamily.InterNetwork&&!a.Address.ToString().StartsWith("169.254."));l.IPv6=p.GatewayAddresses.Any(g=>g.Address.AddressFamily==AddressFamily.InterNetworkV6)&&p.UnicastAddresses.Any(a=>a.Address.AddressFamily==AddressFamily.InterNetworkV6&&!a.Address.IsIPv6LinkLocal&&!a.Address.IsIPv6Multicast&&(a.Address.GetAddressBytes()[0]&0xe0)==0x20);}catch{}return l;}).Where(l=>l.IPv4||l.IPv6).ToArray();}
 public void Start(string exe,string dir,string cfg){Stop();var p=new Process();p.StartInfo=new ProcessStartInfo(exe,"-d \""+dir+"\" -f \""+cfg+"\""){WorkingDirectory=dir,UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true};LastError=null;p.OutputDataReceived+=(s,e)=>{if(e.Data!=null&&(e.Data.Contains("level=error")||e.Data.Contains("level=fatal")))LastError=e.Data.Substring(0,Math.Min(512,e.Data.Length));};p.ErrorDataReceived+=(s,e)=>{if(e.Data!=null)LastError=e.Data.Substring(0,Math.Min(512,e.Data.Length));};p.Start();if(!AssignProcessToJobObject(job,p.Handle)){p.Kill();p.Dispose();throw new Win32Exception(Marshal.GetLastWin32Error());}p.BeginOutputReadLine();p.BeginErrorReadLine();Engine=p;}
 public void Stop(){if(Engine!=null){try{if(!Engine.HasExited){Engine.Kill();Engine.WaitForExit(5000);}}finally{Engine.Dispose();Engine=null;}}}
 public void Dispose(){Stop();if(job!=IntPtr.Zero){CloseHandle(job);job=IntPtr.Zero;}if(handle!=IntPtr.Zero){WlanCloseHandle(handle,IntPtr.Zero);handle=IntPtr.Zero;}}
}
'@
function New-NetworkConfig($Links,$Secret) {
 $v4=@($Links|Where-Object IPv4);$v6=@($Links|Where-Object IPv6);$out=@();$groups=@();$rules=@();$dnsServers=@();$fallback=@($v4+$v6)[0].Name
 foreach($family in 4,6){$list=if($family -eq 4){$v4}else{$v6};$names=@();foreach($l in $list){$name='STA'+$family+'-'+$l.Id;$names+=$name;$out+=@{name=$name;type='direct';'interface-name'=$l.Name;'ip-version'=('ipv'+$family)}
   foreach($server in @((Get-DnsClientServerAddress -InterfaceAlias $l.Name -AddressFamily ('IPv'+$family) -ErrorAction SilentlyContinue).ServerAddresses)){$ip=$null;if(![Net.IPAddress]::TryParse($server,[ref]$ip)){continue};$address=if($family -eq 6){'['+$server.Replace('%','%25')+']'}else{$server};$dnsServers+=('udp://'+$address+':53#'+$name)}
  }
  if($names.Count){$groups+=@{name=('WoA-IPv'+$family);type='load-balance';strategy='round-robin';proxies=$names;url=$(if($family -eq 6){'https://www.qq.com/'}else{'http://www.msftconnecttest.com/connecttest.txt'});interval=10;lazy=$false;timeout=3000;'max-failed-times'=1};$rules+=if($family -eq 6){'IP-CIDR6,::/0,WoA-IPv6,no-resolve'}else{'IP-CIDR,0.0.0.0/0,WoA-IPv4,no-resolve'}}
 }
 if(!$dnsServers.Count){$dnsServers=@('system')};$rules+='MATCH,DIRECT';$exclude=@('0.0.0.0/8','10.0.0.0/8','100.64.0.0/10','127.0.0.0/8','169.254.0.0/16','172.16.0.0/12','192.168.0.0/16','224.0.0.0/4','240.0.0.0/4','::/128','::1/128','fc00::/7','fe80::/10','ff00::/8')
 foreach($l in $Links){foreach($a in @(Get-NetIPAddress -InterfaceAlias $l.Name -AddressFamily IPv6 -ErrorAction SilentlyContinue|Where-Object {$_.IPAddress -match '^[23]' -and $_.PrefixLength -lt 128})){$exclude+=($a.IPAddress+'/'+$a.PrefixLength)}}
 @{mode='rule';ipv6=($v6.Count -gt 0);'log-level'='silent';'allow-lan'=$false;'interface-name'=$fallback;'external-controller'='127.0.0.1:10829';secret=$Secret;'unified-delay'=$true;'find-process-mode'='off';profile=@{'store-selected'=$false;'store-fake-ip'=$false};dns=@{enable=$true;listen='127.0.0.1:1053';ipv6=($v6.Count -gt 0);'enhanced-mode'='redir-host';'default-nameserver'=@('system');nameserver=@($dnsServers|Select-Object -Unique)};tun=@{enable=$true;device='WoA-Aggregate';stack='mixed';mtu=1500;'auto-route'=$true;'auto-detect-interface'=$false;'strict-route'=$false;'dns-hijack'=@();'route-exclude-address'=@($exclude|Select-Object -Unique)};proxies=$out;'proxy-groups'=$groups;rules=$rules}|ConvertTo-Json -Depth 10
}
function SaveState($Mode,$Links,$Message){[ordered]@{mode=$Mode;links=@($Links);cleanupPending=(Test-Path -LiteralPath (Join-Path $runtime 'cleanup.pending'));enginePid=$(if($native -and $native.Engine -and !$native.Engine.HasExited){$native.Engine.Id}else{$null});lastError=$Message}|ConvertTo-Json -Depth 5 -Compress|Set-Content -LiteralPath $state -Encoding UTF8}
$mutex=[Threading.Mutex]::new($false,'Global\WoA.Network.Aggregation');if(!$mutex.WaitOne(0)){$mutex.Dispose();return};$native=$null;$last='';$secret=[Guid]::NewGuid().ToString('N')
try {
 while(Test-Path -LiteralPath $enabled){
  try{
   if(!$native){$a=Get-NetAdapter -Name $InterfaceAlias -ErrorAction Stop;if($a.InterfaceDescription -notlike 'Qualcomm WCN7861*'){throw 'Unexpected primary adapter'};$native=[WoANetwork]::new([Guid]$a.InterfaceGuid);$e=$native.Hold($true);if($e){$native.Dispose();$native=$null;throw "Secondary STA request failed: $e"}}
   $e=$native.EnsureHold();if($e){$native.Dispose();$native=$null;throw "Secondary STA handle lost: $e"}
   $foreign=Get-Process -ErrorAction SilentlyContinue|Where-Object {$_.ProcessName -match 'mihomo|clash|sing-box|openvpn|wireguard|tailscale|v2ray|xray' -and $_.Path -ne $engine};if($foreign){$native.Stop();$last='';SaveState 'PausedForOtherTrafficManager' @() $null;Start-Sleep -Seconds 3;continue}
   $links=@($native.Links());if(!$links.Count){$native.Stop();$last='';SaveState 'WaitingForInternet' @() $null;Start-Sleep -Seconds 3;continue}
   try{Clear-OldNetworkDiagnostics}catch{}
   $json=New-NetworkConfig $links $secret
   if(!$native.Engine -or $native.Engine.HasExited){[IO.File]::WriteAllText($config,$json,[Text.UTF8Encoding]::new($false));$native.Start($engine,$runtime,$config);$last=$json}
   elseif($json -ne $last){[IO.File]::WriteAllText($config,$json,[Text.UTF8Encoding]::new($false));$body=@{path=$config}|ConvertTo-Json -Compress;$null=Api Put '/configs?force=true' $body;$last=$json}
   if($native.Engine.HasExited){throw ('Traffic engine exited: '+$native.Engine.ExitCode+' '+$native.LastError)};$null=Api Get '/version' $null;SaveState 'GlobalTcpUdp' $links $null
  }catch{SaveState 'Retrying' @() $_.Exception.Message;if($native -and $native.Engine -and $native.Engine.HasExited){$native.Stop()}}
  Start-Sleep -Seconds 3
 }
}finally{if($native){$null=$native.Hold($false);$native.Dispose()};SaveState 'Stopped' @() $null;$mutex.ReleaseMutex();$mutex.Dispose()}
