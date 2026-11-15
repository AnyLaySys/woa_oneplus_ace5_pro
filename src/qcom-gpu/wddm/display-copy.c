#include "adapter.h"

static BOOLEAN RectValid(ULONG width,ULONG height,const RECT *r) {
    return width && height && width<=16384 && height<=16384 && r->left>=0 && r->top>=0 &&
        r->right>=r->left && r->bottom>=r->top && (ULONG)r->right<=width && (ULONG)r->bottom<=height;
}
NTSTATUS A830BlitFrame(PUCHAR dst,ULONG pitch,ULONG width,ULONG height,const VOID *src,LONG srcPitch,const RECT *r) {
    if(!dst || !src || !r || !RectValid(width,height,r) || pitch<width*4 || srcPitch<=0 || (ULONG)srcPitch<width*4)return STATUS_INVALID_PARAMETER;
    SIZE_T bytes=(SIZE_T)(r->right-r->left)*4;
    for(LONG y=r->top;y<r->bottom;y++)RtlCopyMemory(dst+(SIZE_T)y*pitch+(SIZE_T)r->left*4,
        (const UCHAR *)src+(SIZE_T)y*(ULONG)srcPitch+(SIZE_T)r->left*4,bytes);
    return STATUS_SUCCESS;
}
NTSTATUS A830MoveFrame(PUCHAR dst,ULONG pitch,ULONG width,ULONG height,const POINT *src,const RECT *r) {
    if(!dst || !src || !r || !RectValid(width,height,r) || pitch<width*4 || src->x<0 || src->y<0 ||
        (ULONG)src->x>width || (ULONG)src->y>height || (ULONG)(r->right-r->left)>width-(ULONG)src->x ||
        (ULONG)(r->bottom-r->top)>height-(ULONG)src->y)return STATUS_INVALID_PARAMETER;
    LONG rows=r->bottom-r->top;SIZE_T bytes=(SIZE_T)(r->right-r->left)*4;
    for(LONG i=0;i<rows;i++) {
        LONG y=r->top>src->y?rows-i-1:i;
        RtlMoveMemory(dst+(SIZE_T)(r->top+y)*pitch+(SIZE_T)r->left*4,
            dst+(SIZE_T)(src->y+y)*pitch+(SIZE_T)src->x*4,bytes);
    }
    return STATUS_SUCCESS;
}
