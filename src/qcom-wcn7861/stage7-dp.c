/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * QCC2072 polling runtime for bounded stage7 packets, based on ath12k's
 * wifi7/{hal_tx,dp_tx,dp_rx,hal_qcc2072}. Cookie/DMA ownership is validated
 * before every recycle; firmware-provided virtual pointers are never dereferenced. */
#include "wcn7861.h"
#include "stage6-protocol.h"
#include "stage7-util.h"
#include "stage7-rx-protocol.h"
#include "stage7-tx-protocol.h"
#include "security-policy.h"
#define JRXPPT (WCN_DP_RX_PPT_BASE<<9)
static PWCN_DP_RING Dring(PWCN_DEVICE D,ULONG Id)
{ULONG I;for(I=0;I<D->Dp.RingCount;I++)if(D->Dp.Ring[I].Id==Id)return &D->Dp.Ring[I];return NULL;}
static BOOLEAN Dpointer(PWCN_DEVICE D,PWCN_DP_RING R,ULONG *Value)
{
    if(!R||!R->Desc.Buffer)return FALSE;
    *Value=((volatile ULONG*)D->Dp.Rdp.Va)[R->Id];KeMemoryBarrier();
    if(*Value>=R->Entries*R->EntryDwords||*Value%R->EntryDwords){D->Join.RuntimeErrors++;WcnStage7Fail(D,"invalid DMA ring pointer/alignment");return FALSE;}
    return TRUE;
}
static VOID Dpublish(PWCN_DEVICE D,PWCN_DP_RING R)
{
    KeMemoryBarrier();
    if(R->Lmac)((volatile ULONG*)D->Dp.Wrp.Va)[R->Id-160]=R->Head;
    else WcnWrite(D,R->PointerReg+(R->Source?0:4),R->Head);
}
VOID WcnStage7Htt(PWCN_DEVICE D,const UCHAR *P,ULONG N)
{
    PWCN_JOIN J=&D->Join;ULONG Type,Info,I1,I2;
    if((!J->Started&&!J->Resetting)||(J->Failed&&!J->Resetting)||N<4)return;
    Info=Jle32(P);Type=Info&255;
    if((Type==3||Type==0x1e||Type==0x2b)&&N>=16&&((Info>>8)&255)==0){
        I1=Jle32(P+8);I2=Jle32(P+12);
        if(!Jeq(P+4,J->Ap.Bssid,4)||!Jeq(P+8,J->Ap.Bssid+4,2))return;
        if(J->PeerMapped&&J->PeerId!=(Info>>16)){WcnStage7Fail(D,"peer map changed during connection");return;}
        J->PeerId=Info>>16;J->AstIndex=Type==0x2b?(I2&65535):(I1>>16);J->AstHash=Type==0x2b?(I2>>16):(I2&65535);J->PeerMapped=TRUE;
        WcnLog(D,"stage7: HTT peer map type=0x%x peer=%u AST=%u hash=0x%x",Type,J->PeerId,J->AstIndex,J->AstHash);
    }else if((Type==4||Type==0x1f)&&J->PeerMapped&&(Info>>16)==J->PeerId){if(J->Resetting){J->PeerUnmapped=TRUE;WcnLog(D,"stage8: matched HTT peer unmap during teardown peer=%u",J->PeerId);}else WcnStage7Fail(D,"selected peer unmapped by firmware");}
}
NTSTATUS WcnStage7DpInit(PWCN_DEVICE D)
{
    PWCN_JOIN J=&D->Join;ULONG I,Tid,Ext,Ac,*Q;
    if(J->RuntimeReady&&!J->FreshReo)return STATUS_SUCCESS;
    if(J->FreshReo){
        WCN_DMA New={0};if(J->RetiredReoCount>=8)return STATUS_INSUFFICIENT_RESOURCES;
        JTRY(WcnDmaAllocAligned(D,S7_RX_TID_COUNT*S7_RX_QUEUE_STRIDE,0xfff,&New));
        J->RetiredReo[J->RetiredReoCount++]=J->ReoQueue;J->ReoQueue=New;
    }
    if(!D->Dp.ReadyForWmi||!D->Dp.HttRxReady||!Dring(D,24)||!Dring(D,128)||!Dring(D,160))return STATUS_DEVICE_NOT_READY;
    if(!J->RuntimeReady){JTRY(WcnDmaAllocAligned(D,64*2048,0xfff,&J->TxDma));
    JTRY(WcnDmaAllocAligned(D,S7_RX_TID_COUNT*S7_RX_QUEUE_STRIDE,0xfff,&J->ReoQueue));}
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
    if(!J->RuntimeReady)for(I=0;I<4095;I++)J->RxPosted[I]=1;
    for(I=0;I<64;I++)D->Dp.Cookies[I].BufferAddress=J->TxDma.La+I*2048;
    KeMemoryBarrier();J->RuntimeReady=TRUE;J->FreshReo=FALSE;
    WcnLog(D,"stage7: runtime prepared TX64, RX4095 recycling, 17 RX TID queues/512-byte slots; QCC offsets independently verified");
    return STATUS_SUCCESS;
}
NTSTATUS WcnStage7Tx(PWCN_DEVICE D,const UCHAR *F,ULONG N,BOOLEAN ClearEapol,UCHAR Purpose)
{
    PWCN_JOIN J=&D->Join;PWCN_DP_RING R=Dring(D,24);ULONG I,Tail,Next,*B,Length=N;UCHAR *Out;ULONG64 Pa;
    if(!J->RuntimeReady||!J->VdevStarted||!J->PeerMapped||J->Failed||D->Mhi.SysErr||D->Mhi.RddmSaved)return STATUS_DEVICE_NOT_READY;
    if(N<14||N>1514||!Jeq(F+6,D->Ce.MacAddress,6))return STATUS_INVALID_PARAMETER;
    if(ClearEapol){if(Jbe16(F+12)!=0x888e||(!Jeq(F,J->Ap.Bssid,6)&&!Jeq(F,(const UCHAR*)"\x01\x80\xc2\x00\x00\x03",6)))return STATUS_INVALID_PARAMETER;Length=N+18;}
    else if(!J->Authorized||(J->Ap.Auth!=WCN_AUTH_OPEN&&(!J->PairwiseInstalled||!J->GroupInstalled)))return STATUS_DEVICE_NOT_READY;
    if(!Dpointer(D,R,&Tail))return STATUS_DEVICE_HARDWARE_ERROR;
    Next=(R->Head+8)%(R->Entries*8);if(Next==Tail)return STATUS_DEVICE_BUSY;
    for(I=0;I<64;I++)if(!J->TxUsed[I])break;if(I==64)return STATUS_DEVICE_BUSY;
    Out=J->TxDma.Va+I*2048;Pa=J->TxDma.La+I*2048;RtlZeroMemory(Out,2048);
    if(ClearEapol){
        /* Native WiFi, unprotected legacy data ToDS. Firmware supplies the
         * sequence and uses the AP AST index; no WMI data-frame shortcut. */
        Out[0]=8;Out[1]=1;RtlCopyMemory(Out+4,J->Ap.Bssid,6);RtlCopyMemory(Out+10,D->Ce.MacAddress,6);RtlCopyMemory(Out+16,F,6);
        RtlCopyMemory(Out+24,"\xaa\xaa\x03\x00\x00\x00",6);RtlCopyMemory(Out+30,F+12,N-12);
    }else RtlCopyMemory(Out,F,N);
    /* Native-WiFi and Ethernet banks use firmware peer keys (the bank's
     * encryption selector only applies to RAW). Linux STA requires addressY,
     * index lookup and vdev checking; AST comes from the actual HTT map. */
    WcnWrite(D,0xa4408c,(1u<<1)|(1u<<9)|(1u<<11)|(1u<<14));
    WcnWrite(D,0xa44090,(2u<<1)|(1u<<9)|(1u<<11)|(1u<<14));
    B=(ULONG*)(R->Desc.Va+R->Head*4);RtlZeroMemory(B,32);
    B[0]=(ULONG)Pa;B[1]=(ULONG)(Pa>>32)|(5u<<8)|(I<<12); /* SW0_BM -> WBM ring128 */
    B[2]=ClearEapol?0:4; /* bank0 native, bank1 Ethernet */
    B[3]=(1u|(1u<<11))<<16; /* vdev-metadata type1, vdev0, pdev1, HTT-extension invalid */
    B[4]=Length;B[5]=1u|(J->MacId<<9); /* TID overwrite0, actual firmware MAC routing */
    B[6]=(J->AstIndex&0xfffffu)|((J->AstHash&15)<<20);
    J->TxUsed[I]=1;J->TxKind[I]=Purpose;R->Head=Next;Dpublish(D,R);
    WcnLog(D,"stage7: data TX submitted cookie=%u purpose=%u length=%u encapsulation=%s",I,Purpose,Length,ClearEapol?"native-clear-EAPOL":"Ethernet-peer-key");
    return STATUS_SUCCESS;
}
static VOID DtxDone(PWCN_DEVICE D,const ULONG *B)
{
    PWCN_JOIN J=&D->Join;ULONG Cookie,Info=B[2],Source,Status;BOOLEAN Ack;ULONG64 Va,Base;
    if(Info&(1u<<30)){
        Va=(ULONG64)B[0]|((ULONG64)B[1]<<32);Base=(ULONG64)(ULONG_PTR)D->Dp.Cookies;
        if(Va<Base||Va>=Base+64*sizeof(WCN_DP_COOKIE)||(Va-Base)%sizeof(WCN_DP_COOKIE))goto Bad;
        Cookie=(ULONG)((Va-Base)/sizeof(WCN_DP_COOKIE));
    }else{Cookie=B[1]>>12;if(Cookie>=64||B[0]!=(ULONG)(J->TxDma.La+Cookie*2048))goto Bad;}
    if(!J->TxUsed[Cookie]||(Info&0x80000000u))goto Bad;
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
    WcnStage8TxDone(D,J->TxKind[Cookie],Ack);J->TxCompleted++;
    if(Ack)J->TxAcked++;
    WcnLog(D,"stage7: data TX completion cookie=%u purpose=%u source=%u status-kind=%s status=%u ACK=%u",Cookie,J->TxKind[Cookie],Source,Source==S7_TX_SOURCE_FW?"HTT_FW":"TQM",Status,Ack);
    RtlSecureZeroMemory(J->TxDma.Va+Cookie*2048,2048);J->TxUsed[Cookie]=J->TxKind[Cookie]=0;return;
Bad:J->RuntimeErrors++;WcnStage7Fail(D,"invalid/duplicate TX completion cookie, DMA identity or source");
}
static VOID DrxWbmLog(PWCN_DEVICE D,const ULONG *B,const UCHAR *Buffer,ULONG Length,ULONG Pad,ULONG Decap,ULONG Info14,ULONG Fc,ULONG Drop)
{
    ULONG Source=B[2]&7,Push=255,Code=255,Type=0,Bootp=0,H;const UCHAR *F,*Ip,*U;
    if(Source==2){Push=(B[2]>>24)&3;Code=(B[2]>>26)&31;}
    else if(Source==1){Push=(B[2]>>17)&3;Code=(B[2]>>19)&31;}
    if(Decap==2&&Length>=14&&Length<=2048-S6_QCC_DESCRIPTOR_BYTES-Pad&&(!(Fc&0x4000)||((Info14>>10)&7)==0)){
        F=Buffer+S6_QCC_DESCRIPTOR_BYTES+Pad;Type=Jbe16(F+12);
        if(Type==0x0800&&Length>=14+20+8+240){
            Ip=F+14;H=(Ip[0]&15)*4;
            if((Ip[0]>>4)==4&&H>=20&&H<=Length-14-8-240&&Ip[9]==17){
                U=Ip+H;
                if((Jbe16(U)==67||Jbe16(U)==68)&&(Jbe16(U+2)==67||Jbe16(U+2)==68)&&Jeq(U+8+236,(const UCHAR*)"\x63\x82\x53\x63",4))Bootp=U[8];
            }
        }
    }
    WcnLog(D,"stage7: WBM %s drop=%u source=%u desc-type=%u cc=%u push=%u error-code=%u info0=0x%x rxq=%u qaddr=0x%x enc-type=%u ethertype=0x%04x bootp-op=%u (1=request 2=reply; no payload/key dump)",
        Drop?"rejected":"NULL_QUEUE admitted",Drop,Source,(B[2]>>6)&7,(B[2]>>16)&1,Push,Code,B[2],(Jle32(Buffer+156)>>8)&65535,Jle32(Buffer+152),(Jle32(Buffer+148)>>2)&15,Type,Bootp);
    WcnLogHex(D,"stage7 WBM descriptor metadata ",(const UCHAR*)B,32);
}
/* NULL queue means REO could not perform reordering/PN validation. Do not
 * treat it as generic success: only the observed REO/NULL/MSDU class is
 * eligible, then hardware CCMP integrity plus our per-key/TID replay check
 * is mandatory. The same replay table is shared with normal REO delivery. */
