/* SPDX-License-Identifier: GPL-2.0-only
 * Iris VPU 3.5 decoder buffer sizing, adapted from the Linux Iris driver
 * and Qualcomm iris35 hfi_buffer_iris35.h (OnePlus sm8750, d50b305f7da9).
 * Copyright (c) 2020-2022 The Linux Foundation.
 * Copyright (c) 2022-2025 Qualcomm Innovation Center, Inc.
 */
#pragma once
#include "uapi.h"
static inline GPU_U32 IrisDecodeMax(GPU_U32 a,GPU_U32 b){return a>b?a:b;}
static inline GPU_U32 IrisDecodeMin(GPU_U32 a,GPU_U32 b){return a<b?a:b;}
#define IrisDecode_AV1D_SIZE_BSE_COL_MV_64x64 512u
#define IrisDecode_AV1D_SIZE_BSE_COL_MV_128x128 2816u
static inline GPU_U32 IrisDecodeAlign(GPU_U32 n,GPU_U32 a){
return (n+a-1)/a*a;}
static inline GPU_U32 IrisDecodeCeil(GPU_U32 n,GPU_U32 a){
return (n+a-1)/a;}
#define IrisDecode_BIN_BUFFER_THRESHOLD        (1280 * 736)
#define IrisDecode_H264_CABAC_HDR_RATIO_HD_TOT    1
#define IrisDecode_H264_CABAC_RES_RATIO_HD_TOT    3
#define IrisDecode_DMA_ALIGNMENT            256
#define IrisDecode_H265_CABAC_HDR_RATIO_HD_TOT 2
#define IrisDecode_H265_CABAC_RES_RATIO_HD_TOT 2
#define IrisDecode_VPX_DECODER_FRAME_BIN_HDR_BUDGET 1
#define IrisDecode_VPX_DECODER_FRAME_BIN_DENOMINATOR 2
#define IrisDecode_VPX_DECODER_FRAME_CONCURENCY_LVL (2)
#define IrisDecode_VPX_DECODER_FRAME_BIN_RES_BUDGET 3
#define IrisDecode_AV1_CABAC_HDR_RATIO_HD_TOT 2
#define IrisDecode_AV1_CABAC_RES_RATIO_HD_TOT 2
#define IrisDecode_SIZE_H264D_BUFTAB_T        256
#define IrisDecode_H264D_MAX_SLICE            1800
#define IrisDecode_SIZE_H264D_BSE_CMD_PER_BUF    (32 * 4)
#define IrisDecode_SIZE_H264D_VPP_CMD_PER_BUF    512
#define IrisDecode_VPP_CMD_MAX_SIZE        (1048576u)
#define IrisDecode_NUM_HW_PIC_BUF            32
#define IrisDecode_SIZE_HW_PIC(size_per_buf)    (IrisDecode_NUM_HW_PIC_BUF * (size_per_buf))
#define IrisDecode_SIZE_H264D_HW_PIC_T        (2048u)
#define IrisDecode_LCU_MAX_SIZE_PELS 64
#define IrisDecode_LCU_MIN_SIZE_PELS 16
#define IrisDecode_H265D_MAX_SLICE    1200
#define IrisDecode_SIZE_H265D_BSE_CMD_PER_BUF (16 * sizeof(GPU_U32))
#define IrisDecode_SIZE_H265D_VPP_CMD_PER_BUF (256)
#define IrisDecode_SIZE_H265D_HW_PIC_T IrisDecode_SIZE_H264D_HW_PIC_T
#define IrisDecode_HDR10_HIST_EXTRADATA_SIZE (4 * 1024)
#define IrisDecode_MAX_FE_NBR_DATA_LUMA_LINE_BUFFER_SIZE    640
#define IrisDecode_MAX_FE_NBR_CTRL_LCU64_LINE_BUFFER_SIZE 64
#define IrisDecode_MAX_SE_NBR_CTRL_LCU64_LINE_BUFFER_SIZE    16
#define IrisDecode_MAX_PE_NBR_DATA_LCU64_LINE_BUFFER_SIZE    384
#define IrisDecode_MAX_TILE_COLUMNS        32
#define IrisDecode_MAX_SE_NBR_CTRL_LCU16_LINE_BUFFER_SIZE (128 / 8)
#define IrisDecode_MAX_SE_NBR_CTRL_LCU32_LINE_BUFFER_SIZE (128 / 8)
#define IrisDecode_MAX_FE_NBR_CTRL_LCU16_LINE_BUFFER_SIZE 64
#define IrisDecode_MAX_FE_NBR_CTRL_LCU32_LINE_BUFFER_SIZE 64
#define IrisDecode_BUFFER_ALIGNMENT_32_BYTES 32
#define IrisDecode_AV1D_LCU_MAX_SIZE_PELS 128
#define IrisDecode_BITS_PER_PIX                   16
#define IrisDecode_NUM_LINES_LUMA                 10
#define IrisDecode_NUM_LINES_CHROMA               6
#define IrisDecode_NUM_CTRL_PACK_LCU              10
#define IrisDecode_AV1D_LCU_MIN_SIZE_PELS 64
#define IrisDecode_BITS_PER_CTRL_PACK             128
#define IrisDecode_MAX_PE_NBR_DATA_LCU16_LINE_BUFFER_SIZE 96
#define IrisDecode_MAX_PE_NBR_DATA_LCU32_LINE_BUFFER_SIZE 192
#define IrisDecode_MAX_HEIGHT 2304
#define IrisDecode_HFI_COLOR_FORMAT_YUV420_NV12_UBWC_Y_TILE_WIDTH (32)
#define IrisDecode_AV1D_MAX_TILE_COLS     64
#define IrisDecode_UBWC_TILE_SIZE 256
#define IrisDecode_HFI_COLOR_FORMAT_YUV420_TP10_UBWC_Y_TILE_WIDTH (48)
#define IrisDecode_SIZE_SLIST_BUF_H264        512
#define IrisDecode_NUM_SLIST_BUF_H264        (256 + 32)
#define IrisDecode_H264_DISPLAY_BUF_SIZE        3328
#define IrisDecode_H264_NUM_FRM_INFO        66
#define IrisDecode_SIZE_SEI_USERDATA            4096
#define IrisDecode_SIZE_ARP 9728u
#define IrisDecode_SIZE_SLIST_BUF_H265 (1024u)
#define IrisDecode_NUM_SLIST_BUF_H265 (80 + 20)
#define IrisDecode_H265_NUM_FRM_INFO (48)
#define IrisDecode_H265_DISPLAY_BUF_SIZE (3072)
#define IrisDecode_H265_NUM_TILE_ROW 128
#define IrisDecode_H265_NUM_TILE_COL 32
#define IrisDecode_H265_NUM_TILE (IrisDecode_H265_NUM_TILE_ROW * IrisDecode_H265_NUM_TILE_COL + 1)
#define IrisDecode_SIZE_DOLBY_RPU_METADATA (41 * 1024)
#define IrisDecode_VP9_NUM_FRAME_INFO_BUF 32
#define IrisDecode_VP9_NUM_PROBABILITY_TABLE_BUF (IrisDecode_VP9_NUM_FRAME_INFO_BUF + 4)
#define IrisDecode_VP9_PROB_TABLE_SIZE (3840)
#define IrisDecode_MAX_SUPERFRAME_HEADER_LEN (34)
#define IrisDecode_VP9_UDC_HEADER_BUF_SIZE    (3 * 128)
#define IrisDecode_CCE_TILE_OFFSET_SIZE IrisDecodeAlign(32 * 4 * 4, IrisDecode_BUFFER_ALIGNMENT_32_BYTES)
#define IrisDecode_VP9_FRAME_INFO_BUF_SIZE (6144)
#define IrisDecode_SIZE_AV1D_SEQUENCE_HEADER 768
#define IrisDecode_SIZE_AV1D_METADATA        512
#define IrisDecode_AV1D_NUM_HW_PIC_BUF    16
#define IrisDecode_SIZE_AV1D_TILE_OFFSET     65536
#define IrisDecode_SIZE_AV1D_QM              3328
#define IrisDecode_AV1D_NUM_FRAME_HEADERS 16
#define IrisDecode_SIZE_AV1D_FRAME_HEADER    1280
#define IrisDecode_SIZE_AV1D_PROB_TABLE      22784
#define IrisDecode_HFI_COLOR_FORMAT_YUV420_NV12_UBWC_Y_TILE_HEIGHT (8)
#define IrisDecode_HFI_COLOR_FORMAT_YUV420_NV12_UBWC_UV_TILE_WIDTH (16)
#define IrisDecode_HFI_COLOR_FORMAT_YUV420_NV12_UBWC_UV_TILE_HEIGHT (8)
#define IrisDecode_HFI_ALIGNMENT_4096      4096
#define IrisDecode_HFI_COLOR_FORMAT_YUV420_TP10_UBWC_Y_TILE_HEIGHT (4)
#define IrisDecode_HFI_COLOR_FORMAT_YUV420_TP10_UBWC_UV_TILE_WIDTH (24)
#define IrisDecode_HFI_COLOR_FORMAT_YUV420_TP10_UBWC_UV_TILE_HEIGHT (4)

