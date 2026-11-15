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
    for(ULONG I=1;I<=8;I++)SiaUsbGrant(L,I,512);
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
        if(Cmd==GLINK_CMD_TX_DATA&&!Frame[1]&&Frame[0]<=512){
            UCHAR Payload[512];SiaFifoRead(L,Tail+16,Payload,Frame[0]);QchReceive(L,Payload,Frame[0]);
        }
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
        if(L->Charge.Bridge==2){L->Charge.Fault=13;QchOff(L);}
        SiaGlinkCmd(L,GLINK_CMD_CLOSE_ACK,P1,0);
        SiaUsbResetPeer(U);
        return TRUE;
    }
    return FALSE;
}
static VOID SiaUsbPoll(PSIA_LINK L)
{
    SIA_USB *U=&L->Usb;ULONGLONG Now=KeQueryInterruptTime()/10000;
    SiaUsbOpen(L);SiaUsbActivate(L);
    if(!U->Acked||!U->Rcid||!U->Count)return;
    if(QchSharedPoll(L,Now))return;
    if(U->Otg.Stop==3)OtgShutdown(&U->Otg,Now);
    if(U->Otg.Phase==OtgIdle)OtgStart(&U->Otg,Now);
    OTG_REQUEST R;
    if(!OtgNext(&U->Otg,Now,U->BatteryUntil&&Now<=U->BatteryUntil,&R))return;
    ULONG I;for(I=0;I<U->Count&&!(U->Free[I]&&U->Size[I]>=24);I++);
    if(I==U->Count)return;
    ULONG M[]={GLINK_CMD_TX_DATA|(SIA_USB_LCID<<16),U->Iid[I],24,0,32778,1,R.Write?0x33u:0x32u,0,R.Property,R.Value};
    if(SiaGlinkSend(L,(UCHAR*)M,sizeof(M))){U->Free[I]=FALSE;U->Property=R.Property;U->Write=R.Write;OtgSent(&U->Otg,&R,Now);}
}