static ULONG DrxReplay(PWCN_DEVICE D,const UCHAR *Buffer,const ULONG *B,BOOLEAN Error,ULONG Fc,const UCHAR *F)
{
    PWCN_JOIN J=&D->Join;ULONG Valid=Jle32(Buffer+176),I2=Jle32(Buffer+148),Key=Jle32(Buffer+192)&255;
    ULONG Mpdu=Error?B[3]:B[2],Meta=Error?B[4]:B[3],Group=Buffer[204]&1,Tid=(Fc&0x80)?((I2>>15)&15):16,Mask;
    ULONG64 Pn=(ULONG64)Jle32(Buffer+160)|((ULONG64)(Jle32(Buffer+164)&65535)<<32),Last;
    if(!J->Authorized||!J->PairwiseInstalled||!J->GroupInstalled||!J->PeerMapped)return 14;
    if((Valid&0x4020du)!=0x4020du||!(Mpdu&(1u<<12))||((I2>>2)&15)!=6)return 15;
    if((Fc&0x80)&&!(Valid&(1u<<7)))return 15;
    if(D->Ce.PeerMetadataVersion>1)return 15;
    Mask=D->Ce.PeerMetadataVersion==1?0x3fff:0xffff;
    if((Meta&Mask)!=J->PeerId||((Meta>>16)&255)!=0||!Jeq(Buffer+204,F,6))return 15;
    if(!Group&&!Jeq(Buffer+204,D->Ce.MacAddress,6))return 15;
    if(Key>3||Key!=(Group?(ULONG)J->GtkIndex:0u))return 15;
    Last=J->RxPnValid[Group][Tid]?J->RxPn[Group][Tid]:
        (Group?((ULONG64)Jle32(J->Rsc)|((ULONG64)Jle16(J->Rsc+4)<<32)):0);
    if(!Pn||Pn<=Last){J->ReplayDrops++;return 16;}
    J->RxPn[Group][Tid]=Pn;J->RxPnValid[Group][Tid]=1;
    return 0;
}
static VOID DrxDone(PWCN_DEVICE D,const ULONG *B,BOOLEAN Error)
{
    PWCN_JOIN J=&D->Join;ULONG Cookie,Index,Length,Pad,Info13,Info14,Decap,Head,Route,Drop=0,Fc,Valid,Decrypt,Type=0,Mpdu,Msdu,Rbm;BOOLEAN NullQueue=FALSE;ULONG64 Pa;UCHAR *Buffer;
    if(Error&&(B[2]&(1u<<16))){Cookie=(B[7]>>8)&0xfffff;Pa=B[6]|((ULONG64)(B[7]&255)<<32);}
    else{Cookie=B[1]>>12;Pa=B[0]|((ULONG64)(B[1]&255)<<32);}
    if(Cookie<JRXPPT||Cookie>=JRXPPT+4095)goto Bad;
    Index=Cookie-JRXPPT;
    if(!J->RxPosted[Index]||Pa!=D->Dp.Cookies[Cookie].BufferAddress)goto Bad;
    J->RxPosted[Index]=0;Buffer=D->Dp.RxBuffer[Index/256].Va+(Index%256)*2048;
    KeMemoryBarrier();Info13=Jle32(Buffer+124);Info14=Jle32(Buffer+128);
    J->RxIndications++;
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
    else if(!Jeq(Buffer+210,J->Ap.Bssid,6))Drop=10;
    else if((Fc&0x030c)!=0x0208)Drop=11;
    if(!Drop){
        const UCHAR *Frame=Buffer+S6_QCC_DESCRIPTOR_BYTES+Pad;
        Type=Jbe16(Frame+12);
        /* Only EAPOL can be unprotected. Protected EAPOL/data both require
         * successful hardware decryption. Never weaken cookie, CRC, peer,
         * direction, length, replay-route or controlled-port checks. */
        if(NullQueue&&((Type!=0x0800&&Type!=0x0806&&Type!=0x86dd)||
           (!Jeq(Frame,D->Ce.MacAddress,6)&&!Jeq(Frame,(const UCHAR*)"\xff\xff\xff\xff\xff\xff",6)&&!(Frame[0]&1))))Drop=17;
        if(!Drop&&(Fc&0x4000)){if(Decrypt!=0)Drop=12;else Drop=DrxReplay(D,Buffer,B,Error,Fc,Frame);}
        if(!Drop&&!(Fc&0x4000)){
            if(Decrypt!=0&&Decrypt!=1)Drop=13;
            else if(Type!=0x888e&&(J->Ap.Auth!=WCN_AUTH_OPEN||!J->Authorized))Drop=13;
            else if(NullQueue&&J->Ap.Auth!=WCN_AUTH_OPEN)Drop=13;
        }
        if(!Drop){if(NullQueue)J->NullQueueAccepted++;J->RxFrames++;WcnStage8Ethernet(D,Frame,Length);}
    }
    if(Drop)J->RxDropped++;
    if(Error&&(J->NullQueueAccepted+J->RxDropped<=8||((J->NullQueueAccepted+J->RxDropped)%256)==0)){
        DrxWbmLog(D,B,Buffer,Length,Pad,Decap,Info14,Fc,Drop);
        WcnLog(D,"stage7: CCMP metadata key-id=%u pn=0x%llx software-replay-drops=%u nullq-admitted=%u (no key bytes)",
            Jle32(Buffer+192)&255,(ULONG64)Jle32(Buffer+160)|((ULONG64)(Jle32(Buffer+164)&65535)<<32),J->ReplayDrops,J->NullQueueAccepted);
    }
    if(J->RxIndications<=4||(J->RxIndications%256)==0)
        WcnLog(D,"stage7: RX descriptor #%u cookie=%u length=%u decap=%u error-ring=%u flags13=0x%x flags14=0x%x reo=0x%x route=%u valid=0x%x fc=0x%x pad=%u ethertype=0x%04x drop=%u (metadata only, no payload/key dump)",
            J->RxIndications,Cookie,Length,Decap,Error,Info13,Info14,B[7],Route,Valid,Fc,Pad,Type,Drop);
    Head=(J->RxPendingHead+1)&4095;if(Head==J->RxPendingTail)goto Bad;
    J->RxPending[J->RxPendingHead]=Index;J->RxPendingHead=Head;return;
Bad:J->RuntimeErrors++;WcnStage7Fail(D,"RX cookie/physical-address/ownership or recycle queue violation");
}
static VOID Drefill(PWCN_DEVICE D)
{
    PWCN_JOIN J=&D->Join;PWCN_DP_RING R=Dring(D,160);ULONG Tail,Next,Index,Cookie,*B;ULONG64 Pa;
    if(!Dpointer(D,R,&Tail))return;
    while(J->RxPendingTail!=J->RxPendingHead){
        Next=(R->Head+2)%(R->Entries*2);if(Next==Tail)break;
        Index=J->RxPending[J->RxPendingTail];Cookie=JRXPPT+Index;
        if(Index>=4095||J->RxPosted[Index]){J->RuntimeErrors++;WcnStage7Fail(D,"RX double refill");return;}
        RtlZeroMemory(D->Dp.RxBuffer[Index/256].Va+(Index%256)*2048,2048);
        Pa=D->Dp.Cookies[Cookie].BufferAddress;B=(ULONG*)(R->Desc.Va+R->Head*4);
        B[0]=(ULONG)Pa;B[1]=(ULONG)(Pa>>32)|(6u<<8)|(Cookie<<12);
        J->RxPosted[Index]=1;R->Head=Next;J->RxPendingTail=(J->RxPendingTail+1)&4095;J->RxRecycled++;
    }
    Dpublish(D,R);
}
VOID WcnStage7DpPoll(PWCN_DEVICE D)
{
    PWCN_JOIN J=&D->Join;PWCN_DP_RING R;ULONG I,Head,Count,B[8];
    if(!J->RuntimeReady||(J->Failed&&!J->Resetting)||D->Mhi.SysErr||D->Mhi.RddmSaved)return;
    for(I=0;I<D->Dp.RingCount;I++){
        R=&D->Dp.Ring[I];
        if(R->Source||R->Lmac||!((R->Id>=1&&R->Id<=8)||R->Id==128||R->Id==130||R->Id==132||R->Id==131))continue;
        if(!Dpointer(D,R,&Head))return;
        Count=0;
        while(R->Head!=Head&&Count++<256){
            RtlCopyMemory(B,R->Desc.Va+R->Head*4,32);
            if(R->Id==128||R->Id==130||R->Id==132)DtxDone(D,B);
            else if(R->Id==131)DrxDone(D,B,TRUE);
            else if(!(B[7]&1))DrxDone(D,B,FALSE);
            else {J->RuntimeErrors++;WcnStage7Fail(D,"unexpected REO link descriptor; legacy self-test does not consume chained MSDUs");}
            if(J->Failed&&!J->Resetting)return;
            R->Head=(R->Head+8)%(R->Entries*8);
        }
        Dpublish(D,R);
    }
    Drefill(D);
}

BOOLEAN WcnStage7DataIdle(PWCN_DEVICE D)
{
    PWCN_JOIN J=&D->Join;ULONG I,Head;PWCN_DP_RING R;
    for(I=0;I<64;I++)if(J->TxUsed[I])return FALSE;
    if(!J->RuntimeReady)return TRUE;
    for(I=0;I<D->Dp.RingCount;I++){
        R=&D->Dp.Ring[I];
        if(R->Source||R->Lmac||!((R->Id>=1&&R->Id<=8)||R->Id==128||R->Id==130||R->Id==132||R->Id==131))continue;
        if(!Dpointer(D,R,&Head)||Head!=R->Head)return FALSE;
    }
    return J->RxPendingHead==J->RxPendingTail;
}
