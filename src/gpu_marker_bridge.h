#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
#include <utility>
#include <stdexcept>
namespace PhoneGPU {
inline uint32_t markerColor(uint32_t n){return ((8+16*((n>>8)&15))<<16)|((8+16*((n>>4)&15))<<8)|(8+16*(n&15));}
inline bool matches(uint32_t a,uint32_t b){for(int shift=0;shift<24;shift+=8){int d=int((a>>shift)&255)-int((b>>shift)&255);if(d<-1||d>1)return false;}return true;}
// Capacity is bounded by GPU slots. Unlike std::deque this does not allocate
// and free a block every ~32 frames (roughly one second at 30 FPS).
// External mutex protects CPU queues; image queue stays render-thread only.
template<class T,size_t Limit> class Ring {
 std::array<T,Limit> data{};size_t head=0,used=0;
 public:
 size_t size()const{return used;}bool empty()const{return used==0;}
 T& operator[](size_t i){return data[(head+i)%Limit];}
 T& front(){return (*this)[0];}
 void push_back(const T& v){if(used==Limit)throw std::overflow_error("fixed capture queue overflow");data[(head+used)%Limit]=v;++used;}
 void pop_front(){if(used){head=(head+1)%Limit;--used;}}
 void erase(size_t index){for(size_t n=index+1;n<used;++n)(*this)[n-1]=(*this)[n];--used;}
};
// Owns exact phone surfaces, never the backbuffer. Zero heap allocation.
template<class Surface,size_t Limit> class Images {
 struct Image {
  Surface* source=nullptr;int64_t pts=0;
  Image()=default;Image(const Image&)=delete;
  Image(Image&& b)noexcept:source(b.source),pts(b.pts){b.source=nullptr;}
  ~Image(){if(source)source->Release();}
 };
 Surface* previous=nullptr;std::array<Image,Limit> queue{};size_t head=0,used=0;
 public:
 ~Images(){clear();}
 void remember(Surface* source){if(source)source->AddRef();if(previous)previous->Release();previous=source;}
 int commit(int64_t pts){
  if(!previous)return 0;
  for(size_t i=0;i<used;++i){auto& image=queue[(head+i)%Limit];if(image.source==previous){image.pts=pts;remember(nullptr);return 2;}}
  if(used==Limit){remember(nullptr);return 3;}
  auto& image=queue[(head+used)%Limit];image.source=previous;image.pts=pts;previous=nullptr;++used;return 1;
 }
 template<class F>void drain(F submit){while(used){Image image=std::move(queue[head]);head=(head+1)%Limit;--used;submit(image.source,image.pts);}}
 bool empty()const{return used==0;}
 void clear(){remember(nullptr);while(used){auto& image=queue[head];if(image.source){image.source->Release();image.source=nullptr;}head=(head+1)%Limit;--used;}}
};
}
