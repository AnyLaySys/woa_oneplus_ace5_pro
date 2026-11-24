#pragma once

#define CAM_FRAME_MAP_IOVA 0x10000000ULL
#define CAM_FRAME_MAP_TABLE_BYTES 139264u

static BOOLEAN CamFrameMapTable(PULONGLONG table,ULONGLONG tablePhysical,ULONGLONG physical,ULONG bytes) {
    if(!table||!tablePhysical||((tablePhysical|physical|bytes)&4095)||!bytes||bytes>16384u*4096u||
       tablePhysical>0xffffffffULL-CAM_FRAME_MAP_TABLE_BYTES||physical>0xfffffffffULL-bytes+1)return FALSE;
    RtlZeroMemory(table,CAM_FRAME_MAP_TABLE_BYTES);
    table[0]=(tablePhysical+4096)|3;
    for(ULONG page=0;page<bytes/4096;page++) {
        ULONG leaf=page/512;
        table[512+((CAM_FRAME_MAP_IOVA>>21)&511)+leaf]=(tablePhysical+(leaf+2)*4096)|3;
        table[(leaf+2)*512+(page&511)]=(physical+(ULONGLONG)page*4096)|0xe43;
    }
    return TRUE;
}
