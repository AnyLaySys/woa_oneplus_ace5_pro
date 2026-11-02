/*
 * apps RSC ("RPMh") DRV-2 vote for the touch panel rails on hummer (SM8750).
 *
 * The touch node has no enable GPIO: sm8750-oneplus-hummer-bringup.dtsi gives
 *   vddio-supply = <&vreg_l4b_1p8>;     PM8550B LDO4,  1.8 V
 *   avdd-supply  = <&vreg_l16b_2p8>;    PM8550B LDO16, 3.104 V
 * Both rails are RPMh regulators, so nothing in the SoC can switch them on by
 * writing a TLMM register. Without this vote the panel is unpowered, MISO reads
 * a steady 0x00 and the ATTN line never moves - which looks exactly like a SPI
 * or protocol failure.
 *
 * Layout from Linux rpmh-rsc.c (ver_2_7 / ver_3_0) and the working XNU driver
 * for the same SoC family.
 */
#ifndef HUMMER_RPMH_H
#define HUMMER_RPMH_H

#include <ntddk.h>

#define HRPMH_OK           0
#define HRPMH_ERR_PARAM    (-22)
#define HRPMH_ERR_LAYOUT   (-95)
#define HRPMH_ERR_CMDDb    (-19)
#define HRPMH_ERR_BUSY     (-16)
#define HRPMH_ERR_IO       (-5)
#define HRPMH_ERR_NOT_DONE (-110)

typedef struct _HRPMH
{
    PUCHAR Rsc;
    PUCHAR Db;
    ULONG  Major;
    ULONG  Minor;
    ULONG  SBase;
    ULONG  HBase;

    BOOLEAN Initialized;
    BOOLEAN Faulted;
} HRPMH, *PHRPMH;

NTSTATUS
HrpmhInit (
    _Out_ PHRPMH Ctx,
    _In_  PUCHAR Rsc,
    _In_  PUCHAR CmdDb
    );

NTSTATUS
HrpmhVoteLdo (
    _In_ PHRPMH Ctx,
    _In_ ULONG  Id,
    _In_ ULONG  MilliVolts
    );

#endif
