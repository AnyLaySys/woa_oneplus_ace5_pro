#include "cam.h"
#include "clock-measure.h"
#include "../platform/lock.h"

NTSTATUS CamMeasureClocks(CAM *c) {
    if(!c->Powered||c->DmaActive||c->RxActive||c->ScmPending||c->CrmPending||c->CdmPending)return STATUS_DEVICE_BUSY;
    PLATFORM_LOCK_GUARD guard={0};NTSTATUS status=PlatformLockAcquire(&guard,FALSE);
    if(status!=STATUS_SUCCESS)return status;
    PUCHAR gcc=c->Base[CamGcc],cc=c->Base[CamCc];
    const ULONG gccOffsets[]={0x62024,0x62000,0x62004,0x62008,0x62048};
    const ULONG ccOffsets[]={0x16000,0x12004,0x12008};
    ULONG gccSaved[5],ccSaved[3];
    for(ULONG i=0;i<5;i++)gccSaved[i]=CamRead(gcc,gccOffsets[i]);
    for(ULONG i=0;i<3;i++)ccSaved[i]=CamRead(cc,ccOffsets[i]);
    c->Diag.Step=166;c->Diag.Region=CamGcc;c->Diag.Offset=0x62048;
    c->Diag.Mask=0x100000;c->Diag.Expected=0;c->Diag.Observed=gccSaved[4];
    if(gccSaved[4]&0x100000){PlatformLockRelease(&guard);return STATUS_DEVICE_BUSY;}
    RtlZeroMemory(c->Trace.ClockSamples,sizeof(c->Trace.ClockSamples));
    RtlZeroMemory(c->Trace.MeasuredHz,sizeof(c->Trace.MeasuredHz));
    for(ULONG i=0;i<10;i++)c->Trace.MeasureStatus[i]=STATUS_DEVICE_NOT_READY;
    CamWrite(gcc,0x62008,gccSaved[3]|1);
    CamWrite(gcc,0x62000,(gccSaved[1]&~15u)|1);
    CamWrite(gcc,0x62004,gccSaved[2]|1);
    CamWrite(cc,0x12004,(ccSaved[1]&~15u)|3);
    CamWrite(cc,0x12008,ccSaved[2]|1);
    const ULONG selectors[]={0x65,0x67,0x62,0x5a,0x5c,0x58,0x25,0x03,0x69,0x6b};
    for(ULONG i=0;i<10;i++) {
        ULONG gccSel=i<8?0x6e:selectors[i];
        if(i<8)CamWrite(cc,0x16000,(ccSaved[0]&~255u)|selectors[i]);
        CamWrite(gcc,0x62024,(gccSaved[0]&~0x3ffu)|gccSel);
        status=CamWait(gcc,0x62024,0x3ff,gccSel,1000);
        if(NT_SUCCESS(status)&&i<8)status=CamWait(cc,0x16000,255,selectors[i],1000);
        c->Diag.Phase=i;c->Diag.Offset=0x6204c;c->Diag.Mask=0x2000000;c->Diag.Expected=0x2000000;
        if(NT_SUCCESS(status))status=CamMeasureCount(gcc,0x1000,&c->Trace.ClockSamples[i][0]);
        if(NT_SUCCESS(status))status=CamMeasureCount(gcc,0x20000,&c->Trace.ClockSamples[i][1]);
        c->Diag.Observed=CamRead(gcc,0x6204c);c->Trace.MeasureStatus[i]=status;
        if(!NT_SUCCESS(status))break;
        c->Trace.MeasuredHz[i]=CamMeasuredHz(c->Trace.ClockSamples[i][0],c->Trace.ClockSamples[i][1],i<8?8:2);
    }
    CamWrite(gcc,0x62048,gccSaved[4]);
    CamWrite(gcc,0x62008,gccSaved[3]);CamWrite(gcc,0x62004,gccSaved[2]);CamWrite(cc,0x12008,ccSaved[2]);
    CamWrite(cc,0x12004,ccSaved[1]);CamWrite(cc,0x16000,ccSaved[0]);
    CamWrite(gcc,0x62000,gccSaved[1]);CamWrite(gcc,0x62024,gccSaved[0]);
    NTSTATUS released=PlatformLockRelease(&guard);return NT_SUCCESS(status)?released:status;
}
