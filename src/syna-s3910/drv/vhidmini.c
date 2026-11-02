/*++

Copyright (C) Microsoft Corporation, All Rights Reserved.

Module Name:

    vhidmini.cpp

Abstract:

    This module contains the implementation of the driver

Environment:

    Windows Driver Framework (WDF)

--*/

#include "vhidmini.h"

static NTSTATUS S3910BringUp(_In_ PDEVICE_CONTEXT pDevice);
static VOID S3910InitializeTimer(_In_ WDFTIMER Timer);
static VOID S3910PollTouch(_In_ WDFDEVICE Device);
static void S3910PollTimer(_In_ WDFTIMER Timer);
static NTSTATUS HummerSpiTransfer(_In_ PDEVICE_CONTEXT pDevice, _In_opt_ PVOID pInputBuffer, _Out_opt_ PVOID pOutputBuffer, _In_ size_t length);

BYTE cmd_reset[1] = { 0x04 };
BYTE cmd_tcm2_ack[1] = { 0x07 };
BYTE cmd_get_application_info[3] = { 0x20, 0x00, 0x00 };
BYTE eventbuf[256];

typedef struct {
    BYTE version[2];
    BYTE status[2];
    BYTE static_config_size[2];
    BYTE dynamic_config_size[2];
    BYTE app_config_start_write_block[2];
    BYTE app_config_size[2];
    BYTE max_touch_report_config_size[2];
    BYTE max_touch_report_payload_size[2];
    BYTE customer_config_id[16];
    BYTE max_x[2];
    BYTE max_y[2];
    BYTE max_objects[2];
    BYTE num_of_buttons[2];
    BYTE num_of_image_rows[2];
    BYTE num_of_image_cols[2];
    BYTE has_hybrid_data[2];
} tcm_application_info_t;

typedef struct
{
    BYTE  reportId;                                 // Report ID = 0x54 (84) 'T'
                                                       // Collection: TouchScreen
    BYTE  DIG_TouchScreenContactCountMaximum;       // Usage 0x000D0055: Contact Count Maximum, Value = 0 to 8
} featureReport54_t;

typedef struct __declspec(align(2))
{
    BYTE  DIG_TouchScreenFingerState;               // Usage 0x000D0042: Tip Switch, Value = 0 to 1
    BYTE  DIG_TouchScreenFingerContactIdentifier;   // Usage 0x000D0051: Contact Identifier, Value = 0 to 1
    BYTE GD_TouchScreenFingerXL;                    // Usage 0x00010030: X, Value = 0 to 32767
    BYTE GD_TouchScreenFingerXH;                    // Usage 0x00010030: X, Value = 0 to 32767
    BYTE GD_TouchScreenFingerYL;                    // Usage 0x00010031: Y, Value = 0 to 32767
    BYTE GD_TouchScreenFingerYH;                    // Usage 0x00010031: Y, Value = 0 to 32767
}inputpoint;

typedef struct __declspec(align(2))
{
    BYTE  reportId;                                 // Report ID = 0x54 (84) 'T'
                                                       // Collection: TouchScreen Finger
    BYTE points[60];

    BYTE  DIG_TouchScreenContactCount;              // Usage 0x000D0054: Contact Count, Value = 0 to 8
} inputReport54_t;

//
// This is the default report descriptor for the virtual Hid device returned
// by the mini driver in response to IOCTL_HID_GET_REPORT_DESCRIPTOR.
//
/*HID_REPORT_DESCRIPTOR       G_DefaultReportDescriptor[] = {
    0x06,0x00, 0xFF,                // USAGE_PAGE (Vender Defined Usage Page)
    0x09,0x01,                      // USAGE (Vendor Usage 0x01)
    0xA1,0x01,                      // COLLECTION (Application)
    0x85,CONTROL_FEATURE_REPORT_ID,    // REPORT_ID (1)
    0x09,0x01,                         // USAGE (Vendor Usage 0x01)
    0x15,0x00,                         // LOGICAL_MINIMUM(0)
    0x26,0xff, 0x00,                   // LOGICAL_MAXIMUM(255)
    0x75,0x08,                         // REPORT_SIZE (0x08)
    0x96,(FEATURE_REPORT_SIZE_CB & 0xff), (FEATURE_REPORT_SIZE_CB >> 8), // REPORT_COUNT
    0xB1,0x00,                         // FEATURE (Data,Ary,Abs)
    0x09,0x01,                         // USAGE (Vendor Usage 0x01)
    0x75,0x08,                         // REPORT_SIZE (0x08)
    0x96,(INPUT_REPORT_SIZE_CB & 0xff), (INPUT_REPORT_SIZE_CB >> 8), // REPORT_COUNT
    0x81,0x00,                         // INPUT (Data,Ary,Abs)
    0x09,0x01,                         // USAGE (Vendor Usage 0x01)
    0x75,0x08,                         // REPORT_SIZE (0x08)
    0x96,(OUTPUT_REPORT_SIZE_CB & 0xff), (OUTPUT_REPORT_SIZE_CB >> 8), // REPORT_COUNT
    0x91,0x00,                         // OUTPUT (Data,Ary,Abs)
    0xC0,                           // END_COLLECTION
};*/

HID_REPORT_DESCRIPTOR       G_DefaultReportDescriptor[] = {
    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x04,     //   (LOCAL)USAGE              0x000D0004 Touch Screen(Application Collection)
    0xA1, 0x01,     //   (MAIN)COLLECTION         0x01 Application(Usage = 0x000D0004: Page = Digitizer Device Page, Usage = Touch Screen, Type = Application Collection)
    0x85, 0x54,     //     (GLOBAL)REPORT_ID          0x54 (84) 'T'

    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x14,           //    (GLOBAL)LOGICAL_MINIMUM(0)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //46 47
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //55 56
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical
    
    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical

    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical

    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical

    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical
    
    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical

    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical

    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical

    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical

    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0xFF, 0x7F,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical

    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x54,     //     (LOCAL)USAGE              0x000D0054 Contact Count(Dynamic Value)
    0x75, 0x08,     //     (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x25, 0x0A,     //     (GLOBAL)LOGICAL_MAXIMUM    0x08 (8)
    0x81, 0x02,     //     (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x55,     //     (LOCAL)USAGE              0x000D0055 Contact Count Maximum(Static Value)
    0xB1, 0x02,     //     (MAIN)FEATURE            0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Application

};

featureReport54_t features = {0x54,10};
//
// This is the default HID descriptor returned by the mini driver
// in response to IOCTL_HID_GET_DEVICE_DESCRIPTOR. The size
// of report descriptor is currently the size of G_DefaultReportDescriptor.
//

HID_DESCRIPTOR              G_DefaultHidDescriptor = {
    0x09,   // length of HID descriptor
    0x21,   // descriptor type == HID  0x21
    0x0100, // hid spec release
    0x00,   // country code == Not Specified
    0x01,   // number of HID class descriptors
    {                                       //DescriptorList[0]
        0x22,                               //report descriptor type 0x22
        sizeof(G_DefaultReportDescriptor)   //total length of report descriptor
    }
};