static inline GPU_U32 IrisDecode_size_h264d_hw_bin_buffer(GPU_U32 frame_width, GPU_U32 frame_height, GPU_U32 num_vpp_pipes)
{
    GPU_U32 size_yuv, size_bin_hdr, size_bin_res;

    size_yuv = ((frame_width * frame_height) <= IrisDecode_BIN_BUFFER_THRESHOLD) ?
            ((IrisDecode_BIN_BUFFER_THRESHOLD * 3) >> 1) :
            ((frame_width * frame_height * 3) >> 1);
    size_bin_hdr = size_yuv * IrisDecode_H264_CABAC_HDR_RATIO_HD_TOT;
    size_bin_res = size_yuv * IrisDecode_H264_CABAC_RES_RATIO_HD_TOT;
    size_bin_hdr = IrisDecodeAlign(size_bin_hdr / num_vpp_pipes,
                 IrisDecode_DMA_ALIGNMENT) * num_vpp_pipes;
    size_bin_res = IrisDecodeAlign(size_bin_res / num_vpp_pipes,
                 IrisDecode_DMA_ALIGNMENT) * num_vpp_pipes;

    return size_bin_hdr + size_bin_res;
}

static inline GPU_U32 IrisDecode_hfi_buffer_bin_h264d(GPU_U32 frame_width, GPU_U32 frame_height, GPU_U32 num_vpp_pipes)
{
    GPU_U32 n_aligned_h = IrisDecodeAlign(frame_height, 16);
    GPU_U32 n_aligned_w = IrisDecodeAlign(frame_width, 16);

    return IrisDecode_size_h264d_hw_bin_buffer(n_aligned_w, n_aligned_h, num_vpp_pipes);
}

static inline GPU_U32 IrisDecode_size_h265d_hw_bin_buffer(GPU_U32 frame_width, GPU_U32 frame_height, GPU_U32 num_vpp_pipes)
{
    GPU_U32 product = frame_width * frame_height;
    GPU_U32 size_yuv, size_bin_hdr, size_bin_res;

    size_yuv = (product <= IrisDecode_BIN_BUFFER_THRESHOLD) ?
        ((IrisDecode_BIN_BUFFER_THRESHOLD * 3) >> 1) : ((product * 3) >> 1);
    size_bin_hdr = size_yuv * IrisDecode_H265_CABAC_HDR_RATIO_HD_TOT;
    size_bin_res = size_yuv * IrisDecode_H265_CABAC_RES_RATIO_HD_TOT;
    size_bin_hdr = IrisDecodeAlign(size_bin_hdr / num_vpp_pipes, IrisDecode_DMA_ALIGNMENT) * num_vpp_pipes;
    size_bin_res = IrisDecodeAlign(size_bin_res / num_vpp_pipes, IrisDecode_DMA_ALIGNMENT) * num_vpp_pipes;

    return size_bin_hdr + size_bin_res;
}

static inline GPU_U32 IrisDecode_hfi_buffer_bin_h265d(GPU_U32 frame_width, GPU_U32 frame_height, GPU_U32 num_vpp_pipes)
{
    GPU_U32 n_aligned_w = IrisDecodeAlign(frame_width, 16);
    GPU_U32 n_aligned_h = IrisDecodeAlign(frame_height, 16);

    return IrisDecode_size_h265d_hw_bin_buffer(n_aligned_w, n_aligned_h, num_vpp_pipes);
}

static inline GPU_U32 IrisDecode_hfi_buffer_bin_vp9d(GPU_U32 frame_width, GPU_U32 frame_height, GPU_U32 num_vpp_pipes)
{
    GPU_U32 _size_yuv = IrisDecodeAlign(frame_width, 16) * IrisDecodeAlign(frame_height, 16) * 3 / 2;
    GPU_U32 _size = IrisDecodeAlign(((IrisDecodeMax(_size_yuv, ((IrisDecode_BIN_BUFFER_THRESHOLD * 3) >> 1)) *
            IrisDecode_VPX_DECODER_FRAME_BIN_HDR_BUDGET / IrisDecode_VPX_DECODER_FRAME_BIN_DENOMINATOR *
            IrisDecode_VPX_DECODER_FRAME_CONCURENCY_LVL) / num_vpp_pipes), IrisDecode_DMA_ALIGNMENT) +
            IrisDecodeAlign(((IrisDecodeMax(_size_yuv, ((IrisDecode_BIN_BUFFER_THRESHOLD * 3) >> 1)) *
            IrisDecode_VPX_DECODER_FRAME_BIN_RES_BUDGET / IrisDecode_VPX_DECODER_FRAME_BIN_DENOMINATOR *
            IrisDecode_VPX_DECODER_FRAME_CONCURENCY_LVL) / num_vpp_pipes), IrisDecode_DMA_ALIGNMENT);

    return _size * num_vpp_pipes;
}

static inline GPU_U32 IrisDecode_size_av1d_hw_bin_buffer(GPU_U32 frame_width, GPU_U32 frame_height, GPU_U32 num_vpp_pipes)
{
    GPU_U32 size_yuv, size_bin_hdr, size_bin_res;

    size_yuv = ((frame_width * frame_height) <= IrisDecode_BIN_BUFFER_THRESHOLD) ?
        ((IrisDecode_BIN_BUFFER_THRESHOLD * 3) >> 1) :
        ((frame_width * frame_height * 3) >> 1);
    size_bin_hdr = size_yuv * IrisDecode_AV1_CABAC_HDR_RATIO_HD_TOT;
    size_bin_res = size_yuv * IrisDecode_AV1_CABAC_RES_RATIO_HD_TOT;
    size_bin_hdr = IrisDecodeAlign(size_bin_hdr / num_vpp_pipes,
                 IrisDecode_DMA_ALIGNMENT) * num_vpp_pipes;
    size_bin_res = IrisDecodeAlign(size_bin_res / num_vpp_pipes,
                 IrisDecode_DMA_ALIGNMENT) * num_vpp_pipes;

    return size_bin_hdr + size_bin_res;
}

