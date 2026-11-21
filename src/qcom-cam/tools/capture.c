#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "../uapi.h"
#include "../trace-regs.h"

static BOOL Info(HANDLE device) {
    QCAM_INFO info={0};DWORD bytes;
    if(!DeviceIoControl(device,QCAM_QUERY,NULL,0,&info,sizeof(info),&bytes,NULL))return FALSE;
    if(bytes!=sizeof(info)||info.Size!=sizeof(info)||info.Abi!=QCAM_ABI)return FALSE;
    printf("sensor=0x%04lx flags=0x%lx stage=%lu status=0x%08lx\n",info.SensorId,info.Flags,info.Stage,(ULONG)info.Status);
    printf("size=%lux%lu stride=%lu frame_bytes=%lu frames=%lu\n",info.Width,info.Height,info.Stride,info.FrameBytes,info.Frames);
    printf("cci=0x%08lx csid=0x%08lx tfe=0x%08lx\n",info.CciVersion,info.CsidVersion,info.TfeVersion);
    printf("cci_irq=0x%08lx csid_irq=0x%08lx rx_irq=0x%08lx tfe_irq=0x%08lx bus_irq=0x%08lx\n",info.CciIrq,info.CsidIrq,info.CsidRxIrq,info.TfeIrq,info.TfeBusIrq);
    printf("packets=%lu measured=0x%08lx exposure=%lu gain=%lu frame_length=%lu\n",info.Packets,info.MeasuredSize,info.Exposure,info.Gain,info.FrameLength);
    return TRUE;
}

