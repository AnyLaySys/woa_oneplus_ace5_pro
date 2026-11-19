#define SEE_CID 6u
typedef struct {QCOM_ROTATION Value;volatile LONG Sequence;} SEE_SAMPLE;
typedef struct {ULONGLONG Suid[2];UCHAR Stage,Rate,Alternate;} SEE_SENSOR;
static SEE_SAMPLE Samples[QcomSensorCount],Activity;
static volatile LONG64 Demands[QcomSensorCount];
typedef struct {const CHAR *Name;ULONG Sample,Request;} SEE_TYPE;
static const SEE_TYPE SensorTypes[]={
    {"wise_rgb",QcomLight,514},{"accel",QcomSimpleOrientation,513},{"gyro",QcomGyro,513},{"rotv",QcomRotation,513},
    {"gravity",QcomGravity,513},{"mag",QcomMagnetometer,513},{"game_rv",QcomRelativeOrientation,513},{"geomag_rv",QcomGeomagneticOrientation,513},
    {"proximity",QcomProximity,514},{"pedometer",QcomPedometer,514},{"rmd",QcomActivity,514},{"activity_recognition",QcomActivity,514}
};
#define SEE_AR (RTL_NUMBER_OF(SensorTypes)-1)
static SEE_SENSOR Sensors[RTL_NUMBER_OF(SensorTypes)];
static const CHAR *SeeName(ULONG i) {return i==10 && Sensors[i].Alternate?"amd":SensorTypes[i].Name;}
static VOID SeePublish(SEE_SAMPLE *s,const LONG *values,ULONG accuracy,NTSTATUS status) {
    InterlockedIncrement(&s->Sequence);RtlCopyMemory(s->Value.Micro,values,sizeof(s->Value.Micro));s->Value.Accuracy=accuracy;s->Value.Status=status;
    s->Value.Time=NT_SUCCESS(status)?KeQueryInterruptTime():0;InterlockedIncrement(&s->Sequence);
}
static VOID SeeValue(SEE_SAMPLE *s,LONG x,LONG y,LONG z,NTSTATUS status) {
    const LONG values[]={x,y,z,0};SeePublish(s,values,0,status);
}
static VOID SeeReset(NTSTATUS status) {
    RtlZeroMemory(Sensors,sizeof(Sensors));for(ULONG i=0;i<RTL_NUMBER_OF(Samples);i++)SeeValue(&Samples[i],0,0,0,status);
    SeeValue(&Activity,0,0,0,status);
}
static VOID SeeState(ULONG i,NTSTATUS status) {
    SeeValue(i==SEE_AR?&Activity:&Samples[SensorTypes[i].Sample],0,0,0,status);if(i==1)SeeValue(&Samples[QcomAccel],0,0,0,status);if(i==4)SeeValue(&Samples[QcomLinearAccel],0,0,0,status);
}
static QCOM_ROTATION SeeRead(SEE_SAMPLE *s) {
    LONG seq=InterlockedCompareExchange(&s->Sequence,0,0);QCOM_ROTATION sample=s->Value;
    if((seq&1) || seq!=InterlockedCompareExchange(&s->Sequence,0,0))sample.Status=STATUS_DEVICE_NOT_READY;
    return sample;
}
NTSTATUS QcomSensorQuery(ULONG sensor,QCOM_ROTATION *value) {
    if(sensor>=QcomSensorCount)return STATUS_INVALID_PARAMETER;
    InterlockedExchange64(&Demands[sensor],(LONG64)KeQueryInterruptTime());*value=SeeRead(&Samples[sensor]);
    if(sensor==QcomActivity && Sensors[SEE_AR].Stage==3) {
        QCOM_ROTATION sample=SeeRead(&Activity);if(NT_SUCCESS(sample.Status) && sample.Time)*value=sample;
    }
    return STATUS_SUCCESS;
}
static LONG SeeScaled(ULONG bits,ULONG scale,ULONG limit) {
    // Scale an IEEE-754 value with a Q16 multiplier, without kernel floating point.
    LONG shift=166-(LONG)((bits>>23)&255);ULONGLONG n=(ULONGLONG)((bits&0x7fffff)|0x800000u)*scale;
    n=shift>=64?0:shift>0?n>>shift:limit;LONG value=(LONG)min(n,limit);return bits>>31?-value:value;
}
static BOOLEAN SeeQuaternion(const ULONG *bits,LONG *q) {
    // Cache ADSP x,y,z,w as w,x,y,z in millionths, without kernel floating point.
    LONGLONG norm=0;
    for(ULONG i=0;i<4;i++) {
        ULONG b=bits[(i+3)%4];if((b&0x7fffffffu)>0x3f800000u)return FALSE;
        LONG shift=150-(LONG)((b>>23)&255);ULONGLONG n=(ULONGLONG)((b&0x7fffffu)|0x800000u)*1000000;
        LONG value=(LONG)(shift>=64?0:n>>shift);q[i]=b>>31?-value:value;norm+=(LONGLONG)value*value;
    }
    return norm>=500000000000LL && norm<=1500000000000LL;
}
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
static ULONG SeeActivityState(const ULONG *states,ULONG count) {
    // QTI publishes hierarchical activity states.
    if(count==1)return states[0]==1?2u:states[0]==4?32u:1u;
    if(count==2) {
        if(states[0]==2)return states[1]==5?8u:states[1]==6?16u:1u;
        if(states[0]==3 && states[1]==7)return 64u;
        if(states[0]==4)return states[1]==1?2u:states[1]==8 || states[1]==9?32u:1u;
    }
    if(count==3 && states[2]==1 && ((states[0]==3 && states[1]==7) || (states[0]==4 && states[1]==8)))return 2u;
    return 1u;
}
static VOID SeeActivity(SEE_PB payload) {
    ULONG states[3],count=0,tag;ULONGLONG value;SEE_PB data;
    while(payload.At<payload.End) {
        if(!SeeField(&payload,&tag,&data,&value))return;if(tag!=1)continue;
        if(data.At) {
            while(data.At<data.End) {
                if(count==RTL_NUMBER_OF(states) || !SeeVar(&data,&value) || value>9)return;
                states[count++]=(ULONG)value;
            }
        } else {if(count==RTL_NUMBER_OF(states) || value>9)return;states[count++]=(ULONG)value;}
    }
    if(count)SeeValue(&Activity,(LONG)SeeActivityState(states,count),0,0,STATUS_SUCCESS);
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
                SIZE_T n=strlen(SeeName(i));if((SIZE_T)(data.End-data.At)==n && RtlCompareMemory(data.At,SeeName(i),n)==n)sensor=(LONG)i;
            }
            else if(tag==2 && data.At && !suid[0] && !suid[1])while(data.At<data.End) {
                SEE_PB field;if(!SeeField(&data,&tag,&field,&value))return;
                if(tag==1 || tag==2)suid[tag-1]=value;
            }
        }
        if(sensor<0)return;SEE_SENSOR *s=&Sensors[sensor];
        if(!suid[0] && !suid[1]){if(sensor==10 && !s->Alternate){s->Alternate=1;s->Stage=0;SeeState((ULONG)sensor,STATUS_DEVICE_NOT_READY);}else SeeState((ULONG)sensor,STATUS_NO_SUCH_DEVICE);return;}
        if(s->Stage<2 || RtlCompareMemory(s->Suid,suid,sizeof(suid))!=sizeof(suid)) {
            RtlCopyMemory(s->Suid,suid,sizeof(suid));s->Stage=2;SeeState((ULONG)sensor,STATUS_DEVICE_NOT_READY);
        }
        return;
    }
    if(id!=1025 && id!=769 && id!=1028 && id!=775 && id!=772 && id!=800)return;
    for(ULONG i=0;i<RTL_NUMBER_OF(Sensors);i++) {
        SEE_SENSOR *sensor=&Sensors[i];if(sensor->Stage!=3 || RtlCompareMemory(sensor->Suid,source,sizeof(sensor->Suid))!=sizeof(sensor->Suid))continue;
        if(i==SEE_AR){if(id==800)SeeActivity(payload);return;}
        ULONG kind=SensorTypes[i].Sample,bits[6]={0},items=0,status=0,near=MAXULONG,steps=0,mask=0;BOOLEAN countSeen=FALSE,activitySeen=FALSE;SEE_SAMPLE *s=&Samples[kind];
        while(payload.At<payload.End) {
            if(!SeeField(&payload,&tag,&data,&value))return;
            if(kind==QcomPedometer && (id==1028 || id==775)){if(tag==1 && !data.At && value<=MAXULONG){steps=(ULONG)value;countSeen=TRUE;}continue;}
            if(kind==QcomActivity && id==772) {if(tag==1 && !data.At && value<=2){mask=value==1?2u:1u;activitySeen=TRUE;}continue;}
            if(kind==QcomProximity && id==769) {if(tag==1)near=(ULONG)value;else if(tag==3)status=(ULONG)value;continue;}
            if(tag==1 && data.At) {
                SIZE_T bytes=(SIZE_T)(data.End-data.At);
                if(!i && bytes==64){RtlCopyMemory(bits,data.At+36,4);items=1;}
                else if(i && bytes && !(bytes&3) && bytes/4<=6-items){RtlCopyMemory(bits+items,data.At,bytes);items+=(ULONG)bytes/4;}
            }
            if(kind==QcomGravity && tag==2 && data.At && data.End-data.At==12){RtlCopyMemory(bits+3,data.At,12);items=6;}
            else if((tag==2 || (kind==QcomGravity && tag==3)) && !data.At)status=(ULONG)value;
        }
        if(kind==QcomPedometer){if(countSeen)SeeValue(s,(LONG)steps,0,0,STATUS_SUCCESS);return;}
        if(kind==QcomActivity){if(activitySeen)SeeValue(s,(LONG)mask,0,0,STATUS_SUCCESS);return;}
        if(status>3)return;
        if(kind==QcomProximity){if(near<=1 && status)SeeValue(s,(LONG)near,0,0,STATUS_SUCCESS);return;}
        ULONG count=kind==QcomRotation || kind==QcomRelativeOrientation || kind==QcomGeomagneticOrientation?4:kind==QcomGravity?6:kind==QcomLight?1:3;
        if(items!=count || ((!status || (!i && status!=3)) && kind<=QcomGyro))return;
        for(ULONG j=0;j<count;j++)if((bits[j]&0x7f800000u)==0x7f800000u)return;
        if(count==4) {
            LONG q[4];if(!SeeQuaternion(bits,q))return;
            SeePublish(s,q,max(status,1u),STATUS_SUCCESS);
        } else if(kind==QcomGyro)SeeValue(s,SeeScaled(bits[0],3754936206u,2000000),SeeScaled(bits[1],3754936206u,2000000),SeeScaled(bits[2],3754936206u,2000000),STATUS_SUCCESS);
        else if(kind==QcomGravity) {
            SeeValue(s,SeeScaled(bits[0],6682812,16000),SeeScaled(bits[1],6682812,16000),SeeScaled(bits[2],6682812,16000),STATUS_SUCCESS);
            SeeValue(&Samples[QcomLinearAccel],SeeScaled(bits[3],6682812,16000),SeeScaled(bits[4],6682812,16000),SeeScaled(bits[5],6682812,16000),STATUS_SUCCESS);
        } else if(kind==QcomMagnetometer) {
            const LONG v[]={SeeScaled(bits[0],655360000,100000000),SeeScaled(bits[1],655360000,100000000),SeeScaled(bits[2],655360000,100000000),0};SeePublish(s,v,max(status,1u),STATUS_SUCCESS);
        } else if(kind==QcomSimpleOrientation) {
            SeeValue(&Samples[QcomAccel],SeeScaled(bits[0],6682812,16000),SeeScaled(bits[1],6682812,16000),SeeScaled(bits[2],6682812,16000),STATUS_SUCCESS);
            ULONG axis=0;for(ULONG j=1;j<3;j++)if((bits[j]&0x7fffffffu)>(bits[axis]&0x7fffffffu))axis=j;
            if((bits[axis]&0x7fffffffu)<0x40f00000u)return;
            ULONG negative=bits[axis]>>31;SeeValue(s,axis==1?negative*2:axis==0?1+negative*2:4+negative,0,0,STATUS_SUCCESS);
        } else {
            if(bits[0]&0x80000000u)return;
            SeeValue(s,SeeScaled(bits[0],65536000,1000000000),0,0,STATUS_SUCCESS);
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
static BOOLEAN SeeActive(ULONG i,ULONGLONG now) {
    ULONGLONG demand=(ULONGLONG)InterlockedCompareExchange64(&Demands[i],0,0);return demand && now-demand<50000000;
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
            UCHAR body[32];UCHAR n=(UCHAR)strlen(SeeName(i));body[0]=10;body[1]=n;RtlCopyMemory(body+2,SeeName(i),n);
            body[n+2]=16;body[n+3]=1;body[n+4]=24;body[n+5]=1;
            if(SeeRequest(l,uid,512,body,n+6))sensor->Stage=1;
        } else {
            ULONGLONG now=KeQueryInterruptTime();BOOLEAN raw=i==1 && SeeActive(QcomAccel,now),active=raw || SeeActive(SensorTypes[i].Sample,now) || (i==4 && SeeActive(QcomLinearAccel,now));UCHAR rate=(UCHAR)(i>=5 || i==3?2:i==1 && !raw?0:1);
            if(active && (sensor->Stage==2 || (sensor->Stage==3 && sensor->Rate!=rate))) {
                static const UCHAR rates[][5]={{13,0,0,16,65},{13,0,0,72,66},{13,0,0,32,65}};
                if(SeeRequest(l,sensor->Suid,SensorTypes[i].Request,SensorTypes[i].Request==513?rates[rate]:NULL,SensorTypes[i].Request==513?sizeof(rates[0]):0)){sensor->Stage=3;sensor->Rate=rate;}
            } else if(sensor->Stage==3 && !active && SeeRequest(l,sensor->Suid,10,NULL,0)){sensor->Stage=2;SeeState(i,STATUS_DEVICE_NOT_READY);}
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
