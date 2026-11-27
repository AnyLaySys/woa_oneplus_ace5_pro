static BOOLEAN UsbClockByte(PUCHAR Base,UCHAR Address,BOOLEAN Write,UCHAR *Value)
{
    if(Write)SiaWrite32(Base,0x10,*Value);
    SiaWrite32(Base,0,((Write?0u:1u)<<27)|((ULONG)Address<<4));
    for(ULONG I=0;I<1000;I++){
        ULONG Status=SiaRead32(Base,8);
        if(Status&1){if(Status&14)return FALSE;if(!Write)*Value=(UCHAR)SiaRead32(Base,0x18);return TRUE;}
        KeStallExecutionProcessor(1);
    }
    return FALSE;
}
static BOOLEAN UsbClockMap(PSIA_DEVICE Dev)
{
    PHYSICAL_ADDRESS P;P.QuadPart=0x0c400000;
    PUCHAR Core=MmMapIoSpaceEx(P,0x3000,PAGE_READWRITE|PAGE_NOCACHE);
    P.QuadPart=0x0c42d000;PUCHAR Owners=MmMapIoSpaceEx(P,0x1000,PAGE_READWRITE|PAGE_NOCACHE);
    ULONG Apid=MAXULONG;
    if(Core&&Owners){
        ULONG Version=SiaRead32(Core,0),Count=SiaRead32(Core,4)&0x7ff;
        if(Version>=0x70000000&&Version<0x80000000&&Count<=1024)
            for(ULONG I=0;I<Count;I++)if(((SiaRead32(Core,0x2000+I*4)>>8)&0xfff)==0x7fd&&!(SiaRead32(Owners,I*4)&7)){Apid=I;break;}
    }
    if(Core)MmUnmapIoSpace(Core,0x3000);if(Owners)MmUnmapIoSpace(Owners,0x1000);
    if(Apid==MAXULONG)return FALSE;
    P.QuadPart=0x0c440000+0x20ull*Apid;Dev->UsbRead=MmMapIoSpaceEx(P,0x20,PAGE_READWRITE|PAGE_NOCACHE);
    P.QuadPart=0x0c500000+0x1000ull*Apid;Dev->UsbWrite=MmMapIoSpaceEx(P,0x20,PAGE_READWRITE|PAGE_NOCACHE);
    if(Dev->UsbRead&&Dev->UsbWrite)return TRUE;
    if(Dev->UsbRead)MmUnmapIoSpace(Dev->UsbRead,0x20);if(Dev->UsbWrite)MmUnmapIoSpace(Dev->UsbWrite,0x20);
    Dev->UsbRead=Dev->UsbWrite=NULL;return FALSE;
}
static VOID UsbClockPoll(PSIA_DEVICE Dev,BOOLEAN Host)
{
    ULONGLONG Now=KeQueryInterruptTime()/10000;
    if(!Host&&!Dev->UsbClock)return;
    if(Host==Dev->UsbClock&&Now<Dev->UsbClockAt)return;
    Dev->UsbClockAt=Now+300;
    if(!Dev->UsbRead&&!UsbClockMap(Dev))return;
    static const UCHAR Address[]={0xe8,0xed};UCHAR Value[2];
    for(ULONG I=0;I<2;I++)if(!UsbClockByte(Dev->UsbRead,Address[I],FALSE,&Value[I]))return;
    if(!Dev->UsbClock){RtlCopyMemory(Dev->UsbClockSaved,Value,sizeof(Value));Dev->UsbClock=TRUE;}
    for(ULONG I=0;I<2;I++){
        UCHAR Desired=Host?Value[I]|0x40:(Value[I]&~0x40)|(Dev->UsbClockSaved[I]&0x40);
        if(Desired!=Value[I]&&!UsbClockByte(Dev->UsbWrite,Address[I],TRUE,&Desired))return;
    }
    Dev->UsbClock=Host;
}
static VOID UsbClockStop(PSIA_DEVICE Dev)
{
    for(ULONG I=0;I<3&&Dev->UsbClock;I++){Dev->UsbClockAt=0;UsbClockPoll(Dev,FALSE);}
    if(Dev->UsbRead)MmUnmapIoSpace(Dev->UsbRead,0x20);if(Dev->UsbWrite)MmUnmapIoSpace(Dev->UsbWrite,0x20);
    Dev->UsbRead=Dev->UsbWrite=NULL;
}
