#pragma once
#ifndef __cplusplus
enum {
    DXGK_DT_OLED=3,DXGK_DU_GENERIC=1,DXGK_DDT_EDID=1,LinkConfigurationSucceeded=14,
    DXGK_GLITCH_CAUSE_NONE=255,DXGK_GLITCH_EFFECT_SEAMLESS=255,DXGK_GLITCH_DURATION_NONE=255
};
#endif
static inline BOOLEAN A830ScanoutFormat(ULONG format) {
    return format==D3DDDIFMT_X8R8G8B8 || format==D3DDDIFMT_A8R8G8B8 || format==D3DDDIFMT_A8B8G8R8 ||
        format==D3DDDIFMT_A2R10G10B10 || format==D3DDDIFMT_A2B10G10R10;
}
static inline DXGK_COLORIMETRY A830Colorimetry(VOID) {
    DXGK_COLORIMETRY c={0};
    c.RedPoint.cx=696;c.RedPoint.cy=328;c.GreenPoint.cx=271;c.GreenPoint.cy=707;
    c.BluePoint.cx=154;c.BluePoint.cy=61;c.WhitePoint.cx=320;c.WhitePoint.cy=337;
    c.MinLuminance=4000;c.MaxLuminance=c.MaxFullFrameLuminance=2000000;
    c.FormatBitDepths.Rgb=D3DKMDT_BITS_PER_COMPONENT_08|D3DKMDT_BITS_PER_COMPONENT_10;
    c.StandardColorimetryFlags.BT2020RGB=c.StandardColorimetryFlags.ST2084=1;return c;
}
