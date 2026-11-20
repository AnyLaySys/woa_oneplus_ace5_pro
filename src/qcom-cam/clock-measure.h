#pragma once

static NTSTATUS CamMeasureCount(PUCHAR base,ULONG ticks,ULONG *count) {
    ULONG original=CamRead(base,0x62048);
    if(original&0x100000)return STATUS_DEVICE_BUSY;
    ULONG clear=(original&~0x100000u)|0x200000;
    CamWrite(base,0x62048,clear);CamDelay(1);CamWrite(base,0x62048,clear&~0x200000u);
    NTSTATUS status=CamWait(base,0x6204c,0x2000000,0,1000);
    if(!NT_SUCCESS(status))return status;
    ULONG control=(original&~0x3fffffu)|(ticks&0xfffff)|0x100000;
    CamWrite(base,0x62048,control);
    status=CamWait(base,0x6204c,0x2000000,0x2000000,50000);
    CamWrite(base,0x62048,control&~0x100000u);
    if(NT_SUCCESS(status))*count=CamRead(base,0x6204c)&0x1ffffff;
    return status;
}

static ULONGLONG CamMeasuredHz(ULONG shortCount,ULONG fullCount,ULONG divisor) {
    if(shortCount==fullCount)return 0;
    return (((ULONGLONG)fullCount*10+15)*4800000/((ULONGLONG)0x20000*10+35))*divisor;
}