NTSTATUS
DriverEntry(
    _In_  PDRIVER_OBJECT    DriverObject,
    _In_  PUNICODE_STRING   RegistryPath
    )
/*++

Routine Description:
    DriverEntry initializes the driver and is the first routine called by the
    system after the driver is loaded. DriverEntry specifies the other entry
    points in the function driver, such as EvtDevice and DriverUnload.

Parameters Description:

    DriverObject - represents the instance of the function driver that is loaded
    into memory. DriverEntry must initialize members of DriverObject before it
    returns to the caller. DriverObject is allocated by the system before the
    driver is loaded, and it is released by the system after the system unloads
    the function driver from memory.

    RegistryPath - represents the driver specific path in the Registry.
    The function driver can use the path to store driver related data between
    reboots. The path does not store hardware instance specific data.

Return Value:

    STATUS_SUCCESS, or another status value for which NT_SUCCESS(status) equals
                    TRUE if successful,

    STATUS_UNSUCCESSFUL, or another status for which NT_SUCCESS(status) equals
                    FALSE otherwise.

--*/
{
    WDF_DRIVER_CONFIG       config;
    WDF_OBJECT_ATTRIBUTES driverAttributes;
    NTSTATUS                status;


#ifdef _KERNEL_MODE
    //
    // Opt-in to using non-executable pool memory on Windows 8 and later.
    // https://msdn.microsoft.com/en-us/library/windows/hardware/hh920402(v=vs.85).aspx
    //
    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);
