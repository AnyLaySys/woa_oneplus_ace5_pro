#include "..\qcom-battery\power.h"

static VOID SiaUsbOpen(PSIA_LINK L)
{
    const CHAR Name[]="PMIC_RTR_ADSP_APPS";
    UCHAR M[8+sizeof(Name)];
    if(!L->VersionReady||L->Usb.Opening)return;
    *(USHORT UNALIGNED*)M=GLINK_CMD_OPEN;*(USHORT UNALIGNED*)(M+2)=SIA_USB_LCID;
    *(ULONG UNALIGNED*)(M+4)=sizeof(Name);RtlCopyMemory(M+8,Name,sizeof(Name));
    L->Usb.Opening=SiaGlinkSend(L,M,sizeof(M));
}
static VOID SiaUsbGrant(PSIA_LINK L,ULONG Iid,ULONG Size)
{
    ULONG M[]={GLINK_CMD_INTENT|(SIA_USB_LCID<<16),1,Size,Iid};
    SiaGlinkSend(L,(UCHAR*)M,sizeof(M));
}
static VOID SiaUsbActivate(PSIA_LINK L)
{
    SIA_USB *U=&L->Usb;
    if(!L->VersionReady||!U->Rcid||U->Granted)return;
    SiaGlinkCmd(L,GLINK_CMD_OPEN_ACK,(USHORT)U->Rcid,0);
    SiaUsbOpen(L);
    for(ULONG I=1;I<=4;I++)SiaUsbGrant(L,I,512);
    U->Granted=TRUE;
    SiaGlinkCmd(L,GLINK_CMD_RX_INTENT_REQ,SIA_USB_LCID,64);
}
static VOID SiaUsbResetPeer(SIA_USB *U)
{
    OTG_STATE O=U->Otg;RtlZeroMemory(U,sizeof(*U));
    if(O.Touched){O.Phase=OtgFailed;O.Pending=0;O.Stop=3;O.Fault=OtgFirmwareError;U->Otg=O;}
}
static BOOLEAN SiaUsbHandle(PSIA_LINK L,USHORT Cmd,USHORT P1,ULONG P2,ULONG Tail,const ULONG *Frame)
{
    SIA_USB *U=&L->Usb;
    UCHAR Data[64];
    if(Cmd==GLINK_CMD_OPEN){
        RtlZeroMemory(Data,sizeof(Data));SiaFifoRead(L,Tail+8,Data,min(P2&0xffff,63u));
        if(strcmp((CHAR*)Data,"PMIC_RTR_ADSP_APPS"))return FALSE;
        if(U->Rcid)SiaUsbResetPeer(U);
        U->Rcid=P1;SiaUsbActivate(L);return TRUE;
    }
    if(Cmd==GLINK_CMD_OPEN_ACK&&P1==SIA_USB_LCID){
        U->Acked=TRUE;SiaGlinkCmd(L,GLINK_CMD_RX_INTENT_REQ,SIA_USB_LCID,64);return TRUE;
    }
    if(Cmd==GLINK_CMD_CLOSE_ACK&&P1==SIA_USB_LCID){U->Acked=FALSE;return TRUE;}
    if(!U->Rcid||P1!=U->Rcid)return FALSE;
    if(Cmd==GLINK_CMD_INTENT){
        for(ULONG I=0;I<P2;I++){
            SiaFifoRead(L,Tail+8+I*8,Data,8);
            ULONG Size=*(ULONG UNALIGNED*)Data,Iid=*(ULONG UNALIGNED*)(Data+4),J;
            for(J=0;J<U->Count&&U->Iid[J]!=Iid;J++);
            if(J<SIA_USB_INTENTS){U->Iid[J]=Iid;U->Size[J]=Size;U->Free[J]=TRUE;U->Count=max(U->Count,J+1);}
        }
        return TRUE;
    }
    if(Cmd==GLINK_CMD_RX_DONE||Cmd==GLINK_CMD_RX_DONE_REUSE){
        for(ULONG I=0;I<U->Count;I++)if(U->Iid[I]==P2){U->Free[I]=Cmd==GLINK_CMD_RX_DONE_REUSE;if(!U->Free[I])U->Size[I]=0;break;}
        return TRUE;
    }
    if(Cmd==GLINK_CMD_RX_INTENT_REQ){
        SiaGlinkCmd(L,GLINK_CMD_INTENT_REQ_ACK,SIA_USB_LCID,P2<=512?1:0);
        if(P2<=512)SiaUsbGrant(L,100,512);
        return TRUE;
    }
    if(Cmd==GLINK_CMD_TX_DATA||Cmd==GLINK_CMD_TX_DATA_CONT){
        if(Cmd==GLINK_CMD_TX_DATA&&Frame[0]>=24&&!Frame[1]){
            SiaFifoRead(L,Tail+16,Data,24);
            const ULONG UNALIGNED *D=(const ULONG UNALIGNED*)Data;
            if(D[0]==32778&&D[1]==1&&D[2]==(U->Write?0x33u:0x32u)&&D[3]==U->Property&&U->Otg.Pending){
                ULONGLONG Now=KeQueryInterruptTime()/10000;
                if(!U->Write&&!U->Property)U->OnlineAt=!D[5]&&D[4]<=1?Now:0;
                OtgReply(&U->Otg,D[4],D[5],Now);
            }
        }
        if(!Frame[1])SiaGlinkCmd(L,GLINK_CMD_RX_DONE_REUSE,SIA_USB_LCID,P2);
        return TRUE;
    }
    if(Cmd==GLINK_CMD_CLOSE){
        SiaGlinkCmd(L,GLINK_CMD_CLOSE_ACK,P1,0);
        SiaUsbResetPeer(U);
        return TRUE;
    }
    return FALSE;
}
static NTSTATUS SiaUsbBatteryIo(PDEVICE_OBJECT Device,QCB_SOURCE *Source,QCB_POWER *Power)
{
    KEVENT Event;IO_STATUS_BLOCK Io;LARGE_INTEGER Timeout;
    KeInitializeEvent(&Event,NotificationEvent,FALSE);
    PIRP Irp=IoBuildDeviceIoControlRequest(QCB_POWER_EXCHANGE,Device,Source,sizeof(*Source),Power,sizeof(*Power),TRUE,&Event,&Io);
    if(!Irp)return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS Status=IoCallDriver(Device,Irp);
    if(Status==STATUS_PENDING){
        Timeout.QuadPart=-10000000LL;
        Status=KeWaitForSingleObject(&Event,Executive,KernelMode,FALSE,&Timeout);
        if(Status==STATUS_TIMEOUT){IoCancelIrp(Irp);KeWaitForSingleObject(&Event,Executive,KernelMode,FALSE,NULL);return STATUS_IO_TIMEOUT;}
        Status=Io.Status;
    }
    return Status;
}
static VOID SiaUsbBattery(SIA_USB *U,ULONGLONG Now)
{
    static const GUID Guid={0x72631e54,0x78a4,0x11d0,{0xbc,0xf7,0x00,0xaa,0x00,0xb7,0xb3,0x2a}};
    PWSTR Names=NULL;PFILE_OBJECT File;PDEVICE_OBJECT Device;UNICODE_STRING Name;
    QCB_SOURCE Source={U->OnlineAt,U->OnlineAt&&U->OnlineAt<=Now&&Now-U->OnlineAt<=QCB_POWER_MAX_AGE_MS,U->Otg.Live[1]};
    QCB_POWER P={0};
    NTSTATUS Status=IoGetDeviceInterfaces(&Guid,NULL,0,&Names);
    if(NT_SUCCESS(Status)){
        if(!Names[0])Status=STATUS_DEVICE_NOT_READY;
        else{
            RtlInitUnicodeString(&Name,Names);Status=IoGetDeviceObjectPointer(&Name,FILE_READ_DATA,&File,&Device);
            if(NT_SUCCESS(Status)){Status=SiaUsbBatteryIo(Device,&Source,&P);ObDereferenceObject(File);}
        }
        ExFreePool(Names);
    }
    U->BatteryAt=Now;U->BatteryUntil=0;
    if(NT_SUCCESS(Status)&&P.Valid&&P.SampleTimeMs&&P.SampleTimeMs<=Now&&Now-P.SampleTimeMs<=QCB_POWER_MAX_AGE_MS&&
        P.TemperatureDeciK>=2832&&P.TemperatureDeciK<=3182&&P.VoltageMv>=6500&&P.VoltageMv<=8600&&P.Soc>=5&&P.Soc<=100&&P.RemainingMah)
        U->BatteryUntil=P.SampleTimeMs+QCB_POWER_MAX_AGE_MS;
}
static VOID SiaUsbPoll(PSIA_LINK L)
{
    SIA_USB *U=&L->Usb;ULONGLONG Now=KeQueryInterruptTime()/10000;
    SiaUsbOpen(L);SiaUsbActivate(L);
    if(!U->Acked||!U->Rcid||!U->Count)return;
    if(U->Otg.Stop==3)OtgShutdown(&U->Otg,Now);
    if(U->Otg.Phase==OtgIdle)OtgStart(&U->Otg,Now);
    if(Now-U->BatteryAt>=500)SiaUsbBattery(U,Now);
    OTG_REQUEST R;
    if(!OtgNext(&U->Otg,Now,U->BatteryUntil&&Now<=U->BatteryUntil,&R))return;
    ULONG I;for(I=0;I<U->Count&&!(U->Free[I]&&U->Size[I]>=24);I++);
    if(I==U->Count)return;
    ULONG M[]={GLINK_CMD_TX_DATA|(SIA_USB_LCID<<16),U->Iid[I],24,0,32778,1,R.Write?0x33u:0x32u,0,R.Property,R.Value};
    if(SiaGlinkSend(L,(UCHAR*)M,sizeof(M))){U->Free[I]=FALSE;U->Property=R.Property;U->Write=R.Write;OtgSent(&U->Otg,&R,Now);}
}
