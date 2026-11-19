#pragma once
#include <windows.h>
#ifndef GPU_UAPI_VERSION
#include "qcom_gpu_uapi.h"
#endif

typedef struct {
    unsigned Version,Size;
    void *Device,*Queue;
    HRESULT (WINAPI *Allocate)(void *,unsigned,unsigned,GPU_ALLOCATION *);
    void (WINAPI *Free)(void *,const GPU_ALLOCATION *);
    HRESULT (WINAPI *Create)(void *,void *,void **);
    HRESULT (WINAPI *Submit)(void *,const GPU_SUBMIT *,unsigned,uint64_t *);
    HRESULT (WINAPI *Wait)(void *,uint64_t,uint64_t);
    void (WINAPI *Destroy)(void *);
} A830_NATIVE_HOST;

struct pipe_screen;
struct pipe_context;
struct pipe_resource;
struct pipe_sampler_view;
struct pipe_image_view;
struct pipe_stream_output_target;
struct pipe_box;
union pipe_color_union;
struct fd_bo;
struct fd_device;
typedef struct {
    uint64_t Size;
    unsigned Levels,Layers;
    struct {uint64_t Offset;unsigned Pitch,LayerPitch;} Mip[16];
} A830_NATIVE_LAYOUT;
typedef struct {
    unsigned Count,Strides[4],RasterizedStream;
    struct {unsigned Location,Mask,Buffer,Stream,Offset;} Outputs[64];
} A830_NATIVE_STREAM_OUTPUT;

#ifdef __cplusplus
extern "C" {
#endif
void fd_windows_set_native_host(const A830_NATIVE_HOST *);
struct fd_bo *fd_windows_bo_import(struct fd_device *,const GPU_ALLOCATION *,uint64_t);
struct pipe_resource *fd_native_resource(struct pipe_screen *,const struct pipe_resource *,struct fd_bo *,A830_NATIVE_LAYOUT *);
struct pipe_screen *WINAPI A830GpuScreen(const A830_NATIVE_HOST *);
void WINAPI A830GpuDestroyScreen(struct pipe_screen *);
struct pipe_context *WINAPI A830GpuContext(struct pipe_screen *,const A830_NATIVE_HOST *);
struct pipe_resource *WINAPI A830GpuResource(struct pipe_screen *,const struct pipe_resource *,const GPU_ALLOCATION *,uint64_t,A830_NATIVE_LAYOUT *);
void *WINAPI A830GpuShader(struct pipe_context *,unsigned,const uint32_t *,size_t,unsigned[3]);
void *WINAPI A830GpuShaderStreamOutput(struct pipe_context *,unsigned,const uint32_t *,size_t,const A830_NATIVE_STREAM_OUTPUT *);
struct pipe_stream_output_target *WINAPI A830GpuStreamTarget(struct pipe_context *,struct pipe_resource *,struct pipe_resource *);
void WINAPI A830GpuViews(struct pipe_context *,unsigned,unsigned,struct pipe_sampler_view **,unsigned,void **);
HRESULT WINAPI A830GpuClear(struct pipe_context *,const struct pipe_image_view *,const union pipe_color_union *,BOOL,unsigned,unsigned,const struct pipe_box *);
#ifdef __cplusplus
}
#endif