#endif

    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);

    WDF_OBJECT_ATTRIBUTES_INIT(&driverAttributes);
    driverAttributes.EvtCleanupCallback = EvtDriverCleanup;

    status = WdfDriverCreate(DriverObject,
                            RegistryPath,
                            &driverAttributes,
                            &config,
                            WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {

        goto Exit;
    }
Exit:
    return status;
}
VOID
EvtDriverCleanup(
    _In_ WDFOBJECT Object
)
{
    UNREFERENCED_PARAMETER(Object);
}
NTSTATUS
EvtDeviceAdd(
    _In_  WDFDRIVER         Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
/*++
Routine Description:

    EvtDeviceAdd is called by the framework in response to AddDevice
    call from the PnP manager. We create and initialize a device object to
    represent a new instance of the device.

Arguments:

    Driver - Handle to a framework driver object created in DriverEntry

    DeviceInit - Pointer to a framework-allocated WDFDEVICE_INIT structure.

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS                status;
    WDF_OBJECT_ATTRIBUTES   deviceAttributes;
    WDFDEVICE               device;
    PDEVICE_CONTEXT         deviceContext;
    PHID_DEVICE_ATTRIBUTES  hidAttributes;
    UNREFERENCED_PARAMETER  (Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);

    pnpCallbacks.EvtDevicePrepareHardware = OnPrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = OnReleaseHardware;
    pnpCallbacks.EvtDeviceD0Entry = OnD0Entry;
    pnpCallbacks.EvtDeviceD0Exit = OnD0Exit;


    WdfFdoInitSetFilter(DeviceInit);
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
                            &deviceAttributes,
                            DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit,
                            &deviceAttributes,
                            &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    deviceContext = GetDeviceContext(device);
    deviceContext->Device       = device;
    deviceContext->DeviceData   = 0;

    hidAttributes = &deviceContext->HidDeviceAttributes;
    RtlZeroMemory(hidAttributes, sizeof(HID_DEVICE_ATTRIBUTES));
    hidAttributes->Size         = sizeof(HID_DEVICE_ATTRIBUTES);
    hidAttributes->VendorID     = HIDMINI_VID;
    hidAttributes->ProductID    = HIDMINI_PID;
    hidAttributes->VersionNumber = HIDMINI_VERSION;

    status = QueueCreate(device,
                         &deviceContext->DefaultQueue);
    if( !NT_SUCCESS(status) ) {
        return status;
    }

    status = ManualQueueCreate(device,
                               &deviceContext->ManualQueue);
    if( !NT_SUCCESS(status) ) {
        return status;
    }

    //
    // Use default "HID Descriptor" (hardcoded). We will set the
    // wReportLength memeber of HID descriptor when we read the
    // the report descriptor either from registry or the hard-coded
    // one.
    //
    deviceContext->HidDescriptor = G_DefaultHidDescriptor;
    deviceContext->ReportDescriptor = G_DefaultReportDescriptor;

    status = STATUS_SUCCESS;

    return status;
}

NTSTATUS
OnPrepareHardware(
        _In_  WDFDEVICE     FxDevice,
        _In_  WDFCMRESLIST  FxResourcesRaw,
        _In_  WDFCMRESLIST  FxResourcesTranslated
    )
    /*++

        Routine Description:

        This routine caches the SPB resource connection ID.

        Arguments:

        FxDevice - a handle to the framework device object
        FxResourcesRaw - list of translated hardware resources that
            the PnP manager has assigned to the device
        FxResourcesTranslated - list of raw hardware resources that
            the PnP manager has assigned to the device

        Return Value:

        Status

    --*/
{
    PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG resourceCount;
    ULONG i;
    static const ULONG expectedBase[7] = { 0x00A90000, 0x00A00000, 0x0F100000, 0x00100000, 0x16520000, 0x81C60000, 0x15000000 };
    static const ULONG expectedSize[7] = { 0x4000, 0x60000, 0x102000, 0x1F4200, 0x10000, 0x20000, 0x1000 };

    UNREFERENCED_PARAMETER(FxResourcesRaw);

    pDevice->MappedCount = 0;

    //
    // This node is described with memory windows only: the GENI serial engine,
    // the GPI block, TLMM, GCC, the apps RSC and the cmd-db. The SPI, GPIO and
    // interrupt-controller drivers those would normally come from do not exist
    // for Windows here, so the driver drives the hardware from these windows.
    //

    resourceCount = WdfCmResourceListGetCount(FxResourcesTranslated);

    for (i = 0; i < resourceCount; i++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR pDescriptor;

        pDescriptor = WdfCmResourceListGetDescriptor(
            FxResourcesTranslated, i);

        if (pDescriptor->Type != CmResourceTypeMemory)
        {
            continue;
        }

        if (pDevice->MappedCount >= 7)
        {
            break;
        }

        if (pDescriptor->u.Memory.Start.QuadPart != expectedBase[pDevice->MappedCount] ||
            pDescriptor->u.Memory.Length < expectedSize[pDevice->MappedCount])
        {
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
            break;
        }

        pDevice->MappedBase[pDevice->MappedCount] =
            MmMapIoSpace(pDescriptor->u.Memory.Start,
                         pDescriptor->u.Memory.Length,
                         MmNonCached);

        if (pDevice->MappedBase[pDevice->MappedCount] == NULL)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        pDevice->MappedPhys[pDevice->MappedCount] =
            (ULONG)pDescriptor->u.Memory.Start.QuadPart;
        pDevice->MappedSize[pDevice->MappedCount] =
            pDescriptor->u.Memory.Length;
        pDevice->MappedCount++;
    }

    if (NT_SUCCESS(status) && pDevice->MappedCount == 0)
    {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    return status;
}

NTSTATUS
    OnReleaseHardware(
        _In_  WDFDEVICE     FxDevice,
        _In_  WDFCMRESLIST  FxResourcesTranslated
    )
    /*++

        Routine Description:

        Arguments:

        FxDevice - a handle to the framework device object
        FxResourcesTranslated - list of raw hardware resources that
            the PnP manager has assigned to the device

        Return Value:

        Status

    --*/
{
    PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(FxResourcesTranslated);

    InterlockedExchange(&pDevice->Stopping, 1);
    if (pDevice->InitTimer != NULL)
    {
        WdfTimerStop(pDevice->InitTimer, TRUE);
    }
    if (pDevice->PollTimer != NULL)
    {
        WdfTimerStop(pDevice->PollTimer, TRUE);
    }

    if (pDevice->SpiContext != NULL)
    {
        status = HspDeinit((PHSP_CTX)pDevice->SpiContext);
        if (!NT_SUCCESS(status)) return status;
        ExFreePoolWithTag(pDevice->SpiContext, 'PSh9');
        pDevice->SpiContext = NULL;
    }

    for (ULONG m = 0; m < pDevice->MappedCount; m++)
    {
        if (pDevice->MappedBase[m] != NULL)
        {
            MmUnmapIoSpace(pDevice->MappedBase[m], pDevice->MappedSize[m]);
            pDevice->MappedBase[m] = NULL;
        }
    }
    pDevice->MappedCount = 0;

    return status;
}

NTSTATUS
OnD0Entry(
    _In_  WDFDEVICE               FxDevice,
    _In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

    Routine Description:

    This routine allocates objects needed by the driver.

    Arguments:

    FxDevice - a handle to the framework device object
    FxPreviousState - previous power state

    Return Value:

    Status

--*/
{
    UNREFERENCED_PARAMETER(FxPreviousState);

    PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);

    pDevice->SpiReady = FALSE;
    InterlockedExchange(&pDevice->Stopping, 0);
    if (pDevice->InitTimer == NULL) {
        WDF_TIMER_CONFIG timerConfig;
        WDF_OBJECT_ATTRIBUTES timerAttributes;
        NTSTATUS status;
        WDF_TIMER_CONFIG_INIT(&timerConfig, S3910InitializeTimer);
        WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
        timerAttributes.ParentObject = FxDevice;
        timerAttributes.ExecutionLevel = WdfExecutionLevelPassive;
        status = WdfTimerCreate(&timerConfig, &timerAttributes, &pDevice->InitTimer);
        if (!NT_SUCCESS(status)) return STATUS_SUCCESS;
    }
    WdfTimerStart(pDevice->InitTimer, WDF_REL_TIMEOUT_IN_MS(500));
    return STATUS_SUCCESS;
}

NTSTATUS
OnD0Exit(
    _In_  WDFDEVICE               FxDevice,
    _In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

    Routine Description:

    This routine destroys objects needed by the driver.

    Arguments:

    FxDevice - a handle to the framework device object
    FxPreviousState - previous power state

    Return Value:

    Status

--*/
{
    UNREFERENCED_PARAMETER(FxPreviousState);

    PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);

    InterlockedExchange(&pDevice->Stopping, 1);
    if (pDevice->InitTimer != NULL)
    {
        WdfTimerStop(pDevice->InitTimer, TRUE);
    }

    if (pDevice->PollTimer != NULL)
    {
        WdfTimerStop(pDevice->PollTimer, TRUE);
    }

    if (pDevice->SpiContext != NULL)
    {
        NTSTATUS cleanupStatus = HspDeinit((PHSP_CTX)pDevice->SpiContext);
        if (!NT_SUCCESS(cleanupStatus)) return cleanupStatus;
        ExFreePoolWithTag(pDevice->SpiContext, 'PSh9');
        pDevice->SpiContext = NULL;
    }
    pDevice->SpiReady = FALSE;

    return STATUS_SUCCESS;
}


#ifdef _KERNEL_MODE
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtIoDeviceControl;
#else
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL          EvtIoDeviceControl;
#endif

NTSTATUS
QueueCreate(
    _In_  WDFDEVICE         Device,
    _Out_ WDFQUEUE          *Queue
    )
/*++
Routine Description:

    This function creates a default, parallel I/O queue to proces IOCTLs
    from hidclass.sys.

Arguments:

    Device - Handle to a framework device object.

    Queue - Output pointer to a framework I/O queue handle, on success.

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS                status;
    WDF_IO_QUEUE_CONFIG     queueConfig;
    WDF_OBJECT_ATTRIBUTES   queueAttributes;
    WDFQUEUE                queue;
    PQUEUE_CONTEXT          queueContext;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
                            &queueConfig,
                            WdfIoQueueDispatchParallel);

#ifdef _KERNEL_MODE
    queueConfig.EvtIoInternalDeviceControl  = EvtIoDeviceControl;
#else
    //
    // HIDclass uses INTERNAL_IOCTL which is not supported by UMDF. Therefore
    // the hidumdf.sys changes the IOCTL type to DEVICE_CONTROL for next stack
    // and sends it down
    //
    queueConfig.EvtIoDeviceControl          = EvtIoDeviceControl;
#endif

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
                            &queueAttributes,
                            QUEUE_CONTEXT);

    status = WdfIoQueueCreate(
                            Device,
                            &queueConfig,
                            &queueAttributes,
                            &queue);

    if( !NT_SUCCESS(status) ) {
        return status;
    }

    queueContext = GetQueueContext(queue);
    queueContext->Queue         = queue;
    queueContext->DeviceContext = GetDeviceContext(Device);
    queueContext->OutputReport  = 0;

    *Queue = queue;
    
    return status;
}

VOID
EvtIoDeviceControl(
    _In_  WDFQUEUE          Queue,
    _In_  WDFREQUEST        Request,
    _In_  size_t            OutputBufferLength,
    _In_  size_t            InputBufferLength,
    _In_  ULONG             IoControlCode
    )
/*++
Routine Description:

    This event callback function is called when the driver receives an

    (KMDF) IOCTL_HID_Xxx code when handlng IRP_MJ_INTERNAL_DEVICE_CONTROL
    (UMDF) IOCTL_HID_Xxx, IOCTL_UMDF_HID_Xxx when handling IRP_MJ_DEVICE_CONTROL

Arguments:

    Queue - A handle to the queue object that is associated with the I/O request

    Request - A handle to a framework request object.

    OutputBufferLength - The length, in bytes, of the request's output buffer,
            if an output buffer is available.

    InputBufferLength - The length, in bytes, of the request's input buffer, if
            an input buffer is available.

    IoControlCode - The driver or system defined IOCTL associated with the request

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS                status;
    BOOLEAN                 completeRequest = TRUE;
    WDFDEVICE               device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT         deviceContext = NULL;
    PQUEUE_CONTEXT          queueContext = GetQueueContext(Queue);
    UNREFERENCED_PARAMETER  (OutputBufferLength);
    UNREFERENCED_PARAMETER  (InputBufferLength);

    deviceContext = GetDeviceContext(device);

    switch (IoControlCode)
    {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:   // METHOD_NEITHER
        //
        // Retrieves the device's HID descriptor.
        //
        _Analysis_assume_(deviceContext->HidDescriptor.bLength != 0);
        status = RequestCopyFromBuffer(Request,
                            &deviceContext->HidDescriptor,
                            deviceContext->HidDescriptor.bLength);
        break;

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:   // METHOD_NEITHER
        //
        //Retrieves a device's attributes in a HID_DEVICE_ATTRIBUTES structure.
        //
        status = RequestCopyFromBuffer(Request,
                            &queueContext->DeviceContext->HidDeviceAttributes,
                            sizeof(HID_DEVICE_ATTRIBUTES));
        break;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:   // METHOD_NEITHER
        //
        //Obtains the report descriptor for the HID device.
        //
        status = RequestCopyFromBuffer(Request,
                            deviceContext->ReportDescriptor,
                            deviceContext->HidDescriptor.DescriptorList[0].wReportLength);
        break;

    case IOCTL_HID_READ_REPORT:             // METHOD_NEITHER
        //
        // Returns a report from the device into a class driver-supplied
        // buffer.
        //
        status = ReadReport(queueContext, Request, &completeRequest);
        break;

    case IOCTL_HID_WRITE_REPORT:            // METHOD_NEITHER
        //
        // Transmits a class driver-supplied report to the device.
        //
        status = WriteReport(queueContext, Request);
        break;

#ifdef _KERNEL_MODE

    case IOCTL_HID_GET_FEATURE:             // METHOD_OUT_DIRECT

        status = GetFeature(queueContext, Request);
        break;

    case IOCTL_HID_SET_FEATURE:             // METHOD_IN_DIRECT

        status = SetFeature(queueContext, Request);
        break;

    case IOCTL_HID_GET_INPUT_REPORT:        // METHOD_OUT_DIRECT

        status = GetInputReport(queueContext, Request);
        break;

    case IOCTL_HID_SET_OUTPUT_REPORT:       // METHOD_IN_DIRECT

        status = SetOutputReport(queueContext, Request);
        break;

#else // UMDF specific

    //
    // HID minidriver IOCTL uses HID_XFER_PACKET which contains an embedded pointer.
    //
    //   typedef struct _HID_XFER_PACKET {
    //     PUCHAR reportBuffer;
    //     ULONG  reportBufferLen;
    //     UCHAR  reportId;
    //   } HID_XFER_PACKET, *PHID_XFER_PACKET;
    //
    // UMDF cannot handle embedded pointers when marshalling buffers between processes.
    // Therefore a special driver mshidumdf.sys is introduced to convert such IRPs to
    // new IRPs (with new IOCTL name like IOCTL_UMDF_HID_Xxxx) where:
    //
    //   reportBuffer - passed as one buffer inside the IRP
    //   reportId     - passed as a second buffer inside the IRP
    //
    // The new IRP is then passed to UMDF host and driver for further processing.
    //

    case IOCTL_UMDF_HID_GET_FEATURE:        // METHOD_NEITHER

        status = GetFeature(queueContext, Request);
        break;

    case IOCTL_UMDF_HID_SET_FEATURE:        // METHOD_NEITHER

        status = SetFeature(queueContext, Request);
        break;

    case IOCTL_UMDF_HID_GET_INPUT_REPORT:  // METHOD_NEITHER

        status = GetInputReport(queueContext, Request);
        break;

    case IOCTL_UMDF_HID_SET_OUTPUT_REPORT: // METHOD_NEITHER

        status = SetOutputReport(queueContext, Request);
        break;

#endif // _KERNEL_MODE

    case IOCTL_HID_GET_STRING:                      // METHOD_NEITHER

        status = GetString(Request);
        break;

    case IOCTL_HID_GET_INDEXED_STRING:              // METHOD_OUT_DIRECT

        status = GetIndexedString(Request);
        break;

    case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:  // METHOD_NEITHER
        //
        // This has the USBSS Idle notification callback. If the lower driver
        // can handle it (e.g. USB stack can handle it) then pass it down
        // otherwise complete it here as not inplemented. For a virtual
        // device, idling is not needed.
        //
        // Not implemented. fall through...
        //
    case IOCTL_HID_ACTIVATE_DEVICE:                 // METHOD_NEITHER
    case IOCTL_HID_DEACTIVATE_DEVICE:               // METHOD_NEITHER
    case IOCTL_GET_PHYSICAL_DESCRIPTOR:             // METHOD_OUT_DIRECT
        //
        // We don't do anything for these IOCTLs but some minidrivers might.
        //
        // Not implemented. fall through...
        //
    default:
        status = STATUS_NOT_IMPLEMENTED;
        break;
    }

    //
    // Complete the request. Information value has already been set by request
    // handlers.
    //
    if (completeRequest) {
        WdfRequestComplete(Request, status);
    }
}

NTSTATUS
RequestCopyFromBuffer(
    _In_  WDFREQUEST        Request,
    _In_  PVOID             SourceBuffer,
    _When_(NumBytesToCopyFrom == 0, __drv_reportError(NumBytesToCopyFrom cannot be zero))
    _In_  size_t            NumBytesToCopyFrom
    )
/*++

Routine Description:

    A helper function to copy specified bytes to the request's output memory

Arguments:

    Request - A handle to a framework request object.

    SourceBuffer - The buffer to copy data from.

    NumBytesToCopyFrom - The length, in bytes, of data to be copied.

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS                status;
    WDFMEMORY               memory;
    size_t                  outputBufferLength;

    status = WdfRequestRetrieveOutputMemory(Request, &memory);
    if( !NT_SUCCESS(status) ) {
        return status;
    }

    WdfMemoryGetBuffer(memory, &outputBufferLength);
    if (outputBufferLength < NumBytesToCopyFrom) {
        status = STATUS_INVALID_BUFFER_SIZE;
        return status;
    }

    status = WdfMemoryCopyFromBuffer(memory,
                                    0,
                                    SourceBuffer,
                                    NumBytesToCopyFrom);
    if( !NT_SUCCESS(status) ) {
        
        return status;
    }
    WdfRequestSetInformation(Request, NumBytesToCopyFrom);
    return status;
}

NTSTATUS
ReadReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request,
    _Always_(_Out_)
          BOOLEAN*          CompleteRequest
    )
/*++

Routine Description:

    Handles IOCTL_HID_READ_REPORT for the HID collection. Normally the request
    will be forwarded to a manual queue for further process. In that case, the
    caller should not try to complete the request at this time, as the request
    will later be retrieved back from the manually queue and completed there.
    However, if for some reason the forwarding fails, the caller still need
    to complete the request with proper error code immediately.

Arguments:

    QueueContext - The object context associated with the queue

    Request - Pointer to  Request Packet.

    CompleteRequest - A boolean output value, indicating whether the caller
            should complete the request or not

Return Value:

    NT status code.

--*/
{
    NTSTATUS                status;

    //
    // forward the request to manual queue
    //
    status = WdfRequestForwardToIoQueue(
                            Request,
                            QueueContext->DeviceContext->ManualQueue);
    if( !NT_SUCCESS(status) ) {    
        *CompleteRequest = TRUE;
    }
    else {
        *CompleteRequest = FALSE;
    }

    return status;
}

NTSTATUS
WriteReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    )
/*++

Routine Description:

    Handles IOCTL_HID_WRITE_REPORT all the collection.

Arguments:

    QueueContext - The object context associated with the queue

    Request - Pointer to  Request Packet.

Return Value:

    NT status code.

--*/

{
    NTSTATUS                status;
    HID_XFER_PACKET         packet;
    ULONG                   reportSize;
    PHIDMINI_OUTPUT_REPORT  outputReport;

    status = RequestGetHidXferPacket_ToWriteToDevice(
                            Request,
                            &packet);
    if( !NT_SUCCESS(status) ) {
        return status;
    }

    if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
        //
        // Return error for unknown collection
        //
        status = STATUS_INVALID_PARAMETER;
        return status;
    }

    //
    // before touching buffer make sure buffer is big enough.
    //
    reportSize = sizeof(HIDMINI_OUTPUT_REPORT);

    if (packet.reportBufferLen < reportSize) {
        status = STATUS_INVALID_BUFFER_SIZE;
        
        return status;
    }

    outputReport = (PHIDMINI_OUTPUT_REPORT)packet.reportBuffer;

    //
    // Store the device data in device extension.
    //
    QueueContext->DeviceContext->DeviceData = outputReport->Data;

    //
    // set status and information
    //
    WdfRequestSetInformation(Request, reportSize);
    return status;
}


