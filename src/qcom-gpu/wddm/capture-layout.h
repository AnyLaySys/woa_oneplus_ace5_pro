#pragma once
#include "../gpu.h"
/* Diagnostic reads use an existing mapping only. A physical address inside a
   scattered BO is not sufficient to locate its pixels and must be rejected. */
static __forceinline BOOLEAN A830CaptureBo(const GPU_BO *bo,ULONGLONG physical,ULONG pitch,ULONG width,ULONG height,PUCHAR *source) {
    if(!bo->Mdl || !bo->Cpu || !bo->Contiguous || !width || width>MAXULONG/4 || !height || pitch<width*4 || physical<bo->Physical)return FALSE;
    ULONGLONG offset=physical-bo->Physical;
    ULONGLONG bytes=(ULONGLONG)(height-1)*pitch+(ULONGLONG)width*4;
    if(offset>=bo->Size || bytes>bo->Size-offset)return FALSE;
    *source=bo->Cpu+(SIZE_T)offset;return TRUE;
}