static inline GPU_U32 IrisDecode_hfi_buffer_bin_av1d(GPU_U32 frame_width, GPU_U32 frame_height, GPU_U32 num_vpp_pipes)
{
    GPU_U32 n_aligned_h = IrisDecodeAlign(frame_height, 16);
    GPU_U32 n_aligned_w = IrisDecodeAlign(frame_width, 16);

    return IrisDecode_size_av1d_hw_bin_buffer(n_aligned_w, n_aligned_h, num_vpp_pipes);
}

static inline GPU_U32 IrisDecode_hfi_buffer_comv_h264d(GPU_U32 frame_width, GPU_U32 frame_height, GPU_U32 _comv_bufcount)
{
    GPU_U32 frame_height_in_mbs = IrisDecodeCeil(frame_height, 16);
    GPU_U32 frame_width_in_mbs = IrisDecodeCeil(frame_width, 16);
    GPU_U32 col_zero_aligned_width = (frame_width_in_mbs << 2);
    GPU_U32 col_mv_aligned_width = (frame_width_in_mbs << 7);
    GPU_U32 col_zero_size, size_colloc;

    col_mv_aligned_width = IrisDecodeAlign(col_mv_aligned_width, 16);
    col_zero_aligned_width = IrisDecodeAlign(col_zero_aligned_width, 16);
    col_zero_size = col_zero_aligned_width *
            ((frame_height_in_mbs + 1) >> 1);
    col_zero_size = IrisDecodeAlign(col_zero_size, 64);
    col_zero_size <<= 1;
    col_zero_size = IrisDecodeAlign(col_zero_size, 512);
    size_colloc = col_mv_aligned_width * ((frame_height_in_mbs + 1) >> 1);
    size_colloc = IrisDecodeAlign(size_colloc, 64);
    size_colloc <<= 1;
    size_colloc = IrisDecodeAlign(size_colloc, 512);
    size_colloc += (col_zero_size + IrisDecode_SIZE_H264D_BUFTAB_T * 2);

    return (size_colloc * (_comv_bufcount)) + 512;
}

static inline GPU_U32 IrisDecode_hfi_buffer_comv_h265d(GPU_U32 frame_width, GPU_U32 frame_height, GPU_U32 _comv_bufcount)
{
    GPU_U32 frame_height_in_mbs = (frame_height + 15) >> 4;
    GPU_U32 frame_width_in_mbs = (frame_width + 15) >> 4;
    GPU_U32 _size;

    _size = IrisDecodeAlign(((frame_width_in_mbs * frame_height_in_mbs) << 8), 512);

    return (_size * (_comv_bufcount)) + 512;
}

static inline GPU_U32 IrisDecode_num_lcu(GPU_U32 frame_width, GPU_U32 frame_height, GPU_U32 lcu_size)
{
    return ((frame_width + lcu_size - 1) / lcu_size) *
        ((frame_height + lcu_size - 1) / lcu_size);
}

static inline GPU_U32 IrisDecode_hfi_buffer_comv_av1d(GPU_U32 frame_width, GPU_U32 frame_height, GPU_U32 comv_bufcount)
{
    GPU_U32 size;

    size =  2 * IrisDecodeAlign(IrisDecodeMax(IrisDecode_num_lcu(frame_width, frame_height, 64) *
              IrisDecode_AV1D_SIZE_BSE_COL_MV_64x64,
              IrisDecode_num_lcu(frame_width, frame_height, 128) *
              IrisDecode_AV1D_SIZE_BSE_COL_MV_128x128),
              IrisDecode_DMA_ALIGNMENT);
    size *= comv_bufcount;

    return size;
}

static inline GPU_U32 IrisDecode_size_h264d_bse_cmd_buf(GPU_U32 frame_height)
{
    GPU_U32 height = IrisDecodeAlign(frame_height, 32);

    return IrisDecodeMin((IrisDecodeCeil(height, 16) * 48), IrisDecode_H264D_MAX_SLICE) *
        IrisDecode_SIZE_H264D_BSE_CMD_PER_BUF;
}

static inline GPU_U32 IrisDecode_size_h264d_vpp_cmd_buf(GPU_U32 frame_height)
{
    GPU_U32 size, height = IrisDecodeAlign(frame_height, 32);

    size = IrisDecodeMin((IrisDecodeCeil(height, 16) * 48), IrisDecode_H264D_MAX_SLICE) *
            IrisDecode_SIZE_H264D_VPP_CMD_PER_BUF;

    return size > IrisDecode_VPP_CMD_MAX_SIZE ? IrisDecode_VPP_CMD_MAX_SIZE : size;
}

