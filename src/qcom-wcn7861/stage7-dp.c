/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * QCC2072 interrupt-driven multi-vdev data path, based on ath12k's
 * wifi7/{hal_tx,dp_tx,dp_rx,hal_qcc2072}. Cookie/DMA ownership is validated
 * before every recycle; firmware-provided virtual pointers are never dereferenced. */
#include "wcn7861.h"
#include "stage6-protocol.h"
#include "stage7-util.h"
#include "stage7-rx-protocol.h"
#include "stage7-tx-protocol.h"
#include "security-policy.h"
#include "aggregation-policy.h"
#include "hotspot.h"
#define JRXPPT (WCN_DP_RX_PPT_BASE<<9)
static PWCN_DP_RING Dring(PWCN_DEVICE D,ULONG Id)
{ULONG I;for(I=0;I<D->Dp.RingCount;I++)if(D->Dp.Ring[I].Id==Id)return &D->Dp.Ring[I];return NULL;}
static BOOLEAN DcompletionRing(PWCN_DP_RING R)
{return !R->Source&&!R->Lmac&&((R->Id>=1&&R->Id<=8)||R->Id==21||R->Id==128||R->Id==130||R->Id==131||R->Id==132);}
static BOOLEAN Dpointer(PWCN_DEVICE D,PWCN_DP_RING R,ULONG *Value)
{
    if(!R||!R->Desc.Buffer)return FALSE;
    *Value=((volatile ULONG*)D->Dp.Rdp.Va)[R->Id];KeMemoryBarrier();
    if(*Value>=R->Entries*R->EntryDwords||*Value%R->EntryDwords){D->Runtime.RuntimeErrors++;WcnStage7FailAll(D,"invalid DMA ring pointer/alignment");return FALSE;}
    return TRUE;
}
static VOID Dpublish(PWCN_DEVICE D,PWCN_DP_RING R)
{
    KeMemoryBarrier();
    if(R->Lmac)((volatile ULONG*)D->Dp.Wrp.Va)[R->Id-160]=R->Head;
    else WcnWrite(D,R->PointerReg+(R->Source?0:4),R->Head);
}
static VOID DhttStation(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *P,ULONG N)
{
    ULONG Type,Info,I1,I2;
    if((!J->Started&&!J->Resetting)||(J->Failed&&!J->Resetting)||N<4)return;
    Info=Jle32(P);Type=Info&255;
    if((Type==3||Type==0x1e||Type==0x2b)&&N>=16&&((Info>>8)&255)==J->Vdev){
        I1=Jle32(P+8);I2=Jle32(P+12);
        if(!Jeq(P+4,J->Ap.Bssid,4)||!Jeq(P+8,J->Ap.Bssid+4,2))return;
        if(J->PeerMapped&&J->PeerId!=(Info>>16)){WcnStage7Fail(D,J,"peer map changed during connection");return;}
        J->PeerId=Info>>16;J->AstIndex=Type==0x2b?(I2&65535):(I1>>16);J->AstHash=Type==0x2b?(I2>>16):(I2&65535);J->PeerMapped=TRUE;

    }else if((Type==4||Type==0x1f)&&J->PeerMapped&&(Info>>16)==J->PeerId){if(J->Resetting){J->PeerUnmapped=TRUE;}else WcnStage7Fail(D,J,"selected peer unmapped by firmware");}
}
VOID WcnStage7Htt(PWCN_DEVICE D,const UCHAR *P,ULONG N)
{DhttStation(D,&D->Join,P,N);DhttStation(D,&D->Secondary,P,N);if(D->P2pClient)DhttStation(D,&D->P2pJoin,P,N);WcnTdlsHtt(D,P,N);}
VOID WcnStage7PeerHtt(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *P,ULONG N){DhttStation(D,J,P,N);}
NTSTATUS WcnStage7DpInit(PWCN_DEVICE D,PWCN_JOIN J)
{
    ULONG I,Tid,Ext,Ac,*Q;
    if(J->RuntimeReady)return STATUS_SUCCESS;
    if(!D->Dp.ReadyForWmi||!D->Dp.HttRxReady||!Dring(D,24)||!Dring(D,128)||!Dring(D,160))return STATUS_DEVICE_NOT_READY;
    if(!D->Runtime.Ready)JTRY(WcnDmaAllocAligned(D,WCN_TX_SLOTS*2048,0xfff,&D->Runtime.TxDma));
    if(!J->ReoQueue.Buffer)JTRY(WcnDmaAllocAligned(D,S7_RX_TID_COUNT*S7_RX_QUEUE_STRIDE,0xfff,&J->ReoQueue));
    RtlZeroMemory(J->ReoQueue.Va,J->ReoQueue.Length);
    for(Tid=0;Tid<S7_RX_TID_COUNT;Tid++){
        Q=(ULONG*)(J->ReoQueue.Va+Tid*S7_RX_QUEUE_STRIDE);
        Q[0]=(0xddbeefu<<8)|(8u<<4)|4u;Q[1]=Tid;
        /* ath12k_tid_to_ac uses WME order BE0/BK1/VI2/VO3, not the
         * different mac80211 queue-number order. Non-QoS16 maps to VO3. */
        Ac=(Tid==0||Tid==3)?0:((Tid==1||Tid==2)?1:((Tid==4||Tid==5)?2:3));
        Q[2]=1u|2u|(Ac<<5)|(1u<<21)|(1u<<25)|(1u<<27);
        if(Tid==16)Q[2]|=1u<<8; /* non-QoS window1, retry check */
        else{
            Q[2]|=1u<<11; /* Linux promotes QoS no-BA window1 to window2 */
            for(Ext=0;Ext<3;Ext++)Q[(128+Ext*128)/4]=((0xadbeefu+Ext*0x100000u)<<8)|(9u<<4)|4u;
        }
    }
    if(!D->Runtime.Ready)for(I=0;I<4095;I++)D->Runtime.RxPosted[I]=1;
    for(I=0;I<WCN_TX_SLOTS;I++)D->Dp.Cookies[I].BufferAddress=D->Runtime.TxDma.La+I*2048;
    if(!D->Runtime.Ready){
        ULONG Low=WcnDbiRead(D,DBI_MSI_ADDR_LO),High=WcnDbiRead(D,DBI_MSI_ADDR_HI),Vector=D->MultiMsi?16u:0u;
        if((!Low&&!High)||(High&~255u))return STATUS_DEVICE_CONFIGURATION_ERROR;
        /* QCC2072 destination SRNG offsets/enable bit match ath12k wifi7/hal.
         * Vector16 uses the existing DWC aggregate ISR; no per-packet polling. */
        for(I=0;I<D->Dp.RingCount;I++)if(DcompletionRing(&D->Dp.Ring[I])){
            PWCN_DP_RING R=&D->Dp.Ring[I];ULONG Base=R->RegBase;
            WcnWrite(D,Base+0x4c,0);WcnWrite(D,Base+0x48,Low);WcnWrite(D,Base+0x50,Vector);
            WcnWrite(D,Base+0x24,(8u*R->EntryDwords)|(8u<<16)); /* 8 entries or 64us */
            WcnWrite(D,Base+0x4c,High|0x100u);
            if(WcnRead(D,Base+0x48)!=Low||WcnRead(D,Base+0x4c)!=(High|0x100u)||WcnRead(D,Base+0x50)!=Vector)return STATUS_DEVICE_HARDWARE_ERROR;
        }

    }
    /* Each vdev owns two constant encapsulation banks. */
    WcnWrite(D,0xa4408c+J->Vdev*8,(1u<<1)|(1u<<9)|(1u<<11)|(1u<<14));
    WcnWrite(D,0xa44090+J->Vdev*8,(2u<<1)|(1u<<9)|(1u<<11)|(1u<<14));
    KeMemoryBarrier();D->Runtime.Ready=TRUE;J->RuntimeReady=TRUE;

    return STATUS_SUCCESS;
}
NTSTATUS WcnStage7Tx(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *F,ULONG N,BOOLEAN ClearEapol)
{
    if(!J->VdevStarted||!J->PeerMapped||J->Failed)return STATUS_DEVICE_NOT_READY;
    if(J->CsaState>=2||J->CsaBlock||J->KeyHold)return STATUS_DEVICE_BUSY;
    if(ClearEapol){if(N<14||Jbe16(F+12)!=0x888e||(!Jeq(F,J->Ap.Bssid,6)&&!Jeq(F,(const UCHAR*)"\x01\x80\xc2\x00\x00\x03",6)))return STATUS_INVALID_PARAMETER;}
    else if(!J->Authorized||(J->Ap.Auth!=WCN_AUTH_OPEN&&(!J->PairwiseInstalled||!J->GroupInstalled)))return STATUS_DEVICE_NOT_READY;
    NTSTATUS St=WcnTdlsAwake(D,J);if(!NT_SUCCESS(St))return St;if(!ClearEapol){St=WcnTdlsTx(D,J,F,N);if(St!=STATUS_NOT_FOUND){if(NT_SUCCESS(St))J->TxFrames++;return St;}}
    // EAPOL remains permitted through a closed controlled port; once a PTK
    // exists, use normal firmware encryption for four-way/group-key replies.
    St=WcnDataTx(D,F,N,ClearEapol&&!J->PairwiseInstalled,J->Vdev,J->MacId,J->Mac,J->Ap.Bssid,J->AstIndex,J->AstHash,FALSE);
    if(NT_SUCCESS(St))J->TxFrames++;return St;
}
NTSTATUS WcnDataTx(PWCN_DEVICE D,const UCHAR *F,ULONG N,BOOLEAN ClearEapol,ULONG Vdev,ULONG MacId,const UCHAR Local[6],const UCHAR Receiver[6],ULONG Ast,ULONG Hash,BOOLEAN LookupOverride)
{
    PWCN_DP_RING R=Dring(D,24);ULONG I,Tail,Next,*B,Length=N;UCHAR *Out;ULONG64 Pa;
    if(!D->Runtime.Ready||D->Mhi.SysErr||D->Mhi.RddmSaved||D->Dp.Failed)return STATUS_DEVICE_NOT_READY;
    if(N<14||N>1514||Vdev>2||MacId>1||((Vdev!=1||D->P2pClient)&&!Jeq(F+6,Local,6)))return STATUS_INVALID_PARAMETER;
    if(ClearEapol){if(Jbe16(F+12)!=0x888e)return STATUS_INVALID_PARAMETER;Length=N+18;}
    if(!Dpointer(D,R,&Tail))return STATUS_DEVICE_HARDWARE_ERROR;
    Next=(R->Head+8)%(R->Entries*8);if(Next==Tail)return STATUS_DEVICE_BUSY;
    for(I=0;I<WCN_TX_SLOTS;I++)if(!D->Runtime.TxUsed[I])break;if(I==WCN_TX_SLOTS)return STATUS_DEVICE_BUSY;
    Out=D->Runtime.TxDma.Va+I*2048;Pa=D->Runtime.TxDma.La+I*2048; /* zeroed on allocation and before completion releases ownership */
    if(ClearEapol){
        /* Native WiFi, unprotected legacy data ToDS. Firmware supplies the
         * sequence and uses the AP AST index; no WMI data-frame shortcut. */
        Out[0]=8;Out[1]=Vdev==1&&!D->P2pClient?2:1;RtlCopyMemory(Out+4,Receiver,6);RtlCopyMemory(Out+10,Local,6);RtlCopyMemory(Out+16,Vdev==1&&!D->P2pClient?F+6:F,6);
        RtlCopyMemory(Out+24,"\xaa\xaa\x03\x00\x00\x00",6);RtlCopyMemory(Out+30,F+12,N-12);
    }else RtlCopyMemory(Out,F,N);
    /* Native-WiFi and Ethernet banks use firmware peer keys (the bank's
     * encryption selector only applies to RAW). Linux STA requires addressY,
     * index lookup and vdev checking; AST comes from the actual HTT map. */
    B=(ULONG*)(R->Desc.Va+R->Head*4);RtlZeroMemory(B,32);
    B[0]=(ULONG)Pa;B[1]=(ULONG)(Pa>>32)|(5u<<8)|(I<<12); /* SW0_BM -> WBM ring128 */
    B[2]=(ClearEapol?0u:4u)+(Vdev*8u); /* bank0 native, bank1 Ethernet */
    B[3]=(1u|(Vdev<<3)|(1u<<11))<<16; /* actual vdev, pdev1; no HTT extension */
    B[4]=Length;B[5]=1u|(MacId<<9)|(Vdev<<24); /* TID overwrite0, actual firmware MAC routing */
    B[6]=(Ast&0xfffffu)|((Hash&15)<<20)|(LookupOverride?(1u<<24):0);
    D->Runtime.TxUsed[I]=(UCHAR)((ClearEapol?2:1)|WcnTxOwner(Vdev)|((LookupOverride&&Vdev!=1)?0x20:0));R->Head=Next;

    return STATUS_SUCCESS;
}
VOID WcnStage7TxFlush(PWCN_DEVICE D)
{Dpublish(D,Dring(D,24));}
static VOID DtxDone(PWCN_DEVICE D,const ULONG *B)
{
    PWCN_JOIN J=NULL;ULONG Vdev=0,Cookie,Info=B[2],Source,Status;BOOLEAN Ack;ULONG64 Va,Base;
    if(Info&(1u<<30)){
        Va=(ULONG64)B[0]|((ULONG64)B[1]<<32);Base=(ULONG64)(ULONG_PTR)D->Dp.Cookies;
        if(Va<Base||Va>=Base+WCN_TX_SLOTS*sizeof(WCN_DP_COOKIE)||(Va-Base)%sizeof(WCN_DP_COOKIE))goto Bad;
        Cookie=(ULONG)((Va-Base)/sizeof(WCN_DP_COOKIE));
    }else{Cookie=B[1]>>12;if(Cookie>=WCN_TX_SLOTS||B[0]!=(ULONG)(D->Runtime.TxDma.La+Cookie*2048))goto Bad;}
    if(!D->Runtime.TxUsed[Cookie]||(Info&0x80000000u))goto Bad;
    Vdev=WcnOwnerVdev(D->Runtime.TxUsed[Cookie]);J=WcnStation(D,Vdev);
    Source=(Info&S7_TX_SOURCE_MASK)>>S7_TX_SOURCE_SHIFT;
    if(Source==S7_TX_SOURCE_FW){
        Status=(Info&S7_HTT_TX_STATUS_MASK)>>S7_HTT_TX_STATUS_SHIFT;
        Ack=Status==S7_HTT_TX_STATUS_OK; /* ath12k's HTT FW completion sets ts.acked */
    }else if(Source==S7_TX_SOURCE_TQM){
        Status=(Info&S7_TQM_TX_REASON_MASK)>>S7_TQM_TX_REASON_SHIFT;
        Ack=Status==S7_TQM_TX_FRAME_ACKED;
    }else{
        WcnLog(D,"stage7: unexpected TX completion source=%u info=0x%x; no ownership release or ACK",Source,Info);
        goto Bad;
    }
    if(Source==S7_TX_SOURCE_TQM&&(D->Runtime.TxUsed[Cookie]&0x23)==1&&J&&(B[5]&1)&&((B[5]>>4)&15)>=2)J->TxMcs=(B[5]>>12)&15;
    D->Runtime.TxCompleted++;
    if(Ack){D->Runtime.TxAcked++;if(J&&!(D->Runtime.TxUsed[Cookie]&0x20))J->PeerSeenAt=WcnNowMs();}else if((D->Runtime.TxUsed[Cookie]&0x80)&&!D->P2pClient)WcnHotspotTxFailed(D);else WcnStage8TxFailed(D,Vdev);

    RtlSecureZeroMemory(D->Runtime.TxDma.Va+Cookie*2048,2048);D->Runtime.TxUsed[Cookie]=0;return;
Bad:D->Runtime.RuntimeErrors++;WcnStage7FailAll(D,"invalid/duplicate TX completion cookie, DMA identity or source");
}

