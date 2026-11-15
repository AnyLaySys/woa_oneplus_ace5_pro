// Fixed OnePlus board CP: captured Android DT I2CHUB-SE8, SDA206/SCL207, addr69.
// Native SC8517 analog mode only: no SUPERVOOC protocol, no OVP/SCP threshold changes.
#define SC_OE (1u<<9)
static PUCHAR ScBase;
static ULONG ScCfg[2],ScIo[2];
static BOOLEAN ScBorrowed;
static ULONG ScRd(ULONG pin,ULONG offset){return READ_REGISTER_ULONG((PULONG)(ScBase+pin*0x1000+offset));}
static VOID ScWr(ULONG pin,ULONG offset,ULONG value){WRITE_REGISTER_ULONG((PULONG)(ScBase+pin*0x1000+offset),value);KeMemoryBarrier();}
static VOID ScDelay(VOID){KeStallExecutionProcessor(10);}
static VOID ScReleasePin(ULONG pin){ScWr(pin,0,ScCfg[pin]&~(SC_OE|0x3cu));}
static VOID ScLow(ULONG pin){ScWr(pin,4,ScIo[pin]&~2u);ScWr(pin,0,(ScCfg[pin]&~0x3cu)|SC_OE);}
static BOOLEAN ScScl(VOID){ScReleasePin(1);for(ULONG n=0;n<500;n++){if(ScRd(1,4)&1)return TRUE;KeStallExecutionProcessor(10);}return FALSE;}
static BOOLEAN ScStart(VOID){ScReleasePin(0);if(!ScScl())return FALSE;ScDelay();ScLow(0);ScDelay();ScLow(1);ScDelay();return TRUE;}
static VOID ScStopWire(VOID){ScLow(0);ScDelay();ScScl();ScDelay();ScReleasePin(0);ScDelay();}
static BOOLEAN ScSend(UCHAR value){
 for(ULONG n=0;n<8;n++){if(value&(0x80u>>n))ScReleasePin(0);else ScLow(0);ScDelay();if(!ScScl())return FALSE;ScDelay();ScLow(1);ScDelay();}
 ScReleasePin(0);ScDelay();if(!ScScl())return FALSE;ScDelay();BOOLEAN ack=!(ScRd(0,4)&1);ScLow(1);ScDelay();return ack;
}
static BOOLEAN ScReadWire(UCHAR reg,ULONG *value){
 BOOLEAN ok=FALSE;UCHAR byte=0;
 if(!ScStart() || !ScSend(0xd2) || !ScSend(reg) || !ScStart() || !ScSend(0xd3))goto end;
 ScReleasePin(0);
 for(ULONG n=0;n<8;n++){if(!ScScl())goto end;ScDelay();byte=(UCHAR)((byte<<1)|(ScRd(0,4)&1));ScLow(1);ScDelay();}
 ScReleasePin(0);ScDelay();if(!ScScl())goto end;ScDelay();ScLow(1);ScDelay();*value=byte;ok=TRUE;
end:ScStopWire();return ok;
}
static BOOLEAN ScRestorePins(VOID){
 ScReleasePin(0);ScReleasePin(1);ScWr(0,4,ScIo[0]);ScWr(1,4,ScIo[1]);ScWr(0,0,ScCfg[0]);ScWr(1,0,ScCfg[1]);ScBorrowed=FALSE;
 return ScRd(0,0)==ScCfg[0] && ScRd(1,0)==ScCfg[1] && (ScRd(0,4)&2)==(ScIo[0]&2) && (ScRd(1,4)&2)==(ScIo[1]&2);
}
static BOOLEAN ScWriteWire(UCHAR Reg,UCHAR Value){BOOLEAN Ok=ScStart() && ScSend(0xd2) && ScSend(Reg) && ScSend(Value);ScStopWire();return Ok;}
static BOOLEAN ScBegin(VOID){
    if(!ScBase){PHYSICAL_ADDRESS P;P.QuadPart=0x0f1ce000;ScBase=MmMapIoSpaceEx(P,0x2000,PAGE_READWRITE|PAGE_NOCACHE);if(!ScBase)return FALSE;}
    for(ULONG I=0;I<2;I++){ScCfg[I]=ScRd(I,0);ScIo[I]=ScRd(I,4);}
    if((ScCfg[0]&SC_OE)||(ScCfg[1]&SC_OE)||((ScCfg[0]>>2)&15)>1||((ScCfg[1]>>2)&15)>1||(ScIo[0]&1)==0||(ScIo[1]&1)==0)return FALSE;
    ScBorrowed=TRUE;ScReleasePin(0);ScReleasePin(1);KeStallExecutionProcessor(100);
    if(!(ScRd(0,4)&1)||!(ScRd(1,4)&1)){ScRestorePins();return FALSE;}return TRUE;
}
static BOOLEAN ScEnd(VOID){return ScRestorePins();}
static VOID ScUnmap(VOID){if(ScBorrowed)ScRestorePins();if(ScBase){MmUnmapIoSpace(ScBase,0x2000);ScBase=NULL;}}
static BOOLEAN ScNeedsOff;
static ULONG ScOriginal2c;
static BOOLEAN QchScStop(PSIA_LINK L){
    QCH_LINK*C=&L->Charge;ULONG R=0;
    if(!ScNeedsOff){C->CpActive=0;C->CpOffVerified=1;return TRUE;}
    if(!ScBegin()){C->CpError=1;return FALSE;}
    BOOLEAN Ok=ScWriteWire(2,0x78)&&ScReadWire(2,&R)&&!(R&1);
    if(Ok){Ok=ScWriteWire(4,6)&&ScWriteWire(0x2c,(UCHAR)ScOriginal2c)&&ScReadWire(4,&R)&&R==6;if(Ok)Ok=ScReadWire(0x2c,&R)&&R==ScOriginal2c;}
    BOOLEAN Restored=ScEnd();
    if(!Ok||!Restored){C->CpError=2;return FALSE;}
    C->CpActive=0;C->CpPrepared=0;C->CpOffVerified=1;ScNeedsOff=FALSE;return TRUE;
}
static BOOLEAN QchScPrepare(PSIA_LINK L){
    QCH_LINK*C=&L->Charge;ULONG R=0,Id=0,R0=0,R1=0;
    if(ScNeedsOff || !ScBegin()){C->CpError=3;return FALSE;}
    BOOLEAN Ok=ScReadWire(0x14,&Id)&&Id==7&&ScReadWire(0,&R0)&&(R0==0x50||R0==0x54)&&ScReadWire(1,&R1)&&R1==0x5e&&ScReadWire(0x2c,&ScOriginal2c);
    if(Ok){ScNeedsOff=TRUE;C->CpTouched=1;Ok=ScWriteWire(2,0x78)&&ScReadWire(2,&R)&&!(R&1);}
    if(Ok){Ok=ScWriteWire(4,6)&&ScWriteWire(0x2c,0xd1)&&ScReadWire(0x2c,&R)&&R==0xd1;}
    BOOLEAN Restored=ScEnd();
    if(!Ok||!Restored){C->CpError=4;QchScStop(L);return FALSE;}
    C->CpPrepared=1;C->CpOffVerified=1;return TRUE;
}
static BOOLEAN QchScEnable(PSIA_LINK L){
    QCH_LINK*C=&L->Charge;ULONG R=0;
    if(!C->CpPrepared||!ScNeedsOff||!ScBegin()){C->CpError=5;return FALSE;}
    C->CpOffVerified=0;
    // Same native enable sequence, with hardware watchdog left ON while current can flow.
    BOOLEAN Ok=ScWriteWire(4,0x36)&&ScWriteWire(2,0x7a)&&ScWriteWire(2,0x7b)&&ScWriteWire(4,0x36)&&ScReadWire(2,&R)&&(R&1);

    if(Ok){Ok=ScReadWire(4,&R)&&R==0x36;}
    BOOLEAN Restored=ScEnd();
    if(!Ok||!Restored){C->CpError=6;QchScStop(L);return FALSE;}
    C->CpActive=1;return TRUE;
}
static BOOLEAN QchScKick(PSIA_LINK L){
    QCH_LINK*C=&L->Charge;ULONG R=0;
    if(!C->CpActive||!ScNeedsOff||!ScBegin()){C->CpError=7;return FALSE;}
    BOOLEAN Ok=ScReadWire(2,&R)&&(R&1)&&ScWriteWire(4,0x36);
    if(Ok){Ok=ScReadWire(4,&R)&&R==0x36;}
    BOOLEAN Restored=ScEnd();if(!Ok||!Restored){C->CpError=8;return FALSE;}
    return TRUE;
}
static VOID QchScRelease(PSIA_LINK L){QchScStop(L);ScUnmap();}