HRESULT
GetFeature(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    )
/*++

Routine Description:

    Handles IOCTL_HID_GET_FEATURE for all the collection.

Arguments:

    QueueContext - The object context associated with the queue

    Request - Pointer to  Request Packet.

Return Value:

    NT status code.

--*/
{
    NTSTATUS                status;
    HID_XFER_PACKET         packet;
    ULONG                   reportSize;
    
    UNREFERENCED_PARAMETER(QueueContext);
    status = RequestGetHidXferPacket_ToReadFromDevice(
                            Request,
                            &packet);
    if( !NT_SUCCESS(status) ) {
        return status;
    }

    if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
        //
        // If collection ID is not for control collection then handle
        // this request just as you would for a regular collection.
        //
        status = STATUS_INVALID_PARAMETER;
        
        
        return status;
    }

    //
    // Since output buffer is for write only (no read allowed by UMDF in output
    // buffer), any read from output buffer would be reading garbage), so don't
    // let app embed custom control code in output buffer. The minidriver can
    // support multiple features using separate report ID instead of using
    // custom control code. Since this is targeted at report ID 1, we know it
    // is a request for getting attributes.
    //
    // While KMDF does not enforce the rule (disallow read from output buffer),
    // it is good practice to not do so.
    //

    reportSize = sizeof(features);
    if (packet.reportBufferLen < reportSize) {
        status = STATUS_INVALID_BUFFER_SIZE;
        
        
        return status;
    }

    //
    // Since this device has one report ID, hidclass would pass on the report
    // ID in the buffer (it wouldn't if report descriptor did not have any report
    // ID). However, since UMDF allows only writes to an output buffer, we can't
    // "read" the report ID from "output" buffer. There is no need to read the
    // report ID since we get it other way as shown above, however this is
    // something to keep in mind.
    //
    packet.reportBuffer[0] = features.reportId;
    packet.reportBuffer[1] = features.DIG_TouchScreenContactCountMaximum;
    
    //
    // Report how many bytes were copied
    //
    WdfRequestSetInformation(Request, reportSize);
    return status;
}

