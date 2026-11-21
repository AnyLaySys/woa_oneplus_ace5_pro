/* Copyright (C) Microsoft Corporation. All rights reserved. */
#pragma once
#include <ntddk.h>
#include <wdf.h>
#include <hidport.h>
#include "hummer_spi.h"
#include "hummer_rpmh.h"
#include "tap.h"
#include "..\..\qcom-keys\haptics.h"

#define S3910_REPORT_ID 0x54
#define S3910_CONTACTS 10
#define S3910_TOUCH_BYTES 160

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE OnPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE OnReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY OnD0Entry;
EVT_WDF_DEVICE_D0_EXIT OnD0Exit;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtIoDeviceControl;

typedef struct {
    WDFDEVICE Device;
    WDFQUEUE Reports;
    WDFTIMER InitTimer,PollTimer;
    PHSP_CTX Spi;
    HRPMH Rpmh;
    volatile LONG Stopping;
    BOOLEAN Ready,RetryInit;
    UCHAR InitAttempts;
    ULONG PanelMaxX,PanelMaxY,MappedCount,MappedSize[8];
    PVOID MappedBase[8];
    UCHAR Reply[HSP_DATA_BYTES];
    WDFIOTARGET Haptics;
    TAP Tap;
} DEVICE_CONTEXT,*PDEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT,GetDeviceContext);