/* NULL queue means REO could not perform reordering/PN validation. Do not
 * treat it as generic success: only the observed REO/NULL/MSDU class is
 * eligible, then hardware CCMP integrity plus our per-key/TID replay check
 * is mandatory. The same replay table is shared with normal REO delivery. */
static ULONG DrxReplay(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *Buffer,const ULONG *B,BOOLEAN Error,ULONG Fc,const UCHAR *F)
{
    ULONG Valid=Jle32(Buffer+176),I2=Jle32(Buffer+148),Key=Jle32(Buffer+192)&255;
    ULONG Mpdu=Error?B[3]:B[2],Meta=Error?B[4]:B[3],Group=Buffer[204]&1,Tid=(Fc&0x80)?((I2>>15)&15):16,Mask,Msdu=Error?B[5]:B[4];
    ULONG64 Pn=(ULONG64)Jle32(Buffer+160)|((ULONG64)(Jle32(Buffer+164)&65535)<<32),Last;
    if((!J->Authorized&&(Group||Jbe16(F+12)!=0x888e))||!J->PairwiseInstalled||!J->PeerMapped||J->KeyHold)return 14;
    if((Valid&0x4020du)!=0x4020du||!(Mpdu&(1u<<12))||((I2>>2)&15)!=6)return 15;
    if((Fc&0x80)&&!(Valid&(1u<<7)))return 15;
    if(D->Ce.PeerMetadataVersion>1)return 15;
    Mask=D->Ce.PeerMetadataVersion==1?0x3fff:0xffff;
    if((Meta&Mask)!=J->PeerId||((Meta>>16)&255)!=J->Vdev||!Jeq(Buffer+204,F,6))return 15;
    if(!Group&&!Jeq(Buffer+204,J->Mac,6))return 15;
    ULONG Slot=Group?Key+1u:0;
    if(Key>3||(!Group&&Key)||!J->Cipher[Slot].Installed||J->KeyUpdate.Slot==Slot+1)return 15;
    Last=J->RxPnValid[Slot][Tid]?J->RxPn[Slot][Tid]:J->Cipher[Slot].Floor;
    if(!WcnRxPnAccept(Pn,&Last,&J->RxPnValid[Slot][Tid],Msdu,!Error&&!Group&&(Fc&0x80)))return 16;
    J->RxPn[Slot][Tid]=Last;
    return 0;
}
ULONG WcnStage7RxReplay(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *Buffer,const ULONG *Desc,BOOLEAN Error,ULONG Fc,const UCHAR *F){return DrxReplay(D,J,Buffer,Desc,Error,Fc,F);}
static VOID DrxDone(PWCN_DEVICE D,const ULONG *B,BOOLEAN Error)
{
    PWCN_JOIN J=&D->Join;ULONG Cookie,Index,Length,Pad,Info13,Info14,Decap,Head,Route,Drop=0,Fc,Valid,Decrypt,Type=0,Mpdu,Msdu,Rbm;BOOLEAN NullQueue=FALSE;ULONG64 Pa;UCHAR *Buffer;
    if(Error&&(B[2]&(1u<<16))){Cookie=(B[7]>>8)&0xfffff;Pa=B[6]|((ULONG64)(B[7]&255)<<32);}
    else{Cookie=B[1]>>12;Pa=B[0]|((ULONG64)(B[1]&255)<<32);}
    if(Cookie<JRXPPT||Cookie>=JRXPPT+4095)goto Bad;
    Index=Cookie-JRXPPT;
    if(!D->Runtime.RxPosted[Index]||Pa!=D->Dp.Cookies[Cookie].BufferAddress)goto Bad;
    D->Runtime.RxPosted[Index]=0;Buffer=D->Dp.RxBuffer[Index/256].Va+(Index%256)*2048;
    KeMemoryBarrier();Info13=Jle32(Buffer+124);Info14=Jle32(Buffer+128);
    Length=Jle32(Buffer+76)&0x3fff;Pad=(Jle16(Buffer+46)>>10)&3;Decap=(Jle32(Buffer+80)>>8)&3;
    Route=(B[7]&S7_REO_PUSH_REASON_MASK)>>S7_REO_PUSH_REASON_SHIFT;
    Fc=Jle16(Buffer+200);Valid=Jle32(Buffer+176);Decrypt=(Info14>>10)&7;
    Mpdu=Error?B[3]:B[2];Msdu=Error?B[5]:B[4];
    Rbm=Error?((B[2]&(1u<<16))?((B[2]>>9)&15):((B[1]>>8)&15)):0;
    if(Error)NullQueue=(B[2]&7)==2&&((B[2]>>6)&7)==0&&!(B[2]&0x80000000u)&&
        ((B[2]>>24)&3)==0&&((B[2]>>26)&31)==0&&Rbm==8;
    if(Error&&!NullQueue)Drop=1;
    else if(!Error&&((B[7]&1)||Route!=S7_REO_PUSH_ROUTING))Drop=2;
    else if((Mpdu&((1u<<8)|(1u<<13)|(1u<<14)))||(Fc&(1u<<10)))Drop=3;
    else if(!(Info14&0x80000000u))Drop=4;
    else if(Info13&0xfcc39000u)Drop=5;
    else if(Decap!=2)Drop=6;
    else if(Length<14||Length>2048-S6_QCC_DESCRIPTOR_BYTES-Pad)Drop=7;
    else if((Msdu&((1u<<2)|(1u<<17)))||(NullQueue&&((Mpdu&255)!=1||(Msdu&3)!=3)))Drop=8;
    else if((Valid&9)!=9)Drop=9;
    else if(WcnHotspotRx(D,Buffer,Length,Pad,Error,B))goto Recycle;
    else if((J=WcnStation(D,((Error?B[4]:B[3])>>16)&255))==NULL)Drop=10;
    else if(((Error?B[4]:B[3])&(D->Ce.PeerMetadataVersion==1?0x3fffu:0xffffu))!=J->PeerId&&WcnTdlsRx(D,J,Buffer,Length,Pad,Error,B))goto Recycle;
    else if(!J->PeerMapped||((Error?B[4]:B[3])&(D->Ce.PeerMetadataVersion==1?0x3fffu:0xffffu))!=J->PeerId||!Jeq(Buffer+210,J->Ap.Bssid,6))Drop=10;
    else if((Fc&0x030c)!=0x0208)Drop=11;
    if(!Drop){
        const UCHAR *Frame=Buffer+S6_QCC_DESCRIPTOR_BYTES+Pad;
        Type=Jbe16(Frame+12);
        /* Only EAPOL can be unprotected. Protected EAPOL/data both require
         * successful hardware decryption. Never weaken cookie, CRC, peer,
         * direction, length, replay-route or controlled-port checks. */
        if(NullQueue&&((Type!=0x0800&&Type!=0x0806&&Type!=0x86dd)||
           (!Jeq(Frame,J->Mac,6)&&!Jeq(Frame,(const UCHAR*)"\xff\xff\xff\xff\xff\xff",6)&&!(Frame[0]&1))))Drop=17;
        if(!Drop&&(Fc&0x4000)){if(Decrypt!=0)Drop=12;else Drop=DrxReplay(D,J,Buffer,B,Error,Fc,Frame);}
        if(!Drop&&!(Fc&0x4000)){
            if(Decrypt!=0&&Decrypt!=1)Drop=13;
            else if(Type!=0x888e&&(J->Ap.Auth!=WCN_AUTH_OPEN||!J->Authorized))Drop=13;
            else if(NullQueue&&J->Ap.Auth!=WCN_AUTH_OPEN)Drop=13;
        }
        if(!Drop){ULONG Rate=Jle32(Buffer+92);if(((Rate>>8)&15)>=2)J->RxMcs=(Rate>>14)&15;
            J->RxFrames++;WcnStage8Ethernet(D,J,Frame,Length);}
    }
    if(Drop)D->Runtime.RxDropped++;


Recycle:
    Head=(D->Runtime.RxPendingHead+1)&4095;if(Head==D->Runtime.RxPendingTail)goto Bad;
    D->Runtime.RxPending[D->Runtime.RxPendingHead]=Index;D->Runtime.RxPendingHead=Head;return;
Bad:D->Runtime.RuntimeErrors++;WcnStage7FailAll(D,"RX cookie/physical-address/ownership or recycle queue violation");
}
static VOID Drefill(PWCN_DEVICE D)
{
    PWCN_DP_RING R=Dring(D,160);ULONG Tail,Next,Index,Cookie,*B;ULONG64 Pa;
    if(!Dpointer(D,R,&Tail))return;
    while(D->Runtime.RxPendingTail!=D->Runtime.RxPendingHead){
        Next=(R->Head+2)%(R->Entries*2);if(Next==Tail)break;
        Index=D->Runtime.RxPending[D->Runtime.RxPendingTail];Cookie=JRXPPT+Index;
        if(Index>=4095||D->Runtime.RxPosted[Index]){D->Runtime.RuntimeErrors++;WcnStage7FailAll(D,"RX double refill");return;}
        RtlZeroMemory(D->Dp.RxBuffer[Index/256].Va+(Index%256)*2048,2048);
        Pa=D->Dp.Cookies[Cookie].BufferAddress;B=(ULONG*)(R->Desc.Va+R->Head*4);
        B[0]=(ULONG)Pa;B[1]=(ULONG)(Pa>>32)|(6u<<8)|(Cookie<<12);
        D->Runtime.RxPosted[Index]=1;R->Head=Next;D->Runtime.RxPendingTail=(D->Runtime.RxPendingTail+1)&4095;
    }
    Dpublish(D,R);
}
static NTSTATUS DreoCommand(PWCN_DEVICE D,ULONG Owner,ULONG64 Base,ULONG Tid,ULONG Offset,ULONG Tag,ULONG Info0,ULONG Info1,ULONG Info2)
{
    PWCN_DP_RING R=Dring(D,20);ULONG Tail,Next,*B,Num;ULONG64 Pa=Base+Tid*S7_RX_QUEUE_STRIDE+Offset;
    if(D->Runtime.ReoPending)return STATUS_DEVICE_BUSY;
    if(!R||R->EntryDwords!=10||!D->Runtime.Ready)return STATUS_DEVICE_NOT_READY;
    if(!Dpointer(D,R,&Tail))return STATUS_DEVICE_HARDWARE_ERROR;
    Next=(R->Head+R->EntryDwords)%(R->Entries*R->EntryDwords);if(Next==Tail)return STATUS_DEVICE_BUSY;
    Num=R->Head/R->EntryDwords+1;B=(ULONG*)(R->Desc.Va+R->Head*4);RtlZeroMemory(B,40);
    B[0]=(Tag<<1)|(36u<<10);B[1]=Num|(1u<<16);B[2]=(ULONG)Pa;B[3]=(ULONG)(Pa>>32)|Info0;B[4]=Info1;B[5]=Info2;
    D->Runtime.ReoBase=Base;D->Runtime.ReoOwner=Owner;D->Runtime.ReoCmd=(USHORT)Num;D->Runtime.ReoTid=(USHORT)Tid;
    D->Runtime.ReoTag=(USHORT)(Tag==339?172:287);D->Runtime.ReoPending=TRUE;D->Runtime.ReoDone=FALSE;
    R->Head=Next;Dpublish(D,R);return STATUS_SUCCESS;
}
NTSTATUS WcnReoBaUpdate(PWCN_DEVICE D,ULONG Owner,ULONG64 Reo,ULONG Tid,ULONG Window,ULONG Ssn)
{
    ULONG Flags=1u<<18,Info=Window<2?1:Window-1;
    if(Tid>=16||!Window||Window>S7_RX_BA_MAX)return STATUS_INVALID_PARAMETER;
    if(Ssn<=4095){Flags|=(1u<<25)|(1u<<26);Info|=(1u<<12)|(Ssn<<13);}
    NTSTATUS St=DreoCommand(D,Owner,Reo,Tid,0,339,Flags,0,Info);
    return St;
}
NTSTATUS WcnReoKeyUpdate(PWCN_DEVICE D,PWCN_JOIN J,ULONG Tid,BOOLEAN Enable)
{
    if(Tid>=S7_RX_TID_COUNT)return STATUS_INVALID_PARAMETER;
    /* PEACH PN reset matches ath12k: update PN/check/size/valid + SVLD;
     * leave BA window, SSN, queue identity and DMA ownership untouched. */
    return DreoCommand(D,J->Vdev,J->ReoQueue.La,Tid,0,339,(1u<<19)|(1u<<23)|(1u<<25)|(1u<<29)|(1u<<30),Enable?1u<<27:0,Enable?1u<<10:0);
}
static VOID DreoStatus(PWCN_DEVICE D,const ULONG *B)
{
    ULONG Tag=(B[0]>>1)&511,Num=B[2]&65535,Status=(B[2]>>26)&3;
    if(Tag!=D->Runtime.ReoTag||!D->Runtime.ReoPending||D->Runtime.ReoCmd!=Num)return;
    if(Tag==287&&(B[4]&0x30007u))Status=4;
    D->Runtime.ReoStatus=Status;D->Runtime.ReoDone=TRUE;
    if(Status)WcnLog(D,"reo: command rejected vdev=%u tag=%u command=%u tid=%u status=%u detail=0x%x",D->Runtime.ReoOwner,Tag,Num,D->Runtime.ReoTid,Status,B[4]);
}
NTSTATUS WcnStage7ReoRelease(PWCN_DEVICE D,PWCN_JOIN J)
{
    ULONG Tid,Offset=0,Tag,Info0,Info1=0;NTSTATUS St;
    if(!J->RuntimeReady)return STATUS_SUCCESS;
    if(!J->Resetting||(J->Tdls?!J->PeerDeleteConfirmed:!J->VdevStopConfirmed)||(J->PeerMapped&&!J->PeerUnmapped))return STATUS_INVALID_DEVICE_STATE;
    if(D->Runtime.ReoPending){
        if(D->Runtime.ReoOwner!=J->Vdev)return STATUS_DEVICE_BUSY;
        if(D->Runtime.ReoBase!=J->ReoQueue.La)return STATUS_DEVICE_BUSY;
        if(!D->Runtime.ReoDone)return STATUS_PENDING;
        ULONG Status=D->Runtime.ReoStatus;D->Runtime.ReoPending=D->Runtime.ReoDone=FALSE;
        if(Status)return STATUS_DEVICE_HARDWARE_ERROR;
        if(++J->ReoReleaseStep==S7_RX_TID_COUNT)J->ReoReleaseAt=WcnNowMs();
    }
    if(J->ReoReleaseStep<S7_RX_TID_COUNT){
        Tid=J->ReoReleaseStep;Tag=339;Info0=1u<<9;Info1=1u<<16;
        // Match ath12k's PEACH workaround: retain VLD during the delete update.
    }else{
        if(WcnNowMs()-J->ReoReleaseAt<1000)return STATUS_PENDING;
        ULONG Block=J->ReoReleaseStep-S7_RX_TID_COUNT;
        if(Block==65){J->RuntimeReady=FALSE;return STATUS_SUCCESS;}
        Tid=Block<64?Block/4:16;Offset=Block<64?(3-Block%4)*128:0;Tag=283;Info0=Offset?0:1u<<8;
        // Our QoS descriptors are 512 bytes, not Linux's 1KB layout. Flush
        // each 128-byte extension before its base, with a checked completion.
    }
    St=DreoCommand(D,J->Vdev,J->ReoQueue.La,Tid,Offset,Tag,Info0,Info1,0);
    return NT_SUCCESS(St)?STATUS_PENDING:St;
}
VOID WcnStage7DpPoll(PWCN_DEVICE D)
{
    PWCN_DP_RING R;ULONG I,Head,Count,B[8];
    if(!D->Runtime.Ready||D->Runtime.RuntimeErrors||D->Mhi.SysErr||D->Mhi.RddmSaved)return;
    for(I=0;I<D->Dp.RingCount;I++){
        R=&D->Dp.Ring[I];
        if(!DcompletionRing(R))continue;
        if(!Dpointer(D,R,&Head))return;
        Count=0;
        while(R->Head!=Head&&Count++<256){
            RtlCopyMemory(B,R->Desc.Va+R->Head*4,32);
            if(R->Id==21)DreoStatus(D,B);
            else if(R->Id==128||R->Id==130||R->Id==132)DtxDone(D,B);
            else if(R->Id==131)DrxDone(D,B,TRUE);
            else if(!(B[7]&1))DrxDone(D,B,FALSE);
            else {D->Runtime.RuntimeErrors++;WcnStage7FailAll(D,"unsupported chained REO descriptor; retaining DMA ownership");}
            if(D->Runtime.RuntimeErrors)return;
            R->Head=(R->Head+R->EntryDwords)%(R->Entries*R->EntryDwords);
        }
        Dpublish(D,R);
        if(R->Head!=Head)KeSetEvent(&D->WorkEvent,IO_NO_INCREMENT,FALSE);
    }
    Drefill(D);
}

BOOLEAN WcnStage7DataIdle(PWCN_DEVICE D,PWCN_JOIN J)
{
    for(ULONG I=0;I<WCN_TX_SLOTS;I++)if(D->Runtime.TxUsed[I]&&WcnOwnerVdev(D->Runtime.TxUsed[I])==J->Vdev)return FALSE;
    // RX DMA and other links remain live; only this owner's completed BA
    // command is retired before its asynchronous REO cleanup begins.
    if(D->Runtime.ReoPending&&D->Runtime.ReoOwner==J->Vdev&&D->Runtime.ReoBase==J->ReoQueue.La){if(!D->Runtime.ReoDone)return FALSE;D->Runtime.ReoPending=D->Runtime.ReoDone=FALSE;}
    return !J->MgmtBusy;
}

BOOLEAN WcnStage7RxIdle(PWCN_DEVICE D)
{
    ULONG I,Head;for(I=0;I<D->Dp.RingCount;I++){PWCN_DP_RING R=&D->Dp.Ring[I];
        if(!R->Source&&!R->Lmac&&((R->Id>=1&&R->Id<=8)||R->Id==131)&&(!Dpointer(D,R,&Head)||R->Head!=Head))return FALSE;}
    return TRUE;
}