NTSTATUS
SetFeature(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    )
/*++

Routine Description:

    Handles IOCTL_HID_SET_FEATURE for all the collection.
    For control collection (custom defined collection) it handles
    the user-defined control codes for sideband communication

Arguments:

    QueueContext - The object context associated with the queue

    Request - Pointer to Request Packet.

Return Value:

    NT status code.

--*/
{
    NTSTATUS                status;
    HID_XFER_PACKET         packet;
    ULONG                   reportSize;
    PHIDMINI_CONTROL_INFO   controlInfo;
    PHID_DEVICE_ATTRIBUTES  hidAttributes = &QueueContext->DeviceContext->HidDeviceAttributes;

    status = RequestGetHidXferPacket_ToWriteToDevice(
                            Request,
                            &packet);
    if( !NT_SUCCESS(status) ) {
        return status;
    }

    if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
        //
        // If collection ID is not for control collection then handle
        // this request just as you would for a regular collection.
        //
        status = STATUS_INVALID_PARAMETER;
        
        
        return status;
    }

    //
    // before touching control code make sure buffer is big enough.
    //
    reportSize = sizeof(HIDMINI_CONTROL_INFO);

    if (packet.reportBufferLen < reportSize) {
        status = STATUS_INVALID_BUFFER_SIZE;
        
        
        return status;
    }

    controlInfo = (PHIDMINI_CONTROL_INFO)packet.reportBuffer;

    switch(controlInfo->ControlCode)
    {
    case HIDMINI_CONTROL_CODE_SET_ATTRIBUTES:
        //
        // Store the device attributes in device extension
        //
        hidAttributes->ProductID     = controlInfo->u.Attributes.ProductID;
        hidAttributes->VendorID      = controlInfo->u.Attributes.VendorID;
        hidAttributes->VersionNumber = controlInfo->u.Attributes.VersionNumber;

        //
        // set status and information
        //
        WdfRequestSetInformation(Request, reportSize);
        break;

    case HIDMINI_CONTROL_CODE_DUMMY1:
        status = STATUS_NOT_IMPLEMENTED;
        
        break;

    case HIDMINI_CONTROL_CODE_DUMMY2:
        status = STATUS_NOT_IMPLEMENTED;
        
        break;

    default:
        status = STATUS_NOT_IMPLEMENTED;
        break;
    }
    return status;
}

NTSTATUS
GetInputReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    )
/*++

Routine Description:

    Handles IOCTL_HID_GET_INPUT_REPORT for all the collection.

Arguments:

    QueueContext - The object context associated with the queue

    Request - Pointer to Request Packet.

Return Value:

    NT status code.

--*/
{
    NTSTATUS                status;
    HID_XFER_PACKET         packet;
    ULONG                   reportSize;
    PHIDMINI_INPUT_REPORT   reportBuffer;

    status = RequestGetHidXferPacket_ToReadFromDevice(
                            Request,
                            &packet);
    if( !NT_SUCCESS(status) ) {
        return status;
    }

    if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
        //
        // If collection ID is not for control collection then handle
        // this request just as you would for a regular collection.
        //
        status = STATUS_INVALID_PARAMETER;
        
        return status;
    }

    reportSize = sizeof(HIDMINI_INPUT_REPORT);
    if (packet.reportBufferLen < reportSize) {
        status = STATUS_INVALID_BUFFER_SIZE;
        
        return status;
    }

    reportBuffer = (PHIDMINI_INPUT_REPORT)(packet.reportBuffer);

    reportBuffer->ReportId = CONTROL_COLLECTION_REPORT_ID;
    reportBuffer->Data     = QueueContext->OutputReport;

    //
    // Report how many bytes were copied
    //
    WdfRequestSetInformation(Request, reportSize);
    return status;
}


