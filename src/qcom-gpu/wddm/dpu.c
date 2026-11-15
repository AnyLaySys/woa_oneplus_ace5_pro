#include "adapter.h"
#include "dpu.h"

/* SM8750 DPU12: Linux dpu_12_0_sm8750.h, dpu_hw_sspp.c,
 * dpu_hw_ctl.c and dpu_hw_intf.c. No DSI/DSC/clock programming is performed.
 */
#define DPU_REGISTER_BYTES 0x90000u
static ULONGLONG PreciseTime(VOID) {
    ULONGLONG qpc;
    return KeQueryInterruptTimePrecise(&qpc);
}
static GPU_DISPLAY_ACTIVATION Activation;
static PVOID ActivationAdapter;
static volatile LONG ActivationAction;
static ULONGLONG ActivationDeadline;
static GPU_DPU_TRACE DpuTrace;
static volatile LONG TraceWriter;
static ULONG BranchReadMask;
static ULONG FrameCounterSource;
static ULONG NotificationDivisor=1;
static ULONG ProbedIntfValid,ProbedIntfSync[4],ProbedIntfAuto[4],ProbedIntfTe[4];
static VOID KickDpu(A830_ADAPTER *a);
static NTSTATUS WaitBranch(PUCHAR mapping,ULONG offset,PULONG observed) {
    ULONGLONG end=KeQueryInterruptTime()+20000;
    do {
        ULONG value=READ_REGISTER_ULONG((PULONG)(mapping+offset));*observed=value;
        if(value==MAXULONG)return STATUS_IO_DEVICE_ERROR;
        /* Linux clk_branch2_check_halt: CLK_OFF clear OR NoC FSM ON. */
        if((value&1) && (!(value&0x80000000u) || ((value>>28)&7u)==2))return STATUS_SUCCESS;
        KeStallExecutionProcessor(10);
    } while(KeQueryInterruptTime()<end);
    return STATUS_IO_TIMEOUT;
}
static NTSTATUS RestoreProbeBranches(A830_ADAPTER *a) {
    if(ActivationAdapter!=a || !Activation.ProbeBranchAdded)return STATUS_SUCCESS;
    PHYSICAL_ADDRESS p;p.QuadPart=0xaf08000;
    PUCHAR mapping=MmMapIoSpaceEx(p,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
    if(!mapping)return STATUS_INSUFFICIENT_RESOURCES;
    ULONG value=READ_REGISTER_ULONG((PULONG)(mapping+0xb0));
    if(value==MAXULONG){MmUnmapIoSpace(mapping,PAGE_SIZE);return STATUS_IO_DEVICE_ERROR;}
    if(Activation.ProbeBranchAdded&1) {
        /* Release only our added SW enable, preserving all firmware fields. */
        WRITE_REGISTER_ULONG((PULONG)(mapping+0xb0),value&~1u);KeMemoryBarrier();
        value=READ_REGISTER_ULONG((PULONG)(mapping+0xb0));
        if(value==MAXULONG || (value&1)){MmUnmapIoSpace(mapping,PAGE_SIZE);return STATUS_IO_DEVICE_ERROR;}
        Activation.DispBranches[0]=value;Activation.ProofMask&=~4u;Activation.ProbeBranchAdded&=~1u;
    }
    MmUnmapIoSpace(mapping,PAGE_SIZE);return STATUS_SUCCESS;
}
static NTSTATUS EnableProbeAhb(A830_ADAPTER *a) {
    if(ActivationAdapter!=a || (Activation.ProofMask&3)!=3 || !(BranchReadMask&1) || A830DpuEnabled(a))return STATUS_DEVICE_NOT_READY;
    Activation.LastPhysical=0xaf080b0;Activation.LastStatus=STATUS_PENDING;KeMemoryBarrier();
    PHYSICAL_ADDRESS p;p.QuadPart=0xaf08000;
    PUCHAR mapping=MmMapIoSpaceEx(p,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
    if(!mapping)return STATUS_INSUFFICIENT_RESOURCES;
    ULONG value=READ_REGISTER_ULONG((PULONG)(mapping+0xb0));
    if(value==MAXULONG){MmUnmapIoSpace(mapping,PAGE_SIZE);return STATUS_IO_DEVICE_ERROR;}
    if(!(value&1)) {
        if(!(Activation.ProbeBranchAdded&1))Activation.ProbeBranchOriginal[0]=value;
        Activation.ProbeBranchAdded|=1;KeMemoryBarrier();
        WRITE_REGISTER_ULONG((PULONG)(mapping+0xb0),value|1);KeMemoryBarrier();
    }
    NTSTATUS status=WaitBranch(mapping,0xb0,&Activation.DispBranches[0]);
    MmUnmapIoSpace(mapping,PAGE_SIZE);
    if(NT_SUCCESS(status))Activation.ProofMask|=4;
    else {
        Activation.ProofMask&=~4u;
        NTSTATUS restored=RestoreProbeBranches(a);if(!NT_SUCCESS(restored))return restored;
    }
    return status;
}
static BOOLEAN TranslationProved(A830_ADAPTER *a) {
    if(ActivationAdapter!=a || (Activation.ProofMask&0x1ff)!=0x1ff)return FALSE;
    if(!(Activation.DispPower&(1u<<16)) || !Activation.HwVersion)return FALSE;
    for(ULONG i=0;i<3;i++)if(!(Activation.DispBranches[i]&1))return FALSE;
    for(ULONG i=0;i<2;i++) {
        ULONG smr=Activation.Smr[i],mask=(smr>>16)&0x7fffu;
        if(!(smr&0x80000000u) || (mask!=0 && mask!=2) ||
           (((smr&0xffffu)&~mask)!=0x800u && ((smr&0xffffu)&~mask)!=0x802u))return FALSE;
        ULONG type=(Activation.S2cr[i]>>16)&3u;
        if(type==1)continue;
        if(type || !(Activation.ProofMask&(0x200u<<i)) || !(Activation.ProofMask&(0x800u<<i)) ||
           (Activation.Sctlr[i]&1) || ((Activation.Cbar[i]>>16)&3u)!=1)return FALSE;
    }
    return TRUE;
}
static VOID BackendSnapshot(A830_ADAPTER *a,GPU_DISPLAY_ACTIVATION *out);
static NTSTATUS ActivationRead(ULONGLONG physical,PULONG value) {
    Activation.LastPhysical=physical;Activation.LastStatus=STATUS_PENDING;KeMemoryBarrier();
    PHYSICAL_ADDRESS p;p.QuadPart=(LONGLONG)(physical&~(ULONGLONG)(PAGE_SIZE-1));
    PUCHAR mapping=MmMapIoSpaceEx(p,PAGE_SIZE,PAGE_READONLY|PAGE_NOCACHE);
    if(!mapping)return STATUS_INSUFFICIENT_RESOURCES;
    *value=READ_REGISTER_ULONG((PULONG)(mapping+(ULONG)(physical&(PAGE_SIZE-1))));
    MmUnmapIoSpace(mapping,PAGE_SIZE);
    return *value==MAXULONG?STATUS_IO_DEVICE_ERROR:STATUS_SUCCESS;
}
NTSTATUS A830DisplayActivation(PVOID context,PVOID input,PVOID output) {
    A830_ADAPTER *a=context;GPU_DISPLAY_ACTIVATION_REQUEST request=*(GPU_DISPLAY_ACTIVATION_REQUEST *)input;
    if(!a || KeGetCurrentIrql()!=PASSIVE_LEVEL || request.Version!=1 ||
       request.Phase>6 || request.Flags || request.Reserved)
        return STATUS_INVALID_PARAMETER;
    if(ActivationAdapter!=a){RtlZeroMemory(&Activation,sizeof(Activation));BranchReadMask=0;ProbedIntfValid=0;FrameCounterSource=0;NotificationDivisor=1;ActivationAdapter=a;}
    Activation.Version=1;Activation.Size=sizeof(Activation);
    if(request.Phase) {
        Activation.Phase=request.Phase;Activation.Step=request.Step;Activation.Index=request.Index;
        Activation.LastPhysical=0;NTSTATUS status=STATUS_NOT_SUPPORTED;
        if(InterlockedCompareExchange(&ActivationAction,0,0))status=STATUS_DEVICE_BUSY;
        else if(!a->Hardware.Online)status=STATUS_DEVICE_NOT_READY;
        else if(request.Phase==1 && !request.Step && !request.Index) {
            if(Activation.ProofMask&1)status=STATUS_SUCCESS;
            else {
                GPU_DISPLAY_BUS_INFO bus={0};status=A830DisplayBusInfo(a,&bus);
                if(NT_SUCCESS(status)) {
                    Activation.Cn1Address=bus.Cn1Address;
                    Activation.Cn1Vote=0x60000000u|(bus.Cn1VoteX<<14)|bus.Cn1VoteY;
                    Activation.LastStatus=STATUS_PENDING;KeMemoryBarrier();
                    /* ACT-only persistent floor. GpuRscWrite uses the shared
                       APSS DRV2 lock and waits for the RR command ACK. */
                    status=GpuRscWrite(&a->Hardware,Activation.Cn1Address,Activation.Cn1Vote);
                    if(NT_SUCCESS(status))Activation.ProofMask|=1;
                }
            }
        } else if(request.Phase==2 && (Activation.ProofMask&1)) {
            if(request.Step==0 && !request.Index) {
                Activation.ProofMask&=~2u;
                Activation.GccAhb=GpuRead(a->Hardware.Gcc,0x27004);
                Activation.GccAxi=GpuRead(a->Hardware.Gcc,0x27008);
                if(Activation.GccAhb==MAXULONG || Activation.GccAxi==MAXULONG ||
                   !(Activation.GccAhb&1) || !(Activation.GccAxi&1))status=STATUS_DEVICE_NOT_READY;
                else {
                    /* A critical HW-gated GCC branch can report CLK_OFF
                       while idle. Preserve HW_CTL/RCG; do not force it on. */
                    status=ActivationRead(0xaf09004,&Activation.DispPower);
                    if(NT_SUCCESS(status) && (Activation.DispPower&(1u<<16)))Activation.ProofMask|=2;
                }
            } else if(request.Step==1 && request.Index<3 && (Activation.ProofMask&2)) {
                static const ULONG branches[]={0x80b0,0x8010,0x8030};
                Activation.ProofMask&=~(4u<<request.Index);
                status=ActivationRead(0xaf00000ULL+branches[request.Index],&Activation.DispBranches[request.Index]);
                if(NT_SUCCESS(status))BranchReadMask|=1u<<request.Index;
                if(NT_SUCCESS(status) && (Activation.DispBranches[request.Index]&1))Activation.ProofMask|=4u<<request.Index;
            } else if(request.Step==2 && !request.Index && (Activation.ProofMask&0x1f)==0x1f) {
                Activation.ProofMask&=~0x20u;
                status=ActivationRead(0xae00000,&Activation.HwVersion);
                if(NT_SUCCESS(status) && Activation.HwVersion)Activation.ProofMask|=0x20;
            } else if(request.Step==3 && !request.Index) {
                status=EnableProbeAhb(a);
            } else if(request.Step==4 && !request.Index) {
                status=A830DpuEnabled(a)?STATUS_DEVICE_BUSY:RestoreProbeBranches(a);
            } else status=STATUS_DEVICE_NOT_READY;
        } else if(request.Phase==3 && (Activation.ProofMask&1)) {
            if(request.Step==0 && request.Index<2) {
                Activation.ProofMask&=~(0x1f00u|(0x40u<<request.Index));
                if(!request.Index) {
                    status=ActivationRead(0x15000020,&Activation.SmmuId0);
                    if(NT_SUCCESS(status) && (Activation.SmmuId0&(1u<<27)) && (Activation.SmmuId0&255))Activation.ProofMask|=0x40;
                } else {
                    status=ActivationRead(0x15000024,&Activation.SmmuId1);
                    if(NT_SUCCESS(status))Activation.ProofMask|=0x80;
                }
            } else if(request.Step==1 && !request.Index && (Activation.ProofMask&0xc0)==0xc0) {
                ULONG count=min(Activation.SmmuId0&255u,128u);status=STATUS_SUCCESS;
                Activation.ProofMask&=~0x1f00u;
                Activation.MatchCount=0;Activation.Slots[0]=Activation.Slots[1]=MAXULONG;
                for(ULONG slot=0;slot<count && NT_SUCCESS(status);slot++) {
                    ULONG smr=0;status=ActivationRead(0x15000800ULL+slot*4,&smr);
                    if(!NT_SUCCESS(status) || !(smr&0x80000000u))continue;
                    ULONG mask=(smr>>16)&0x7fffu;BOOLEAN matched=FALSE;
                    for(ULONG i=0;i<2;i++) {
                        ULONG stream=0x800u+i*2;
                        if(((smr^stream)&0xffffu&~mask)!=0)continue;
                        if(Activation.Slots[i]!=MAXULONG){status=STATUS_DEVICE_CONFIGURATION_ERROR;break;}
                        Activation.Slots[i]=slot;Activation.Smr[i]=smr;
                        status=ActivationRead(0x15000c00ULL+slot*4,&Activation.S2cr[i]);matched=TRUE;
                        if(!NT_SUCCESS(status))break;
                    }
                    if(matched)Activation.MatchCount++;
                }
                if(NT_SUCCESS(status) && Activation.Slots[0]!=MAXULONG && Activation.Slots[1]!=MAXULONG)Activation.ProofMask|=0x100;
                else if(NT_SUCCESS(status))status=STATUS_NOT_FOUND;
            } else if((request.Step==2 || request.Step==3) && request.Index<2 && (Activation.ProofMask&0x100)) {
                ULONG bank=Activation.S2cr[request.Index]&255u;
                ULONG shift=(Activation.SmmuId1&0x80000000u)?16u:12u;
                ULONG pages=1u<<(((Activation.SmmuId1>>28)&7u)+1);
                ULONGLONG offset=request.Step==2?((ULONGLONG)(pages+bank)<<shift):((1ULL<<shift)+bank*4);
                if(bank>=(Activation.SmmuId1&255u) || offset>=0x100000)status=STATUS_DEVICE_CONFIGURATION_ERROR;
                else if(request.Step==2) {
                    Activation.ProofMask&=~(0x200u<<request.Index);
                    status=ActivationRead(0x15000000+offset,&Activation.Sctlr[request.Index]);
                    if(NT_SUCCESS(status))Activation.ProofMask|=0x200u<<request.Index;
                } else {
                    Activation.ProofMask&=~(0x800u<<request.Index);
                    status=ActivationRead(0x15000000+offset,&Activation.Cbar[request.Index]);
                    if(NT_SUCCESS(status))Activation.ProofMask|=0x800u<<request.Index;
                }
            } else status=STATUS_DEVICE_NOT_READY;
        } else if(request.Phase==4 && (Activation.ProofMask&0x3f)==0x3f) {
            ULONG base=0;const ULONG *offsets=NULL;
            static const ULONG pipes[]={0x4000,0x6000,0x8000,0xa000,0x24000,0x26000,0x28000,0x2a000,0x2c000,0x2e000};
            static const ULONG pipeOffsets[]={0x14,0x24,0,0x8,0x30};
            static const ULONG ctlOffsets[]={0x12c,0xfc,0xf4,0x18,0x1c};
            static const ULONG intfOffsets[]={0x284,0x288,0x298,0x2b4,0xac};
            static const ULONG tearConfigOffsets[]={0x28c,0x294,0x2a0,0x2a4,0x29c};
            static const ULONG tearExtraOffsets[]={0x290,0x2a8,0x2ac,0x2b0,0x280};
            static const ULONG pipeExtraOffsets[]={0x8,0xc,0x10,0x38,0x70};
            static const ULONG ctlExtraOffsets[]={0x130,0x134,0x14,0xe4,0xe8};
            if(!request.Step && request.Index<RTL_NUMBER_OF(pipes)){base=pipes[request.Index];offsets=pipeOffsets;}
            else if(request.Step==1 && request.Index<6){base=0x15000+request.Index*0x1000;offsets=ctlOffsets;}
            else if(request.Step==2 && request.Index<4){base=0x34000+request.Index*0x1000;offsets=intfOffsets;}
            else if(request.Step==3 && request.Index<4){base=0x34000+request.Index*0x1000;offsets=tearConfigOffsets;}
            else if(request.Step==4 && request.Index<4){base=0x34000+request.Index*0x1000;offsets=tearExtraOffsets;}
            else if(request.Step==5 && request.Index<RTL_NUMBER_OF(pipes)){base=pipes[request.Index];offsets=pipeExtraOffsets;}
            else if(request.Step==6 && request.Index<6){base=0x15000+request.Index*0x1000;offsets=ctlExtraOffsets;}
            if(!offsets)status=STATUS_INVALID_PARAMETER;
            else {
                Activation.TopologyGroup=request.Step;Activation.TopologyIndex=request.Index;status=STATUS_SUCCESS;
                RtlZeroMemory(Activation.TopologyValues,sizeof(Activation.TopologyValues));
                for(ULONG i=0;i<5 && NT_SUCCESS(status);i++) {
                    Activation.TopologyOffsets[i]=base+offsets[i];
                    status=ActivationRead(0xae01000ULL+base+offsets[i],Activation.TopologyValues+i);
                }
                if(NT_SUCCESS(status) && request.Step==2) {
                    ProbedIntfTe[request.Index]=Activation.TopologyValues[0];
                    ProbedIntfSync[request.Index]=Activation.TopologyValues[1];
                    ProbedIntfAuto[request.Index]=Activation.TopologyValues[3];
                    ProbedIntfValid|=1u<<request.Index;
                }
            }
        } else if(request.Phase==5 && request.Step==2 && !request.Index) {
            /* Exercise the actual precise-clock API before any DPU takeover.
               Its QPC output is mandatory, including on ARM64. */
            ULONGLONG first=PreciseTime(),second=PreciseTime();
            Activation.TopologyGroup=6;Activation.TopologyIndex=0;
            RtlZeroMemory(Activation.TopologyOffsets,sizeof(Activation.TopologyOffsets));
            Activation.TopologyValues[0]=(ULONG)first;Activation.TopologyValues[1]=(ULONG)(first>>32);
            Activation.TopologyValues[2]=(ULONG)second;Activation.TopologyValues[3]=(ULONG)(second>>32);
            Activation.TopologyValues[4]=0;
            status=second>=first?STATUS_SUCCESS:STATUS_UNSUCCESSFUL;
        } else if(request.Phase==5 && request.Step==1 && request.Index>=1 && request.Index<=2 && (Activation.ProofMask&0x3f)==0x3f) {
            if(A830DpuEnabled(a))status=STATUS_DEVICE_BUSY;
            else {NotificationDivisor=request.Index;status=STATUS_SUCCESS;}
        } else if(request.Phase==5 && !request.Step && request.Index<3 && (Activation.ProofMask&0x3f)==0x3f) {
            if(A830DpuEnabled(a))status=STATUS_DEVICE_BUSY;
            else if(!request.Index){FrameCounterSource=0;status=STATUS_SUCCESS;}
            else {
                BOOLEAN found=FALSE;
                for(ULONG i=0;i<4;i++)if((ProbedIntfValid&(1u<<i)) && (ProbedIntfTe[i]&1) &&
                    (ProbedIntfSync[i]&0x180000u)==0x180000u && ProbedIntfAuto[i]==0x80000001u)found=TRUE;
                if(!found)status=STATUS_DEVICE_NOT_READY;
                else {FrameCounterSource=request.Index;status=STATUS_SUCCESS;}
            }
        } else if(request.Phase==6 && !request.Step && !request.Index) {
            if(!TranslationProved(a)){Activation.DpuFailStage=1;status=STATUS_DEVICE_NOT_READY;}
            else if(A830DpuEnabled(a))status=STATUS_SUCCESS;
            else {
                ActivationDeadline=KeQueryInterruptTime()+20000000;
                InterlockedIncrement(&a->CpuBarriers);KeMemoryBarrier();
                InterlockedExchange(&ActivationAction,1);A830WakeDisplay(a);status=STATUS_PENDING;
            }
        }
        Activation.LastStatus=(ULONG)status;
    }
    RtlCopyMemory(output,&Activation,sizeof(Activation));BackendSnapshot(a,output);
    return STATUS_SUCCESS;
}
BOOLEAN A830DpuWork(PVOID context) {
    A830_ADAPTER *a=context;
    if(ActivationAdapter!=a || !InterlockedCompareExchange(&ActivationAction,0,0)){KickDpu(a);return FALSE;}
    NTSTATUS status=STATUS_PENDING;
    if(a->Stopping)status=STATUS_DEVICE_REMOVED;
    else if(a->Failed)status=(NTSTATUS)a->Failed;
    else if(!a->Hardware.Online || !a->Hardware.Memory.Cpu)status=STATUS_DEVICE_NOT_READY;
    else if(KeQueryInterruptTime()>=ActivationDeadline)status=STATUS_IO_TIMEOUT;
    else if(a->Hardware.Submitted>(ULONGLONG)*(volatile ULONG *)a->Hardware.Memory.Cpu)return TRUE;
    else {
        Activation.DpuFailStage=2;
        status=A830GpuScanoutReserveStaging(a);
        if(status==STATUS_DEVICE_BUSY || status==STATUS_PENDING)return TRUE;
        if(NT_SUCCESS(status)) {
            if(KeQueryInterruptTime()>=ActivationDeadline)status=STATUS_IO_TIMEOUT;
            else status=A830DpuStart(a);
        }
    }
    Activation.LastStatus=(ULONG)status;KeMemoryBarrier();
    A830Record(L"DpuStatus",(ULONG)status);
    InterlockedExchange(&ActivationAction,0);InterlockedDecrement(&a->CpuBarriers);
    A830WakeDisplay(a);return FALSE;
}
NTSTATUS A830DisplayBusInfo(PVOID context,PVOID output) {
    A830_ADAPTER *a=context;GPU_DISPLAY_BUS_INFO *info=output;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !a || !a->Hardware.Online || !a->Hardware.Db)
        return STATUS_DEVICE_NOT_READY;
    RtlZeroMemory(info,sizeof(*info));info->Version=1;info->Size=sizeof(*info);
    ULONG aux=0,length=0;GPU *g=&a->Hardware;
    NTSTATUS status=GpuDb(g,"CN1",&info->Cn1Address,&aux,&length);
    if(!NT_SUCCESS(status))return status;
    if(length<8 || (aux&3) || aux>0x20000-8)return STATUS_DEVICE_CONFIGURATION_ERROR;
    info->Cn1AuxLength=length;info->Cn1Unit=GpuRead(g->Db,aux);
    ULONG tail=GpuRead(g->Db,aux+4);info->Cn1Width=tail&0xffffu;info->Cn1Vcd=(tail>>16)&255u;
    if(!info->Cn1Unit || !info->Cn1Width)return STATUS_DEVICE_CONFIGURATION_ERROR;
    /* msm_mdss sm8750 regbus peak57000kB/s, CN1 node buswidth4,
       channels1; bcm-voter uses scale1000, then command-db unit divisor.
       This calculates a prospective vote only. No RSC/BCM write occurs. */
    info->RegBusPeakKBps=57000;info->Cn1VoteX=0;
    ULONGLONG peak=(ULONGLONG)info->RegBusPeakKBps*info->Cn1Width/4;
    if(peak>(~0ULL)/1000)return STATUS_INTEGER_OVERFLOW;
    ULONGLONG scaled=peak*1000;
    ULONGLONG vote=scaled/info->Cn1Unit;if(scaled && !vote)vote=1;
    if(vote>0x3fff)return STATUS_INTEGER_OVERFLOW;
    info->Cn1VoteY=(ULONG)vote;
    status=GpuDb(g,"SH1",&info->Sh1Address,&aux,&length);
    if(NT_SUCCESS(status))status=GpuDb(g,"CN0",&info->Cn0Address,&aux,&length);
    return status;
}
NTSTATUS A830DisplayClocks(PVOID context,PVOID input,PVOID output) {
    A830_ADAPTER *a=context;GPU_DISPLAY_CLOCK_REQUEST request=*(GPU_DISPLAY_CLOCK_REQUEST *)input;
    GPU_DISPLAY_CLOCKS *result=output;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || request.Version!=1 || request.Phase<1 ||
       request.Phase>2 || request.Flags || request.Reserved)return STATUS_INVALID_PARAMETER;
    if(!a || !a->Hardware.Gcc)return STATUS_DEVICE_NOT_READY;
    RtlZeroMemory(result,sizeof(*result));result->Version=1;result->Phase=request.Phase;
    /* Phase 1 reuses the GPU driver's existing GCC mapping, touches exactly
       two established branch registers, and performs no register writes. */
    result->GccAhb=READ_REGISTER_ULONG((PULONG)(a->Hardware.Gcc+0x27004));
    result->GccAxi=READ_REGISTER_ULONG((PULONG)(a->Hardware.Gcc+0x27008));
    if((result->GccAhb&0x80000001u)==1)result->Flags|=1;
    if(result->GccAxi!=MAXULONG && (result->GccAxi&1))result->Flags|=2;
    if(request.Phase==1)return STATUS_SUCCESS;
    /* Phase 2 is a separate, explicit experiment. Do not map or read DISPCC
       unless phase-1 gate checks currently pass. Never read MDSS or TCU here. */
    if((result->Flags&3)!=3)return STATUS_DEVICE_NOT_READY;
    PHYSICAL_ADDRESS p;p.QuadPart=0xaf09000;
    PUCHAR registers=MmMapIoSpaceEx(p,PAGE_SIZE,PAGE_READONLY|PAGE_NOCACHE);
    if(!registers)return STATUS_INSUFFICIENT_RESOURCES;
    result->DispPower=READ_REGISTER_ULONG((PULONG)(registers+4));
    MmUnmapIoSpace(registers,PAGE_SIZE);
    if(result->DispPower!=MAXULONG && (result->DispPower&(1u<<16)))result->Flags|=4;
    return STATUS_SUCCESS;
}
typedef struct { ULONG Base,Flush,Active; } DPU_PIPE_DESCRIPTION;
static const DPU_PIPE_DESCRIPTION PipeDescriptions[]={
    {0x4000,1u<<0,1u<<16},{0x6000,1u<<1,1u<<17},
    {0x8000,1u<<2,1u<<18},{0xa000,1u<<18,1u<<19},
    {0x24000,1u<<11,1u<<0},{0x26000,1u<<12,1u<<1},
    {0x28000,1u<<24,1u<<2},{0x2a000,1u<<25,1u<<3},
    {0x2c000,1u<<13,1u<<4},{0x2e000,1u<<14,1u<<5}
};
typedef struct { ULONG Base,OriginalAddress,OriginalStride,Offset; } DPU_PIPE;
typedef struct { ULONG Base,Mask; } DPU_CTL;
typedef struct {
    A830_ADAPTER *Adapter;
    PUCHAR Registers,Smmu,Gcc,Dispcc;
    ULONG ClockAdded;
    PEX_TIMER Timer;
    DPU_PIPE Pipes[10];ULONG PipeCount;
    DPU_CTL Ctls[6];ULONG CtlCount;
    ULONG Intf,Frame,AutoRefresh,CounterOffset;
    ULONG SmmuEntry,SmmuOriginal;BOOLEAN SmmuChanged;
    BOOLEAN Restored;
    volatile LONG Stopping,Enabled,Vsync;
    volatile LONG64 Queued,Applied;
    ULONGLONG PendingAddress;
    ULONGLONG ActiveAddress;
    ULONG PendingPitch,ActivePitch,ActiveFrame;
    volatile LONG64 ActiveSequence;
    volatile LONG64 OwnershipSequence;
    volatile LONG64 PendingPhysical,ActivePhysical;
    volatile LONG64 TransactionPhysical;
    LONG64 TransactionSequence;
    ULONGLONG TransactionAddress,TransactionDeadline;
    ULONG TransactionPitch,TransactionFrame,TransactionState,TransactionSync,TransactionAuto,PpMask;
    BOOLEAN TransactionGated;
    volatile LONG Kick;
    volatile LONG WakeNeeded;
    ULONG ClockSource,NativeTeCounter,TxFrame;
    ULONGLONG LastNativeTe;
    ULONG PendingFrame;
    ULONGLONG LastEdge,Period,PollBegin;
    ULONG EdgeDelta,NotifyDivisor,NotifyCarry,NotifyDelta;
    volatile LONG64 TeEdges,TeObserved,TeSkipped,VsyncObserved,LatchedCount;
    GPU_DISPLAY_TE_SAMPLE TeSamples[GPU_DISPLAY_TE_SAMPLES],VsyncSamples[GPU_DISPLAY_TE_SAMPLES];
} A830_DPU;
static VOID KickDpu(A830_ADAPTER *a) {
    KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);A830_DPU *d=a->Dpu;
    if(d && d->Enabled && !d->Stopping && d->Timer && InterlockedExchange(&d->Kick,0))
        ExSetTimer(d->Timer,-2000,0,NULL);
    KeReleaseSpinLock(&a->QueueLock,old);
}
static VOID PublishSample(GPU_DISPLAY_TE_SAMPLE *ring,volatile LONG64 *head,ULONGLONG time,ULONG frame,ULONG delta) {
    LONG64 sequence=ReadAcquire64(head)+1;
    GPU_DISPLAY_TE_SAMPLE *sample=ring+((ULONGLONG)(sequence-1)%GPU_DISPLAY_TE_SAMPLES);
    InterlockedExchange64((volatile LONG64 *)&sample->Sequence,0);
    sample->Time100ns=time;sample->Frame=frame;sample->Delta=delta;KeMemoryBarrier();
    InterlockedExchange64((volatile LONG64 *)&sample->Sequence,sequence);
    InterlockedExchange64(head,sequence);
}
static VOID SnapshotSamples(GPU_DISPLAY_TE_SAMPLE *out,const GPU_DISPLAY_TE_SAMPLE *ring) {
    for(ULONG i=0;i<GPU_DISPLAY_TE_SAMPLES;i++) {
        LONG64 first=ReadAcquire64((volatile LONG64 *)&ring[i].Sequence);
        if(!first)continue;
        out[i].Time100ns=ring[i].Time100ns;out[i].Frame=ring[i].Frame;out[i].Delta=ring[i].Delta;
        KeMemoryBarrier();
        if(first==ReadAcquire64((volatile LONG64 *)&ring[i].Sequence))out[i].Sequence=(GPU_U64)first;
    }
}
static VOID BackendSnapshot(A830_ADAPTER *a,GPU_DISPLAY_ACTIVATION *out) {
    if(FrameCounterSource)out->BackendFlags|=FrameCounterSource==2?0x80u:0x20u;
    if(NotificationDivisor==2)out->BackendFlags|=0x40;
    KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);A830_DPU *d=a->Dpu;
    if(d) {
        out->BackendFlags=(d->Enabled && !d->Stopping)?3u:0;
        out->BackendFlags|=d->ClockSource==2?0x80u:d->CounterOffset==0xac?0x20u:0x10u;
        if(d->NotifyDivisor==2)out->BackendFlags|=0x40;
        if(d->Vsync)out->BackendFlags|=8;
        out->ActiveAddress=a->ScanoutAddress;out->PendingAddress=d->PendingAddress;
        out->Queued=(GPU_U64)InterlockedCompareExchange64(&d->Queued,0,0);
        out->Applied=(GPU_U64)InterlockedCompareExchange64(&d->Applied,0,0);
        out->TeLast100ns=d->LastEdge;out->TePeriod100ns=d->Period;
        out->TeEdges=(GPU_U64)ReadAcquire64(&d->TeEdges);
        out->TeObserved=(GPU_U64)ReadAcquire64(&d->TeObserved);
        out->TeSkipped=(GPU_U64)ReadAcquire64(&d->TeSkipped);
        out->VsyncObserved=(GPU_U64)ReadAcquire64(&d->VsyncObserved);
        out->LatchedCount=(GPU_U64)ReadAcquire64(&d->LatchedCount);
        SnapshotSamples(out->TeSamples,d->TeSamples);SnapshotSamples(out->VsyncSamples,d->VsyncSamples);
    }
    KeReleaseSpinLock(&a->QueueLock,old);
    A830GpuScanoutSnapshot(a,out);
}
static BOOLEAN Requested(PCWSTR valueName) {
    UNICODE_STRING path=RTL_CONSTANT_STRING(L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\a830_native");
    UNICODE_STRING name;RtlInitUnicodeString(&name,valueName);
    OBJECT_ATTRIBUTES attributes;HANDLE key;
    InitializeObjectAttributes(&attributes,&path,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,NULL,NULL);
    if(!NT_SUCCESS(ZwOpenKey(&key,KEY_QUERY_VALUE,&attributes)))return FALSE;
    UCHAR storage[sizeof(KEY_VALUE_PARTIAL_INFORMATION)+sizeof(ULONG)];ULONG bytes=0;
    NTSTATUS status=ZwQueryValueKey(key,&name,KeyValuePartialInformation,storage,sizeof(storage),&bytes);
    ZwClose(key);PKEY_VALUE_PARTIAL_INFORMATION value=(PKEY_VALUE_PARTIAL_INFORMATION)storage;
    return NT_SUCCESS(status) && value->Type==REG_DWORD && value->DataLength==sizeof(ULONG) && *(PULONG)value->Data==1;
}
/* Preserve the inherited rates, PLLs, power domain and interconnect votes.
 * Linux msm_mdss_enable requires clocks before the first MDSS read. Only
 * retain branch enables on a domain that firmware already powered; bringing
 * up an off domain would also require RPMh/ICC and is deliberately rejected.
 */
static NTSTATUS Clocks(A830_DPU *d) {
    PHYSICAL_ADDRESS p;p.QuadPart=0x100000;
    d->Gcc=MmMapIoSpaceEx(p,0x30000,PAGE_READWRITE|PAGE_NOCACHE);
    if(!d->Gcc)return STATUS_INSUFFICIENT_RESOURCES;
    ULONG axi=READ_REGISTER_ULONG((PULONG)(d->Gcc+0x27008));
    if(axi==MAXULONG || !(axi&1))return STATUS_NOT_SUPPORTED;
    ULONG ahb=READ_REGISTER_ULONG((PULONG)(d->Gcc+0x27004));
    if(ahb==MAXULONG)return STATUS_NOT_SUPPORTED;
    if(!(ahb&1)){WRITE_REGISTER_ULONG((PULONG)(d->Gcc+0x27004),ahb|1);d->ClockAdded|=1;}
    NTSTATUS status=(ahb&2)?STATUS_SUCCESS:GpuPoll(d->Gcc,0x27004,0x80000000u,0,2000);
    if(!NT_SUCCESS(status))return status;
    p.QuadPart=0xaf00000;
    d->Dispcc=MmMapIoSpaceEx(p,0x20000,PAGE_READWRITE|PAGE_NOCACHE);
    if(!d->Dispcc)return STATUS_INSUFFICIENT_RESOURCES;
    ULONG domain=READ_REGISTER_ULONG((PULONG)(d->Dispcc+0x9004));
    if(domain==MAXULONG || !(domain&(1u<<16)))return STATUS_NOT_SUPPORTED;
    static const ULONG branches[]={0x80b0,0x8010,0x8030};
    for(ULONG i=0;i<RTL_NUMBER_OF(branches);i++) {
        ULONG value=READ_REGISTER_ULONG((PULONG)(d->Dispcc+branches[i]));
        if(value==MAXULONG)return STATUS_NOT_SUPPORTED;
        if(!(value&1)) {
            WRITE_REGISTER_ULONG((PULONG)(d->Dispcc+branches[i]),value|1);
            d->ClockAdded|=2u<<i;
        }
        status=GpuPoll(d->Dispcc,branches[i],0x80000000u,0,2000);
        if(!NT_SUCCESS(status))return status;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS ReleaseClocks(A830_DPU *d) {
    static const ULONG branches[]={0x80b0,0x8010,0x8030};
    if(d->Dispcc) {
        for(LONG i=RTL_NUMBER_OF(branches)-1;i>=0;i--)if(d->ClockAdded&(2u<<i)) {
            PULONG reg=(PULONG)(d->Dispcc+branches[i]);
            ULONG value=READ_REGISTER_ULONG(reg);if(value==MAXULONG)return STATUS_IO_DEVICE_ERROR;
            WRITE_REGISTER_ULONG(reg,value&~1u);KeMemoryBarrier();value=READ_REGISTER_ULONG(reg);
            if(value==MAXULONG || (value&1))return STATUS_IO_DEVICE_ERROR;
            d->ClockAdded&=~(2u<<i);
            if(ActivationAdapter==d->Adapter) {
                Activation.DispBranches[i]=value;Activation.ProofMask&=~(4u<<i);
            }
        }
        MmUnmapIoSpace(d->Dispcc,0x20000);d->Dispcc=NULL;
    }
    if(d->Gcc) {
        if(d->ClockAdded&1) {
            PULONG reg=(PULONG)(d->Gcc+0x27004);
            ULONG value=READ_REGISTER_ULONG(reg);if(value==MAXULONG)return STATUS_IO_DEVICE_ERROR;
            WRITE_REGISTER_ULONG(reg,value&~1u);KeMemoryBarrier();value=READ_REGISTER_ULONG(reg);
            if(value==MAXULONG || (value&1))return STATUS_IO_DEVICE_ERROR;
            d->ClockAdded&=~1u;
        }
        MmUnmapIoSpace(d->Gcc,0x30000);d->Gcc=NULL;
    }
    return STATUS_SUCCESS;
}
static ULONG Read(A830_DPU *d,ULONG offset) {
    if(InterlockedCompareExchange(&ActivationAction,0,0))Activation.LastPhysical=0xae01000ULL+offset;
    return READ_REGISTER_ULONG((PULONG)(d->Registers+offset));
}
static VOID Write(A830_DPU *d,ULONG offset,ULONG value) {
    WRITE_REGISTER_ULONG((PULONG)(d->Registers+offset),value);
}
static ULONG Frame(A830_DPU *d) {
    ULONG value=Read(d,d->Intf+d->CounterOffset);
    return d->CounterOffset==0xac?value:value>>16;
}
static ULONG TxFrame(A830_DPU *d) {return Read(d,d->Intf+0xac);}
static VOID Flush(A830_DPU *d) {
    KeMemoryBarrier();
    for(ULONG i=0;i<d->CtlCount;i++)Write(d,d->Ctls[i].Base+0x18,d->Ctls[i].Mask);
    /* Command mode without inherited autorefresh requires a CTL kickoff. */
    if(!d->AutoRefresh)for(ULONG i=0;i<d->CtlCount;i++)Write(d,d->Ctls[i].Base+0x1c,1);
}
static BOOLEAN FlushComplete(A830_DPU *d) {
    for(ULONG i=0;i<d->CtlCount;i++)if(Read(d,d->Ctls[i].Base+0x18)&d->Ctls[i].Mask)return FALSE;
    return TRUE;
}
/* Read-only timing diagnosis. A missed diagnostic sample is counted instead
   of ever spinning at DIRQL on a DPC that this interrupt may have preempted. */
static VOID TraceFlip(A830_DPU *d,ULONG kind,ULONGLONG sequence,ULONGLONG physical,
    ULONGLONG address,ULONGLONG oldPhysical,ULONG queueFrame) {
    if(InterlockedCompareExchange(&TraceWriter,1,0)) {
        InterlockedIncrement64((volatile LONG64 *)&DpuTrace.Dropped);return;
    }
    ULONGLONG serial=(ULONGLONG)ReadAcquire64((volatile LONG64 *)&DpuTrace.LatestSerial)+1;
    GPU_DPU_TRACE_ENTRY *e=DpuTrace.Entries+(serial-1)%GPU_DPU_TRACE_ENTRIES;
    InterlockedExchange64((volatile LONG64 *)&e->Serial,0);
    e->Time100ns=PreciseTime();e->FlipSequence=sequence;e->Physical=physical;e->Address=address;e->OldPhysical=oldPhysical;
    e->Kind=kind;e->Frame=TxFrame(d);e->WrLine=Read(d,d->Intf+0x2b0)&0xffffu;
    e->TearStatus=Read(d,d->Intf+0x804);e->MdpStatus=Read(d,0x14);e->Flush=0;
    for(ULONG i=0;i<d->CtlCount;i++)e->Flush|=Read(d,d->Ctls[i].Base+0x18);
    e->QueueFrame=queueFrame;e->Reserved=0;KeMemoryBarrier();
    InterlockedExchange64((volatile LONG64 *)&e->Serial,(LONG64)serial);
    InterlockedExchange64((volatile LONG64 *)&DpuTrace.LatestSerial,(LONG64)serial);
    InterlockedExchange(&TraceWriter,0);
}
VOID A830DpuTraceSnapshot(PVOID output) {
    GPU_DPU_TRACE *out=output;RtlZeroMemory(out,sizeof(*out));out->Version=1;out->Size=sizeof(*out);
    out->LatestSerial=(GPU_U64)ReadAcquire64((volatile LONG64 *)&DpuTrace.LatestSerial);
    out->Dropped=(GPU_U64)ReadAcquire64((volatile LONG64 *)&DpuTrace.Dropped);
    for(ULONG i=0;i<GPU_DPU_TRACE_ENTRIES;i++) {
        LONG64 first=ReadAcquire64((volatile LONG64 *)&DpuTrace.Entries[i].Serial);if(!first)continue;
        RtlCopyMemory(out->Entries+i,DpuTrace.Entries+i,sizeof(out->Entries[i]));KeMemoryBarrier();
        if(first!=ReadAcquire64((volatile LONG64 *)&DpuTrace.Entries[i].Serial))out->Entries[i].Serial=0;
    }
}
static VOID RestoreTransaction(A830_DPU *d) {
    if(!d->TransactionGated)return;
    /* Restore only the exact configuration held by this transaction. */
    Write(d,d->Intf+0x2b4,d->TransactionAuto);
    Write(d,d->Intf+0x288,d->TransactionSync);KeMemoryBarrier();
    d->TransactionGated=FALSE;
}
static VOID CompleteTransaction(A830_DPU *d) {
    ULONGLONG old=(ULONGLONG)ReadAcquire64(&d->ActivePhysical);
    InterlockedIncrement64(&d->OwnershipSequence);InterlockedIncrement64(&d->ActiveSequence);
    d->ActiveAddress=d->TransactionAddress;d->ActivePitch=d->TransactionPitch;d->ActiveFrame=TxFrame(d);
    InterlockedExchange64((volatile LONG64 *)&d->Adapter->ScanoutAddress,(LONG64)d->TransactionAddress);
    InterlockedExchange64(&d->ActivePhysical,ReadAcquire64(&d->TransactionPhysical));
    InterlockedExchange64(&d->Applied,d->TransactionSequence);
    InterlockedIncrement64(&d->ActiveSequence);
    TraceFlip(d,3,(ULONGLONG)d->TransactionSequence,(ULONGLONG)ReadAcquire64(&d->TransactionPhysical),
        d->TransactionAddress,old,d->TransactionFrame);
    InterlockedExchange64(&d->TransactionPhysical,0);d->TransactionState=0;
    InterlockedIncrement64(&d->OwnershipSequence);
    InterlockedIncrement((volatile LONG *)&d->Adapter->PresentCount);InterlockedIncrement64(&d->LatchedCount);
    InterlockedExchange(&d->WakeNeeded,1);
}
/* Short display-interrupt-lock sections only. The timer provides all waits;
 * an Address callback never waits or writes a live SSPP source register. */
static BOOLEAN AdvanceTransaction(PVOID context) {
    A830_DPU *d=context;
    if(d->Stopping || !d->Enabled || d->TransactionState==4)return TRUE;
    ULONGLONG now=PreciseTime();
    if(d->TransactionState && now>=d->TransactionDeadline) {
        RestoreTransaction(d);d->TransactionState=4;
        Activation.LastStatus=(ULONG)STATUS_IO_TIMEOUT;Activation.DpuFailStage=11;
        InterlockedExchange(&d->Adapter->Failed,STATUS_IO_TIMEOUT);InterlockedExchange(&d->WakeNeeded,1);
        TraceFlip(d,9,(ULONGLONG)d->TransactionSequence,(ULONGLONG)ReadAcquire64(&d->TransactionPhysical),
            d->TransactionAddress,(ULONGLONG)ReadAcquire64(&d->ActivePhysical),d->TransactionFrame);
        return TRUE;
    }
    if(!d->TransactionState) {
        LONG64 sequence=ReadAcquire64(&d->Queued);
        if((sequence&1) || sequence<=ReadAcquire64(&d->Applied))return TRUE;
        InterlockedIncrement64(&d->OwnershipSequence);
        d->TransactionSequence=sequence;d->TransactionAddress=d->PendingAddress;
        d->TransactionPitch=d->PendingPitch;d->TransactionFrame=d->PendingFrame;
        InterlockedExchange64(&d->TransactionPhysical,ReadAcquire64(&d->PendingPhysical));
        InterlockedIncrement64(&d->OwnershipSequence);
        d->TransactionSync=Read(d,d->Intf+0x288);d->TransactionAuto=Read(d,d->Intf+0x2b4);
        d->TransactionDeadline=now+500000;d->TransactionGated=TRUE;
        /* Qualcomm prepare_commit: prevent a subsequent automatic kickoff,
           then allow an already running pixel transfer to finish. */
        Write(d,d->Intf+0x288,d->TransactionSync&~(1u<<20));
        Write(d,d->Intf+0x2b4,d->TransactionAuto&~(1u<<31));KeMemoryBarrier();
        if((Read(d,d->Intf+0x288)&(1u<<20)) || (Read(d,d->Intf+0x2b4)&(1u<<31))) {
            RestoreTransaction(d);d->TransactionState=4;
            Activation.LastStatus=(ULONG)STATUS_IO_DEVICE_ERROR;Activation.DpuFailStage=13;
            InterlockedExchange(&d->Adapter->Failed,STATUS_IO_DEVICE_ERROR);
            InterlockedExchange(&d->WakeNeeded,1);return TRUE;
        }
        d->TransactionState=1;
        TraceFlip(d,4,(ULONGLONG)sequence,(ULONGLONG)ReadAcquire64(&d->TransactionPhysical),
            d->TransactionAddress,(ULONGLONG)ReadAcquire64(&d->ActivePhysical),d->TransactionFrame);
    }
    ULONG line=Read(d,d->Intf+0x2b0)&0xffffu;
    BOOLEAN active=line>0 && line<d->Adapter->DisplayInfo.Height;
    if(d->TransactionState==1) {
        if(active)return TRUE;
        /* Both events are cleared only after old PPTX has drained. No old
           latched 4f/100 may be mistaken for completion of this commit. */
        Write(d,d->Intf+0x808,1);Write(d,0x18,d->PpMask);KeMemoryBarrier();
        if((Read(d,d->Intf+0x804)&1) || (Read(d,0x14)&d->PpMask)) {
            RestoreTransaction(d);d->TransactionState=4;
            Activation.LastStatus=(ULONG)STATUS_IO_DEVICE_ERROR;Activation.DpuFailStage=12;
            InterlockedExchange(&d->Adapter->Failed,STATUS_IO_DEVICE_ERROR);return TRUE;
        }
        ULONGLONG physical=(ULONGLONG)ReadAcquire64(&d->TransactionPhysical);
        TraceFlip(d,1,(ULONGLONG)d->TransactionSequence,physical,d->TransactionAddress,
            (ULONGLONG)ReadAcquire64(&d->ActivePhysical),d->TransactionFrame);
        for(ULONG i=0;i<d->PipeCount;i++) {
            DPU_PIPE *p=d->Pipes+i;
            ULONG row=p->Offset/d->Adapter->DisplayInfo.Pitch,column=p->Offset%d->Adapter->DisplayInfo.Pitch;
            Write(d,p->Base+0x14,(ULONG)physical+row*d->TransactionPitch+column);
            Write(d,p->Base+0x24,(p->OriginalStride&0xffff0000u)|d->TransactionPitch);
        }
        /* Do not issue CTL_START: the exact inherited autorefresh and TE
           configuration below supplies the kickoff after this atomic flush. */
        KeMemoryBarrier();
        for(ULONG i=0;i<d->CtlCount;i++)Write(d,d->Ctls[i].Base+0x18,d->Ctls[i].Mask);
        d->TransactionFrame=TxFrame(d);RestoreTransaction(d);d->TransactionState=2;
        TraceFlip(d,2,(ULONGLONG)d->TransactionSequence,physical,d->TransactionAddress,
            (ULONGLONG)ReadAcquire64(&d->ActivePhysical),d->TransactionFrame);
        return TRUE;
    }
    ULONG event=Read(d,d->Intf+0x804),done=Read(d,0x14)&d->PpMask;
    if(d->TransactionState==2 && (event&1) && active && FlushComplete(d)) {
        /* Fresh AUTOREFRESH_DONE proves the double-buffer kickoff; observing
           active WRPTR associates the following PPdone with this transfer. */
        Write(d,0x18,d->PpMask);KeMemoryBarrier();d->TransactionState=3;
        TraceFlip(d,6,(ULONGLONG)d->TransactionSequence,(ULONGLONG)ReadAcquire64(&d->TransactionPhysical),
            d->TransactionAddress,(ULONGLONG)ReadAcquire64(&d->ActivePhysical),d->TransactionFrame);
    } else if(d->TransactionState==3 && !active && done && TxFrame(d)!=d->TransactionFrame && FlushComplete(d)) {
        CompleteTransaction(d);
    }
    return TRUE;
}
static BOOLEAN Notify(PVOID context) {
    A830_DPU *d=context;A830_ADAPTER *a=d->Adapter;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA n={0};
    n.InterruptType=DXGK_INTERRUPT_CRTC_VSYNC;n.Flags.ValidPhysicalAdapterMask=1;
    n.CrtcVsync.PhysicalAdapterMask=1;n.CrtcVsync.PhysicalAddress.QuadPart=(LONGLONG)a->ScanoutAddress;
    ULONGLONG now=PreciseTime();
    ULONGLONG previous=(ULONGLONG)InterlockedExchange64((volatile LONG64 *)&a->Telemetry.LastVsyncTime,now);
    if(previous && now>previous)A830Sample(a->Telemetry.VsyncInterval,now-previous);
    a->Dxgk.DxgkCbNotifyInterrupt(a->Dxgk.DeviceHandle,&n);
    PublishSample(d->VsyncSamples,&d->VsyncObserved,now,d->Frame,d->NotifyDelta);
    InterlockedExchange(&a->SchedulerDpcPending,1);
    return a->Dxgk.DxgkCbQueueDpc(a->Dxgk.DeviceHandle);
}
static VOID Poll(PEX_TIMER timer,PVOID context) {
    A830_DPU *d=context;UNREFERENCED_PARAMETER(timer);
    if(d->Stopping || !d->Enabled)return;
    BOOLEAN advanced=FALSE;
    NTSTATUS transaction=d->Adapter->Dxgk.DxgkCbSynchronizeExecution(d->Adapter->Dxgk.DeviceHandle,AdvanceTransaction,d,0,&advanced);
    if(!NT_SUCCESS(transaction))InterlockedExchange(&d->Adapter->Failed,transaction);
    if(InterlockedExchange(&d->WakeNeeded,0))KeSetEvent(&d->Adapter->Wake,IO_NO_INCREMENT,FALSE);
    ULONGLONG now=PreciseTime();d->TxFrame=TxFrame(d);ULONG frame=d->ClockSource?d->TxFrame:Frame(d);
    if(d->ClockSource==2) {
        if(Read(d,d->Intf+0x804)&8) {
            Write(d,d->Intf+0x808,8);d->NativeTeCounter++;d->LastNativeTe=now;
        }
        frame=d->NativeTeCounter;
    }
    if(frame!=d->Frame) {
        ULONG delta=(frame-d->Frame)&0xffffu;
        d->EdgeDelta=delta<0x8000?delta:0;
        if(d->EdgeDelta) {
            InterlockedAdd64(&d->TeEdges,d->EdgeDelta);
            InterlockedAdd64(&d->TeSkipped,d->EdgeDelta-1);
        }
        PublishSample(d->TeSamples,&d->TeObserved,now,frame,d->EdgeDelta);
        if(d->LastEdge && delta && delta<8) {
            ULONGLONG period=(now-d->LastEdge)/delta;
            if(period>=60000 && period<=400000)d->Period=(d->Period*3+period)/4;
        }
        d->LastEdge=now;d->Frame=frame;
        if(!d->Vsync)d->NotifyCarry=0;
        else if(d->EdgeDelta)d->NotifyCarry+=d->EdgeDelta;
        if(d->Vsync && d->NotifyCarry>=d->NotifyDivisor) {
            d->NotifyDelta=d->NotifyCarry;d->NotifyCarry=0;
            BOOLEAN result=FALSE;
            NTSTATUS status=d->Adapter->Dxgk.DxgkCbSynchronizeExecution(
                d->Adapter->Dxgk.DeviceHandle,Notify,d,0,&result);
            if(!NT_SUCCESS(status))InterlockedExchange(&d->Adapter->Failed,status);
        }
    }
    /* Sleep until shortly before the next hardware edge. Only the narrow
       edge window is polled at 200 us; no fabricated VSync is ever emitted. */
    now=PreciseTime();ULONGLONG wait=2000;
    if(d->LastEdge && now<d->LastEdge+d->Period && d->LastEdge+d->Period-now>10000)
        wait=d->LastEdge+d->Period-now-10000;
    if((!d->Vsync && d->Applied==InterlockedCompareExchange64(&d->Queued,0,0)) ||
       (d->LastEdge && now-d->LastEdge>d->Period*2) ||
       (!d->LastEdge && now-d->PollBegin>d->Period*2))wait=100000;
    if(d->TransactionState || ReadAcquire64(&d->Queued)>ReadAcquire64(&d->Applied))wait=2000;
    if(d->TransactionState==4)wait=100000;
    if(!d->Stopping)ExSetTimer(d->Timer,-(LONGLONG)wait,0,NULL);
}
static NTSTATUS Translation(A830_DPU *d) {
    if(!TranslationProved(d->Adapter))return STATUS_NOT_SUPPORTED;
    /* Revalidate only the explicitly probed MDSS entries. A mask of 2 is
       the Linux DT's two MDSS stream IDs (0x800/0x802), not another device.
       No S2CR or context-bank register is ever modified here. */
    for(ULONG i=0;i<2;i++) {
        ULONG slot=Activation.Slots[i];if(slot>=128)return STATUS_NOT_SUPPORTED;
        ULONG smr=READ_REGISTER_ULONG((PULONG)(d->Smmu+0x800+slot*4));
        ULONG value=READ_REGISTER_ULONG((PULONG)(d->Smmu+0xc00+slot*4));
        if(smr!=Activation.Smr[i] || value!=Activation.S2cr[i])return STATUS_DEVICE_CONFIGURATION_ERROR;
        if(((value>>16)&3u)==0) {
            ULONG bank=value&255u,shift=(Activation.SmmuId1&0x80000000u)?16u:12u;
            ULONG pages=1u<<(((Activation.SmmuId1>>28)&7u)+1),sctlr=0,cbar=0;
            NTSTATUS status=ActivationRead(0x15000000ULL+((ULONGLONG)(pages+bank)<<shift),&sctlr);
            if(NT_SUCCESS(status))status=ActivationRead(0x15000000ULL+(1ULL<<shift)+bank*4,&cbar);
            if(!NT_SUCCESS(status))return status;
            if(sctlr!=Activation.Sctlr[i] || cbar!=Activation.Cbar[i])return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
    d->SmmuEntry=Activation.Slots[0];d->SmmuOriginal=Activation.S2cr[0];return STATUS_SUCCESS;
}
static NTSTATUS InitializeStep(A830_ADAPTER *a,ULONG phase,ULONG step,ULONG index) {
    GPU_DISPLAY_ACTIVATION_REQUEST request={1,phase,step,index,0,0};
    GPU_DISPLAY_ACTIVATION report;
    NTSTATUS status=A830DisplayActivation(a,&request,&report);
    return NT_SUCCESS(status)?(NTSTATUS)report.LastStatus:status;
}
NTSTATUS A830DpuInitialize(PVOID context) {
    A830_ADAPTER *a=context;
    if(a->DisplayOnly || a->RenderOnly || !Requested(L"EnableDpu"))return STATUS_NOT_SUPPORTED;
    if(A830DpuEnabled(a))return STATUS_SUCCESS;
    ActivationAdapter=NULL;
    InterlockedExchange(&ActivationAction,0);
    static const ULONG before[][3]={{5,2,0},{1,0,0},{2,0,0},{2,1,0},{2,1,1},{2,1,2}};
    static const ULONG after[][3]={
        {2,2,0},{3,0,0},{3,0,1},{3,1,0},{3,2,0},{3,3,0},{3,2,1},{3,3,1},
        {4,2,1},{4,3,1},{4,4,1},{5,0,2},{5,1,1}};
    NTSTATUS status=STATUS_SUCCESS;
    for(ULONG i=0;i<RTL_NUMBER_OF(before) && NT_SUCCESS(status);i++)
        status=InitializeStep(a,before[i][0],before[i][1],before[i][2]);
    if(NT_SUCCESS(status) && !(Activation.DispBranches[0]&1))status=InitializeStep(a,2,3,0);
    for(ULONG i=0;i<RTL_NUMBER_OF(after) && NT_SUCCESS(status);i++)
        status=InitializeStep(a,after[i][0],after[i][1],after[i][2]);
    if(NT_SUCCESS(status))status=A830GpuScanoutReserveStaging(a);
    if(NT_SUCCESS(status))status=A830DpuStart(a);
    if(!NT_SUCCESS(status)) {
        NTSTATUS restored=RestoreProbeBranches(a);
        if(!NT_SUCCESS(restored))status=restored;
    }
    Activation.Phase=6;Activation.Step=0;Activation.Index=0;Activation.LastStatus=(ULONG)status;
    return status;
}
NTSTATUS A830DpuStart(PVOID context) {
    A830_ADAPTER *a=context;
    Activation.DpuFailStage=1;
    if(a->DisplayOnly || a->RenderOnly || a->Dpu)return STATUS_NOT_SUPPORTED;
    if(!TranslationProved(a))return STATUS_DEVICE_NOT_READY;
    if(!A830GpuScanoutStagingReady(a))return STATUS_NOT_SUPPORTED;
    if(!Requested(L"EnableDpu"))return STATUS_NOT_SUPPORTED;
    A830_DPU *d=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*d),GPU_TAG);
    if(!d)return STATUS_INSUFFICIENT_RESOURCES;
    d->Adapter=a;d->Period=166667;a->Dpu=d;
    Activation.DpuFailStage=3;NTSTATUS status=Clocks(d);if(!NT_SUCCESS(status))goto failed;
    /* Access to global apps_smmu requires its own platform verification.
       MDSS clocks do not establish TCU register accessibility. */
    if(!TranslationProved(a)){status=STATUS_NOT_SUPPORTED;goto failed;}
    PHYSICAL_ADDRESS p;p.QuadPart=0xae01000;
    d->Registers=MmMapIoSpaceEx(p,DPU_REGISTER_BYTES,PAGE_READWRITE|PAGE_NOCACHE);
    p.QuadPart=0x15000000;d->Smmu=MmMapIoSpaceEx(p,0x1000,PAGE_READWRITE|PAGE_NOCACHE);
    if(!d->Registers || !d->Smmu){status=STATUS_INSUFFICIENT_RESOURCES;goto failed;}
    Activation.DpuFailStage=4;status=Translation(d);if(!NT_SUCCESS(status))goto failed;
    ULONGLONG firmware=(ULONGLONG)a->DisplayInfo.PhysicAddress.QuadPart;
    Activation.DpuFailStage=5;if(firmware>MAXULONG){status=STATUS_NOT_SUPPORTED;goto failed;}
    Activation.DpuFailStage=6;ULONG active=0,flush=0;
    for(ULONG i=0;i<RTL_NUMBER_OF(PipeDescriptions);i++) {
        ULONG base=PipeDescriptions[i].Base,address=Read(d,base+0x14),stride=Read(d,base+0x24);
        if((ULONGLONG)address<firmware || (ULONGLONG)address>=firmware+a->FramebufferSize ||
           (stride&0xffffu)!=a->DisplayInfo.Pitch)continue;
        if((Read(d,base+0x30)&0xc0000000u) || Read(d,base+0x170)){status=STATUS_NOT_SUPPORTED;goto failed;}
        DPU_PIPE *pipe=d->Pipes+d->PipeCount++;
        pipe->Base=base;pipe->OriginalAddress=address;pipe->OriginalStride=stride;
        pipe->Offset=(ULONG)((ULONGLONG)address-firmware);
        active|=PipeDescriptions[i].Active;flush|=PipeDescriptions[i].Flush;
    }
    if(!d->PipeCount){status=STATUS_NOT_SUPPORTED;goto failed;}
    Activation.DpuFailStage=7;ULONG intfs=0;
    for(ULONG i=0;i<6;i++) {
        ULONG base=0x15000+i*0x1000,pipes=Read(d,base+0x12c);
        if(!(pipes&active))continue;
        if(pipes&~active){status=STATUS_NOT_SUPPORTED;goto failed;}
        d->Ctls[d->CtlCount].Base=base;d->Ctls[d->CtlCount++].Mask=flush;
        intfs|=Read(d,base+0xf4);
    }
    if(!d->CtlCount || !intfs){status=STATUS_NOT_SUPPORTED;goto failed;}
    ULONG mixers=0;for(ULONG i=0;i<d->CtlCount;i++)mixers|=Read(d,d->Ctls[i].Base+0x130);
    /* This inherited split/merged DSI0 topology uses master PP0. PP1 does
       not generate done (confirmed 0x100 status), even with two VIG pipes. */
    if(!(mixers&1) || (mixers&~3u)){status=STATUS_NOT_SUPPORTED;goto failed;}
    d->PpMask=1u<<8;
    Activation.DpuFailStage=8;for(ULONG i=0;i<4;i++)if(intfs&(1u<<i)) {
        ULONG base=0x34000+i*0x1000;
        if(!(Read(d,base+0x284)&1) || (Read(d,base+0x288)&0x180000u)!=0x180000u){status=STATUS_NOT_SUPPORTED;goto failed;}
        if(!d->Intf)d->Intf=base;
        d->AutoRefresh|=Read(d,base+0x2b4)&0x80000000u;
    }
    d->ClockSource=FrameCounterSource;d->CounterOffset=FrameCounterSource?0xacu:0x298u;d->NotifyDivisor=NotificationDivisor;
    if(FrameCounterSource && (Read(d,d->Intf+0x2b4)!=0x80000001u ||
        (ProbedIntfValid&(1u<<((d->Intf-0x34000)/0x1000)))==0)) {
        status=STATUS_DEVICE_NOT_READY;goto failed;
    }
    /* Verify that the selected hardware counter is alive before replacing
       the old software-vblank producer. A silent rdptr counter must never
       disable the desktop's only progress source. */
    Activation.DpuFailStage=10;ULONG initial=Frame(d);
    ULONGLONG deadline=PreciseTime()+500000;
    LARGE_INTEGER pause;pause.QuadPart=-10000;
    do {
        KeDelayExecutionThread(KernelMode,FALSE,&pause);d->Frame=Frame(d);
    } while(d->Frame==initial && PreciseTime()<deadline);
    if(d->Frame==initial){status=STATUS_IO_TIMEOUT;goto failed;}
    if(d->ClockSource==2) {
        /* Prove a freshly acknowledged input TE event can recur. Do not use
           the inherited permanently-latched bit as a clock sample. */
        Write(d,d->Intf+0x808,8);ULONG edges=0;
        deadline=PreciseTime()+500000;
        do {
            KeDelayExecutionThread(KernelMode,FALSE,&pause);
            if(Read(d,d->Intf+0x804)&8){Write(d,d->Intf+0x808,8);edges++;}
        }while(edges<2 && PreciseTime()<deadline);
        if(edges<2){status=STATUS_IO_TIMEOUT;goto failed;}
        d->Frame=0;d->NativeTeCounter=0;d->LastNativeTe=PreciseTime();
    }
    d->TxFrame=TxFrame(d);
    Activation.DpuFailStage=9;d->Timer=ExAllocateTimer(Poll,d,EX_TIMER_HIGH_RESOLUTION);
    if(!d->Timer){status=STATUS_INSUFFICIENT_RESOURCES;goto failed;}
    d->Vsync=InterlockedCompareExchange(&a->VsyncEnabled,0,0);d->PollBegin=PreciseTime();
    /* Transfer the explicit probe's added branch vote to this backend.
       Backend failure/stop then releases the same enable exactly once. */
    d->ClockAdded|=Activation.ProbeBranchAdded<<1;Activation.ProbeBranchAdded=0;
    InterlockedExchange(&d->Enabled,1);ExSetTimer(d->Timer,-10000,0,NULL);Activation.DpuFailStage=0;
    return STATUS_SUCCESS;
failed:A830DpuStop(a);return status;
}
BOOLEAN A830DpuEnabled(PVOID context) {
    A830_ADAPTER *a=context;
    if(KeGetCurrentIrql()>DISPATCH_LEVEL) {
        A830_DPU *d=a->Dpu;return d && d->Enabled && !d->Stopping;
    }
    KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);A830_DPU *d=a->Dpu;
    BOOLEAN enabled=d && d->Enabled && !d->Stopping;
    KeReleaseSpinLock(&a->QueueLock,old);return enabled;
}
BOOLEAN A830DpuReferences(PVOID context,ULONGLONG physical) {
    A830_ADAPTER *a=context;KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);
    A830_DPU *d=a->Dpu;
    BOOLEAN referenced=FALSE;
    if(d) {
        LONG64 before=ReadAcquire64(&d->OwnershipSequence);
        referenced=(before&1) || (ULONGLONG)ReadAcquire64(&d->PendingPhysical)==physical ||
            (ULONGLONG)ReadAcquire64(&d->ActivePhysical)==physical || (ULONGLONG)ReadAcquire64(&d->TransactionPhysical)==physical;
        KeMemoryBarrier();if(before!=ReadAcquire64(&d->OwnershipSequence))referenced=TRUE;
    }
    KeReleaseSpinLock(&a->QueueLock,old);return referenced;
}
BOOLEAN A830DpuActive(PVOID context,PULONGLONG physical,PULONG pitch,PULONGLONG address,PULONG frame) {
    if(!physical || !pitch || !address || !frame || KeGetCurrentIrql()>DISPATCH_LEVEL)return FALSE;
    A830_ADAPTER *a=context;BOOLEAN valid=FALSE;KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);
    A830_DPU *d=a->Dpu;
    if(d && d->Enabled && !d->Stopping)for(ULONG attempt=0;attempt<3;attempt++) {
        LONG64 first=ReadAcquire64(&d->ActiveSequence);if(first&1)continue;
        ULONGLONG p=(ULONGLONG)ReadAcquire64(&d->ActivePhysical),w=d->ActiveAddress;
        ULONG stride=d->ActivePitch,f=d->ActiveFrame;KeMemoryBarrier();
        if(first!=ReadAcquire64(&d->ActiveSequence))continue;
        if(p && stride){*physical=p;*pitch=stride;*address=w;*frame=f;valid=TRUE;}
        break;
    }
    KeReleaseSpinLock(&a->QueueLock,old);return valid;
}
NTSTATUS A830DpuQueue(PVOID context,ULONGLONG physical,ULONG pitch,ULONGLONG address) {
    A830_DPU *d=((A830_ADAPTER *)context)->Dpu;
    if(!d || !d->Enabled || d->Stopping || d->TransactionState==4)return STATUS_DEVICE_NOT_READY;
    if(!physical || physical>MAXULONG || pitch<d->Adapter->DisplayInfo.Width*4 || pitch>0xffffu ||
       (ULONGLONG)pitch*d->Adapter->DisplayInfo.Height>MAXULONG-physical)return STATUS_INVALID_PARAMETER;
    /* The display callback is serialized by dxgkrnl. Publish odd before any
       register or metadata write, so the timer never acknowledges a mixture
       of the preceding sequence and the new frame's address. */
    InterlockedIncrement64(&d->OwnershipSequence);InterlockedIncrement64(&d->Queued);
    d->PendingAddress=address;d->PendingPitch=pitch;InterlockedExchange64(&d->PendingPhysical,(LONG64)physical);
    d->PendingFrame=d->TxFrame;KeMemoryBarrier();
    InterlockedIncrement64(&d->Queued);InterlockedIncrement64(&d->OwnershipSequence);
    InterlockedExchange(&d->Kick,1);A830WakeDisplay(d->Adapter);return STATUS_SUCCESS;
}
VOID A830DpuEnableVsync(PVOID context,BOOLEAN enabled) {
    A830_ADAPTER *a=context;
    if(KeGetCurrentIrql()>DISPATCH_LEVEL) {
        /* Display DIRQL callers already hold dxgkrnl's interrupt lock. */
        A830_DPU *d=a->Dpu;if(d && !d->Stopping)InterlockedExchange(&d->Vsync,enabled);
        return;
    }
    KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);A830_DPU *d=a->Dpu;
    if(d && !d->Stopping) {
        InterlockedExchange(&d->Vsync,enabled);
        if(enabled && d->Timer)ExSetTimer(d->Timer,-10000,0,NULL);
    }
    KeReleaseSpinLock(&a->QueueLock,old);
}
VOID A830DpuRestoreRegisters(PVOID context) {
    A830_DPU *d=((A830_ADAPTER *)context)->Dpu;
    if(!d || !d->Enabled || d->Restored)return;
    RestoreTransaction(d);
    for(ULONG i=0;i<d->PipeCount;i++) {
        Write(d,d->Pipes[i].Base+0x14,d->Pipes[i].OriginalAddress);
        Write(d,d->Pipes[i].Base+0x24,d->Pipes[i].OriginalStride);
    }
    Flush(d);
}
typedef struct { A830_ADAPTER *Adapter;A830_DPU *Dpu;BOOLEAN Retire; } DPU_STOP_ACCESS;
static BOOLEAN StopAccess(PVOID context) {
    DPU_STOP_ACCESS *access=context;
    if(access->Adapter->Dpu!=access->Dpu)return FALSE;
    if(access->Retire)access->Adapter->Dpu=NULL;
    else InterlockedExchange(&access->Dpu->Stopping,1);
    return TRUE;
}
static NTSTATUS SynchronizeStop(A830_ADAPTER *a,A830_DPU *d,BOOLEAN retire) {
    DPU_STOP_ACCESS access={a,d,retire};BOOLEAN result=FALSE;
    /* Same order as scheduler IssueJob: QueueLock -> display interrupt lock.
       The DIRQL callback performs no wait, event signal or lock acquisition. */
    KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);
    NTSTATUS status=a->Dxgk.DxgkCbSynchronizeExecution(a->Dxgk.DeviceHandle,StopAccess,&access,0,&result);
    KeReleaseSpinLock(&a->QueueLock,old);
    return NT_SUCCESS(status)?(result?STATUS_SUCCESS:STATUS_DEVICE_BUSY):status;
}
NTSTATUS A830DpuStop(PVOID context) {
    A830_ADAPTER *a=context;A830_DPU *d=a->Dpu;if(!d)return RestoreProbeBranches(a);
    NTSTATUS stopped=SynchronizeStop(a,d,FALSE);if(!NT_SUCCESS(stopped))return stopped;
    if(d->Timer){ExDeleteTimer(d->Timer,TRUE,TRUE,NULL);d->Timer=NULL;}
    RestoreTransaction(d);
    if(d->Enabled && !d->Restored) {
        A830DpuRestoreRegisters(a);
        /* Returning POST ownership must not free the active BO before the
           firmware address has actually latched. Stop is passive and bounded. */
        ULONG frame=TxFrame(d);ULONGLONG end=KeQueryInterruptTime()+1000000;
        LARGE_INTEGER delay;delay.QuadPart=-10000;
        while(KeQueryInterruptTime()<end && (TxFrame(d)==frame || !FlushComplete(d)))
            KeDelayExecutionThread(KernelMode,FALSE,&delay);
        /* Keep mappings and both references alive if hardware never latches.
           The caller must preserve the pool/segment and fail device stop. */
        if(TxFrame(d)==frame || !FlushComplete(d))return STATUS_IO_TIMEOUT;
        d->Restored=TRUE;
    }
    NTSTATUS released=ReleaseClocks(d);if(!NT_SUCCESS(released))return released;
    stopped=SynchronizeStop(a,d,TRUE);if(!NT_SUCCESS(stopped))return stopped;
    if(d->Registers)MmUnmapIoSpace(d->Registers,DPU_REGISTER_BYTES);
    if(d->Smmu)MmUnmapIoSpace(d->Smmu,0x1000);
    ExFreePoolWithTag(d,GPU_TAG);return RestoreProbeBranches(a);
}
