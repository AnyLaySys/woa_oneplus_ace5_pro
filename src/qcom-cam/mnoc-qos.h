#pragma once

static const ULONG CamMnocQosRegs[]={0x29008,0x2a008,0x37008};
static const ULONG CamMnocQosValues[]={8,8,0x01000028};
#define CAM_MNOC_QOS_MASK 0x01000078u

static inline ULONG CamMnocQosValue(ULONG index,ULONG original) {
    return (original&~CAM_MNOC_QOS_MASK)|CamMnocQosValues[index];
}

static inline ULONG CamMnocQosRestore(ULONG current,ULONG original) {
    return (current&~CAM_MNOC_QOS_MASK)|(original&CAM_MNOC_QOS_MASK);
}
