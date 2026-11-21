/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Extracted from local ath12k/hal.h and wifi7/hal_desc.h.
 * REO reason 0 means ERROR; normal routing is 1 (not zero). */
#pragma once
#define S7_REO_PUSH_REASON_MASK 0x6u
#define S7_REO_PUSH_REASON_SHIFT 1u
#define S7_REO_PUSH_ROUTING 1u

/* Base128 plus three ext128 descriptors covers a 256-frame RX BA window. */
#define S7_RX_BA_MAX 256u
#define S7_RX_TID_COUNT 17u
#define S7_RX_QUEUE_STRIDE 512u
