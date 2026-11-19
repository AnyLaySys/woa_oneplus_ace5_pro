#pragma once

static NTSTATUS CamPoll(PUCHAR base,ULONG offset,ULONG mask,ULONG value,ULONG us) {
    ULONGLONG deadline=KeQueryInterruptTime()+(ULONGLONG)us*10;
    do {if((CamRead(base,offset)&mask)==value)return STATUS_SUCCESS;CamDelay(10);}while(KeQueryInterruptTime()<deadline);
    return (CamRead(base,offset)&mask)==value?STATUS_SUCCESS:STATUS_IO_TIMEOUT;
}
