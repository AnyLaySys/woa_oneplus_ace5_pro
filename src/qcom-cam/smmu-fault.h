#pragma once

#define CAM_SMMU_FSR_FAULT 0xc00001feu

static BOOLEAN CamSmmuFault(ULONG fsr) { return (fsr&CAM_SMMU_FSR_FAULT)!=0; }
