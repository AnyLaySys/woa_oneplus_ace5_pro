#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <cstring>
#include "../../mesa/umd/profile.h"

int main(int argc,char **argv){
    if(argc!=2)return 2;
    DWORD pid=strtoul(argv[1],nullptr,10);WCHAR name[96];swprintf(name,96,L"Local\\A830Profile-%lu",pid);
    HANDLE file=OpenFileMappingW(FILE_MAP_READ,FALSE,name);
    if(!file){printf("{\"pid\":%lu,\"profile_open_error\":%lu}\n",pid,GetLastError());return 1;}
    auto profile=(const A830Profile *)MapViewOfFile(file,FILE_MAP_READ,0,0,sizeof(A830Profile));CloseHandle(file);
    if(!profile)return 1;
    if(profile->version!=1 || profile->size!=sizeof(*profile) || !profile->frequency){UnmapViewOfFile(profile);return 1;}
    for(const auto &frame:profile->frames){
        LONG64 sequence=frame.sequence;if(!sequence)continue;MemoryBarrier();
        A830FrameProfile copy;memcpy(&copy,&frame,sizeof(copy));MemoryBarrier();if(sequence!=frame.sequence)continue;
        double ms=1000.0/(double)profile->frequency;
        printf("{\"pid\":%lu,\"serial\":%lld,\"context\":%llu,\"start_qpc\":%llu,\"end_qpc\":%llu,\"draws\":%llu,\"draw_ms\":%.6f,\"passes\":%llu,\"pipelines\":%llu,\"pipeline_ms\":%.6f,\"shaders\":%llu,\"shader_ms\":%.6f,\"buffers\":%llu,\"buffer_ms\":%.6f,\"reused\":%llu,\"record_ms\":%.6f,\"submit_ms\":%.6f,\"wait_ms\":%.6f,\"recycle_ms\":%.6f,\"total_ms\":%.6f,\"image_ms\":%.6f,\"map_ms\":%.6f,\"unmap_ms\":%.6f,\"upload_ms\":%.6f,\"copy_ms\":%.6f,\"blt_ms\":%.6f,\"clear_ms\":%.6f,\"sync_ms\":%.6f,\"close_ms\":%.6f,\"barrier_ms\":%.6f}\n",
            pid,sequence,copy.context,copy.start,copy.finished,copy.draws,copy.draw_ticks*ms,copy.passes,copy.pipelines,copy.pipeline_ticks*ms,copy.shaders,copy.shader_ticks*ms,copy.buffers,copy.buffer_ticks*ms,copy.reused,
            (copy.prepared-copy.start)*ms,(copy.submitted-copy.prepared)*ms,(copy.waited-copy.submitted)*ms,(copy.finished-copy.waited)*ms,(copy.finished-copy.start)*ms,
            copy.image_ticks*ms,copy.map_ticks*ms,copy.unmap_ticks*ms,copy.upload_ticks*ms,copy.copy_ticks*ms,copy.blt_ticks*ms,copy.clear_ticks*ms,copy.sync_ticks*ms,copy.close_ticks*ms,copy.barrier_ticks*ms);
    }
    UnmapViewOfFile(profile);return 0;
}