int wmain(int argc,wchar_t **argv) {
    if(argc<2){fwprintf(stderr,L"Usage: qcom_cam_capture info | diag | trace | routes | scm | scm-io | probe [LIMIT] | pipeline LIMIT | capture FILE.raw [DATARATE [SETTLE]] | exposure LINES GAIN\n");return 2;}
    HANDLE device=CreateFileW(L"\\\\.\\qcom-cam",GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(device==INVALID_HANDLE_VALUE){fprintf(stderr,"Open failed: %lu\n",GetLastError());return 1;}
    BOOL ok=FALSE;DWORD bytes=0,error=ERROR_INVALID_PARAMETER;
    if(!wcscmp(argv[1],L"info")&&argc==2)ok=Info(device);
    else if(!wcscmp(argv[1],L"context-read")&&argc==4) {
        QCAM_CONTEXT_REGISTER reg={wcstoul(argv[2],NULL,0),wcstoul(argv[3],NULL,0)};ULONG value=0;
        printf("Reading SMMU context %lu register %lx\n",reg.Bank,reg.Offset);fflush(stdout);
        ok=DeviceIoControl(device,QCAM_CONTEXT_READ,&reg,sizeof(reg),&value,sizeof(value),&bytes,NULL);error=GetLastError();
        if(ok)printf("context[%lu][%lx]=%08lx\n",reg.Bank,reg.Offset,value);
    }
    else if(!wcscmp(argv[1],L"scm")&&argc==2) {
        printf("Querying camera secure firmware capability\n");fflush(stdout);
        ok=DeviceIoControl(device,QCAM_SCM_PROBE,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if(!wcscmp(argv[1],L"scm-context")&&argc==2) {
        printf("Reading secure wait queue context, fid=c2002403\n");fflush(stdout);
        ok=DeviceIoControl(device,QCAM_SCM_WAITQ_CONTEXT,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if(!wcscmp(argv[1],L"scm-continue")&&argc==2) {
        puts("Continuing the pending camera firmware call");fflush(stdout);
        ok=DeviceIoControl(device,QCAM_SCM_CONTINUE,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if(!wcscmp(argv[1],L"scm-waitq")&&argc==2) {
        printf("Querying secure wait queue information, fid=c2002404\n");fflush(stdout);
        ok=DeviceIoControl(device,QCAM_SCM_WAITQ_PROBE,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if(!wcscmp(argv[1],L"scm-io")&&argc==2) {
        printf("Reading camera CRM version through secure firmware\n");fflush(stdout);
        ok=DeviceIoControl(device,QCAM_SCM_IO_PROBE,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if((!wcscmp(argv[1],L"scm-qos")||!wcscmp(argv[1],L"scm-qos-diag")||!wcscmp(argv[1],L"scm-qos-raw"))&&argc==2) {
        printf("Applying camera static QoS with CRM clocks active\n");fflush(stdout);
        ok=DeviceIoControl(device,!wcscmp(argv[1],L"scm-qos-raw")?QCAM_SCM_QOS_RAW:
            !wcscmp(argv[1],L"scm-qos-diag")?QCAM_SCM_QOS_DIAGNOSTIC:QCAM_SCM_QOS_PROBE,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if(!wcscmp(argv[1],L"tbu-probe")&&argc==2) {
        printf("Reading camera translation bridge status without submitting memory transactions\n");fflush(stdout);
        ok=DeviceIoControl(device,QCAM_TBU_PROBE,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if(!wcscmp(argv[1],L"measure-clocks")&&argc==2) {
        printf("Measuring camera clock frequencies with the hardware counter\n");fflush(stdout);
        ok=DeviceIoControl(device,QCAM_CLOCK_MEASURE,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if(!wcscmp(argv[1],L"prepare-release")&&argc==2) {
        puts("Releasing prepared camera resources");fflush(stdout);
        ok=DeviceIoControl(device,QCAM_PREPARE_RELEASE,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if((!wcscmp(argv[1],L"scm-noc-faults")||!wcscmp(argv[1],L"scm-noc-faults32"))&&argc==2) {
        puts("Querying camera NoC errors through the firmware read interface");fflush(stdout);
        ok=DeviceIoControl(device,!wcscmp(argv[1],L"scm-noc-faults32")?QCAM_SCM_NOC_FAULTS32:QCAM_SCM_NOC_FAULTS,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if(!wcscmp(argv[1],L"rpmh-probe")&&argc==2) {
        puts("Reading camera DDR and MMSS firmware bus state");fflush(stdout);
        ok=DeviceIoControl(device,QCAM_CAMERA_BUS_PROBE,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if(!wcscmp(argv[1],L"frame-route-probe")&&argc==2) {
        puts("Validating and restoring the main camera route through an existing identity context");fflush(stdout);
        ok=DeviceIoControl(device,QCAM_FRAME_ROUTE_PROBE,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if(!wcscmp(argv[1],L"bus-snapshot")&&argc==2) {
        puts("Reading powered camera bus state");fflush(stdout);
        ok=DeviceIoControl(device,QCAM_BUS_SNAPSHOT,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if((!wcscmp(argv[1],L"scm-qos-smart")||!wcscmp(argv[1],L"scm-qos-rdi")||!wcscmp(argv[1],L"scm-qos-cdm"))&&argc==2) {
        puts(!wcscmp(argv[1],L"scm-qos-cdm")?"Requesting camera CDM read port urgency from firmware":
            !wcscmp(argv[1],L"scm-qos-rdi")?"Requesting the five main camera RDI QoS settings from firmware":
            "Submitting two main camera RDI QoS priority entries to firmware");fflush(stdout);
        ok=DeviceIoControl(device,!wcscmp(argv[1],L"scm-qos-cdm")?QCAM_SCM_QOS_CDM:
            !wcscmp(argv[1],L"scm-qos-rdi")?QCAM_SCM_QOS_RDI:QCAM_SCM_QOS_SMART,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if((!wcscmp(argv[1],L"mnoc-probe")||!wcscmp(argv[1],L"mnoc-qos-hold"))&&argc==2) {
        puts(!wcscmp(argv[1],L"mnoc-probe")?"Reading camera MMSS NoC QoS":"Applying camera MMSS NoC QoS and retaining resources");fflush(stdout);
        ok=DeviceIoControl(device,!wcscmp(argv[1],L"mnoc-probe")?QCAM_MNOC_PROBE:QCAM_MNOC_QOS_HOLD,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if(!wcscmp(argv[1],L"cdm-fetch")&&argc==2) {
        puts("Submitting a camera command buffer containing only a completion marker");fflush(stdout);
        ok=DeviceIoControl(device,QCAM_CDM_FETCH,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if((!wcscmp(argv[1],L"tbu-translate")||!wcscmp(argv[1],L"frame-translate"))&&argc==2) {
        puts(!wcscmp(argv[1],L"frame-translate")?"Querying frame buffer write translations through the HF bridge":
            "Querying the camera command page translation through the SF bridge");fflush(stdout);
        ok=DeviceIoControl(device,!wcscmp(argv[1],L"frame-translate")?QCAM_FRAME_TRANSLATE:QCAM_TBU_TRANSLATE,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if((!wcscmp(argv[1],L"cdm-config")&&argc==2)||
            ((!wcscmp(argv[1],L"cdm-prepare")||!wcscmp(argv[1],L"cdm-prepare-hold"))&&(argc==2||argc==3))) {
        BOOL prepare=wcscmp(argv[1],L"cdm-config")!=0,hold=!wcscmp(argv[1],L"cdm-prepare-hold");
        ULONG limit=argc==3?wcstoul(argv[2],NULL,0):4;
        printf(prepare?"Preparing camera command route without submission\n":"Submitting bounded camera path configuration\n");fflush(stdout);
        ok=DeviceIoControl(device,hold?QCAM_CDM_PREPARE_HOLD:prepare?QCAM_CDM_PREPARE:QCAM_CDM_CONFIG,prepare?&limit:NULL,prepare?sizeof(limit):0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if(!wcscmp(argv[1],L"cdm-probe")&&argc==2) {
        printf("Reading camera command processor registers\n");fflush(stdout);
        ok=DeviceIoControl(device,QCAM_CDM_PROBE,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if(!wcscmp(argv[1],L"routes")&&argc==2) {
        QCAM_DMA_ROUTE_INFO r={0};ok=DeviceIoControl(device,QCAM_DMA_ROUTES,NULL,0,&r,sizeof(r),&bytes,NULL);error=GetLastError();
        if(ok&&(bytes!=sizeof(r)||r.Size!=sizeof(r)||r.Abi!=QCAM_ABI||r.Count>128)){ok=FALSE;error=ERROR_INVALID_DATA;}
        if(ok) {
            printf("smmu id0=%08lx id1=%08lx global=%08lx slots=%lu\n",r.Id0,r.Id1,r.Global,r.Count);
            printf("smmu fault=%08lx syndrome=%08lx %08lx %08lx\n",r.FaultStatus,r.FaultSyndrome[0],r.FaultSyndrome[1],r.FaultSyndrome[2]);
            for(ULONG i=0;i<r.Count;i++)printf("slot=%lu smr=%08lx s2cr=%08lx valid=%lu sid=%04lx mask=%04lx type=%lu bank=%lu cbar=%08lx sctlr=%08lx\n",
                i,r.Smr[i],r.S2cr[i],r.Smr[i]>>31,r.Smr[i]&0x7fff,(r.Smr[i]>>16)&0x7fff,(r.S2cr[i]>>16)&3,r.S2cr[i]&255,r.Cbar[i],r.Sctlr[i]);
        }
    }
    else if(!wcscmp(argv[1],L"trace")&&argc==2) {
        QCAM_TRACE t={0};ok=DeviceIoControl(device,QCAM_TRACE_QUERY,NULL,0,&t,sizeof(t),&bytes,NULL);error=GetLastError();
        if(ok&&(bytes!=sizeof(t)||t.Size!=sizeof(t)||t.Abi!=QCAM_ABI)){ok=FALSE;error=ERROR_REVISION_MISMATCH;}
        if(ok) {
            static const ULONG cpas[]={0,4,8,0xdc,0xec,0xf0,0xf4,0xf8};
            printf("trace step=%lu\n",t.Step);
            printf("frame route slot=%lu original=%08lx selected=%08lx cbar=%08lx sctlr=%08lx status=%08lx/%08lx\n",
                t.FrameRoute[0],t.FrameRoute[1],t.FrameRoute[2],t.FrameRoute[3],t.FrameRoute[4],t.FrameRouteStatus[0],t.FrameRouteStatus[1]);
            printf("firmware IO function=%08lx\n",t.ScmIoFunction);
            printf("camera RPMh FE DDR=%08lx MMNOC=%08lx BE DDR=%08lx MMNOC=%08lx SHUB=%08lx\n",
                t.CameraRpmh[0],t.CameraRpmh[1],t.CameraRpmh[2],t.CameraRpmh[3],t.CameraRpmh[4]);
            const ULONG mnocOffsets[]={0x29008,0x2a008,0x37008};
            for(ULONG i=0;i<3;i++)printf("mnoc[%lx] original=%08lx requested=%08lx observed=%08lx\n",
                mnocOffsets[i],t.MnocQos[i][0],t.MnocQos[i][1],t.MnocQos[i][2]);
            for(ULONG i=0;i<sizeof(cpas)/sizeof(cpas[0]);i++)printf("cpas[%lx]=%08lx\n",cpas[i],t.Cpas[i]);
            for(ULONG i=0;i<sizeof(CamClockTraceRegs)/sizeof(CamClockTraceRegs[0]);i++)printf("clock[%lx]=%08lx\n",CamClockTraceRegs[i],t.Clocks[i]);
            printf("smmu id0=%08lx id1=%08lx id2=%08lx\n",t.Smmu[0],t.Smmu[1],t.Smmu[2]);
            printf("frame dma=%llx physical=%llx\n",t.FrameAddress,t.FramePhysicalAddress);
            printf("smmu global=%08lx slots=%lu banks=%lu camera_slot=%lu translated=%lu cbar=%08lx sctlr=%08lx control=%08lx\n",
                t.SmmuState[0],t.SmmuState[1],t.SmmuState[2],t.SmmuState[3],t.SmmuState[4],t.SmmuState[5],t.SmmuState[6],t.SmmuState[7]);
            printf("sensor stream=%lu counter=%lu\n",t.SensorStream,t.SensorCounter);
            printf("frame prefix:");for(ULONG i=0;i<8;i++)printf(" %08lx",t.FrameWords[i]);printf("\n");
            for(ULONG i=0;i<sizeof(CamPhyTraceRegs)/sizeof(CamPhyTraceRegs[0]);i++)printf("phy[%lx]=%08lx\n",CamPhyTraceRegs[i],t.Phy[i]);
            for(ULONG i=0;i<sizeof(CamCsidTraceRegs)/sizeof(CamCsidTraceRegs[0]);i++)printf("csid[%lx]=%08lx\n",CamCsidTraceRegs[i],t.Csid[i]);
            for(ULONG i=0;i<sizeof(CamWriterTraceRegs)/sizeof(CamWriterTraceRegs[0]);i++)printf("writer[%lx]=%08lx\n",CamWriterTraceRegs[i],t.Writer[i]);
            for(ULONG i=0;i<t.TfeTopCount&&i<sizeof(CamTfeTopTraceRegs)/sizeof(CamTfeTopTraceRegs[0]);i++)
                printf("tfe_top[%lx]=%08lx\n",CamTfeTopTraceRegs[i],t.TfeTop[i]);
            printf("tfe prime original test_bus=%08lx\n",t.TfePrimeTestBus);
            printf("noc fill reads=%lu\n",t.NocFillReads);
            printf("raw drain status=%08lx stopped=%08lx/%08lx/%08lx/%08lx\n",t.RawDrainStatus,
                t.NocFillStopped[0],t.NocFillStopped[1],t.NocFillStopped[2],t.NocFillStopped[3]);
            for(ULONG n=0;n<2;n++) {
                printf("raw tbu_%s before qsm=%llx idle=%llx ot=%llx/%llx after qsm=%llx idle=%llx ot=%llx/%llx seen=%llx/%llx\n",
                    n?"sf":"hf",t.RawTbuBefore[n][0],t.RawTbuBefore[n][1],t.RawTbuBefore[n][2],t.RawTbuBefore[n][3],
                    t.RawTbuAfter[n][0],t.RawTbuAfter[n][1],t.RawTbuAfter[n][2],t.RawTbuAfter[n][3],
                    t.RawOutstandingSeen[n][0],t.RawOutstandingSeen[n][1]);
            }
            printf("raw context before=%08lx %08lx %08lx %08lx after=%08lx %08lx %08lx %08lx\n",
                t.RawContextBefore[0],t.RawContextBefore[1],t.RawContextBefore[2],t.RawContextBefore[3],
                t.RawContextAfter[0],t.RawContextAfter[1],t.RawContextAfter[2],t.RawContextAfter[3]);
            printf("raw context read status=%08lx/%08lx\n",t.RawContextStatus[0],t.RawContextStatus[1]);
            for(ULONG i=0;i<4;i++)printf("noc fill[%lx] before=%08lx after=%08lx peak_queued=%lu peak_pending=%lu\n",
                CamNocFillRegs[i],t.NocFillBefore[i],t.NocFillAfter[i],t.NocFillPeak[i][0],t.NocFillPeak[i][1]);
            for(ULONG i=0;i<3;i++)printf("tfe prime[%lu] consumed=%08lx frame=%08lx fifo=%08lx client=%08lx\n",
                i,t.TfePrime[i][0],t.TfePrime[i][1],t.TfePrime[i][2],t.TfePrime[i][3]);
            if(t.TfeTopCount==sizeof(CamTfeTopTraceRegs)/sizeof(CamTfeTopTraceRegs[0]))for(ULONG i=0;i<5;i++) {
                ULONG value=(t.TfeTop[12]>>(12+i*4))&15;
                printf("rdi%lu splitter_to_bus idle=%lu valid=%lu ready=%lu raw=%lx\n",i,(value>>2)&1,(value>>1)&1,value&1,value);
            }
            for(ULONG i=0;i<sizeof(CamGccTraceRegs)/sizeof(CamGccTraceRegs[0]);i++)printf("gcc[%lx]=%08lx\n",CamGccTraceRegs[i],t.Gcc[i]);
            for(ULONG i=0;i<sizeof(CamNocTraceRegs)/sizeof(CamNocTraceRegs[0]);i++)printf("noc[%lx]=%08lx\n",CamNocTraceRegs[i],t.Noc[i]);
            const char *names[]={"MM0","MM1","SH1","CN0","SH0","MC0"};
            for(ULONG i=0;i<6;i++)printf("bcm %s original=%08lx request=%08lx observed=%08lx\n",names[i],t.BusVotes[i][0],t.BusVotes[i][1],t.BusVotes[i][2]);
            printf("firmware qos=%lu\n",t.FirmwareQos);
            printf("scm return:");for(ULONG i=0;i<8;i++)printf(" %llx",t.ScmReturn[i]);printf("\n");
            printf("scm smart payload pa=%llx bytes=%lu words:",t.ScmSmartAddress,t.ScmPayloadBytes);
            for(ULONG i=0;i<10;i++)printf(" %08lx",t.ScmSmartPayload[i]);puts("");
            printf("scm IO capability:");for(ULONG i=0;i<8;i++)printf(" %llx",t.ScmIoCapability[i]);puts("");
            printf("scm IO return:");for(ULONG i=0;i<8;i++)printf(" %llx",t.ScmIoReturn[i]);puts("");
            const ULONG errorOffsets[]={0x10,0x20,0x24,0x28,0x2c,0x30,0x34,0x38,0x3c};
            for(ULONG n=0;n<2;n++) {
                printf("noc_%s error snapshot count=%lu status=%08lx\n",n?"nrt":"rt",t.NocErrorCount[n],(ULONG)t.NocErrorStatus[n]);
                for(ULONG i=0;i<t.NocErrorCount[n]&&i<9;i++)printf("noc_%s error[%lx]=%08lx\n",n?"nrt":"rt",errorOffsets[i],t.NocErrors[n][i]);
            }
            for(ULONG i=0;i<sizeof(CamCrmTraceRegs)/sizeof(CamCrmTraceRegs[0]);i++)printf("crm[%lx]=%08lx\n",CamCrmTraceRegs[i],t.Crm[i]);
            printf("scm waitq:");for(ULONG i=0;i<8;i++)printf(" %llx",t.ScmWaitq[i]);puts("");
            for(ULONG i=0;i<sizeof(CamCdmTraceRegs)/sizeof(CamCdmTraceRegs[0]);i++)printf("cdm[%lx]=%08lx\n",CamCdmTraceRegs[i],t.Cdm[i]);
            printf("cdm command dma=%llx marker=%08lx\n",t.CommandAddress,t.CommandMarker);
            for(ULONG i=0;i<16;i++)printf("cdm debug[%lu]=%08lx\n",i,t.CdmDebug[i]);
            for(ULONG i=0;i<t.CdmSampleCount&&i<8;i++) {
                const QCAM_CDM_SAMPLE *s=&t.CdmSamples[i];
                printf("cdm sample[%lu] phase=%lu us=%lu status=%08lx irq=%08lx pending=%08lx user=%08lx base=%08lx len=%08lx debug12=%08lx qchannel=%08lx/%08lx\n",
                    i,s->Phase,s->ElapsedUs,s->Status,s->Irq,s->Pending,s->UserData,s->CurrentBase,s->CurrentLength,s->Debug,s->Qchannel[0],s->Qchannel[1]);
                for(ULONG n=0;n<2;n++)printf("cdm sample[%lu] tbu_%s qsm=%08lx idle=%08lx outstanding=%016llx/%016llx\n",
                    i,n?"sf":"hf",s->TbuStatus[n][0],s->TbuStatus[n][1],s->Outstanding[n][0],s->Outstanding[n][1]);
            }
            for(ULONG n=0;n<2;n++)printf("cdm observed tbu_%s outstanding=%016llx/%016llx\n",
                n?"sf":"hf",t.CdmOutstandingSeen[n][0],t.CdmOutstandingSeen[n][1]);
            for(ULONG i=0;i<8;i++)printf("tbu translation[%lu]=%016llx\n",i,t.TbuTranslation[i]);
            for(ULONG n=0;n<2;n++)for(ULONG i=0;i<8;i++)printf("frame translation[%lu][%lu]=%016llx\n",n,i,t.FrameTranslation[n][i]);
            printf("cdm route slot=%lu control=%08lx cbar=%08lx sctlr=%08lx\n",
                t.CommandRoute[0],t.CommandRoute[1],t.CommandRoute[2],t.CommandRoute[3]);
            for(ULONG n=0;n<2;n++) {
                printf("context_%s before:",n?"cdm":"frame");for(ULONG i=0;i<4;i++)printf(" %08lx",t.ContextBefore[n][i]);
                printf(" after:");for(ULONG i=0;i<4;i++)printf(" %08lx",t.ContextAfter[n][i]);puts("");
            }
            printf("cdm command words:");for(ULONG i=0;i<6;i++)printf(" %08lx",t.CommandWords[i]);
            printf(" abort=%08lx\n",(ULONG)t.CommandAbortStatus);
            const char *clockNames[]={"cam_top_fast_ahb","cam_top_ahb","core_ahb","camnoc_rt_axi","camnoc_nrt_axi",
                "csid","tfe0_main","csid_phy_rx","gcc_camera_hf_axi","gcc_camera_sf_axi"};
            for(ULONG i=0;i<10;i++)printf("measured %s hz=%llu short=%lu full=%lu status=%08lx\n",
                clockNames[i],t.MeasuredHz[i],t.ClockSamples[i][0],t.ClockSamples[i][1],(ULONG)t.MeasureStatus[i]);
            for(ULONG n=0;n<2;n++)for(ULONG i=0;i<sizeof(CamTbuTraceRegs)/sizeof(CamTbuTraceRegs[0]);i++)
                printf("tbu_%s[%lx]=%016llx\n",n?"sf":"hf",CamTbuTraceRegs[i],t.Tbu[n][i]);
        }
    }
    else if(!wcscmp(argv[1],L"diag")&&argc==2) {
        QCAM_DIAG d={0};ok=DeviceIoControl(device,QCAM_DIAGNOSTICS,NULL,0,&d,sizeof(d),&bytes,NULL);error=GetLastError();
        if(ok) {
            printf("step=%lu status=0x%08lx resource=%.8s address=0x%08lx slot=%lu phase=%lu count=%lu\n",d.Step,(ULONG)d.Status,d.Resource,d.Address,d.Slot,d.Phase,d.Count);
            printf("region=%lu offset=0x%lx observed=0x%08lx mask=0x%08lx expected=0x%08lx saved=%lu faults=%lu\n",d.Region,d.Offset,d.Observed,d.Mask,d.Expected,d.SavedCount,d.Faults);
            printf("rsc=0x%08lx config=0x%08lx db=0x%08lx\n",d.RscId,d.RscConfig,d.DbMagic);
            for(ULONG i=0;i<3;i++)printf("tcs%lu wait=%lx ctrl=%lx state=%lx enable=%lx msg=%lx addr=%lx data=%lx cmdstatus=%lx response=%lx\n",i,d.Tcs[i][0],d.Tcs[i][1],d.Tcs[i][2],d.Tcs[i][3],d.Tcs[i][4],d.Tcs[i][5],d.Tcs[i][6],d.Tcs[i][7],d.Tcs[i][8]);
        }
    }
    else if(!wcscmp(argv[1],L"pipeline")&&argc==3) {
        wchar_t *end;QCAM_PROBE_OPTIONS o={sizeof(o),QCAM_ABI,wcstoul(argv[2],&end,0)};
        if(*end||!o.Limit||o.Limit>5)goto done;
        printf("Pipeline test limit=%lu\n",o.Limit);fflush(stdout);
        ok=DeviceIoControl(device,QCAM_PIPELINE_PROBE,&o,sizeof(o),NULL,0,&bytes,NULL);error=GetLastError();Info(device);
    }
    else if(!wcscmp(argv[1],L"probe")&&(argc==2||argc==3)) {
        QCAM_PROBE_OPTIONS o={sizeof(o),QCAM_ABI,0};
        if(argc==3) {
            wchar_t *end;o.Limit=wcstoul(argv[2],&end,0);
            if(*end||!o.Limit||o.Limit>10)goto done;
            printf("Platform test limit=%lu\n",o.Limit);fflush(stdout);
        }
        ok=DeviceIoControl(device,argc==3?QCAM_PLATFORM_PROBE:QCAM_PROBE,argc==3?&o:NULL,argc==3?sizeof(o):0,NULL,0,&bytes,NULL);error=GetLastError();Info(device);
    } else if(!wcscmp(argv[1],L"exposure")&&argc==4) {
        wchar_t *end1,*end2;
        ULONG lines=wcstoul(argv[2],&end1,0),gain=wcstoul(argv[3],&end2,0);
        if(!*end1&&!*end2) {
            QCAM_EXPOSURE_OPTIONS options={sizeof(options),QCAM_ABI,lines,gain};
            ok=DeviceIoControl(device,QCAM_EXPOSURE,&options,sizeof(options),NULL,0,&bytes,NULL);error=GetLastError();
        }
    } else if((!wcscmp(argv[1],L"capture")||!wcscmp(argv[1],L"capture-diag")||!wcscmp(argv[1],L"capture-frame")||!wcscmp(argv[1],L"capture-auto")||!wcscmp(argv[1],L"capture-prefetch")||!wcscmp(argv[1],L"capture-context")||!wcscmp(argv[1],L"capture-primed")||!wcscmp(argv[1],L"dump-frame"))&&argc>=3&&argc<=5) {
        BOOLEAN context=!wcscmp(argv[1],L"capture-context"),primed=!wcscmp(argv[1],L"capture-primed"),prefetch=!wcscmp(argv[1],L"capture-prefetch"),automatic=!wcscmp(argv[1],L"capture-auto"),frameMode=!wcscmp(argv[1],L"capture-frame");
        BOOLEAN diagnostic=!wcscmp(argv[1],L"capture-diag")||frameMode||automatic||prefetch||primed||context,dump=!wcscmp(argv[1],L"dump-frame");
        QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,diagnostic?1000:15000,0,4980000000ULL};
        wchar_t *end=NULL;
        if(argc>3){options.DataRate=_wcstoui64(argv[3],&end,0);if(*end)goto done;}
        if(argc>4){options.SettleCount=wcstoul(argv[4],&end,0);if(*end)goto done;}
        UCHAR *raw=VirtualAlloc(NULL,QCAM_FRAME_BYTES,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        if(!raw){error=GetLastError();goto done;}
        printf(dump?"Reading stopped frame buffer\n":"Starting RAW10 capture, timeout=%lu ms\n",options.TimeoutMs);fflush(stdout);
        ok=DeviceIoControl(device,dump?QCAM_FRAME_DIAGNOSTIC:context?QCAM_CAPTURE_CONTEXT_DIAGNOSTIC:primed?QCAM_CAPTURE_PRIMED_DIAGNOSTIC:prefetch?QCAM_CAPTURE_PREFETCH_DIAGNOSTIC:automatic?QCAM_CAPTURE_AUTO_DIAGNOSTIC:frameMode?QCAM_CAPTURE_FRAME_DIAGNOSTIC:diagnostic?QCAM_CAPTURE_DIAGNOSTIC:QCAM_CAPTURE,dump?NULL:&options,dump?0:sizeof(options),raw,QCAM_FRAME_BYTES,&bytes,NULL);
        error=GetLastError();Info(device);
        if(ok&&bytes!=QCAM_FRAME_BYTES){ok=FALSE;error=ERROR_INVALID_DATA;}
        if(ok) {
            HANDLE file=CreateFileW(argv[2],GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
            if(file==INVALID_HANDLE_VALUE){ok=FALSE;error=GetLastError();}
            else {
                DWORD written=0;ok=WriteFile(file,raw,QCAM_FRAME_BYTES,&written,NULL)&&written==QCAM_FRAME_BYTES;
                if(!ok)error=GetLastError();else printf(dump?"Saved %lu bytes of diagnostic buffer data\n":"Saved %lu bytes of RGGB RAW10\n",written);
                CloseHandle(file);
            }
        }
        VirtualFree(raw,0,MEM_RELEASE);
    }
done:
    CloseHandle(device);if(!ok)fprintf(stderr,"Operation failed: %lu\n",error);return ok?0:1;
}