NTSTATUS
SetOutputReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    )
/*++

Routine Description:

    Handles IOCTL_HID_SET_OUTPUT_REPORT for all the collection.

Arguments:

    QueueContext - The object context associated with the queue

    Request - Pointer to Request Packet.

Return Value:

    NT status code.

--*/
{
    NTSTATUS                status;
    HID_XFER_PACKET         packet;
    ULONG                   reportSize;
    PHIDMINI_OUTPUT_REPORT  reportBuffer;

    status = RequestGetHidXferPacket_ToWriteToDevice(
                            Request,
                            &packet);
    if( !NT_SUCCESS(status) ) {
        return status;
    }

    if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
        //
        // If collection ID is not for control collection then handle
        // this request just as you would for a regular collection.
        //
        status = STATUS_INVALID_PARAMETER;
        
        return status;
    }

    //
    // before touching buffer make sure buffer is big enough.
    //
    reportSize = sizeof(HIDMINI_OUTPUT_REPORT);

    if (packet.reportBufferLen < reportSize) {
        status = STATUS_INVALID_BUFFER_SIZE;
        return status;
    }

    reportBuffer = (PHIDMINI_OUTPUT_REPORT)packet.reportBuffer;

    QueueContext->OutputReport = reportBuffer->Data;

    //
    // Report how many bytes were copied
    //
    WdfRequestSetInformation(Request, reportSize);
    return status;
}


NTSTATUS
GetStringId(
    _In_  WDFREQUEST        Request,
    _Out_ ULONG            *StringId,
    _Out_ ULONG            *LanguageId
    )
/*++

Routine Description:

    Helper routine to decode IOCTL_HID_GET_INDEXED_STRING and IOCTL_HID_GET_STRING.

Arguments:

    Request - Pointer to Request Packet.

Return Value:

    NT status code.

--*/
{
    NTSTATUS                status;
    ULONG                   inputValue;

#ifdef _KERNEL_MODE

    WDF_REQUEST_PARAMETERS  requestParameters;

    //
    // IOCTL_HID_GET_STRING:                      // METHOD_NEITHER
    // IOCTL_HID_GET_INDEXED_STRING:              // METHOD_OUT_DIRECT
    //
    // The string id (or string index) is passed in Parameters.DeviceIoControl.
    // Type3InputBuffer. However, Parameters.DeviceIoControl.InputBufferLength
    // was not initialized by hidclass.sys, therefore trying to access the
    // buffer with WdfRequestRetrieveInputMemory will fail
    //
    // Another problem with IOCTL_HID_GET_INDEXED_STRING is that METHOD_OUT_DIRECT
    // expects the input buffer to be Irp->AssociatedIrp.SystemBuffer instead of
    // Type3InputBuffer. That will also fail WdfRequestRetrieveInputMemory.
    //
    // The solution to the above two problems is to get Type3InputBuffer directly
    //
    // Also note that instead of the buffer's content, it is the buffer address
    // that was used to store the string id (or index)
    //

    WDF_REQUEST_PARAMETERS_INIT(&requestParameters);
    WdfRequestGetParameters(Request, &requestParameters);

    inputValue = PtrToUlong(
        requestParameters.Parameters.DeviceIoControl.Type3InputBuffer);

    status = STATUS_SUCCESS;

#else

    WDFMEMORY               inputMemory;
    size_t                  inputBufferLength;
    PVOID                   inputBuffer;

    //
    // mshidumdf.sys updates the IRP and passes the string id (or index) through
    // the input buffer correctly based on the IOCTL buffer type
    //

    status = WdfRequestRetrieveInputMemory(Request, &inputMemory);
    if( !NT_SUCCESS(status) ) {
        return status;
    }
    inputBuffer = WdfMemoryGetBuffer(inputMemory, &inputBufferLength);

    //
    // make sure buffer is big enough.
    //
    if (inputBufferLength < sizeof(ULONG))
    {
        status = STATUS_INVALID_BUFFER_SIZE;
        return status;
    }

    inputValue = (*(PULONG)inputBuffer);

#endif

    //
    // The least significant two bytes of the INT value contain the string id.
    //
    *StringId = (inputValue & 0x0ffff);

    //
    // The most significant two bytes of the INT value contain the language
    // ID (for example, a value of 1033 indicates English).
    //
    *LanguageId = (inputValue >> 16);
    return status;
}


NTSTATUS
GetIndexedString(
    _In_  WDFREQUEST        Request
    )
/*++

Routine Description:

    Handles IOCTL_HID_GET_INDEXED_STRING

Arguments:

    Request - Pointer to Request Packet.

Return Value:

    NT status code.

--*/
{
    NTSTATUS                status;
    ULONG                   languageId, stringIndex;

    status = GetStringId(Request, &stringIndex, &languageId);

    // While we don't use the language id, some minidrivers might.
    //
    UNREFERENCED_PARAMETER(languageId);

    if (NT_SUCCESS(status)) {

        if (stringIndex != VHIDMINI_DEVICE_STRING_INDEX)
        {
            status = STATUS_INVALID_PARAMETER;
            
            return status;
        }

        status = RequestCopyFromBuffer(Request, VHIDMINI_DEVICE_STRING, sizeof(VHIDMINI_DEVICE_STRING));
    }
    return status;
}


NTSTATUS
GetString(
    _In_  WDFREQUEST        Request
    )
/*++

Routine Description:

    Handles IOCTL_HID_GET_STRING.

Arguments:

    Request - Pointer to Request Packet.

Return Value:

    NT status code.

--*/
{
    NTSTATUS                status;
    ULONG                   languageId, stringId;
    size_t                  stringSizeCb;
    PWSTR                   string;

    status = GetStringId(Request, &stringId, &languageId);

    // While we don't use the language id, some minidrivers might.
    //
    UNREFERENCED_PARAMETER(languageId);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    switch (stringId){
    case HID_STRING_ID_IMANUFACTURER:
        stringSizeCb = sizeof(VHIDMINI_MANUFACTURER_STRING);
        string = VHIDMINI_MANUFACTURER_STRING;
        break;
    case HID_STRING_ID_IPRODUCT:
        stringSizeCb = sizeof(VHIDMINI_PRODUCT_STRING);
        string = VHIDMINI_PRODUCT_STRING;
        break;
    case HID_STRING_ID_ISERIALNUMBER:
        return STATUS_NOT_SUPPORTED;
    default:
        status = STATUS_INVALID_PARAMETER;
        
        return status;
    }

    status = RequestCopyFromBuffer(Request, string, stringSizeCb);
    return status;
}


