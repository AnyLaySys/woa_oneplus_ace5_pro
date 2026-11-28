#pragma once
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

/* Convert DXVA's picture parameters and slice NALs to the Annex B stream
   consumed by Iris. References use explicit RPS; CABAC bytes stay intact. */
class HevcStream {
 struct Invalid {};
 struct Bits {
  std::vector<uint8_t> data;size_t pos=0;
  uint32_t Read(unsigned n) {
   if(n>32 || pos>data.size()*8 || n>data.size()*8-pos)throw Invalid{};
   uint32_t value=0;while(n--){value=(value<<1)|((data[pos/8]>>(7-pos%8))&1);pos++;}return value;
  }
  void Skip(size_t n){if(pos>data.size()*8 || n>data.size()*8-pos)throw Invalid{};pos+=n;}
  uint32_t Ue(){unsigned n=0;while(!Read(1))if(++n>31)throw Invalid{};return ((1u<<n)-1)+Read(n);}
  void Se(){Ue();}
  void Put(uint64_t value,unsigned n){if(n>64)throw Invalid{};while(n--){if(!(pos%8))data.push_back(0);data[pos/8]|=((value>>n)&1)<<(7-pos%8);pos++;}}
  void Ue(uint32_t value){uint64_t v=(uint64_t)value+1;unsigned n=0;for(uint64_t t=v;t>>=1;)n++;Put(0,n);Put(v,n+1);}
  void Se(int32_t value){int64_t v=value;uint64_t code=v<=0?-v*2:v*2-1;if(code>UINT32_MAX)throw Invalid{};Ue((uint32_t)code);}
  void Align(){Put(1,1);while(pos%8)Put(0,1);}
  void Copy(const Bits &source,size_t begin,size_t end){if(end<begin || end>source.data.size()*8)throw Invalid{};for(size_t i=begin;i<end;i++)Put((source.data[i/8]>>(7-i%8))&1,1);}
 };
 struct Ref {int poc;bool used;};
 static unsigned Log2(unsigned n){unsigned bits=0;for(unsigned t=n?n-1:0;t;t>>=1)bits++;return bits;}
 static bool Used(const uint8_t *list,unsigned index){for(unsigned i=0;i<8;i++)if(list[i]==index)return true;return false;}
 static void Nal(std::vector<uint8_t> &out,unsigned type,const Bits &bits,unsigned temporal=1) {
  out.reserve(out.size()+6+bits.data.size()+bits.data.size()/2);
  out.insert(out.end(),{0,0,0,1,(uint8_t)(type<<1),(uint8_t)temporal});unsigned zeros=0;
  for(size_t i=0;i<bits.data.size();i++){
   if(!zeros){
    auto *zero=(const uint8_t *)memchr(bits.data.data()+i,0,bits.data.size()-i);
    size_t end=zero?(size_t)(zero-bits.data.data()):bits.data.size();
    out.insert(out.end(),bits.data.data()+i,bits.data.data()+end);i=end;if(i==bits.data.size())break;
   }
   uint8_t byte=bits.data[i];if(zeros==2 && byte<=3){out.push_back(3);zeros=0;}out.push_back(byte);zeros=byte?0:zeros+1;
  }
 }
 static Bits Rbsp(const uint8_t *data,size_t size) {
  Bits b;b.data.resize(size);size_t used=0;unsigned zeros=0;
  for(size_t i=0;i<size;i++){
   if(!zeros){
    auto *zero=(const uint8_t *)memchr(data+i,0,size-i);size_t end=zero?(size_t)(zero-data):size;
    memcpy(b.data.data()+used,data+i,end-i);used+=end-i;i=end;if(i==size)break;
   }
   uint8_t v=data[i];if(zeros==2 && v==3){if(i+1>=size || data[i+1]>3)throw Invalid{};zeros=0;continue;}
   b.data[used++]=v;zeros=v?0:zeros+1;
  }b.data.resize(used);return b;
 }
 static void Profile(Bits &b,bool ten) {
  unsigned profile=ten?2:1;
  b.Put(0,2);b.Put(1,1);b.Put(profile,5);b.Put(1u<<(31-profile),32);
  b.Put(1,1);b.Put(0,1);b.Put(0,1);b.Put(1,1);b.Put(0,44);b.Put(186,8);
  for(unsigned i=0;i<6;i++){b.Put(0,1);b.Put(0,1);}for(unsigned i=6;i<8;i++)b.Put(0,2);
 }
 static void Order(Bits &b,const DXVA_PicParams_HEVC &p){b.Put(0,1);b.Ue(p.sps_max_dec_pic_buffering_minus1);b.Ue(p.NoPicReorderingFlag?0:p.sps_max_dec_pic_buffering_minus1);b.Ue(0);}
 static void Scaling(Bits &b,const DXVA_Qmatrix_HEVC &q) {
  for(unsigned size=0;size<4;size++)for(unsigned matrix=0;matrix<6;matrix+=size==3?3:1){
   b.Put(1,1);int next=8;
   if(size>1){next=size==2?q.ucScalingListDCCoefSizeID2[matrix]:q.ucScalingListDCCoefSizeID3[matrix/3];b.Se(next-8);}
   const uint8_t *coeff=size==0?q.ucScalingLists0[matrix]:size==1?q.ucScalingLists1[matrix]:size==2?q.ucScalingLists2[matrix]:q.ucScalingLists3[matrix/3];
   for(unsigned i=0;i<(size?64u:16u);i++){int delta=(coeff[i]-next+256)%256;if(delta>127)delta-=256;b.Se(delta);next=coeff[i];}
  }
 }
 static void Headers(std::vector<uint8_t> &out,const DXVA_PicParams_HEVC &p,const DXVA_Qmatrix_HEVC &q,unsigned id,unsigned width,unsigned height) {
  unsigned cw=(unsigned)p.PicWidthInMinCbsY<<(p.log2_min_luma_coding_block_size_minus3+3),ch=(unsigned)p.PicHeightInMinCbsY<<(p.log2_min_luma_coding_block_size_minus3+3);
  if(cw<width || ch<height || ((cw-width)|(ch-height))&1)throw Invalid{};
  Bits v;v.Put(0,4);v.Put(1,1);v.Put(1,1);v.Put(0,6);v.Put(6,3);v.Put(1,1);v.Put(0xffff,16);Profile(v,p.bit_depth_luma_minus8!=0);Order(v,p);v.Put(0,6);v.Ue(0);v.Put(0,1);v.Put(0,1);v.Align();Nal(out,32,v);
  Bits s;s.Put(0,4);s.Put(6,3);s.Put(1,1);Profile(s,p.bit_depth_luma_minus8!=0);
  s.Ue(0);s.Ue(1);s.Ue(cw);s.Ue(ch);bool crop=cw!=width || ch!=height;s.Put(crop,1);
  if(crop){s.Ue(0);s.Ue((cw-width)/2);s.Ue(0);s.Ue((ch-height)/2);}
  s.Ue(p.bit_depth_luma_minus8);s.Ue(p.bit_depth_chroma_minus8);s.Ue(p.log2_max_pic_order_cnt_lsb_minus4);Order(s,p);
  s.Ue(p.log2_min_luma_coding_block_size_minus3);s.Ue(p.log2_diff_max_min_luma_coding_block_size);s.Ue(p.log2_min_transform_block_size_minus2);s.Ue(p.log2_diff_max_min_transform_block_size);s.Ue(p.max_transform_hierarchy_depth_inter);s.Ue(p.max_transform_hierarchy_depth_intra);
  s.Put(p.scaling_list_enabled_flag,1);if(p.scaling_list_enabled_flag){s.Put(1,1);Scaling(s,q);}
  s.Put(p.amp_enabled_flag,1);s.Put(p.sample_adaptive_offset_enabled_flag,1);s.Put(p.pcm_enabled_flag,1);
  if(p.pcm_enabled_flag){s.Put(p.pcm_sample_bit_depth_luma_minus1,4);s.Put(p.pcm_sample_bit_depth_chroma_minus1,4);s.Ue(p.log2_min_pcm_luma_coding_block_size_minus3);s.Ue(p.log2_diff_max_min_pcm_luma_coding_block_size);s.Put(p.pcm_loop_filter_disabled_flag,1);}
  s.Ue(0);s.Put(p.long_term_ref_pics_present_flag,1);if(p.long_term_ref_pics_present_flag)s.Ue(0);
  s.Put(p.sps_temporal_mvp_enabled_flag,1);s.Put(p.strong_intra_smoothing_enabled_flag,1);s.Put(0,1);s.Put(0,1);s.Align();Nal(out,33,s);
  Bits b;b.Ue(id);b.Ue(0);b.Put(p.dependent_slice_segments_enabled_flag,1);b.Put(p.output_flag_present_flag,1);b.Put(p.num_extra_slice_header_bits,3);
  b.Put(p.sign_data_hiding_enabled_flag,1);b.Put(p.cabac_init_present_flag,1);b.Ue(p.num_ref_idx_l0_default_active_minus1);b.Ue(p.num_ref_idx_l1_default_active_minus1);b.Se(p.init_qp_minus26);
  b.Put(p.constrained_intra_pred_flag,1);b.Put(p.transform_skip_enabled_flag,1);b.Put(p.cu_qp_delta_enabled_flag,1);if(p.cu_qp_delta_enabled_flag)b.Ue(p.diff_cu_qp_delta_depth);
  b.Se(p.pps_cb_qp_offset);b.Se(p.pps_cr_qp_offset);b.Put(p.pps_slice_chroma_qp_offsets_present_flag,1);b.Put(p.weighted_pred_flag,1);b.Put(p.weighted_bipred_flag,1);b.Put(p.transquant_bypass_enabled_flag,1);b.Put(p.tiles_enabled_flag,1);b.Put(p.entropy_coding_sync_enabled_flag,1);
  if(p.tiles_enabled_flag){b.Ue(p.num_tile_columns_minus1);b.Ue(p.num_tile_rows_minus1);b.Put(p.uniform_spacing_flag,1);if(!p.uniform_spacing_flag){for(unsigned i=0;i<p.num_tile_columns_minus1;i++)b.Ue(p.column_width_minus1[i]);for(unsigned i=0;i<p.num_tile_rows_minus1;i++)b.Ue(p.row_height_minus1[i]);}b.Put(p.loop_filter_across_tiles_enabled_flag,1);}
  b.Put(p.pps_loop_filter_across_slices_enabled_flag,1);b.Put(1,1);b.Put(p.deblocking_filter_override_enabled_flag,1);b.Put(p.pps_deblocking_filter_disabled_flag,1);if(!p.pps_deblocking_filter_disabled_flag){b.Se(p.pps_beta_offset_div2);b.Se(p.pps_tc_offset_div2);}
  b.Put(0,1);b.Put(p.lists_modification_present_flag,1);b.Ue(p.log2_parallel_merge_level_minus2);b.Put(p.slice_segment_header_extension_present_flag,1);b.Put(0,1);b.Align();Nal(out,34,b);
 }
 static void ShortRefs(Bits &b,const DXVA_PicParams_HEVC &p) {
  std::vector<Ref> negative,positive;
  for(unsigned i=0;i<15;i++)if(p.RefPicList[i].bPicEntry!=0xff && !p.RefPicList[i].AssociatedFlag){
   int64_t delta=(int64_t)p.PicOrderCntValList[i]-p.CurrPicOrderCntVal;if(!delta || delta<INT32_MIN || delta>INT32_MAX)throw Invalid{};
   (delta<0?negative:positive).push_back({(int)delta,Used(p.RefPicSetStCurrBefore,i)||Used(p.RefPicSetStCurrAfter,i)});
  }
  std::sort(negative.begin(),negative.end(),[](auto a,auto c){return a.poc>c.poc;});std::sort(positive.begin(),positive.end(),[](auto a,auto c){return a.poc<c.poc;});
  b.Ue((uint32_t)negative.size());b.Ue((uint32_t)positive.size());
  int64_t prev=0;for(auto r:negative){b.Ue((uint32_t)(prev-r.poc-1));b.Put(r.used,1);prev=r.poc;}
  prev=0;for(auto r:positive){b.Ue((uint32_t)(r.poc-prev-1));b.Put(r.used,1);prev=r.poc;}
 }
 static void LongRefs(Bits &b,const DXVA_PicParams_HEVC &p) {
  unsigned bits=p.log2_max_pic_order_cnt_lsb_minus4+4,mask=(1u<<bits)-1;
  struct Long{int poc;unsigned cycle;bool used;};std::vector<Long> refs;
  for(unsigned i=0;i<15;i++)if(p.RefPicList[i].bPicEntry!=0xff && p.RefPicList[i].AssociatedFlag){
   int64_t delta=((int64_t)p.CurrPicOrderCntVal-(p.CurrPicOrderCntVal&mask))-((int64_t)p.PicOrderCntValList[i]-(p.PicOrderCntValList[i]&mask));
   if(delta<0 || delta>>bits>UINT32_MAX)throw Invalid{};
   refs.push_back({p.PicOrderCntValList[i],(unsigned)(delta>>bits),Used(p.RefPicSetLtCurr,i)});
  }
  std::stable_sort(refs.begin(),refs.end(),[](auto a,auto c){return a.cycle<c.cycle;});b.Ue((uint32_t)refs.size());
  unsigned prev=0;for(auto r:refs){b.Put(r.poc&mask,bits);b.Put(r.used,1);b.Put(1,1);b.Ue(r.cycle-prev);prev=r.cycle;}
 }
 static unsigned CurrentRefs(const DXVA_PicParams_HEVC &p){unsigned count=0;for(unsigned i=0;i<8;i++){count+=p.RefPicSetStCurrBefore[i]!=0xff;count+=p.RefPicSetStCurrAfter[i]!=0xff;count+=p.RefPicSetLtCurr[i]!=0xff;}return count;}
 static void Weights(Bits &b,unsigned l0,unsigned l1,bool bi) {
  b.Ue();b.Se();
  for(unsigned list=0;list<(bi?2u:1u);list++){
   unsigned n=list?l1:l0;bool luma[16]{},chroma[16]{};if(n>16)throw Invalid{};
   for(unsigned i=0;i<n;i++)luma[i]=b.Read(1)!=0;
   for(unsigned i=0;i<n;i++)chroma[i]=b.Read(1)!=0;
   for(unsigned i=0;i<n;i++){if(luma[i]){b.Se();b.Se();}if(chroma[i])for(unsigned j=0;j<4;j++)b.Se();}
  }
 }
 static void Slice(std::vector<uint8_t> &out,const DXVA_PicParams_HEVC &p,const DXVA_Qmatrix_HEVC &q,const uint8_t *data,size_t size,unsigned width,unsigned height,bool first) {
  if(size<3 || size>33554432 || (data[0]&0x81) || (data[1]&0xf8) || !(data[1]&7))throw Invalid{};
  unsigned nal=data[0]>>1;if(nal>31)throw Invalid{};
  Bits b=Rbsp(data+2,size-2);bool start=b.Read(1)!=0;if(nal>=16 && nal<=23)b.Skip(1);unsigned id=b.Ue();if(id>63)throw Invalid{};
  if(first){if(!start)throw Invalid{};Headers(out,p,q,id,width,height);}
  bool dependent=false;
  if(!start){if(p.dependent_slice_segments_enabled_flag)dependent=b.Read(1)!=0;unsigned shift=p.log2_min_luma_coding_block_size_minus3+3+p.log2_diff_max_min_luma_coding_block_size;
   unsigned cw=(unsigned)p.PicWidthInMinCbsY<<(p.log2_min_luma_coding_block_size_minus3+3),ch=(unsigned)p.PicHeightInMinCbsY<<(p.log2_min_luma_coding_block_size_minus3+3);
   b.Skip(Log2(((cw+((1u<<shift)-1))>>shift)*((ch+((1u<<shift)-1))>>shift)));
  }
  if(dependent || nal==19 || nal==20){out.insert(out.end(),{0,0,0,1});out.insert(out.end(),data,data+size);return;}
  b.Skip(p.num_extra_slice_header_bits);unsigned type=b.Ue();if(type>2)throw Invalid{};
  if(p.output_flag_present_flag)b.Skip(1);
  b.Skip(p.log2_max_pic_order_cnt_lsb_minus4+4);size_t begin=b.pos;
  bool sps=b.Read(1)!=0;
  if(sps){if(!p.num_short_term_ref_pic_sets)throw Invalid{};b.Skip(Log2(p.num_short_term_ref_pic_sets));}
  else b.Skip(p.wNumBitsForShortTermRPSInSlice);
  if(p.long_term_ref_pics_present_flag){unsigned ns=p.num_long_term_ref_pics_sps?b.Ue():0,ni=b.Ue();if(ns>p.num_long_term_ref_pics_sps || ns+ni>32)throw Invalid{};
   for(unsigned i=0;i<ns+ni;i++){if(i<ns)b.Skip(Log2(p.num_long_term_ref_pics_sps));else b.Skip(p.log2_max_pic_order_cnt_lsb_minus4+5);if(b.Read(1))b.Ue();}
  }
  size_t end=b.pos;bool temporal=p.sps_temporal_mvp_enabled_flag && b.Read(1);bool saoL=false,saoC=false;
  if(p.sample_adaptive_offset_enabled_flag){saoL=b.Read(1)!=0;saoC=b.Read(1)!=0;}
  if(type!=2){unsigned l0=p.num_ref_idx_l0_default_active_minus1+1,l1=type==0?p.num_ref_idx_l1_default_active_minus1+1:0;
   if(b.Read(1)){l0=b.Ue()+1;if(type==0)l1=b.Ue()+1;}if(l0>15 || l1>15)throw Invalid{};
   unsigned refs=CurrentRefs(p);if(!refs || refs>15)throw Invalid{};
   if(p.lists_modification_present_flag && refs>1){if(b.Read(1))b.Skip((size_t)l0*Log2(refs));if(type==0 && b.Read(1))b.Skip((size_t)l1*Log2(refs));}
   if(type==0)b.Skip(1);
   if(p.cabac_init_present_flag)b.Skip(1);
   if(temporal){bool from0=type!=0 || b.Read(1);if((from0?l0:l1)>1)b.Ue();}
   if((p.weighted_pred_flag && type==1) || (p.weighted_bipred_flag && type==0))Weights(b,l0,l1,type==0);
   b.Ue();
  }
  b.Se();if(p.pps_slice_chroma_qp_offsets_present_flag){b.Se();b.Se();}
  bool disabled=p.pps_deblocking_filter_disabled_flag!=0;
  if(p.deblocking_filter_override_enabled_flag && b.Read(1)){disabled=b.Read(1)!=0;if(!disabled){b.Se();b.Se();}}
  if(p.pps_loop_filter_across_slices_enabled_flag && (saoL || saoC || !disabled))b.Skip(1);
  if(p.tiles_enabled_flag || p.entropy_coding_sync_enabled_flag){unsigned n=b.Ue();if(n){unsigned bits=b.Ue()+1;if(bits>32)throw Invalid{};b.Skip((size_t)n*bits);}}
  if(p.slice_segment_header_extension_present_flag)b.Skip((size_t)b.Ue()*8);
  size_t header=b.pos;if(!b.Read(1))throw Invalid{};while(b.pos%8)if(b.Read(1))throw Invalid{};
  Bits changed;changed.Copy(b,0,begin);changed.Put(0,1);ShortRefs(changed,p);if(p.long_term_ref_pics_present_flag)LongRefs(changed,p);
  changed.Copy(b,end,header);changed.Align();changed.data.insert(changed.data.end(),b.data.begin()+b.pos/8,b.data.end());Nal(out,nal,changed,data[1]&7);
 }
public:
 static bool Append(std::vector<uint8_t> &out,const DXVA_PicParams_HEVC &p,const DXVA_Qmatrix_HEVC &q,const uint8_t *data,size_t size,unsigned width,unsigned height,bool first) {
  if(!data || !width || !height || p.chroma_format_idc!=1 || p.separate_colour_plane_flag ||
     (p.bit_depth_luma_minus8!=0 && p.bit_depth_luma_minus8!=2) || p.bit_depth_chroma_minus8!=p.bit_depth_luma_minus8 ||
     p.log2_min_luma_coding_block_size_minus3>3 || p.log2_diff_max_min_luma_coding_block_size>3 || p.log2_min_luma_coding_block_size_minus3+p.log2_diff_max_min_luma_coding_block_size>3 ||
     p.sps_max_dec_pic_buffering_minus1>15 || p.num_tile_columns_minus1>19 || p.num_tile_rows_minus1>21)return false;
  size_t before=out.size();try{Slice(out,p,q,data,size,width,height,first);return out.size()<=33554432;}catch(const Invalid &){out.resize(before);return false;}
 }
};
