#pragma once
#include <vector>
#include <string>
#include <type_traits>
#include <cstring>

template<class T,class=void> struct A830TypedSignature:std::false_type{};
template<class T> struct A830TypedSignature<T,std::void_t<decltype(T::RegisterComponentType)>>:std::true_type{};
template<class Entry> static std::vector<uint32_t> A830Signature(const Entry *entries,unsigned count,bool pixel,bool output) {
    std::vector<uint32_t> words(2+count*6);words[0]=count;words[1]=8;
    for(unsigned i=0;i<count;i++) {
        unsigned sys=entries[i].SystemValue,reg=entries[i].Register,type=3,index=0;
        std::string name="TEXCOORD";
        static const char *names[]={"TEXCOORD","SV_Position","SV_ClipDistance","SV_CullDistance","SV_RenderTargetArrayIndex",
            "SV_ViewportArrayIndex","SV_VertexID","SV_PrimitiveID","SV_InstanceID","SV_IsFrontFace","SV_SampleIndex"};
        if(sys<11)name=names[sys];
        if(sys>=11 && sys<=22) {
            unsigned token=sys;
            if(token<=14){sys=11;index=token-11;}
            else if(token<=16){sys=12;index=token-15;}
            else if(token<=19){sys=13;index=token-17;}
            else if(token==20)sys=14;
            else {sys=token==21?15:16;index=token==21?1:0;}
            name=(sys==12 || sys==14)?"SV_InsideTessFactor":"SV_TessFactor";
        }
        if(pixel && output && (!sys || sys==64)){name="SV_Target";sys=64;}
        if constexpr(A830TypedSignature<Entry>::value)type=entries[i].RegisterComponentType?entries[i].RegisterComponentType:3;
        else if(sys==6 || sys==7 || sys==8 || sys==9)type=1;
        unsigned at=2+i*6;words[at]=(unsigned)words.size()*4;words[at+1]=(name=="TEXCOORD" || name=="SV_Target")?reg:index;
        words[at+2]=sys;words[at+3]=type;words[at+4]=reg;words[at+5]=entries[i].Mask|((output?0:entries[i].Mask)<<8);
        size_t offset=words.size()*4;words.resize(words.size()+(name.size()+4)/4);memcpy((char *)words.data()+offset,name.c_str(),name.size()+1);
    }
    return words;
}
