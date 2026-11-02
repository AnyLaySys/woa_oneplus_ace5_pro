/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Extracted from local ath12k/{hal.h,dp_htt.h,wifi7/hal_desc.h}.
 * FW completions use HTT status, TQM completions use TQM release reason. */
#pragma once
#define S7_TX_SOURCE_MASK 0x7u
#define S7_TX_SOURCE_SHIFT 0x0u
#define S7_TX_SOURCE_TQM 0x0u
#define S7_TX_SOURCE_FW 0x3u
#define S7_HTT_TX_STATUS_MASK 0x1e000u
#define S7_HTT_TX_STATUS_SHIFT 0xdu
#define S7_HTT_TX_STATUS_OK 0x0u
#define S7_TQM_TX_REASON_MASK 0x1e000u
#define S7_TQM_TX_REASON_SHIFT 0xdu
#define S7_TQM_TX_FRAME_ACKED 0x0u
