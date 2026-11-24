#pragma once

static NTSTATUS CamTbuWait(PUCHAR base,ULONG offset,ULONGLONG mask,ULONGLONG expected) {
    ULONGLONG deadline=KeQueryInterruptTime()+1000;
    do {
        if((CamRead64(base,offset)&mask)==expected)return STATUS_SUCCESS;
        CamDelay(1);
    } while(KeQueryInterruptTime()<deadline);
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS CamTbuTranslateExpected(PUCHAR base,ULONG sid,ULONGLONG address,ULONGLONG expected,ULONG attributes,ULONGLONG sample[8]) {
    RtlZeroMemory(sample,8*sizeof(*sample));
    if(!((sid==0x1860&&attributes==0x408)||(sid==0x1c00&&attributes==0x409))||
       !address||(address&255)||address>0xffffff00u||!expected||(expected&255)||expected>0xfffffff00ULL)return STATUS_INVALID_PARAMETER;
    sample[0]=CamRead64(base,0x410);sample[1]=CamRead64(base,0x418);
    sample[2]=CamRead64(base,0x450);sample[3]=CamRead64(base,0x458);
    if(((sample[0]|sample[1])&1)||CamRead64(base,0x448)||(sample[2]&1)||
       CamRead64(base,0xc10)||CamRead64(base,0xc20))return STATUS_DEVICE_BUSY;
    ULONGLONG original[3];
    for(ULONG i=0;i<3;i++)original[i]=CamRead64(base,0x430+i*8);
    CamWrite64(base,0x410,sample[0]|1);
    NTSTATUS status=CamTbuWait(base,0x418,1,1);
    if(NT_SUCCESS(status)) {
        CamWrite64(base,0x430,sid&0x3ff);
        CamWrite64(base,0x438,attributes);
        CamWrite64(base,0x440,address);
        CamWrite64(base,0x448,1);
        ULONGLONG deadline=KeQueryInterruptTime()+1000;
        status=STATUS_IO_TIMEOUT;
        do {
            sample[4]=CamRead64(base,0x450);sample[5]=CamRead64(base,0x458);
            if(sample[5]&1){status=STATUS_DATA_ERROR;break;}
            if(sample[4]&1) {
                status=(sample[5]>>12)==expected?STATUS_SUCCESS:STATUS_DATA_ERROR;
                break;
            }
            CamDelay(1);
        } while(KeQueryInterruptTime()<deadline);
        CamWrite64(base,0x448,0);sample[6]=CamRead64(base,0x448);
        if(sample[6]){sample[7]=CamRead64(base,0x418);return STATUS_DEVICE_BUSY;}
        for(ULONG i=0;i<3;i++)CamWrite64(base,0x430+i*8,original[i]);
    }
    CamWrite64(base,0x410,sample[0]);
    NTSTATUS resumed=CamTbuWait(base,0x418,1,0);sample[7]=CamRead64(base,0x418);
    return NT_SUCCESS(resumed)?status:STATUS_DEVICE_BUSY;
}

static NTSTATUS CamTbuTranslateAccess(PUCHAR base,ULONG sid,ULONGLONG address,ULONG attributes,ULONGLONG sample[8]) {
    return CamTbuTranslateExpected(base,sid,address,address,attributes,sample);
}

static NTSTATUS CamTbuTranslatePage(PUCHAR base,ULONG sid,ULONGLONG address,ULONGLONG sample[8]) {
    return CamTbuTranslateAccess(base,sid,address,0x408,sample);
}
