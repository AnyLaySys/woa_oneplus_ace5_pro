#pragma once

/* The caller holds the display interrupt lock. PresentId belongs to the
   frame latched by the display, not the most recently queued primary. */
static inline VOID A830BuildVsyncNotification(DXGK_INTERRUPT_TYPE type,ULONGLONG address,ULONGLONG presentId,
    DXGKARGCB_NOTIFY_INTERRUPT_DATA *n,DXGK_MULTIPLANE_OVERLAY_VSYNC_INFO *legacy,
    DXGK_MULTIPLANE_OVERLAY_VSYNC_INFO2 *modern) {
    RtlZeroMemory(n,sizeof(*n));n->Flags.ValidPhysicalAdapterMask=1;
    if(type==DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY2) {
        RtlZeroMemory(modern,sizeof(*modern));modern->PresentId=presentId;
        n->InterruptType=type;n->CrtcVsyncWithMultiPlaneOverlay2.PhysicalAdapterMask=1;
        n->CrtcVsyncWithMultiPlaneOverlay2.MultiPlaneOverlayVsyncInfoCount=1;
        n->CrtcVsyncWithMultiPlaneOverlay2.pMultiPlaneOverlayVsyncInfo=modern;
    } else if(type==DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY) {
        RtlZeroMemory(legacy,sizeof(*legacy));legacy->Enabled=address!=0;
        legacy->PhysicalAddress.QuadPart=(LONGLONG)address;
        n->InterruptType=type;n->CrtcVsyncWithMultiPlaneOverlay.PhysicalAdapterMask=1;
        n->CrtcVsyncWithMultiPlaneOverlay.MultiPlaneOverlayVsyncInfoCount=1;
        n->CrtcVsyncWithMultiPlaneOverlay.pMultiPlaneOverlayVsyncInfo=legacy;
    } else {
        n->InterruptType=DXGK_INTERRUPT_CRTC_VSYNC;n->CrtcVsync.PhysicalAdapterMask=1;
        n->CrtcVsync.PhysicalAddress.QuadPart=(LONGLONG)address;
    }
}
