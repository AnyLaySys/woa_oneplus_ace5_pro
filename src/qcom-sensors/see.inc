#define SEE_CID 6u
typedef struct {
    QCOM_LIGHT Value;volatile LONG Sequence;volatile LONG64 Demand;
    ULONGLONG Suid[2];UCHAR Stage;
} SEE_SENSOR;
static SEE_SENSOR Sensors[]={{{0,STATUS_DEVICE_NOT_CONNECTED,0}},{{0,STATUS_DEVICE_NOT_CONNECTED,0}}};
static const CHAR *const SensorNames[]={"wise_rgb","device_orient"};
static VOID SeeValue(SEE_SENSOR *s,ULONG value,NTSTATUS status) {
    InterlockedIncrement(&s->Sequence);s->Value.LuxMilli=value;s->Value.Status=status;
    s->Value.Time=NT_SUCCESS(status)?KeQueryInterruptTime():0;InterlockedIncrement(&s->Sequence);
}
static VOID SeeReset(NTSTATUS status) {
    for(ULONG i=0;i<RTL_NUMBER_OF(Sensors);i++){Sensors[i].Stage=0;RtlZeroMemory(Sensors[i].Suid,sizeof(Sensors[i].Suid));SeeValue(&Sensors[i],0,status);}
}
static VOID SeeQuery(ULONG i,PVOID value) {
    SEE_SENSOR *s=&Sensors[i];InterlockedExchange64(&s->Demand,(LONG64)KeQueryInterruptTime());
    LONG seq=InterlockedCompareExchange(&s->Sequence,0,0);RtlCopyMemory(value,&s->Value,sizeof(s->Value));
    if((seq&1) || seq!=InterlockedCompareExchange(&s->Sequence,0,0))((QCOM_LIGHT*)value)->Status=STATUS_DEVICE_NOT_READY;
}
VOID QcomLightQuery(QCOM_LIGHT *value) {SeeQuery(0,value);}
VOID QcomOrientationQuery(QCOM_ORIENTATION *value) {SeeQuery(1,value);}
typedef struct {const UCHAR *At,*End;} SEE_PB;
static BOOLEAN SeeVar(SEE_PB *p,ULONGLONG *v) {
    *v=0;for(ULONG s=0;s<64;s+=7) {
        if(p->At==p->End)return FALSE;UCHAR b=*p->At++;
        if(s==63 && b>1)return FALSE;*v|=(ULONGLONG)(b&127)<<s;if(!(b&128))return TRUE;
    }return FALSE;
}
static BOOLEAN SeeField(SEE_PB *p,ULONG *tag,SEE_PB *data,ULONGLONG *value) {
    ULONGLONG key,n;if(!SeeVar(p,&key) || !(key>>3) || key>MAXULONG)return FALSE;
    *tag=(ULONG)key>>3;data->At=data->End=NULL;*value=0;
    switch(key&7) {
    case 0:return SeeVar(p,value);
    case 1:n=8;break;
    case 2:if(!SeeVar(p,&n))return FALSE;break;
    case 5:n=4;break;
    default:return FALSE;
    }
    if(n>(ULONGLONG)(p->End-p->At))return FALSE;
    data->At=p->At;data->End=p->At+(SIZE_T)n;p->At=data->End;
    if((key&7)!=2)RtlCopyMemory(value,data->At,(SIZE_T)n);return TRUE;
}
static VOID SeeEvent(const ULONGLONG *source,SEE_PB event) {
    ULONG tag,id=0;ULONGLONG value;SEE_PB data,payload={0};
    while(event.At<event.End) {
        if(!SeeField(&event,&tag,&data,&value))return;
        if(tag==1)id=(ULONG)value;else if(tag==3)payload=data;
    }
    if(!payload.At)return;
    if(id==768) {
        ULONGLONG suid[2]={0};LONG sensor=-1;
        while(payload.At<payload.End) {
            if(!SeeField(&payload,&tag,&data,&value))return;
            if(tag==1 && data.At)for(ULONG i=0;i<RTL_NUMBER_OF(Sensors);i++) {
                SIZE_T n=strlen(SensorNames[i]);if((SIZE_T)(data.End-data.At)==n && RtlCompareMemory(data.At,SensorNames[i],n)==n)sensor=(LONG)i;
            }
            else if(tag==2 && data.At && !suid[0] && !suid[1])while(data.At<data.End) {
                SEE_PB field;if(!SeeField(&data,&tag,&field,&value))return;
                if(tag==1 || tag==2)suid[tag-1]=value;
            }
        }
        if(sensor<0)return;SEE_SENSOR *s=&Sensors[sensor];
        if(!suid[0] && !suid[1]){SeeValue(s,0,STATUS_NO_SUCH_DEVICE);return;}
        if(s->Stage<2 || RtlCompareMemory(s->Suid,suid,sizeof(suid))!=sizeof(suid)) {
            RtlCopyMemory(s->Suid,suid,sizeof(suid));s->Stage=2;SeeValue(s,0,STATUS_DEVICE_NOT_READY);
        }
        return;
    }
    for(ULONG i=0;i<RTL_NUMBER_OF(Sensors);i++) {
        SEE_SENSOR *s=&Sensors[i];if(s->Stage!=3 || RtlCompareMemory(s->Suid,source,sizeof(s->Suid))!=sizeof(s->Suid))continue;
        if(i==1 && id==776) {
            while(payload.At<payload.End){if(!SeeField(&payload,&tag,&data,&value))return;if(tag==1 && value<4){SeeValue(s,(ULONG)value,STATUS_SUCCESS);return;}}
        } else if(i==0 && id==1025) {
            ULONG bits=0,status=0;BOOLEAN sample=FALSE;
            while(payload.At<payload.End) {
                if(!SeeField(&payload,&tag,&data,&value))return;
                if(tag==1 && data.At && data.End-data.At==64 && !sample){RtlCopyMemory(&bits,data.At+36,4);sample=TRUE;}
                if(tag==2)status=(ULONG)value;
            }
            if(!sample || status!=3 || bits&0x80000000u || (bits&0x7f800000u)==0x7f800000u)return;
            LONG exponent=(LONG)((bits>>23)&255)-150;
            ULONGLONG lux=(ULONGLONG)((bits&0x7fffff)|0x800000u)*1000;
            if(exponent>=0)lux=exponent<16?lux<<exponent:1000000000ULL;
            else lux=exponent>-64?lux>>(-exponent):0;
            SeeValue(s,(ULONG)min(lux,1000000000ULL),STATUS_SUCCESS);
        }
        return;
    }
}
static BOOLEAN SeePacket(PSIA_LINK l,ULONG type,ULONG node,ULONG port,const UCHAR *data,ULONG size) {
    QCOM_SEE *s=&l->See;if(size>1024 || !s->Acked || !s->Rcid)return FALSE;
    ULONG i;for(i=0;i<s->Count && !(s->Free[i] && s->Size[i]>=32+size);i++);
    if(i==s->Count){if(!s->Requested)s->Requested=SiaGlinkCmd(l,GLINK_CMD_RX_INTENT_REQ,SEE_CID,32+size);return FALSE;}
    UCHAR packet[16+32+1024];ULONG header[]={GLINK_CMD_TX_DATA|(SEE_CID<<16),s->Iid[i],32+size,0,1,type,s->OwnNode,type==1 || type==7?0x100u:0xfffffffeu,0,size,node,port};
    RtlCopyMemory(packet,header,sizeof(header));RtlCopyMemory(packet+sizeof(header),data,size);
    if(!SiaGlinkSend(l,packet,sizeof(header)+size))return FALSE;s->Free[i]=FALSE;return TRUE;
}
static BOOLEAN SeeRequest(PSIA_LINK l,const ULONGLONG *suid,ULONG id,const UCHAR *body,UCHAR size) {
    if(size>80)return FALSE;QCOM_SEE *s=&l->See;UCHAR packet[128];
    packet[0]=0;*(USHORT UNALIGNED*)(packet+1)=++s->Transaction;*(USHORT UNALIGNED*)(packet+3)=0x20;
    packet[7]=1;
    UCHAR *p=packet+12;*p++=0x0a;*p++=18;*p++=9;RtlCopyMemory(p,suid,8);p+=8;*p++=17;RtlCopyMemory(p,suid+1,8);p+=8;
    *p++=0x15;RtlCopyMemory(p,&id,4);p+=4;*p++=0x1a;*p++=4;*p++=8;*p++=1;*p++=16;*p++=0;
    *p++=0x22;*p++=size+2;*p++=0x12;*p++=size;if(size)RtlCopyMemory(p,body,size);p+=size;
    *(USHORT UNALIGNED*)(packet+5)=(USHORT)(p-packet-7);*(USHORT UNALIGNED*)(packet+8)=(USHORT)(p-packet-10);*(USHORT UNALIGNED*)(packet+10)=(USHORT)(p-packet-12);
    return SeePacket(l,1,s->Node,s->Port,packet,(ULONG)(p-packet));
}
static VOID SeePoll(PSIA_LINK l);
static VOID SeeReceive(PSIA_LINK l,const UCHAR *packet,ULONG size) {
    QCOM_SEE *s=&l->See;if(size<32)return;const ULONG UNALIGNED *h=(const ULONG UNALIGNED*)packet;
    if(h[0]!=1 || h[5]>size-32)return;if(h[6]!=MAXULONG)s->OwnNode=h[6];size=h[5];packet+=32;
    if(h[1]==1 && h[4]){s->ResumeNode=h[2];s->ResumePort=h[3];}
    if(h[1]==2){s->Due=0;return;}
    if(h[1]==4 && size>=20) {
        const ULONG UNALIGNED *v=(const ULONG UNALIGNED*)packet;
        if(v[1]==400 && (v[2]&255)==1 && (s->Node!=v[3] || s->Port!=v[4])){s->Node=v[3];s->Port=v[4];s->Due=0;SeeReset(STATUS_DEVICE_NOT_READY);}
        return;
    }
    if(h[1]!=1 || h[2]!=s->Node || h[3]!=s->Port || size<7 || (packet[0]!=2 && packet[0]!=4))return;
    BOOLEAN response=packet[0]==2;USHORT id=*(USHORT UNALIGNED*)(packet+3);
    if(id!=(response?0x20:0x21))return;
    ULONG bytes=*(USHORT UNALIGNED*)(packet+5);if(bytes>size-7)return;packet+=7;
    while(bytes>=3) {
        UCHAR type=packet[0];ULONG count=*(USHORT UNALIGNED*)(packet+1);packet+=3;bytes-=3;if(count>bytes)return;
        if(response) {
            if((type==2 && count==4 && *(USHORT UNALIGNED*)packet) || (type==0x11 && count==4 && *(ULONG UNALIGNED*)packet))SeeReset(STATUS_DEVICE_PROTOCOL_ERROR);
        } else if(type==2 && count>=2) {
            ULONG n=*(USHORT UNALIGNED*)packet;if(n>count-2)return;SEE_PB p={packet+2,packet+2+n};ULONGLONG source[2]={0};
            while(p.At<p.End) {
                ULONG tag;SEE_PB data;ULONGLONG value;if(!SeeField(&p,&tag,&data,&value))return;
                if(tag==1 && data.At)while(data.At<data.End){SEE_PB field;if(!SeeField(&data,&tag,&field,&value))return;if(tag==1 || tag==2)source[tag-1]=value;}
                else if(tag==2 && data.At)SeeEvent(source,data);
            }
        }
        packet+=count;bytes-=count;
    }
    s->Due=0;SeePoll(l);
}
static VOID SeePoll(PSIA_LINK l) {
    QCOM_SEE *s=&l->See;if(!l->VersionReady || !l->PowerHeld || l->Charge.Closing)return;
    if(s->ResumePort) {
        ULONG confirm[]={7,s->OwnNode,0x100,0,0};
        if(!SeePacket(l,7,s->ResumeNode,s->ResumePort,(UCHAR*)confirm,sizeof(confirm)))return;
        s->ResumePort=0;
    }
    if(!s->Opening){const ULONG open[]={GLINK_CMD_OPEN|(SEE_CID<<16),7,0x52435049,0x00005254};s->Opening=SiaGlinkSend(l,(const UCHAR*)open,sizeof(open));}
    if(!s->Rcid || !s->Acked || !s->Count || KeQueryInterruptTime()<s->Due)return;
    s->Due=KeQueryInterruptTime()+20000000;
    if(!s->Port){ULONG hello[]={2,0,0,0,0};SeePacket(l,2,0xffffffff,0xfffffffe,(UCHAR*)hello,sizeof(hello));}
    else for(ULONG i=0;i<RTL_NUMBER_OF(Sensors);i++) {
        SEE_SENSOR *sensor=&Sensors[i];
        if(!sensor->Stage) {
            static const ULONGLONG uid[]={0xababababababababULL,0xababababababababULL};
            UCHAR body[32];UCHAR n=(UCHAR)strlen(SensorNames[i]);body[0]=10;body[1]=n;RtlCopyMemory(body+2,SensorNames[i],n);
            body[n+2]=16;body[n+3]=1;body[n+4]=24;body[n+5]=1;
            if(SeeRequest(l,uid,512,body,n+6))sensor->Stage=1;
        } else {
            ULONGLONG demand=(ULONGLONG)InterlockedCompareExchange64(&sensor->Demand,0,0);BOOLEAN active=demand && KeQueryInterruptTime()-demand<50000000;
            if(sensor->Stage==2 && active){if(SeeRequest(l,sensor->Suid,514,NULL,0))sensor->Stage=3;}
            else if(sensor->Stage==3 && !active && SeeRequest(l,sensor->Suid,10,NULL,0)){sensor->Stage=2;SeeValue(sensor,0,STATUS_DEVICE_NOT_READY);}
        }
    }
}
static BOOLEAN SeeHandle(PSIA_LINK l,USHORT cmd,USHORT p1,ULONG p2,ULONG tail,const ULONG *frame) {
    QCOM_SEE *s=&l->See;
    if(cmd==GLINK_CMD_OPEN) {
        UCHAR name[32]={0};SiaFifoRead(l,tail+8,name,min(p2&0xffff,31u));if(strcmp((CHAR*)name,"IPCRTR"))return FALSE;
        BOOLEAN opening=s->Opening;RtlZeroMemory(s,sizeof(*s));s->Opening=opening;s->Rcid=p1;s->OwnNode=1;SiaGlinkCmd(l,GLINK_CMD_OPEN_ACK,p1,0);
        s->LocalIid=1;ULONG intent[]={GLINK_CMD_INTENT|(SEE_CID<<16),1,2048,s->LocalIid};SiaGlinkSend(l,(UCHAR*)intent,sizeof(intent));SeePoll(l);return TRUE;
    }
    if(cmd==GLINK_CMD_OPEN_ACK && p1==SEE_CID){s->Acked=TRUE;SiaGlinkCmd(l,GLINK_CMD_RX_INTENT_REQ,SEE_CID,1100);return TRUE;}
    if(cmd==GLINK_CMD_CLOSE_ACK && p1==SEE_CID){s->Acked=FALSE;return TRUE;}
    if(!s->Rcid || p1!=s->Rcid)return FALSE;
    if(cmd==GLINK_CMD_INTENT) {
        s->Requested=FALSE;for(ULONG i=0;i<p2;i++){ULONG pair[2];SiaFifoRead(l,tail+8+i*8,(UCHAR*)pair,8);ULONG j;for(j=0;j<s->Count && s->Iid[j]!=pair[1];j++);if(j<8){s->Size[j]=pair[0];s->Iid[j]=pair[1];s->Free[j]=TRUE;s->Count=max(s->Count,j+1);}}return TRUE;
    }
    if(cmd==GLINK_CMD_INTENT_REQ_ACK){if(!p2)s->Requested=FALSE;return TRUE;}
    if(cmd==GLINK_CMD_RX_DONE || cmd==GLINK_CMD_RX_DONE_REUSE){for(ULONG i=0;i<s->Count;i++)if(s->Iid[i]==p2){if(cmd==GLINK_CMD_RX_DONE_REUSE)s->Free[i]=TRUE;else {--s->Count;s->Iid[i]=s->Iid[s->Count];s->Size[i]=s->Size[s->Count];s->Free[i]=s->Free[s->Count];}break;}return TRUE;}
    if(cmd==GLINK_CMD_RX_INTENT_REQ){ULONG intent[]={GLINK_CMD_INTENT|(SEE_CID<<16),1,2048,s->LocalIid+1};BOOLEAN granted=p2<=2048 && s->LocalIid<MAXULONG && SiaGlinkSend(l,(UCHAR*)intent,sizeof(intent));if(granted)s->LocalIid++;SiaGlinkCmd(l,GLINK_CMD_INTENT_REQ_ACK,SEE_CID,granted);return TRUE;}
    if(cmd==GLINK_CMD_TX_DATA || cmd==GLINK_CMD_TX_DATA_CONT) {
        if(cmd==GLINK_CMD_TX_DATA && !frame[1] && frame[0]<=2048){UCHAR data[2048];SiaFifoRead(l,tail+16,data,frame[0]);SeeReceive(l,data,frame[0]);}
        return TRUE;
    }
    if(cmd==GLINK_CMD_CLOSE){SiaGlinkCmd(l,GLINK_CMD_CLOSE_ACK,p1,0);RtlZeroMemory(s,sizeof(*s));SeeReset(STATUS_DEVICE_NOT_CONNECTED);return TRUE;}
    return FALSE;
}
