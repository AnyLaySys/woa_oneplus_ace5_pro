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
    else if(!wcscmp(argv[1],L"scm")&&argc==2) {
        printf("Querying camera secure firmware capability\n");fflush(stdout);
        ok=DeviceIoControl(device,QCAM_SCM_PROBE,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if(!wcscmp(argv[1],L"scm-io")&&argc==2) {
        printf("Reading camera CRM version through secure firmware\n");fflush(stdout);
        ok=DeviceIoControl(device,QCAM_SCM_IO_PROBE,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if(!wcscmp(argv[1],L"scm-qos")&&argc==2) {
        printf("Applying camera static QoS with CRM clocks active\n");fflush(stdout);
        ok=DeviceIoControl(device,QCAM_SCM_QOS_PROBE,NULL,0,NULL,0,&bytes,NULL);error=GetLastError();
    }
    else if(!wcscmp(argv[1],L"routes")&&argc==2) {
        QCAM_DMA_ROUTE_INFO r={0};ok=DeviceIoControl(device,QCAM_DMA_ROUTES,NULL,0,&r,sizeof(r),&bytes,NULL);error=GetLastError();
        if(ok&&(bytes!=sizeof(r)||r.Size!=sizeof(r)||r.Abi!=QCAM_ABI||r.Count>128)){ok=FALSE;error=ERROR_INVALID_DATA;}
        if(ok) {
            printf("smmu id0=%08lx id1=%08lx global=%08lx slots=%lu\n",r.Id0,r.Id1,r.Global,r.Count);
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
            for(ULONG i=0;i<sizeof(CamGccTraceRegs)/sizeof(CamGccTraceRegs[0]);i++)printf("gcc[%lx]=%08lx\n",CamGccTraceRegs[i],t.Gcc[i]);
            for(ULONG i=0;i<sizeof(CamNocTraceRegs)/sizeof(CamNocTraceRegs[0]);i++)printf("noc[%lx]=%08lx\n",CamNocTraceRegs[i],t.Noc[i]);
            const char *names[]={"MM0","MM1","SH1"};
            for(ULONG i=0;i<3;i++)printf("bcm %s original=%08lx request=%08lx observed=%08lx\n",names[i],t.BusVotes[i][0],t.BusVotes[i][1],t.BusVotes[i][2]);
            printf("firmware qos=%lu\n",t.FirmwareQos);
            printf("scm return:");for(ULONG i=0;i<8;i++)printf(" %llx",t.ScmReturn[i]);printf("\n");
            for(ULONG i=0;i<sizeof(CamCrmTraceRegs)/sizeof(CamCrmTraceRegs[0]);i++)printf("crm[%lx]=%08lx\n",CamCrmTraceRegs[i],t.Crm[i]);
            printf("scm waitq:");for(ULONG i=0;i<8;i++)printf(" %llx",t.ScmWaitq[i]);puts("");
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
        if(*end||!o.Limit||o.Limit>3)goto done;
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
    } else if(!wcscmp(argv[1],L"capture")&&argc>=3&&argc<=5) {
        QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,15000,0,4980000000ULL};
        wchar_t *end=NULL;
        if(argc>3){options.DataRate=_wcstoui64(argv[3],&end,0);if(*end)goto done;}
        if(argc>4){options.SettleCount=wcstoul(argv[4],&end,0);if(*end)goto done;}
        UCHAR *raw=VirtualAlloc(NULL,QCAM_FRAME_BYTES,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        if(!raw){error=GetLastError();goto done;}
        printf("Starting RAW10 capture, timeout=%lu ms\n",options.TimeoutMs);fflush(stdout);
        ok=DeviceIoControl(device,QCAM_CAPTURE,&options,sizeof(options),raw,QCAM_FRAME_BYTES,&bytes,NULL);
        error=GetLastError();Info(device);
        if(ok&&bytes!=QCAM_FRAME_BYTES){ok=FALSE;error=ERROR_INVALID_DATA;}
        if(ok) {
            HANDLE file=CreateFileW(argv[2],GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
            if(file==INVALID_HANDLE_VALUE){ok=FALSE;error=GetLastError();}
            else {
                DWORD written=0;ok=WriteFile(file,raw,QCAM_FRAME_BYTES,&written,NULL)&&written==QCAM_FRAME_BYTES;
                if(!ok)error=GetLastError();else printf("Saved %lu bytes of RGGB RAW10\n",written);
                CloseHandle(file);
            }
        }
        VirtualFree(raw,0,MEM_RELEASE);
    }
done:
    CloseHandle(device);if(!ok)fprintf(stderr,"Operation failed: %lu\n",error);return ok?0:1;
}
