#pragma once

/* sun-camera.dtsi pairs RT-CDM2 with IFE2. OEM
   cam_vfe_top_ver4_get_base() and cam_ife_csid_get_base() subtract
   rt-wrapper-base for CAM_CDM_RT; reg-cam-base alone addresses the
   external camera AHB aperture, not this CDM's local wrapper. */
#define CAM_CDM_RT_WRAPPER_BASE 0x86000u
#define CAM_CDM_TFE2_BASE (0xa6000u-CAM_CDM_RT_WRAPPER_BASE)
#define CAM_CDM_CSID2_BASE (0x12d000u-CAM_CDM_RT_WRAPPER_BASE)
