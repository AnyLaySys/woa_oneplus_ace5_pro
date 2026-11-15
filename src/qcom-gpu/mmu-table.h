#pragma once
#define GPU_TABLE_BYTES 0x2100000u
static __forceinline PULONGLONG Entry(GPU_VM *vm,ULONGLONG address,BOOLEAN create) {
    PULONGLONG table=(PULONGLONG)vm->Tables.Cpu;
    for(ULONG shift=30;shift>12;shift-=9) {
        PULONGLONG p=table+((address>>shift)&511);
        if(!*p) {
            if(!create || vm->Used>=vm->Tables.Size/4096)return NULL;
            *p=(vm->Tables.Physical+((ULONGLONG)vm->Used++<<12))|3;
        }
        ULONGLONG offset=(*p&0xFFFFFFFFF000ULL)-vm->Tables.Physical;
        if(offset>=vm->Tables.Size)return NULL;
        table=(PULONGLONG)(vm->Tables.Cpu+offset);
    }
    return table+((address>>12)&511);
}
