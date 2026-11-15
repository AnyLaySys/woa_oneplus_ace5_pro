#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <d3dkmthk.h>
#include "../wddm/abi.h"
#include "../uapi.h"

int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    if(argc==2 && !strcmp(argv[1],"scheduler")) {
        HANDLE gpu=CreateFileW(L"\\\\.\\A830Gpu",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
        if(gpu==INVALID_HANDLE_VALUE)return 1;
        GPU_SCHEDULER_INFO s={0};DWORD bytes=0;
        BOOL ok=DeviceIoControl(gpu,GPU_IOCTL_SCHEDULER_INFO,NULL,0,&s,sizeof(s),&bytes,NULL);DWORD error=ok?0:GetLastError();
        CloseHandle(gpu);
        if(!ok || bytes!=sizeof(s) || s.Version!=1 || s.Size!=sizeof(s)){printf("{\"scheduler_error\":%lu,\"bytes\":%lu}\n",error,bytes);return 1;}
        printf("{\"generation\":%u,\"flags\":%u,\"failed\":\"%08x\",\"submitted\":%u,\"completed\":%u,\"irq_read\":%u,\"irq_write\":%u,\"pending\":%u,\"inflight\":%u,\"cpu_barriers\":%u,\"preemption_fence\":%u,\"pending_operation\":%u,\"resets\":%u,\"hardware_submitted\":%llu,\"hardware_completed\":%llu,\"oldest_hardware_fence\":%llu,\"completion_polls\":%llu,\"polled_completions\":%llu}\n",
            s.Generation,s.Flags,s.Failed,s.Submitted,s.Completed,s.IrqRead,s.IrqWrite,s.Pending,s.Inflight,s.CpuBarriers,s.PreemptionFence,s.PendingOperation,s.ResetCount,
            s.HardwareSubmitted,s.HardwareCompleted,s.OldestHardwareFence,s.CompletionPolls,s.PolledCompletions);return 0;
    }
    if(argc>=2 && !strcmp(argv[1],"context-protocol")) {
        BOOL set=argc==3;
        if(argc>3 || (set && strcmp(argv[2],"0") && strcmp(argv[2],"1")))return 1;
        HANDLE gpu=CreateFileW(L"\\\\.\\A830Gpu",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
        if(gpu==INVALID_HANDLE_VALUE){printf("{\"open_error\":%lu}\n",GetLastError());return 1;}
        if(set) {
            HANDLE token=NULL;TOKEN_PRIVILEGES p={0};p.PrivilegeCount=1;
            if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token) ||
               !LookupPrivilegeValueW(NULL,L"SeLoadDriverPrivilege",&p.Privileges[0].Luid)){CloseHandle(gpu);return 1;}
            p.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;AdjustTokenPrivileges(token,FALSE,&p,0,NULL,NULL);CloseHandle(token);
        }
        GPU_CONTEXT_PROTOCOL policy={1,sizeof(policy),set?(GPU_U32)strtoul(argv[2],NULL,10):0,0,0,0},reply={0};DWORD bytes=0;
        BOOL ok=DeviceIoControl(gpu,GPU_IOCTL_CONTEXT_PROTOCOL,set?&policy:NULL,set?sizeof(policy):0,&reply,sizeof(reply),&bytes,NULL);
        if(!ok)printf("{\"context_protocol_error\":%lu}\n",GetLastError());
        else if(bytes!=sizeof(reply) || reply.Version!=1 || reply.Size!=sizeof(reply)){ok=FALSE;puts("{\"context_protocol_version_error\":true}");}
        else printf("{\"context_protocol_enabled\":%s,\"switches\":%llu,\"cookie_valid\":%s,\"last_cookie\":%llu}\n",
            reply.Enabled?"true":"false",reply.Switches,reply.CookieValid?"true":"false",reply.LastCookie);
        CloseHandle(gpu);return ok?0:1;
    }
    if(argc>=2 && !strcmp(argv[1],"dpu-trace")) {
        unsigned seconds=argc==3?strtoul(argv[2],NULL,10):10;
        if(argc>3 || !seconds || seconds>30)return 1;
        HANDLE gpu=CreateFileW(L"\\\\.\\A830Gpu",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
        if(gpu==INVALID_HANDLE_VALUE){printf("{\"open_error\":%lu}\n",GetLastError());return 1;}
        GPU_DPU_TRACE *t=calloc(1,sizeof(*t));if(!t){CloseHandle(gpu);return 1;}
        ULONGLONG last=0,lost=0,records=0,droppedStart=0;BOOL first=TRUE;int result=0;
        LARGE_INTEGER begin,now,freq;QueryPerformanceFrequency(&freq);QueryPerformanceCounter(&begin);
        do {
            DWORD used=0;BOOL ok=DeviceIoControl(gpu,GPU_IOCTL_DPU_TRACE,NULL,0,t,sizeof(*t),&used,NULL);
            if(!ok || used!=sizeof(*t) || t->Version!=1 || t->Size!=sizeof(*t)) {
                printf("{\"dpu_trace_error\":%lu,\"bytes\":%lu}\n",GetLastError(),used);result=1;break;
            }
            if(first) {
                last=t->LatestSerial;droppedStart=t->Dropped;first=FALSE;
                printf("{\"trace_begin_serial\":%llu,\"seconds\":%u}\n",last,seconds);
            }
            if(t->LatestSerial<last){puts("{\"dpu_trace_error\":\"producer reset\"}");result=1;break;}
            if(t->LatestSerial-last>GPU_DPU_TRACE_ENTRIES){lost+=t->LatestSerial-last-GPU_DPU_TRACE_ENTRIES;last=t->LatestSerial-GPU_DPU_TRACE_ENTRIES;}
            for(ULONGLONG serial=last+1;serial<=t->LatestSerial;serial++) {
                GPU_DPU_TRACE_ENTRY *e=t->Entries+(serial-1)%GPU_DPU_TRACE_ENTRIES;
                if(e->Serial!=serial)break;
                printf("{\"serial\":%llu,\"kind\":%u,\"time_100ns\":%llu,\"flip_sequence\":%llu,\"physical\":\"%llx\",\"wddm_address\":\"%llx\",\"old_physical\":\"%llx\",\"frame\":%u,\"queue_frame\":%u,\"wr_line\":%u,\"tear_status\":\"%08x\",\"mdp_status\":\"%08x\",\"flush\":\"%08x\"}\n",
                    serial,e->Kind,e->Time100ns,e->FlipSequence,e->Physical,e->Address,e->OldPhysical,e->Frame,e->QueueFrame,e->WrLine,e->TearStatus,e->MdpStatus,e->Flush);
                last=serial;records++;
            }
            Sleep(50);QueryPerformanceCounter(&now);
        }while(now.QuadPart-begin.QuadPart<(LONGLONG)seconds*freq.QuadPart);
        printf("{\"trace_records\":%llu,\"lost_overwritten\":%llu,\"producer_dropped\":%llu}\n",records,lost,t->Dropped-droppedStart);
        free(t);CloseHandle(gpu);return result;
    }
    if(argc>=2 && !strcmp(argv[1],"display-activate")) {
        GPU_DISPLAY_ACTIVATION_REQUEST request={1,0,0,0,0,0};
        if(argc>5)return 1;
        if(argc>2)request.Phase=strtoul(argv[2],NULL,10);
        if(argc>3)request.Step=strtoul(argv[3],NULL,10);
        if(argc>4)request.Index=strtoul(argv[4],NULL,10);
        if(request.Phase) {
            HANDLE token;TOKEN_PRIVILEGES p={0};p.PrivilegeCount=1;
            if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token))return 1;
            if(!LookupPrivilegeValueW(NULL,L"SeLoadDriverPrivilege",&p.Privileges[0].Luid)){CloseHandle(token);return 1;}
            p.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
            BOOL adjusted=AdjustTokenPrivileges(token,FALSE,&p,0,NULL,NULL);DWORD error=GetLastError();CloseHandle(token);
            if(!adjusted || error!=ERROR_SUCCESS){printf("{\"privilege_error\":%lu}\n",error);return 1;}
        }
        HANDLE gpu=CreateFileW(L"\\\\.\\A830Gpu",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
        if(gpu==INVALID_HANDLE_VALUE){printf("{\"open_error\":%lu}\n",GetLastError());return 1;}
        GPU_DISPLAY_ACTIVATION s={0};DWORD used=0;
        BOOL ok=DeviceIoControl(gpu,GPU_IOCTL_DISPLAY_ACTIVATION,&request,sizeof(request),&s,sizeof(s),&used,NULL);
        DWORD error=ok?0:GetLastError();CloseHandle(gpu);
        if(!ok || used!=sizeof(s) || s.Version!=1 || s.Size!=sizeof(s)) {
            printf("{\"activation_error\":%lu,\"bytes\":%lu}\n",error,used);return 1;
        }
        printf("{\"phase\":%u,\"step\":%u,\"index\":%u,\"proof_mask\":\"%08x\",\"status\":\"%08x\",\"last_physical\":\"%llx\",\"cn1_address\":\"%08x\",\"cn1_vote\":\"%08x\",\"gcc_ahb\":\"%08x\",\"gcc_axi\":\"%08x\",\"disp_power\":\"%08x\",\"disp_branches\":[\"%08x\",\"%08x\",\"%08x\"],\"mdss_version\":\"%08x\",\"smmu_id0\":\"%08x\",\"smmu_id1\":\"%08x\",\"slots\":[%u,%u],\"smr\":[\"%08x\",\"%08x\"],\"s2cr\":[\"%08x\",\"%08x\"],\"sctlr\":[\"%08x\",\"%08x\"],\"cbar\":[\"%08x\",\"%08x\"],\"backend_flags\":%u,\"probe_branch_original\":[\"%08x\",\"%08x\",\"%08x\"],\"probe_branch_added\":%u",
            s.Phase,s.Step,s.Index,s.ProofMask,s.LastStatus,s.LastPhysical,s.Cn1Address,s.Cn1Vote,s.GccAhb,s.GccAxi,s.DispPower,
            s.DispBranches[0],s.DispBranches[1],s.DispBranches[2],s.HwVersion,s.SmmuId0,s.SmmuId1,s.Slots[0],s.Slots[1],s.Smr[0],s.Smr[1],s.S2cr[0],s.S2cr[1],s.Sctlr[0],s.Sctlr[1],s.Cbar[0],s.Cbar[1],s.BackendFlags,s.ProbeBranchOriginal[0],s.ProbeBranchOriginal[1],s.ProbeBranchOriginal[2],s.ProbeBranchAdded);
        printf(",\"topology_group\":%u,\"topology_index\":%u,\"topology_offsets\":[\"%08x\",\"%08x\",\"%08x\",\"%08x\",\"%08x\"],\"topology_values\":[\"%08x\",\"%08x\",\"%08x\",\"%08x\",\"%08x\"],\"dpu_fail_stage\":%u}\n",
            s.TopologyGroup,s.TopologyIndex,s.TopologyOffsets[0],s.TopologyOffsets[1],s.TopologyOffsets[2],s.TopologyOffsets[3],s.TopologyOffsets[4],
            s.TopologyValues[0],s.TopologyValues[1],s.TopologyValues[2],s.TopologyValues[3],s.TopologyValues[4],s.DpuFailStage);
        return request.Phase && (int32_t)s.LastStatus<0?1:0;
    }
    if(argc==2 && !strcmp(argv[1],"display-bus")) {
        HANDLE gpu=CreateFileW(L"\\\\.\\A830Gpu",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
        if(gpu==INVALID_HANDLE_VALUE){printf("{\"open_error\":%lu}\n",GetLastError());return 1;}
        GPU_DISPLAY_BUS_INFO b={0};DWORD used=0;
        BOOL ok=DeviceIoControl(gpu,GPU_IOCTL_DISPLAY_BUS_INFO,NULL,0,&b,sizeof(b),&used,NULL);
        DWORD error=ok?0:GetLastError();CloseHandle(gpu);
        if(!ok || used!=sizeof(b) || b.Version!=1 || b.Size!=sizeof(b)) {
            printf("{\"display_bus_error\":%lu,\"bytes\":%lu}\n",error,used);return 1;
        }
        printf("{\"version\":%u,\"cn1_address\":\"%08x\",\"aux_length\":%u,\"unit\":%u,\"width\":%u,\"vcd\":%u,\"vote_x\":%u,\"vote_y\":%u,\"sh1_address\":\"%08x\",\"cn0_address\":\"%08x\",\"regbus_peak_kbps\":%u,\"writes\":0,\"tcu_access\":false}\n",
            b.Version,b.Cn1Address,b.Cn1AuxLength,b.Cn1Unit,b.Cn1Width,b.Cn1Vcd,
            b.Cn1VoteX,b.Cn1VoteY,b.Sh1Address,b.Cn0Address,b.RegBusPeakKBps);return 0;
    }
    if(argc>=2 && !strcmp(argv[1],"display-clocks")) {
        GPU_DISPLAY_CLOCK_REQUEST request={1,1,0,0};
        if(argc==3)request.Phase=strtoul(argv[2],NULL,10);
        if(argc>3 || request.Phase<1 || request.Phase>2)return 1;
        HANDLE gpu=CreateFileW(L"\\\\.\\A830Gpu",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
        if(gpu==INVALID_HANDLE_VALUE){printf("{\"open_error\":%lu}\n",GetLastError());return 1;}
        GPU_DISPLAY_CLOCKS c={0};DWORD used=0;
        BOOL ok=DeviceIoControl(gpu,GPU_IOCTL_DISPLAY_CLOCKS,&request,sizeof(request),&c,sizeof(c),&used,NULL);
        DWORD error=ok?0:GetLastError();CloseHandle(gpu);
        if(!ok || used!=sizeof(c)){printf("{\"display_clocks_error\":%lu,\"phase\":%u}\n",error,request.Phase);return 1;}
        printf("{\"phase\":%u,\"flags\":%u,\"gcc_disp_ahb\":\"%08x\",\"gcc_disp_axi\":\"%08x\",\"disp_power\":\"%08x\",\"writes\":0,\"tcu_access\":false}\n",
            c.Phase,c.Flags,c.GccAhb,c.GccAxi,c.DispPower);return 0;
    }
    if(argc>=2 && (!strcmp(argv[1],"power") || !strcmp(argv[1],"power-set"))) {
        BOOL set=!strcmp(argv[1],"power-set");
        HANDLE gpu=CreateFileW(L"\\\\.\\A830Gpu",GENERIC_READ|(set?GENERIC_WRITE:0),FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
        if(gpu==INVALID_HANDLE_VALUE){printf("{\"open_error\":%lu}\n",GetLastError());return 1;}
        if(set) {
            if(argc!=4){CloseHandle(gpu);return 1;}
            HANDLE token=NULL;TOKEN_PRIVILEGES privileges={0};privileges.PrivilegeCount=1;
            if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token) ||
                !LookupPrivilegeValueW(NULL,L"SeLoadDriverPrivilege",&privileges.Privileges[0].Luid)){CloseHandle(gpu);return 1;}
            privileges.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(token,FALSE,&privileges,0,NULL,NULL);CloseHandle(token);
            GPU_POWER_POLICY policy={strtoul(argv[2],NULL,10),strtoul(argv[3],NULL,10)};DWORD bytes;
            if(!DeviceIoControl(gpu,GPU_IOCTL_POWER_SET,&policy,sizeof(policy),NULL,0,&bytes,NULL)) {
                printf("{\"policy_error\":%lu}\n",GetLastError());CloseHandle(gpu);return 1;
            }
        }
        GPU_POWER_INFO p={0};DWORD bytes;
        BOOL ok=DeviceIoControl(gpu,GPU_IOCTL_POWER,NULL,0,&p,sizeof(p),&bytes,NULL);CloseHandle(gpu);
        if(!ok || bytes!=sizeof(p) || p.Version!=1 || p.Count>GPU_POWER_LEVELS || p.Index>=p.Count){printf("{\"power_error\":%lu}\n",GetLastError());return 1;}
        printf("{\"speed_bin\":%u,\"index\":%u,\"voted_khz\":%u,\"busy_percent\":%u,\"status\":\"%08x\",\"fixed_khz\":%u,\"max_khz\":%u,\"transitions\":%llu,\"levels\":[",p.SpeedBin,p.Index,p.Khz[p.Index],p.BusyPercent,p.Status,p.FixedKhz,p.MaxKhz,p.Transitions);
        for(unsigned i=1;i<p.Count;i++)printf("%s{\"khz\":%u,\"bandwidth_kbps\":%u,\"residency_100ns\":%llu}",i>1?",":"",p.Khz[i],p.Bandwidth[i],p.Residency100ns[i]);
        puts("]}");return 0;
    }
    if(argc==2 && !strcmp(argv[1],"telemetry")) {
        HANDLE gpu=CreateFileW(L"\\\\.\\A830Gpu",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
        if(gpu==INVALID_HANDLE_VALUE){printf("{\"open_error\":%lu}\n",GetLastError());return 1;}
        GPU_TELEMETRY t={0};DWORD bytes=0;
        BOOL ok=DeviceIoControl(gpu,GPU_IOCTL_TELEMETRY,NULL,0,&t,sizeof(t),&bytes,NULL);
        DWORD error=ok?0:GetLastError();CloseHandle(gpu);
        if(!ok || bytes!=sizeof(t) || t.Version!=GPU_TELEMETRY_VERSION || t.Size!=sizeof(t)){
            printf("{\"telemetry_error\":%lu,\"version\":%u,\"bytes\":%lu}\n",error,t.Version,bytes);return 1;
        }
        printf("{\"version\":%u",t.Version);
#define FIELD(x) printf(",\""#x"\":%llu",t.x)
#define ARRAY(x) do {printf(",\""#x"\":[");for(unsigned i=0;i<sizeof(t.x)/sizeof(t.x[0]);i++)printf("%s%llu",i?",":"",t.x[i]);printf("]");}while(0)
        FIELD(PresentBlt);FIELD(PresentFlip);FIELD(PresentOther);FIELD(AddressCalls);FIELD(AddressDirql);ARRAY(AddressFlags);
        FIELD(PresentRotate);FIELD(AddressRejected);FIELD(RejectedWidth);FIELD(RejectedHeight);FIELD(RejectedPitch);
        FIELD(CpuPresentBytes);FIELD(CpuScanoutBytes);FIELD(CpuPagingBytes);FIELD(PagingCalls);FIELD(CreateCalls);FIELD(OpenCalls);
        FIELD(SubmitCalls);FIELD(SubmitRejected);FIELD(Completed);FIELD(ExecutionFailures);FIELD(LastExecutionStatus);FIELD(LastPagingOperation);
        ARRAY(SubmitLatency);ARRAY(VsyncInterval);
        FIELD(HardwareIssued);FIELD(HardwareCompleted);FIELD(MaxHardwareInflight);
        FIELD(Umd.SyncCalls);FIELD(Umd.UploadBytes);FIELD(Umd.ReadbackBytes);FIELD(Umd.RotateCalls);FIELD(Umd.BltCalls);
#undef ARRAY
#undef FIELD
        puts("}");return 0;
    }
    if(argc==4 && !strcmp(argv[1],"capture")) {
        HANDLE token=NULL;TOKEN_PRIVILEGES privileges={0};privileges.PrivilegeCount=1;
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token) ||
            !LookupPrivilegeValueW(NULL,L"SeDebugPrivilege",&privileges.Privileges[0].Luid))return 1;
        privileges.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
        BOOL enabled=AdjustTokenPrivileges(token,FALSE,&privileges,0,NULL,NULL);DWORD privilegeError=GetLastError();CloseHandle(token);
        if(!enabled || privilegeError!=ERROR_SUCCESS){printf("{\"capture_privilege_error\":%lu}\n",privilegeError);return 1;}
        HANDLE gpu=CreateFileW(L"\\\\.\\A830Gpu",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
        if(gpu==INVALID_HANDLE_VALUE)return 1;
        DWORD primary=strtoul(argv[2],NULL,10),bytes=0,capacity=64*1024*1024;
        BYTE *data=malloc(capacity);if(!data){CloseHandle(gpu);return 1;}
        BOOL ok=DeviceIoControl(gpu,GPU_IOCTL_DISPLAY_CAPTURE,&primary,sizeof(primary),data,capacity,&bytes,NULL);
        if(!ok){printf("{\"capture_error\":%lu}\n",GetLastError());free(data);CloseHandle(gpu);return 1;}
        GPU_DISPLAY_CAPTURE *image=(GPU_DISPLAY_CAPTURE *)data;
        BITMAPFILEHEADER file={0};BITMAPINFOHEADER info={0};
        file.bfType=0x4D42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+bytes-sizeof(*image);
        info.biSize=sizeof(info);info.biWidth=image->Width;info.biHeight=-(LONG)image->Height;info.biPlanes=1;info.biBitCount=32;
        FILE *output=NULL;if(fopen_s(&output,argv[3],"wb")){free(data);CloseHandle(gpu);return 1;}
        BOOL saved=fwrite(&file,sizeof(file),1,output)==1 && fwrite(&info,sizeof(info),1,output)==1 &&
            fwrite(data+sizeof(*image),bytes-sizeof(*image),1,output)==1;
        fclose(output);printf("{\"source\":%lu,\"frame\":%u,\"address\":\"%llx\",\"width\":%u,\"height\":%u,\"saved\":%s}\n",primary,image->Frame,image->Address,image->Width,image->Height,saved?"true":"false");
        free(data);CloseHandle(gpu);return saved?0:1;
    }
    if(argc>1 && !strcmp(argv[1],"patch-diagnostics")) {
        HANDLE gpu=CreateFileW(L"\\\\.\\A830Gpu",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
        if(gpu==INVALID_HANDLE_VALUE)return 1;
        GPU_PATCH_DIAGNOSTICS d={0};DWORD bytes=0;
        BOOL ok=DeviceIoControl(gpu,GPU_IOCTL_PATCH_DIAGNOSTICS,NULL,0,&d,sizeof(d),&bytes,NULL);
        if(!ok)printf("{\"patch_diagnostics_error\":%lu}\n",GetLastError());
        else if(bytes!=sizeof(d) || d.Version!=1 || d.Size!=sizeof(d)){ok=FALSE;puts("{\"patch_diagnostics_version_error\":true}");}
        else printf("{\"calls\":%llu,\"failures\":%llu,\"status\":\"%08x\",\"flags\":%u,\"pid\":%u,\"start\":%u,\"end\":%u,\"private_start\":%u,\"private_end\":%u,\"dma_size\":%u,\"private_size\":%u,\"operation\":%u,\"offset_operation\":%u,\"sequence\":%llu,\"lease\":%llu}\n",
            d.Calls,d.Failures,d.Status,d.Flags,d.Pid,d.Start,d.End,d.PrivateStart,d.PrivateEnd,d.DmaSize,d.PrivateSize,d.Operation,d.OffsetOperation,d.Sequence,d.Lease);
        CloseHandle(gpu);return ok?0:1;
    }
    if(argc>1 && !strcmp(argv[1],"clients-detail")) {
        HANDLE gpu=CreateFileW(L"\\\\.\\A830Gpu",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
        if(gpu==INVALID_HANDLE_VALUE)return 1;
        GPU_CLIENT_DIAGNOSTICS stats[256];DWORD bytes=0;
        BOOL ok=DeviceIoControl(gpu,GPU_IOCTL_CLIENT_DIAGNOSTICS,NULL,0,stats,sizeof(stats),&bytes,NULL);
        if(!ok)printf("{\"client_diagnostics_error\":%lu}\n",GetLastError());
        else for(DWORD i=0;i<bytes/sizeof(*stats);i++) {
            GPU_CLIENT_DIAGNOSTICS *s=stats+i;
            if(s->Version!=1 || s->Size!=sizeof(*s)){ok=FALSE;puts("{\"client_diagnostics_version_error\":true}");break;}
            printf("{\"pid\":%u,\"lease\":%llu,\"draining\":%s,\"pending_refs\":%u,\"prepared\":%llu,\"issued\":%llu,\"retired\":%llu,\"cancelled\":%llu,\"unissued\":%lld,\"issued_not_retired\":%lld}\n",
                s->Pid,s->Lease,(s->Flags&1)?"true":"false",s->PendingRefs,s->Prepared,s->Issued,s->Retired,s->Cancelled,
                (long long)s->Prepared-(long long)s->Issued-(long long)s->Cancelled,(long long)s->Issued-(long long)s->Retired);
        }
        CloseHandle(gpu);return ok?0:1;
    }
    if(argc>1 && !strcmp(argv[1],"clients")) {
        HANDLE gpu=CreateFileW(L"\\\\.\\A830Gpu",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
        if(gpu==INVALID_HANDLE_VALUE)return 1;
        GPU_CLIENT_STATS stats[256];DWORD bytes=0;
        BOOL ok=DeviceIoControl(gpu,GPU_IOCTL_CLIENT_STATS,NULL,0,stats,sizeof(stats),&bytes,NULL);
        if(!ok)printf("{\"client_statistics_error\":%lu}\n",GetLastError());
        else for(DWORD i=0;i<bytes/sizeof(*stats);i++)printf("{\"pid\":%u,\"buffers\":%u,\"bytes\":%llu,\"hardware_submissions\":%llu,\"hardware_completions\":%llu,\"lease\":%llu}\n",
            stats[i].Pid,stats[i].Buffers,stats[i].Bytes,stats[i].Submissions,stats[i].Completions,stats[i].Lease);
        CloseHandle(gpu);return ok?0:1;
    }
    D3DKMT_ADAPTERINFO adapters[16]={0};D3DKMT_ENUMADAPTERS2 list={16,adapters};
    NTSTATUS status=D3DKMTEnumAdapters2(&list);if(status<0)return 1;
    LUID luid={0};int found=0;
    for(UINT i=0;i<list.NumAdapters;i++) {
        A830_ADAPTER_INFO info={0};D3DKMT_QUERYADAPTERINFO query={adapters[i].hAdapter,KMTQAITYPE_UMDRIVERPRIVATE,&info,sizeof(info)};
        if(D3DKMTQueryAdapterInfo(&query)>=0 && info.Version==A830_ABI_VERSION && info.Chip==0x44050001 && (info.Flags&1)) {
            luid=adapters[i].AdapterLuid;found=1;
            D3DKMT_DRIVERVERSION version=0;D3DKMT_ADAPTERTYPE type={0};
            query.Type=KMTQAITYPE_DRIVERVERSION;query.pPrivateDriverData=&version;query.PrivateDriverDataSize=sizeof(version);
            NTSTATUS versionStatus=D3DKMTQueryAdapterInfo(&query);
            query.Type=KMTQAITYPE_ADAPTERTYPE;query.pPrivateDriverData=&type;query.PrivateDriverDataSize=sizeof(type);
            NTSTATUS typeStatus=D3DKMTQueryAdapterInfo(&query);
            printf("{\"wddm\":%u,\"version_status\":\"%08lx\",\"type_status\":\"%08lx\",\"render\":%u,\"software\":%u}\n",version,versionStatus,typeStatus,type.RenderSupported,type.SoftwareDevice);
            D3DKMT_SEGMENTSIZEINFO memory={0};
            query.Type=KMTQAITYPE_GETSEGMENTSIZE;query.pPrivateDriverData=&memory;query.PrivateDriverDataSize=sizeof(memory);
            NTSTATUS memoryStatus=D3DKMTQueryAdapterInfo(&query);
            printf("{\"memory_status\":\"%08lx\",\"dedicated_video_bytes\":%llu,\"dedicated_system_bytes\":%llu,\"shared_system_bytes\":%llu}\n",
                memoryStatus,memory.DedicatedVideoMemorySize,memory.DedicatedSystemMemorySize,memory.SharedSystemMemorySize);
            if(argc==2 && !strcmp(argv[1],"memory"))for(UINT segment=0;segment<2;segment++) {
                D3DKMT_QUERYSTATISTICS usage={0};usage.Type=D3DKMT_QUERYSTATISTICS_SEGMENT;usage.AdapterLuid=luid;usage.QuerySegment.SegmentId=segment;
                NTSTATUS result=D3DKMTQueryStatistics(&usage);
                if(result>=0) {
                    D3DKMT_QUERYSTATISTICS_SEGMENT_INFORMATION *s=&usage.QueryResult.SegmentInformation;
                    ULONGLONG evicted=0;for(UINT priority=0;priority<D3DKMT_QUERYSTATISTICS_ALLOCATION_PRIORITY_CLASS_MAX;priority++)evicted+=s->TotalBytesEvictedByPriority[priority];
                    printf("{\"segment\":%u,\"limit\":%llu,\"committed\":%llu,\"resident\":%llu,\"evicted\":%llu,\"aperture\":%lu}\n",segment,s->CommitLimit,s->BytesCommitted,s->BytesResident,evicted,s->Aperture);
                }
            }
        }
        D3DKMT_CLOSEADAPTER close={adapters[i].hAdapter};D3DKMTCloseAdapter(&close);
    }
    if(!found){puts("{\"native_adapter\":\"NOT_FOUND\"}");return 1;}
    if(argc==2 && !strcmp(argv[1],"memory"))return 0;
    DWORD pid=argc>1?strtoul(argv[1],NULL,10):0;
    HANDLE process=pid?OpenProcess(PROCESS_QUERY_INFORMATION,FALSE,pid):NULL;
    if(pid && !process){printf("{\"process_open_error\":%lu}\n",GetLastError());return 1;}
    ULONGLONG previous=0;int advanced=0;
    for(UINT sample=0;sample<10;sample++) {
        D3DKMT_QUERYSTATISTICS query={0};query.Type=process?D3DKMT_QUERYSTATISTICS_PROCESS_NODE:D3DKMT_QUERYSTATISTICS_NODE;
        query.hProcess=process;query.AdapterLuid=luid;query.QueryNode.NodeId=0;
        status=D3DKMTQueryStatistics(&query);
        if(status<0){printf("{\"query_status\":\"%08lx\"}\n",status);return 1;}
        ULONGLONG running=(ULONGLONG)(process?query.QueryResult.ProcessNodeInformation.RunningTime.QuadPart:
            query.QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart);
        printf("{\"luid\":\"%08lx:%08lx\",\"node\":0,\"running_time_raw\":%llu,\"delta\":%llu}\n",
            (ULONG)luid.HighPart,luid.LowPart,running,sample?running-previous:0);
        if(sample && running>previous)advanced=1;previous=running;
        if(sample<9)Sleep(500);
    }
    printf("{\"windows_gpu_statistics\":\"%s\"}\n",advanced?"PASS":"NO_ACTIVITY");
    if(process)CloseHandle(process);
    return advanced?0:1;
}