NTSTATUS
ManualQueueCreate(
    _In_  WDFDEVICE         Device,
    _Out_ WDFQUEUE          *Queue
    )
/*++
Routine Description:

    This function creates a manual I/O queue to receive IOCTL_HID_READ_REPORT
    forwarded from the device's default queue handler.

    It also creates a periodic timer to check the queue and complete any pending
    request with data from the device. Here timer expiring is used to simulate
    a hardware event that new data is ready.

    The workflow is like this:

    - Hidclass.sys sends an ioctl to the miniport to read input report.

    - The request reaches the driver's default queue. As data may not be avaiable
      yet, the request is forwarded to a second manual queue temporarily.

    - Later when data is ready (as simulated by timer expiring), the driver
      checks for any pending request in the manual queue, and then completes it.

    - Hidclass gets notified for the read request completion and return data to
      the caller.

    On the other hand, for IOCTL_HID_WRITE_REPORT request, the driver simply
    sends the request to the hardware (as simulated by storing the data at
    DeviceContext->DeviceData) and completes the request immediately. There is
    no need to use another queue for write operation.

Arguments:

    Device - Handle to a framework device object.

    Queue - Output pointer to a framework I/O queue handle, on success.

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS                status;
    WDF_IO_QUEUE_CONFIG     queueConfig;
    WDF_OBJECT_ATTRIBUTES   queueAttributes;
    WDFQUEUE                queue;
    PMANUAL_QUEUE_CONTEXT   queueContext;
    
    WDF_IO_QUEUE_CONFIG_INIT(
                            &queueConfig,
                            WdfIoQueueDispatchManual);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
                            &queueAttributes,
                            MANUAL_QUEUE_CONTEXT);

    status = WdfIoQueueCreate(
                            Device,
                            &queueConfig,
                            &queueAttributes,
                            &queue);

    if( !NT_SUCCESS(status) ) {
        
        
        return status;
    }

    queueContext = GetManualQueueContext(queue);
    queueContext->Queue         = queue;
    queueContext->DeviceContext = GetDeviceContext(Device);

    *Queue = queue;
    return status;
}


static
VOID
S3910InitializeTimer(
    _In_ WDFTIMER Timer
)
{
    WDFDEVICE device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PDEVICE_CONTEXT pDevice = GetDeviceContext(device);

    if (InterlockedCompareExchange(&pDevice->Stopping, 0, 0) == 0) {
        (void)S3910BringUp(pDevice);
    }
}

static
void
S3910PollTimer(
    _In_ WDFTIMER Timer
)
{
    WDFDEVICE device;
    PDEVICE_CONTEXT pDevice;

    device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    pDevice = GetDeviceContext(device);

    if (pDevice->SpiReady && InterlockedCompareExchange(&pDevice->Stopping, 0, 0) == 0) {
        S3910PollTouch(device);
        if (pDevice->SpiReady && InterlockedCompareExchange(&pDevice->Stopping, 0, 0) == 0) {
            WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(8));
        }
    }
}

static
VOID
S3910PollTouch(
    _In_ WDFDEVICE Device
)
/*++

  Routine Description:

    This routine responds to interrupts generated by the H/W.
    It then waits indefinitely for the user to signal that
    the interrupt has been acknowledged, allowing the ISR to
    return. This ISR is called at PASSIVE_LEVEL.

  Arguments:

    Interrupt - a handle to a framework interrupt object
    MessageID - message number identifying the device's
        hardware interrupt message (if using MSI)

  Return Value:

    TRUE if interrupt recognized.

--*/
{
    UINT8 touchCount;
    WDFDEVICE device;
    PDEVICE_CONTEXT pDevice;
    NTSTATUS status;
    WDFREQUEST request;
    inputReport54_t    readReport;
    device = Device;
    pDevice = GetDeviceContext(device);

    {
        RtlZeroMemory(&readReport, sizeof(readReport));
        status = HummerSpiTransfer(pDevice, NULL, &eventbuf[0], SYNAPTICS_S3910_TOUCH_BUFFER_SIZE);
        if (!NT_SUCCESS(status)) {
            return;
        }

        if (eventbuf[1] == SYNAPTICS_S3910_TOUCH_EVENT) {
            if (eventbuf[2] < 0x1F || (eventbuf[2] - 0x1F) % 12 != 0) {
                return;
            }
            touchCount = SYNAPTICS_S3910_GET_TOUCH_COUNT(eventbuf[2]);
            if (touchCount > sizeof(readReport.points) / 6 ||
                (touchCount != 0 && 35 + 12 * (touchCount - 1) + 4 >= SYNAPTICS_S3910_TOUCH_BUFFER_SIZE)) {
                return;
            }

            if (touchCount == 0) {
                readReport.points[0] = 0x06;
                readReport.points[1] = 0x0;
                readReport.DIG_TouchScreenContactCount = 1;
            }
            else {
                for (int i = 0; i < touchCount; i++) {
                    int offset = 35 + 12 * i;

                    USHORT rawX = (eventbuf[offset + 2] << 8) | eventbuf[offset + 1];
                    USHORT rawY = (eventbuf[offset + 4] << 8) | eventbuf[offset + 3];
                    USHORT xCoord;
                    USHORT yCoord;

                    if (pDevice->PanelMaxX == 0 || pDevice->PanelMaxY == 0) {
                        return;
                    }
                    if (rawX > pDevice->PanelMaxX) rawX = (USHORT)pDevice->PanelMaxX;
                    if (rawY > pDevice->PanelMaxY) rawY = (USHORT)pDevice->PanelMaxY;
                    xCoord = (USHORT)(((ULONG)rawX * 32767u) / pDevice->PanelMaxX);
                    yCoord = (USHORT)(((ULONG)rawY * 32767u) / pDevice->PanelMaxY);
                    readReport.points[i * 6 + 0] = 0x07;
                    readReport.points[i * 6 + 1] = eventbuf[offset] & 0x0F;
                    readReport.points[i * 6 + 2] = xCoord & 0xFF;
                    readReport.points[i * 6 + 3] = (xCoord >> 8) & 0xFF;
                    readReport.points[i * 6 + 4] = yCoord & 0xFF;
                    readReport.points[i * 6 + 5] = (yCoord >> 8) & 0xFF;

                    readReport.DIG_TouchScreenContactCount = touchCount;
                }
            }

            status = WdfIoQueueRetrieveNextRequest(
                pDevice->ManualQueue,
                &request);

            if (NT_SUCCESS(status)) {

                readReport.reportId = CONTROL_FEATURE_REPORT_ID;

                status = RequestCopyFromBuffer(request,
                    &readReport,
                    sizeof(readReport));

                WdfRequestComplete(request, status);
            }
        }
    }
}

