#pragma once
#include <windows.h>
#include <stdint.h>

struct A830FrameProfile {
 volatile LONG64 sequence;
 uint64_t context,start,prepared,submitted,waited,finished;
 uint64_t draws,draw_ticks,passes,pipelines,pipeline_ticks,shaders,shader_ticks,buffers,buffer_ticks,reused;
 uint64_t image_ticks,map_ticks,unmap_ticks,upload_ticks,copy_ticks,blt_ticks,clear_ticks,sync_ticks;
 uint64_t close_ticks,barrier_ticks;
};
struct A830Profile {
 uint32_t version,size,pid,reserved;
 uint64_t frequency;
 volatile LONG64 serial;
 volatile LONG writer;
 A830FrameProfile frames[256];
};
static inline uint64_t profile_clock(){LARGE_INTEGER time;QueryPerformanceCounter(&time);return (uint64_t)time.QuadPart;}
struct ProfileTimer {
 uint64_t &ticks,start;
 explicit ProfileTimer(uint64_t &value):ticks(value),start(profile_clock()){}
 void finish(){if(start){ticks+=profile_clock()-start;start=0;}}
 ~ProfileTimer(){finish();}
};