static inline GPU_U32 IrisDecode_hfi_buffer_non_comv_h264d(GPU_U32 frame_width, GPU_U32 frame_height, GPU_U32 num_vpp_pipes)
{
    (void)frame_width;
    (void)num_vpp_pipes;
    GPU_U32 size_bse = IrisDecode_size_h264d_bse_cmd_buf(frame_height);
    GPU_U32 size_vpp = IrisDecode_size_h264d_vpp_cmd_buf(frame_height);
    GPU_U32 size = IrisDecodeAlign(size_bse, IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(size_vpp, IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_SIZE_HW_PIC(IrisDecode_SIZE_H264D_HW_PIC_T), IrisDecode_DMA_ALIGNMENT);

    return IrisDecodeAlign(size, IrisDecode_DMA_ALIGNMENT);
}

static inline GPU_U32 IrisDecode_size_h265d_bse_cmd_buf(GPU_U32 frame_width, GPU_U32 frame_height)
{
    GPU_U32 _size = IrisDecodeAlign(((IrisDecodeAlign(frame_width, IrisDecode_LCU_MAX_SIZE_PELS) / IrisDecode_LCU_MIN_SIZE_PELS) *
               (IrisDecodeAlign(frame_height, IrisDecode_LCU_MAX_SIZE_PELS) / IrisDecode_LCU_MIN_SIZE_PELS)) *
                IrisDecode_NUM_HW_PIC_BUF, IrisDecode_DMA_ALIGNMENT);
    _size = IrisDecodeMin(_size, IrisDecode_H265D_MAX_SLICE + 1);
    _size = 2 * _size * IrisDecode_SIZE_H265D_BSE_CMD_PER_BUF;

    return _size;
}

static inline GPU_U32 IrisDecode_size_h265d_vpp_cmd_buf(GPU_U32 frame_width, GPU_U32 frame_height)
{
    GPU_U32 _size = IrisDecodeAlign(((IrisDecodeAlign(frame_width, IrisDecode_LCU_MAX_SIZE_PELS) / IrisDecode_LCU_MIN_SIZE_PELS) *
               (IrisDecodeAlign(frame_height, IrisDecode_LCU_MAX_SIZE_PELS) / IrisDecode_LCU_MIN_SIZE_PELS)) *
              IrisDecode_NUM_HW_PIC_BUF, IrisDecode_DMA_ALIGNMENT);
    _size = IrisDecodeMin(_size, IrisDecode_H265D_MAX_SLICE + 1);
    _size = IrisDecodeAlign(_size, 4);
    _size = 2 * _size * IrisDecode_SIZE_H265D_VPP_CMD_PER_BUF;
    if (_size > IrisDecode_VPP_CMD_MAX_SIZE)
        _size = IrisDecode_VPP_CMD_MAX_SIZE;

    return _size;
}

static inline GPU_U32 IrisDecode_hfi_buffer_non_comv_h265d(GPU_U32 frame_width, GPU_U32 frame_height, GPU_U32 num_vpp_pipes)
{
    (void)num_vpp_pipes;
    GPU_U32 _size_bse = IrisDecode_size_h265d_bse_cmd_buf(frame_width, frame_height);
    GPU_U32 _size_vpp = IrisDecode_size_h265d_vpp_cmd_buf(frame_width, frame_height);
    GPU_U32 _size = IrisDecodeAlign(_size_bse, IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(_size_vpp, IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_NUM_HW_PIC_BUF * 20 * 22 * 4, IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(2 * 2u *
        (IrisDecodeAlign(frame_width, IrisDecode_LCU_MAX_SIZE_PELS) / IrisDecode_LCU_MIN_SIZE_PELS) *
        (IrisDecodeAlign(frame_height, IrisDecode_LCU_MAX_SIZE_PELS) / IrisDecode_LCU_MIN_SIZE_PELS), IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_SIZE_HW_PIC(IrisDecode_SIZE_H265D_HW_PIC_T), IrisDecode_DMA_ALIGNMENT) +
        IrisDecode_HDR10_HIST_EXTRADATA_SIZE;

    return IrisDecodeAlign(_size, IrisDecode_DMA_ALIGNMENT);
}

static inline GPU_U32 IrisDecode_size_h264d_lb_fe_top_data(GPU_U32 frame_width)
{
    return IrisDecode_MAX_FE_NBR_DATA_LUMA_LINE_BUFFER_SIZE * IrisDecodeAlign(frame_width, 16) * 3;
}

static inline GPU_U32 IrisDecode_size_h264d_lb_fe_top_ctrl(GPU_U32 frame_width)
{
    return IrisDecode_MAX_FE_NBR_CTRL_LCU64_LINE_BUFFER_SIZE * IrisDecodeCeil(frame_width, 16);
}

static inline GPU_U32 IrisDecode_size_h264d_lb_fe_left_ctrl(GPU_U32 frame_height)
{
    return IrisDecode_MAX_FE_NBR_CTRL_LCU64_LINE_BUFFER_SIZE * IrisDecodeCeil(frame_height, 16);
}

static inline GPU_U32 IrisDecode_size_h264d_lb_se_top_ctrl(GPU_U32 frame_width)
{
    return IrisDecode_MAX_SE_NBR_CTRL_LCU64_LINE_BUFFER_SIZE * IrisDecodeCeil(frame_width, 16);
}

static inline GPU_U32 IrisDecode_size_h264d_lb_se_left_ctrl(GPU_U32 frame_height)
{
    return IrisDecode_MAX_SE_NBR_CTRL_LCU64_LINE_BUFFER_SIZE * IrisDecodeCeil(frame_height, 16);
}

static inline GPU_U32 IrisDecode_size_h264d_lb_pe_top_data(GPU_U32 frame_width)
{
    return IrisDecode_MAX_PE_NBR_DATA_LCU64_LINE_BUFFER_SIZE * IrisDecodeCeil(frame_width, 16);
}

static inline GPU_U32 IrisDecode_size_h264d_lb_vsp_top(GPU_U32 frame_width)
{
    return (IrisDecodeCeil(frame_width, 16) << 7);
}

static inline GPU_U32 IrisDecode_size_h264d_lb_recon_dma_metadata_wr(GPU_U32 frame_height)
{
    return IrisDecodeAlign(frame_height, 16) * 32;
}

static inline GPU_U32 IrisDecode_size_h264d_qp(GPU_U32 frame_width, GPU_U32 frame_height)
{
    return IrisDecodeCeil(frame_width, 64) * IrisDecodeCeil(frame_height, 64) * 128;
}

static inline GPU_U32 IrisDecode_size_vpss_lb(GPU_U32 frame_width, GPU_U32 frame_height)
{
    GPU_U32 opb_lb_wr_llb_y_buffer_size, opb_lb_wr_llb_uv_buffer_size;
    GPU_U32 opb_wr_top_line_chroma_buffer_size;
    GPU_U32 opb_wr_top_line_luma_buffer_size;
    GPU_U32 macrotiling_size = 32;

    opb_wr_top_line_luma_buffer_size =
        IrisDecodeAlign(frame_width, macrotiling_size) / macrotiling_size * 256;
    opb_wr_top_line_luma_buffer_size =
        IrisDecodeAlign(opb_wr_top_line_luma_buffer_size, IrisDecode_DMA_ALIGNMENT) +
        (IrisDecode_MAX_TILE_COLUMNS - 1) * 256;
    opb_wr_top_line_luma_buffer_size =
        IrisDecodeMax(opb_wr_top_line_luma_buffer_size, (32 * IrisDecodeAlign(frame_height, 8)));
    opb_wr_top_line_chroma_buffer_size = opb_wr_top_line_luma_buffer_size;
    opb_lb_wr_llb_uv_buffer_size =
        IrisDecodeAlign((IrisDecodeAlign(frame_height, 8) / (4 / 2)) * 64, 32);
    opb_lb_wr_llb_y_buffer_size =
        IrisDecodeAlign((IrisDecodeAlign(frame_height, 8) / (4 / 2)) * 64, 32);
    return opb_wr_top_line_luma_buffer_size +
        opb_wr_top_line_chroma_buffer_size +
        opb_lb_wr_llb_uv_buffer_size +
        opb_lb_wr_llb_y_buffer_size;
}

static inline GPU_U32 IrisDecode_hfi_buffer_line_h264d(GPU_U32 frame_width, GPU_U32 frame_height,
                 int is_opb, GPU_U32 num_vpp_pipes)
{
    GPU_U32 vpss_lb_size = 0;
    GPU_U32 size;

    size = IrisDecodeAlign(IrisDecode_size_h264d_lb_fe_top_data(frame_width), IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_h264d_lb_fe_top_ctrl(frame_width), IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_h264d_lb_fe_left_ctrl(frame_height), IrisDecode_DMA_ALIGNMENT) * num_vpp_pipes +
        IrisDecodeAlign(IrisDecode_size_h264d_lb_se_top_ctrl(frame_width), IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_h264d_lb_se_left_ctrl(frame_height), IrisDecode_DMA_ALIGNMENT) * num_vpp_pipes +
        IrisDecodeAlign(IrisDecode_size_h264d_lb_pe_top_data(frame_width), IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_h264d_lb_vsp_top(frame_width), IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_h264d_lb_recon_dma_metadata_wr(frame_height), IrisDecode_DMA_ALIGNMENT) * 2 +
        IrisDecodeAlign(IrisDecode_size_h264d_qp(frame_width, frame_height), IrisDecode_DMA_ALIGNMENT);
    size = IrisDecodeAlign(size, IrisDecode_DMA_ALIGNMENT);
    if (is_opb)
        vpss_lb_size = IrisDecode_size_vpss_lb(frame_width, frame_height);

    return IrisDecodeAlign((size + vpss_lb_size), IrisDecode_DMA_ALIGNMENT);
}

static inline GPU_U32 IrisDecode_size_h265d_lb_fe_top_data(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_height;
    return IrisDecode_MAX_FE_NBR_DATA_LUMA_LINE_BUFFER_SIZE *
        (IrisDecodeAlign(frame_width, 64) + 8) * 2;
}

static inline GPU_U32 IrisDecode_size_h265d_lb_fe_top_ctrl(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_height;
    return IrisDecode_MAX_FE_NBR_CTRL_LCU64_LINE_BUFFER_SIZE *
        (IrisDecodeAlign(frame_width, IrisDecode_LCU_MAX_SIZE_PELS) / IrisDecode_LCU_MIN_SIZE_PELS);
}

static inline GPU_U32 IrisDecode_size_h265d_lb_fe_left_ctrl(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_width;
    return IrisDecode_MAX_FE_NBR_CTRL_LCU64_LINE_BUFFER_SIZE *
        (IrisDecodeAlign(frame_height, IrisDecode_LCU_MAX_SIZE_PELS) / IrisDecode_LCU_MIN_SIZE_PELS);
}

static inline GPU_U32 IrisDecode_size_h265d_lb_se_left_ctrl(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_width;
    return IrisDecodeMax(((frame_height + 16 - 1) / 8) *
        IrisDecode_MAX_SE_NBR_CTRL_LCU16_LINE_BUFFER_SIZE,
        IrisDecodeMax(((frame_height + 32 - 1) / 8) *
        IrisDecode_MAX_SE_NBR_CTRL_LCU32_LINE_BUFFER_SIZE,
        ((frame_height + 64 - 1) / 8) *
        IrisDecode_MAX_SE_NBR_CTRL_LCU64_LINE_BUFFER_SIZE));
}

static inline GPU_U32 IrisDecode_size_h265d_lb_se_top_ctrl(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_height;
    return (IrisDecode_LCU_MAX_SIZE_PELS / 8 * (128 / 8)) * ((frame_width + 15) >> 4);
}

static inline GPU_U32 IrisDecode_size_h265d_lb_pe_top_data(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_height;
    return IrisDecode_MAX_PE_NBR_DATA_LCU64_LINE_BUFFER_SIZE *
        (IrisDecodeAlign(frame_width, IrisDecode_LCU_MIN_SIZE_PELS) / IrisDecode_LCU_MIN_SIZE_PELS);
}

static inline GPU_U32 IrisDecode_size_h265d_lb_vsp_top(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_height;
    return ((frame_width + 63) >> 6) * 128;
}

static inline GPU_U32 IrisDecode_size_h265d_lb_vsp_left(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_width;
    return ((frame_height + 63) >> 6) * 128;
}

static inline GPU_U32 IrisDecode_size_h265d_lb_recon_dma_metadata_wr(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_width;
    return IrisDecode_size_h264d_lb_recon_dma_metadata_wr(frame_height);
}

static inline GPU_U32 IrisDecode_size_h265d_qp(GPU_U32 frame_width, GPU_U32 frame_height)
{
    return IrisDecode_size_h264d_qp(frame_width, frame_height);
}

static inline GPU_U32 IrisDecode_hfi_buffer_line_h265d(GPU_U32 frame_width, GPU_U32 frame_height, int is_opb, GPU_U32 num_vpp_pipes)
{
    GPU_U32 vpss_lb_size = 0, _size;

    _size = IrisDecodeAlign(IrisDecode_size_h265d_lb_fe_top_data(frame_width, frame_height), IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_h265d_lb_fe_top_ctrl(frame_width, frame_height), IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_h265d_lb_fe_left_ctrl(frame_width, frame_height),
              IrisDecode_DMA_ALIGNMENT) * num_vpp_pipes +
        IrisDecodeAlign(IrisDecode_size_h265d_lb_se_left_ctrl(frame_width, frame_height),
              IrisDecode_DMA_ALIGNMENT) * num_vpp_pipes +
        IrisDecodeAlign(IrisDecode_size_h265d_lb_se_top_ctrl(frame_width, frame_height), IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_h265d_lb_pe_top_data(frame_width, frame_height), IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_h265d_lb_vsp_top(frame_width, frame_height), IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_h265d_lb_vsp_left(frame_width, frame_height),
              IrisDecode_DMA_ALIGNMENT) * num_vpp_pipes +
        IrisDecodeAlign(IrisDecode_size_h265d_lb_recon_dma_metadata_wr(frame_width, frame_height),
              IrisDecode_DMA_ALIGNMENT) * 4 +
        IrisDecodeAlign(IrisDecode_size_h265d_qp(frame_width, frame_height), IrisDecode_DMA_ALIGNMENT);
    if (is_opb)
        vpss_lb_size = IrisDecode_size_vpss_lb(frame_width, frame_height);

    return IrisDecodeAlign((_size + vpss_lb_size), IrisDecode_DMA_ALIGNMENT);
}

static inline GPU_U32 IrisDecode_size_vpxd_lb_fe_left_ctrl(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_width;
    return IrisDecodeMax(((frame_height + 15) >> 4) *
             IrisDecode_MAX_FE_NBR_CTRL_LCU16_LINE_BUFFER_SIZE,
             IrisDecodeMax(((frame_height + 31) >> 5) *
               IrisDecode_MAX_FE_NBR_CTRL_LCU32_LINE_BUFFER_SIZE,
               ((frame_height + 63) >> 6) *
               IrisDecode_MAX_FE_NBR_CTRL_LCU64_LINE_BUFFER_SIZE));
}

static inline GPU_U32 IrisDecode_size_vpxd_lb_se_left_ctrl(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_width;
    return IrisDecodeMax(((frame_height + 15) >> 4) *
             IrisDecode_MAX_SE_NBR_CTRL_LCU16_LINE_BUFFER_SIZE,
             IrisDecodeMax(((frame_height + 31) >> 5) *
               IrisDecode_MAX_SE_NBR_CTRL_LCU32_LINE_BUFFER_SIZE,
               ((frame_height + 63) >> 6) *
               IrisDecode_MAX_SE_NBR_CTRL_LCU64_LINE_BUFFER_SIZE));
}

static inline GPU_U32 IrisDecode_size_vp9d_lb_vsp_top(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_height;
    return (((IrisDecodeAlign(IrisDecodeAlign(frame_width, 8), 64) >> 6) * 64 * 8) + 256);
}

static inline GPU_U32 IrisDecode_size_vpxd_lb_fe_top_ctrl(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_height;
    return ((IrisDecodeAlign(frame_width, 64) + 8) * 10 * 2);
}

static inline GPU_U32 IrisDecode_size_vpxd_lb_recon_dma_metadata_wr(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_width;
    return IrisDecodeAlign((IrisDecodeAlign(frame_height, 8) / (4 / 2)) * 64,
        IrisDecode_BUFFER_ALIGNMENT_32_BYTES);
}

static inline GPU_U32 IrisDecode_size_vpxd_lb_se_top_ctrl(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_height;
    return ((frame_width + 15) >> 4) * IrisDecode_MAX_FE_NBR_CTRL_LCU16_LINE_BUFFER_SIZE;
}

static inline GPU_U32 IrisDecode_size_vp9d_lb_pe_top_data(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_height;
    return ((IrisDecodeAlign(IrisDecodeAlign(frame_width, 8), 64) >> 6) * 176);
}

static inline GPU_U32 IrisDecode_size_vp9d_lb_fe_top_data(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_height;
    return (IrisDecodeAlign(IrisDecodeAlign(frame_width, 8), 64) + 8) * 10 * 2;
}

static inline GPU_U32 IrisDecode_size_vp9d_qp(GPU_U32 frame_width, GPU_U32 frame_height)
{
    return IrisDecode_size_h264d_qp(frame_width, frame_height);
}

static inline GPU_U32 IrisDecode_hfi_iris3_vp9d_lb_size(GPU_U32 frame_width, GPU_U32 frame_height, GPU_U32 num_vpp_pipes)
{
    return IrisDecodeAlign(IrisDecode_size_vpxd_lb_fe_left_ctrl(frame_width, frame_height), IrisDecode_DMA_ALIGNMENT) *
        num_vpp_pipes +
        IrisDecodeAlign(IrisDecode_size_vpxd_lb_se_left_ctrl(frame_width, frame_height), IrisDecode_DMA_ALIGNMENT) *
        num_vpp_pipes +
        IrisDecodeAlign(IrisDecode_size_vp9d_lb_vsp_top(frame_width, frame_height), IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_vpxd_lb_fe_top_ctrl(frame_width, frame_height), IrisDecode_DMA_ALIGNMENT) +
        2 * IrisDecodeAlign(IrisDecode_size_vpxd_lb_recon_dma_metadata_wr(frame_width, frame_height),
              IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_vpxd_lb_se_top_ctrl(frame_width, frame_height), IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_vp9d_lb_pe_top_data(frame_width, frame_height), IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_vp9d_lb_fe_top_data(frame_width, frame_height), IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_vp9d_qp(frame_width, frame_height), IrisDecode_DMA_ALIGNMENT);
}

static inline GPU_U32 IrisDecode_hfi_buffer_line_vp9d(GPU_U32 frame_width, GPU_U32 frame_height, GPU_U32 _yuv_bufcount_min, int is_opb,
             GPU_U32 num_vpp_pipes)
{
    (void)_yuv_bufcount_min;
    GPU_U32 vpss_lb_size = 0;
    GPU_U32 _lb_size;

    _lb_size = IrisDecode_hfi_iris3_vp9d_lb_size(frame_width, frame_height, num_vpp_pipes);

    if (is_opb)
        vpss_lb_size = IrisDecode_size_vpss_lb(frame_width, frame_height);

    return _lb_size + vpss_lb_size;
}

static inline GPU_U32 IrisDecode_size_av1d_lb_fe_top_data(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_height;
    return (IrisDecodeAlign(frame_width, IrisDecode_AV1D_LCU_MAX_SIZE_PELS) *
        ((IrisDecode_BITS_PER_PIX * IrisDecode_NUM_LINES_LUMA) >> 3) +
          IrisDecodeAlign(frame_width, IrisDecode_AV1D_LCU_MAX_SIZE_PELS) / 2 *
        ((IrisDecode_BITS_PER_PIX * IrisDecode_NUM_LINES_CHROMA) >> 3) * 2);
}

static inline GPU_U32 IrisDecode_size_av1d_lb_fe_top_ctrl(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_height;
    return (IrisDecode_NUM_CTRL_PACK_LCU * ((frame_width + IrisDecode_AV1D_LCU_MIN_SIZE_PELS - 1) /
        IrisDecode_AV1D_LCU_MIN_SIZE_PELS) * IrisDecode_BITS_PER_CTRL_PACK / 8);
}

static inline GPU_U32 IrisDecode_size_av1d_lb_fe_left_data(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_width;
    return (32 * (IrisDecodeAlign(frame_height, IrisDecode_AV1D_LCU_MAX_SIZE_PELS) +
              IrisDecodeAlign(frame_height, IrisDecode_AV1D_LCU_MAX_SIZE_PELS) /
              IrisDecode_AV1D_LCU_MIN_SIZE_PELS * 16) +
        16 * (IrisDecodeAlign(frame_height, IrisDecode_AV1D_LCU_MAX_SIZE_PELS) / 2 +
              IrisDecodeAlign(frame_height, IrisDecode_AV1D_LCU_MAX_SIZE_PELS) /
              IrisDecode_AV1D_LCU_MIN_SIZE_PELS * 8) * 2 +
        24 * (IrisDecodeAlign(frame_height, IrisDecode_AV1D_LCU_MAX_SIZE_PELS) +
              IrisDecodeAlign(frame_height, IrisDecode_AV1D_LCU_MAX_SIZE_PELS) /
              IrisDecode_AV1D_LCU_MIN_SIZE_PELS * 16) +
        24 * (IrisDecodeAlign(frame_height, IrisDecode_AV1D_LCU_MAX_SIZE_PELS) / 2 +
              IrisDecodeAlign(frame_height, IrisDecode_AV1D_LCU_MAX_SIZE_PELS) /
              IrisDecode_AV1D_LCU_MIN_SIZE_PELS * 12) * 2 +
        24 * (IrisDecodeAlign(frame_height, IrisDecode_AV1D_LCU_MAX_SIZE_PELS) +
              IrisDecodeAlign(frame_height, IrisDecode_AV1D_LCU_MAX_SIZE_PELS) /
              IrisDecode_AV1D_LCU_MIN_SIZE_PELS * 16) +
        16 * (IrisDecodeAlign(frame_height, IrisDecode_AV1D_LCU_MAX_SIZE_PELS) +
              IrisDecodeAlign(frame_height, IrisDecode_AV1D_LCU_MAX_SIZE_PELS) /
              IrisDecode_AV1D_LCU_MIN_SIZE_PELS * 16) +
        16 * (IrisDecodeAlign(frame_height, IrisDecode_AV1D_LCU_MAX_SIZE_PELS) / 2 +
              IrisDecodeAlign(frame_height, IrisDecode_AV1D_LCU_MAX_SIZE_PELS) /
              IrisDecode_AV1D_LCU_MIN_SIZE_PELS * 12) * 2);
}

static inline GPU_U32 IrisDecode_size_av1d_lb_fe_left_ctrl(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_width;
    return (16 * ((IrisDecodeAlign(frame_height, IrisDecode_AV1D_LCU_MAX_SIZE_PELS) / 16) +
        (IrisDecodeAlign(frame_height, IrisDecode_AV1D_LCU_MAX_SIZE_PELS) /
         IrisDecode_AV1D_LCU_MIN_SIZE_PELS)) +
         3 * 16 * (IrisDecodeAlign(frame_height, IrisDecode_AV1D_LCU_MAX_SIZE_PELS) /
         IrisDecode_AV1D_LCU_MIN_SIZE_PELS));
}

static inline GPU_U32 IrisDecode_size_av1d_lb_se_left_ctrl(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_width;
    return (IrisDecodeMax(((frame_height + 15) / 16) *
        IrisDecode_MAX_SE_NBR_CTRL_LCU16_LINE_BUFFER_SIZE,
        IrisDecodeMax(((frame_height + 31) / 32) *
        IrisDecode_MAX_SE_NBR_CTRL_LCU32_LINE_BUFFER_SIZE,
        ((frame_height + 63) / 64) *
        IrisDecode_MAX_SE_NBR_CTRL_LCU64_LINE_BUFFER_SIZE)));
}

static inline GPU_U32 IrisDecode_size_av1d_lb_se_top_ctrl(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_height;
    return (((frame_width + 7) / 8) * IrisDecode_MAX_SE_NBR_CTRL_LCU16_LINE_BUFFER_SIZE);
}

static inline GPU_U32 IrisDecode_size_av1d_lb_pe_top_data(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_height;
    return (IrisDecodeMax(((frame_width + 15) / 16) *
        IrisDecode_MAX_PE_NBR_DATA_LCU16_LINE_BUFFER_SIZE,
        IrisDecodeMax(((frame_width + 31) / 32) *
        IrisDecode_MAX_PE_NBR_DATA_LCU32_LINE_BUFFER_SIZE,
        ((frame_width + 63) / 64) *
        IrisDecode_MAX_PE_NBR_DATA_LCU64_LINE_BUFFER_SIZE)));
}

static inline GPU_U32 IrisDecode_size_av1d_lb_vsp_top(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_height;
    return (IrisDecodeMax(((frame_width + 63) / 64) * 1280,
            ((frame_width + 127) / 128) * IrisDecode_MAX_HEIGHT));
}

static inline GPU_U32 IrisDecode_size_av1d_lb_recon_dma_metadata_wr(GPU_U32 frame_width,
                             GPU_U32 frame_height)
{
    (void)frame_width;
    return ((IrisDecodeAlign(frame_height, 8) / (4 / 2)) * 64);
}

static inline GPU_U32 IrisDecode_size_av1d_qp(GPU_U32 frame_width, GPU_U32 frame_height)
{
    return IrisDecode_size_h264d_qp(frame_width, frame_height);
}

static inline GPU_U32 IrisDecode_size_av1d_lb_opb_wr1_nv12_ubwc(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_height;
    GPU_U32 size, y_width, y_width_a = 128;

    y_width = IrisDecodeAlign(frame_width, y_width_a);

    size = ((y_width + IrisDecode_HFI_COLOR_FORMAT_YUV420_NV12_UBWC_Y_TILE_WIDTH - 1) /
         IrisDecode_HFI_COLOR_FORMAT_YUV420_NV12_UBWC_Y_TILE_WIDTH  +
         (IrisDecode_AV1D_MAX_TILE_COLS - 1));
    return size * IrisDecode_UBWC_TILE_SIZE;
}

static inline GPU_U32 IrisDecode_size_av1d_lb_opb_wr1_tp10_ubwc(GPU_U32 frame_width, GPU_U32 frame_height)
{
    (void)frame_height;
    GPU_U32 size, y_width, y_width_a = 256;

    y_width = IrisDecodeAlign(IrisDecodeAlign(frame_width,192)*4/3,y_width_a);

    size = ((y_width + IrisDecode_HFI_COLOR_FORMAT_YUV420_TP10_UBWC_Y_TILE_WIDTH - 1) /
         IrisDecode_HFI_COLOR_FORMAT_YUV420_TP10_UBWC_Y_TILE_WIDTH +
         (IrisDecode_AV1D_MAX_TILE_COLS - 1));

    return size * IrisDecode_UBWC_TILE_SIZE;
}

static inline GPU_U32 IrisDecode_hfi_buffer_line_av1d(GPU_U32 frame_width, GPU_U32 frame_height,
                int is_opb, GPU_U32 num_vpp_pipes)
{

    GPU_U32 size, vpss_lb_size, opbwrbufsize, opbwr8, opbwr10;

    size = IrisDecodeAlign(IrisDecode_size_av1d_lb_fe_top_data(frame_width, frame_height),
             IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_av1d_lb_fe_top_ctrl(frame_width, frame_height),
              IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_av1d_lb_fe_left_data(frame_width, frame_height),
              IrisDecode_DMA_ALIGNMENT) * num_vpp_pipes +
        IrisDecodeAlign(IrisDecode_size_av1d_lb_fe_left_ctrl(frame_width, frame_height),
              IrisDecode_DMA_ALIGNMENT) * num_vpp_pipes +
        IrisDecodeAlign(IrisDecode_size_av1d_lb_se_left_ctrl(frame_width, frame_height),
              IrisDecode_DMA_ALIGNMENT) * num_vpp_pipes +
        IrisDecodeAlign(IrisDecode_size_av1d_lb_se_top_ctrl(frame_width, frame_height),
              IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_av1d_lb_pe_top_data(frame_width, frame_height),
              IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_av1d_lb_vsp_top(frame_width, frame_height),
              IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_size_av1d_lb_recon_dma_metadata_wr
              (frame_width, frame_height), IrisDecode_DMA_ALIGNMENT) * 2 +
        IrisDecodeAlign(IrisDecode_size_av1d_qp(frame_width, frame_height), IrisDecode_DMA_ALIGNMENT);
    opbwr8 = IrisDecode_size_av1d_lb_opb_wr1_nv12_ubwc(frame_width, frame_height);
    opbwr10 = IrisDecode_size_av1d_lb_opb_wr1_tp10_ubwc(frame_width, frame_height);
    opbwrbufsize = opbwr8 >= opbwr10 ? opbwr8 : opbwr10;
    size = IrisDecodeAlign((size + opbwrbufsize), IrisDecode_DMA_ALIGNMENT);
    if (is_opb) {
        vpss_lb_size = IrisDecode_size_vpss_lb(frame_width, frame_height);
        size = IrisDecodeAlign(size + vpss_lb_size, IrisDecode_DMA_ALIGNMENT);
    }

    return size;
}

static inline GPU_U32 IrisDecode_hfi_buffer_persist_h264d(void)
{
    return IrisDecodeAlign(IrisDecode_SIZE_SLIST_BUF_H264 * IrisDecode_NUM_SLIST_BUF_H264 +
            IrisDecode_H264_DISPLAY_BUF_SIZE * IrisDecode_H264_NUM_FRM_INFO +
            IrisDecode_NUM_HW_PIC_BUF * (IrisDecode_SIZE_SEI_USERDATA + IrisDecode_SIZE_ARP),
            IrisDecode_DMA_ALIGNMENT);
}

static inline GPU_U32 IrisDecode_hfi_buffer_persist_h265d(GPU_U32 rpu_enabled)
{
    return IrisDecodeAlign((IrisDecode_SIZE_SLIST_BUF_H265 * IrisDecode_NUM_SLIST_BUF_H265 +
              IrisDecode_H265_NUM_FRM_INFO * IrisDecode_H265_DISPLAY_BUF_SIZE +
              IrisDecode_H265_NUM_TILE * sizeof(GPU_U32) +
              IrisDecode_NUM_HW_PIC_BUF * (IrisDecode_SIZE_SEI_USERDATA + IrisDecode_SIZE_ARP) +
              rpu_enabled * IrisDecode_NUM_HW_PIC_BUF * IrisDecode_SIZE_DOLBY_RPU_METADATA),
             IrisDecode_DMA_ALIGNMENT);
}

static inline GPU_U32 IrisDecode_hfi_iris3_vp9d_comv_size(void)
{
    return (((8192 + 63) >> 6) * ((4320 + 63) >> 6) * 8 * 8 * 2 * 8);
}

static inline GPU_U32 IrisDecode_hfi_buffer_persist_vp9d(void)
{
    return IrisDecodeAlign(IrisDecode_VP9_NUM_PROBABILITY_TABLE_BUF * IrisDecode_VP9_PROB_TABLE_SIZE, IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_hfi_iris3_vp9d_comv_size(), IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_MAX_SUPERFRAME_HEADER_LEN, IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_VP9_UDC_HEADER_BUF_SIZE, IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_VP9_NUM_FRAME_INFO_BUF * IrisDecode_CCE_TILE_OFFSET_SIZE, IrisDecode_DMA_ALIGNMENT) +
        IrisDecodeAlign(IrisDecode_VP9_NUM_FRAME_INFO_BUF * IrisDecode_VP9_FRAME_INFO_BUF_SIZE, IrisDecode_DMA_ALIGNMENT) +
        IrisDecode_HDR10_HIST_EXTRADATA_SIZE;
}

static inline GPU_U32 IrisDecode_hfi_buffer_persist_av1d(GPU_U32 max_width, GPU_U32 max_height, GPU_U32 total_ref_count)
{
    GPU_U32 comv_size, size;

    comv_size =  IrisDecode_hfi_buffer_comv_av1d(max_width, max_height, total_ref_count);
    size = IrisDecodeAlign((IrisDecode_SIZE_AV1D_SEQUENCE_HEADER * 2 + IrisDecode_SIZE_AV1D_METADATA +
    IrisDecode_AV1D_NUM_HW_PIC_BUF * (IrisDecode_SIZE_AV1D_TILE_OFFSET + IrisDecode_SIZE_AV1D_QM + IrisDecode_SIZE_ARP) +
    IrisDecode_AV1D_NUM_FRAME_HEADERS * (IrisDecode_SIZE_AV1D_FRAME_HEADER +
    2 * IrisDecode_SIZE_AV1D_PROB_TABLE) + comv_size + IrisDecode_HDR10_HIST_EXTRADATA_SIZE +
    IrisDecode_SIZE_AV1D_METADATA * IrisDecode_AV1D_NUM_HW_PIC_BUF), IrisDecode_DMA_ALIGNMENT);

    return IrisDecodeAlign(size, IrisDecode_DMA_ALIGNMENT);
}

static inline GPU_U32 IrisDecode_size_av1d_ibc_nv12_ubwc(GPU_U32 frame_width, GPU_U32 frame_height)
{
    GPU_U32 size;
    GPU_U32 y_width_a = 128, y_height_a = 32;
    GPU_U32 uv_width_a = 128, uv_height_a = 32;
    GPU_U32 ybufsize, uvbufsize, y_width, y_height, uv_width, uv_height;
    GPU_U32 y_meta_width_a = 64, y_meta_height_a = 16;
    GPU_U32 uv_meta_width_a = 64, uv_meta_height_a = 16;
    GPU_U32 meta_height, meta_stride, meta_size;
    GPU_U32 tile_width_y = IrisDecode_HFI_COLOR_FORMAT_YUV420_NV12_UBWC_Y_TILE_WIDTH;
    GPU_U32 tile_height_y = IrisDecode_HFI_COLOR_FORMAT_YUV420_NV12_UBWC_Y_TILE_HEIGHT;
    GPU_U32 tile_width_uv = IrisDecode_HFI_COLOR_FORMAT_YUV420_NV12_UBWC_UV_TILE_WIDTH;
    GPU_U32 tile_height_uv = IrisDecode_HFI_COLOR_FORMAT_YUV420_NV12_UBWC_UV_TILE_HEIGHT;

    y_width = IrisDecodeAlign(frame_width, y_width_a);
    y_height = IrisDecodeAlign(frame_height, y_height_a);
    uv_width = IrisDecodeAlign(frame_width, uv_width_a);
    uv_height = IrisDecodeAlign(((frame_height + 1) >> 1), uv_height_a);
    ybufsize = IrisDecodeAlign((y_width * y_height), IrisDecode_HFI_ALIGNMENT_4096);
    uvbufsize = IrisDecodeAlign(uv_width * uv_height, IrisDecode_HFI_ALIGNMENT_4096);
    size = ybufsize + uvbufsize;
    meta_stride = IrisDecodeAlign(((frame_width + (tile_width_y - 1)) / tile_width_y),
                y_meta_width_a);
    meta_height = IrisDecodeAlign(((frame_height + (tile_height_y - 1)) / tile_height_y),
                y_meta_height_a);
    meta_size = IrisDecodeAlign(meta_stride * meta_height, IrisDecode_HFI_ALIGNMENT_4096);
    size += meta_size;
    meta_stride = IrisDecodeAlign(((((frame_width + 1) >> 1) + (tile_width_uv - 1)) /
                tile_width_uv),    uv_meta_width_a);
    meta_height = IrisDecodeAlign(((((frame_height + 1) >> 1) + (tile_height_uv - 1)) /
                tile_height_uv), uv_meta_height_a);
    meta_size = IrisDecodeAlign(meta_stride * meta_height, IrisDecode_HFI_ALIGNMENT_4096);
    size += meta_size;

    return size;
}

static inline GPU_U32 IrisDecode_hfi_yuv420_tp10_calc_y_stride(GPU_U32 frame_width, GPU_U32 stride_multiple)
{
    GPU_U32 stride;

    stride = IrisDecodeAlign(frame_width, 192);
    stride = IrisDecodeAlign(stride * 4 / 3, stride_multiple);

    return stride;
}

static inline GPU_U32 IrisDecode_hfi_yuv420_tp10_calc_y_bufheight(GPU_U32 frame_height, GPU_U32 min_buf_height_multiple)
{
    return IrisDecodeAlign(frame_height, min_buf_height_multiple);
}

static inline GPU_U32 IrisDecode_hfi_yuv420_tp10_calc_uv_stride(GPU_U32 frame_width, GPU_U32 stride_multiple)
{
    GPU_U32 stride;

    stride = IrisDecodeAlign(frame_width, 192);
    stride = IrisDecodeAlign(stride * 4 / 3, stride_multiple);

    return stride;
}

static inline GPU_U32 IrisDecode_hfi_yuv420_tp10_calc_uv_bufheight(GPU_U32 frame_height, GPU_U32 min_buf_height_multiple)
{
    return IrisDecodeAlign(((frame_height + 1) >> 1),    min_buf_height_multiple);
}

static inline GPU_U32 IrisDecode_size_av1d_ibc_tp10_ubwc(GPU_U32 frame_width, GPU_U32 frame_height)
{
    GPU_U32 size;
    GPU_U32 y_width_a = 256, y_height_a = 16,
        uv_width_a = 256, uv_height_a = 16;
    GPU_U32 ybufsize, uvbufsize, y_width, y_height, uv_width, uv_height;
    GPU_U32 y_meta_width_a = 64, y_meta_height_a = 16,
        uv_meta_width_a = 64, uv_meta_height_a = 16;
    GPU_U32 meta_height, meta_stride, meta_size;
    GPU_U32 tile_width_y = IrisDecode_HFI_COLOR_FORMAT_YUV420_TP10_UBWC_Y_TILE_WIDTH;
    GPU_U32 tile_height_y = IrisDecode_HFI_COLOR_FORMAT_YUV420_TP10_UBWC_Y_TILE_HEIGHT;
    GPU_U32 tile_width_uv = IrisDecode_HFI_COLOR_FORMAT_YUV420_TP10_UBWC_UV_TILE_WIDTH;
    GPU_U32 tile_height_uv = IrisDecode_HFI_COLOR_FORMAT_YUV420_TP10_UBWC_UV_TILE_HEIGHT;

    y_width = IrisDecode_hfi_yuv420_tp10_calc_y_stride(frame_width, y_width_a);
    y_height = IrisDecode_hfi_yuv420_tp10_calc_y_bufheight(frame_height, y_height_a);
    uv_width = IrisDecode_hfi_yuv420_tp10_calc_uv_stride(frame_width, uv_width_a);
    uv_height = IrisDecode_hfi_yuv420_tp10_calc_uv_bufheight(frame_height, uv_height_a);
    ybufsize = IrisDecodeAlign(y_width * y_height, IrisDecode_HFI_ALIGNMENT_4096);
    uvbufsize = IrisDecodeAlign(uv_width * uv_height, IrisDecode_HFI_ALIGNMENT_4096);
    size = ybufsize + uvbufsize;
    meta_stride = IrisDecodeAlign(((frame_width + (tile_width_y - 1)) / tile_width_y),
                y_meta_width_a);
    meta_height = IrisDecodeAlign(((frame_height + (tile_height_y - 1)) / tile_height_y),
                y_meta_height_a);
    meta_size = IrisDecodeAlign(meta_stride * meta_height, IrisDecode_HFI_ALIGNMENT_4096);
    size += meta_size;
    meta_stride = IrisDecodeAlign(((((frame_width + 1) >> 1) + (tile_width_uv - 1)) /
                tile_width_uv), uv_meta_width_a);
    meta_height = IrisDecodeAlign(((((frame_height + 1) >> 1) + (tile_height_uv - 1)) /
                tile_height_uv), uv_meta_height_a);
    meta_size = IrisDecodeAlign(meta_stride * meta_height, IrisDecode_HFI_ALIGNMENT_4096);
    size += meta_size;

    return size;
}