NTSTATUS
S3910BringUp(
    _In_ PDEVICE_CONTEXT pDevice
)
{
    PHSP_CTX spi;
    ULONG attempt, length, xmax, ymax;
    NTSTATUS status;
    WDF_TIMER_CONFIG timerConfig;
    WDF_OBJECT_ATTRIBUTES timerAttributes;
    LARGE_INTEGER delay;

    spi = (PHSP_CTX)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*spi), 'PSh9');
    if (spi == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    status = HspInit(spi, pDevice->Device, (PUCHAR)pDevice->MappedBase[0], (PUCHAR)pDevice->MappedBase[1],
        (PUCHAR)pDevice->MappedBase[2], (PUCHAR)pDevice->MappedBase[3], (PUCHAR)pDevice->MappedBase[6]);
    if (!NT_SUCCESS(status)) goto fail;
    pDevice->SpiContext = spi;

    status = HrpmhInit(&pDevice->Rpmh, (PUCHAR)pDevice->MappedBase[4], (PUCHAR)pDevice->MappedBase[5]);
    if (!NT_SUCCESS(status)) goto fail;
    status = HrpmhVoteLdo(&pDevice->Rpmh, 4, 1800);
    if (!NT_SUCCESS(status)) goto fail;
    status = HrpmhVoteLdo(&pDevice->Rpmh, 16, 3104);
    if (!NT_SUCCESS(status)) goto fail;

    status = HspClocksOn(spi);
    if (!NT_SUCCESS(status)) goto fail;
    status = HspResetAndConfigurePins((PUCHAR)pDevice->MappedBase[2]);
    if (!NT_SUCCESS(status)) goto fail;
    status = HspConfigureSmmu(spi);
    if (!NT_SUCCESS(status)) goto fail;
    status = HspStartDma(spi);
    if (!NT_SUCCESS(status)) goto fail;
    pDevice->SpiReady = TRUE;

    status = HummerSpiTransfer(pDevice, cmd_reset, NULL, sizeof(cmd_reset));
    if (!NT_SUCCESS(status)) goto fail;
    delay.QuadPart = -200000;
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
    status = HummerSpiTransfer(pDevice, cmd_tcm2_ack, NULL, sizeof(cmd_tcm2_ack));
    if (!NT_SUCCESS(status)) goto fail;
    delay.QuadPart = -100000;
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
    status = STATUS_IO_TIMEOUT;
    for (attempt = 0; attempt < 30; attempt++) {
        status = HummerSpiTransfer(pDevice, NULL, eventbuf, 50);
        if (!NT_SUCCESS(status)) goto fail;
        if (eventbuf[0] == 0xA5 && eventbuf[1] == 0x10) break;
        delay.QuadPart = -100000;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    if (attempt == 30) { status = STATUS_IO_TIMEOUT; goto fail; }

    status = HummerSpiTransfer(pDevice, cmd_get_application_info, NULL, sizeof(cmd_get_application_info));
    if (!NT_SUCCESS(status)) goto fail;
    delay.QuadPart = -100000;
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
    status = STATUS_IO_TIMEOUT;
    for (attempt = 0; attempt < 30; attempt++) {
        status = HummerSpiTransfer(pDevice, NULL, eventbuf, 60);
        if (!NT_SUCCESS(status)) goto fail;
        if (eventbuf[0] == 0xA5 && eventbuf[1] == 0x01) break;
        delay.QuadPart = -100000;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    if (attempt == 30) { status = STATUS_IO_TIMEOUT; goto fail; }
    length = eventbuf[2] | ((ULONG)eventbuf[3] << 8);
    if (length < sizeof(tcm_application_info_t) || length + 3 > 60) {
        status = STATUS_DEVICE_PROTOCOL_ERROR;
        goto fail;
    }
    {
        tcm_application_info_t *app = (tcm_application_info_t *)(eventbuf + 4);
        xmax = app->max_x[0] | ((ULONG)app->max_x[1] << 8);
        ymax = app->max_y[0] | ((ULONG)app->max_y[1] << 8);
    }
    if (xmax == 0 || ymax == 0 || xmax > 32767 || ymax > 32767) {
        status = STATUS_DEVICE_PROTOCOL_ERROR;
        goto fail;
    }
    pDevice->PanelMaxX = xmax;
    pDevice->PanelMaxY = ymax;
    pDevice->ReportDescriptor = G_DefaultReportDescriptor;
    if (pDevice->PollTimer == NULL) {
        WDF_TIMER_CONFIG_INIT(&timerConfig, S3910PollTimer);
        timerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
        timerAttributes.ParentObject = pDevice->Device;
        timerAttributes.ExecutionLevel = WdfExecutionLevelPassive;
        status = WdfTimerCreate(&timerConfig, &timerAttributes, &pDevice->PollTimer);
        if (!NT_SUCCESS(status)) goto fail;
    }
    WdfTimerStart(pDevice->PollTimer, WDF_REL_TIMEOUT_IN_MS(100));
    return STATUS_SUCCESS;

fail:
    pDevice->SpiReady = FALSE;
    if (pDevice->SpiContext != NULL) {
        if (NT_SUCCESS(HspDeinit((PHSP_CTX)pDevice->SpiContext))) {
            ExFreePoolWithTag(pDevice->SpiContext, 'PSh9');
            pDevice->SpiContext = NULL;
        }
    } else {
        ExFreePoolWithTag(spi, 'PSh9');
    }
    return status;
}
static
NTSTATUS
HummerSpiTransfer(
    _In_ PDEVICE_CONTEXT pDevice,
    _In_opt_ PVOID pInputBuffer,
    _Out_opt_ PVOID pOutputBuffer,
    _In_ size_t length
)
{
    struct hsp_cfg cfg;
    PHSP_CTX spi = (PHSP_CTX)pDevice->SpiContext;
    UCHAR tx[HSP_DATA_BYTES];
    UCHAR rx[HSP_DATA_BYTES];
    NTSTATUS status;

    if (spi == NULL || !pDevice->SpiReady) return STATUS_DEVICE_NOT_READY;
    if (length == 0 || length > HSP_DATA_BYTES) return STATUS_INVALID_PARAMETER;

    if (pInputBuffer != NULL) {
        RtlCopyMemory(tx, pInputBuffer, length);
    } else {
        RtlFillMemory(tx, length, 0xFF);
    }
    RtlZeroMemory(rx, length);

    //
    // mainline spi_geni_prepare_message(): word_len is bits_per_word - 4 and
    // clk_div is DIV_ROUND_UP(sclk, oversampling * speed). This engine's SE4
    // runs off DFS level 0 (TCXO, 19.2 MHz) and the panel asks for 19 MHz, so
    // the divider is 2 for oversampling 1 and the line ends up at 9.6 MHz,
    // which the controller and the S3910 both accept.
    //
    cfg.word_len = 8 - 4;
    cfg.cpol = 0;
    cfg.cpha = 0;
    cfg.clk_div = 2;
    cfg.clk_src = 0;
    cfg.cs = 0;

    status = HspXfer(spi, &cfg, tx, rx, (ULONG)length);

    if (NT_SUCCESS(status) && pOutputBuffer != NULL) {
        RtlCopyMemory(pOutputBuffer, rx, length);
    }

    return status;
}
