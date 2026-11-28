#pragma once
#include <vector>
#include <cstdint>
#include <cstring>

/* Media Foundation carries AVC/HEVC as Annex B or length-prefixed NAL units.
   Iris consumes Annex B; VP9 and AV1 samples already contain complete frames. */
class DecodeStream {
 unsigned codec_=0,length_=0;
 std::vector<uint8_t> header_;
 static bool AnnexB(const uint8_t *p,size_t n){return n>=3 && !p[0] && !p[1] && (p[2]==1 || (n>=4 && !p[2] && p[3]==1));}
 static bool Nals(const uint8_t *p,size_t n,unsigned length,std::vector<uint8_t> &out) {
  while(n) {
   if(n<length)return false;
   unsigned size=0;for(unsigned i=0;i<length;i++)size=size*256+p[i];p+=length;n-=length;
   if(!size || size>n || size>33554428 || out.size()>33554428-size)return false;
   out.insert(out.end(),{0,0,0,1});out.insert(out.end(),p,p+size);p+=size;n-=size;
  }
  return true;
 }
 bool Array(const uint8_t *&p,const uint8_t *end,unsigned count) {
  for(unsigned i=0;i<count;i++) {
   if(end-p<2)return false;
   unsigned n=p[0]*256+p[1];p+=2;
   if(!n || (size_t)(end-p)<n)return false;
   header_.insert(header_.end(),{0,0,0,1});header_.insert(header_.end(),p,p+n);p+=n;
  }
  return true;
 }
public:
 bool Configure(unsigned codec,const uint8_t *p,size_t n) {
  codec_=codec;length_=0;header_.clear();if(codec!=1 && codec!=3 && codec!=5 && codec!=7)return false;if(!n)return true;
  if(n>1048576 || !p)return false;
  if(codec==5)return true;
  if(codec==7){if(n>=4 && p[0]==0x81){p+=4;n-=4;}header_.assign(p,p+n);return true;}
  if(AnnexB(p,n)){header_.assign(p,p+n);return true;}
  const uint8_t *end=p+n;
  if(codec==1) {
   if(n<7 || p[0]!=1)return false;
   length_=(p[4]&3)+1;if(length_==3)return false;unsigned count=p[5]&31;p+=6;
   if(!Array(p,end,count) || p==end)return false;
   count=*p++;return Array(p,end,count);
  }
  if(n<23 || p[0]!=1)return false;
  length_=(p[21]&3)+1;if(length_==3)return false;unsigned arrays=p[22];p+=23;
  for(unsigned i=0;i<arrays;i++) {
   if(end-p<3)return false;
   unsigned count=p[1]*256+p[2];p+=3;
   if(!Array(p,end,count))return false;
  }
  return p==end;
 }
 bool Frame(const uint8_t *p,size_t n,bool first,std::vector<uint8_t> &out) const {
  out.clear();if(!p || !n || n>33554432)return false;
  if(first)out=header_;
  if((codec_==1 || codec_==3) && length_)return Nals(p,n,length_,out);
  if((codec_==1 || codec_==3) && !AnnexB(p,n))return false;
  out.insert(out.end(),p,p+n);return out.size()<=33554432;
 }
};
