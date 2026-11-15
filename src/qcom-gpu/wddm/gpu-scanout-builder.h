#pragma once
/* Native PM4 CP_MEMCPY row builder, shared with the host-side interpreter test.
 * Call only after the rendering fence has cleaned prior render caches. */
static unsigned A830CopyPacket(unsigned op,unsigned count) {
    unsigned a=op,b=count;a^=a>>4;a^=a>>2;a^=a>>1;
    b^=b>>8;b^=b>>4;b^=b>>2;b^=b>>1;
    return 0x70000000u|(op<<16)|count|((~a&1)<<23)|((~b&1)<<15);
}
static unsigned A830BuildGpuCopy(unsigned *commands,unsigned capacity,
    unsigned long long source,unsigned long long sourceBytes,unsigned sourcePitch,
    unsigned long long destination,unsigned long long destinationBytes,unsigned destinationPitch,
    unsigned width,unsigned height) {
    if(!commands || !width || !height || width>0x3fffffffu ||
       ((source|destination|sourcePitch|destinationPitch)&3u) ||
       sourcePitch<width*4u || destinationPitch<width*4u || height>(0xffffffffu-8u)/6u)return 0;
    unsigned long long srcNeed=(unsigned long long)(height-1)*sourcePitch+width*4u;
    unsigned long long dstNeed=(unsigned long long)(height-1)*destinationPitch+width*4u;
    if(srcNeed>sourceBytes || dstNeed>destinationBytes ||
       source+srcNeed<source || destination+dstNeed<destination ||
       (source<destination+dstNeed && destination<source+srcNeed))return 0;
    unsigned words=height*6u+8u;if(capacity<words)return 0;
    unsigned at=0;
    commands[at++]=A830CopyPacket(0x17,1);commands[at++]=1; /* BR only */
    commands[at++]=A830CopyPacket(0x26,0); /* GPU-side WFI, no host wait */
    for(unsigned y=0;y<height;y++) {
        unsigned long long src=source+(unsigned long long)y*sourcePitch;
        unsigned long long dst=destination+(unsigned long long)y*destinationPitch;
        commands[at++]=A830CopyPacket(0x75,5);commands[at++]=width;
        commands[at++]=(unsigned)src;commands[at++]=(unsigned)(src>>32);
        commands[at++]=(unsigned)dst;commands[at++]=(unsigned)(dst>>32);
    }
    commands[at++]=A830CopyPacket(0x12,0); /* CP_WAIT_MEM_WRITES */
    commands[at++]=A830CopyPacket(0x17,1);commands[at++]=3;
    /* Two CP_NOP payload dwords keep an even, stable packet tail. */
    commands[at++]=A830CopyPacket(0x10,1);commands[at++]=0;
    return at;
}
