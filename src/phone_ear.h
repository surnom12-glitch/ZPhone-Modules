// fix63: the phone's own ear, part 1: the sound bank.
// fix64: files are decoded on the bank's OWN background thread (below normal priority), never on
// the recorder's audio thread and never on the game thread; ear_load returns 4 = "busy, again later".
// Lua reports every sound the game starts (file, place, loudness). The file bytes come from
// Lua (file.Open in the GAME path: VPKs, addons, downloads), are decoded here once (FFmpeg:
// WAV PCM / MS-ADPCM / IMA-ADPCM, MP3, OGG Vorbis) to mono float at the file's own rate and
// kept for later recordings. The recorder (audio_capture.h) plays them at the phone.
#pragma once
#include <windows.h>
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <thread>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
namespace PhoneEar {
struct Clip{int rate=0;std::vector<float> pcm;int64_t loopStart=-1;double seconds()const{return rate?double(pcm.size())/rate:0.0;}};
using ClipPtr=std::shared_ptr<const Clip>;
enum State{Unknown=0,Decoding=1,Ready=2,Failed=3};
namespace detail{
struct Mem{const uint8_t* data;size_t size,pos;};
inline int memRead(void* o,uint8_t* buf,int n){auto m=static_cast<Mem*>(o);size_t left=m->size-m->pos;if(!left)return AVERROR_EOF;
 size_t k=std::min<size_t>(left,size_t(std::max(0,n)));std::memcpy(buf,m->data+m->pos,k);m->pos+=k;return int(k);}
inline int64_t memSeek(void* o,int64_t off,int whence){auto m=static_cast<Mem*>(o);if(whence&AVSEEK_SIZE)return int64_t(m->size);whence&=~AVSEEK_FORCE;
 int64_t p=whence==SEEK_SET?off:whence==SEEK_CUR?int64_t(m->pos)+off:whence==SEEK_END?int64_t(m->size)+off:-1;
 if(p<0||p>int64_t(m->size))return -1;m->pos=size_t(p);return p;}
inline uint32_t le32(const std::string& b,size_t at){uint32_t v;std::memcpy(&v,b.data()+at,4);return v;}
// Source loops a WAV that has a "cue " chunk: from its first cue point (dwSampleOffset) to the end
inline int64_t wavLoop(const std::string& b){
 if(b.size()<12||b.compare(0,4,"RIFF")!=0||b.compare(8,4,"WAVE")!=0)return -1;
 size_t p=12;
 while(p+8<=b.size()){uint32_t len=le32(b,p+4);
  if(b.compare(p,4,"cue ")==0&&len>=28&&p+8+28<=b.size()){if(le32(b,p+8)>=1)return int64_t(le32(b,p+8+4+20));return -1;}
  if(len>b.size())break;p+=8+size_t(len)+(len&1);}
 return -1;
}
inline float sampleOf(const uint8_t* p,int f){
 switch(f){
  case AV_SAMPLE_FMT_U8:case AV_SAMPLE_FMT_U8P:return (int(*p)-128)/128.f;
  case AV_SAMPLE_FMT_S16:case AV_SAMPLE_FMT_S16P:{int16_t v;std::memcpy(&v,p,2);return v/32768.f;}
  case AV_SAMPLE_FMT_S32:case AV_SAMPLE_FMT_S32P:{int32_t v;std::memcpy(&v,p,4);return float(v/2147483648.0);}
  case AV_SAMPLE_FMT_FLT:case AV_SAMPLE_FMT_FLTP:{float v;std::memcpy(&v,p,4);return v;}
  case AV_SAMPLE_FMT_DBL:case AV_SAMPLE_FMT_DBLP:{double v;std::memcpy(&v,p,8);return float(v);}
  case AV_SAMPLE_FMT_S64:case AV_SAMPLE_FMT_S64P:{int64_t v;std::memcpy(&v,p,8);return float(double(v)/9223372036854775808.0);}
  default:return 0.f;
 }
}
// whole frame -> mono, appended
inline bool appendMono(const AVFrame* fr,std::vector<float>& out,size_t cap){
 int f=fr->format,ch=std::max(1,fr->ch_layout.nb_channels),bps=av_get_bytes_per_sample(AVSampleFormat(f));if(bps<=0)return false;
 bool planar=av_sample_fmt_is_planar(AVSampleFormat(f))!=0;
 for(int i=0;i<fr->nb_samples&&out.size()<cap;++i){float s=0;
  for(int c=0;c<ch;++c){const uint8_t* p=planar?fr->extended_data[c]+size_t(i)*bps:fr->extended_data[0]+(size_t(i)*ch+c)*bps;s+=sampleOf(p,f);}
  out.push_back(s/ch);}
 return true;
}
}
// bytes of a sound file -> mono float clip (nullptr + reason when it is no usable sound)
inline ClipPtr decode(const std::string& bytes,std::string& why){
 using namespace detail;
 if(bytes.size()<16){why="too small";return nullptr;}
 Mem mem{reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size(),0};
 const int bufSize=1<<15;auto buf=static_cast<uint8_t*>(av_malloc(bufSize));if(!buf){why="memory";return nullptr;}
 AVIOContext* io=avio_alloc_context(buf,bufSize,0,&mem,memRead,nullptr,memSeek);if(!io){av_free(buf);why="memory";return nullptr;}
 AVFormatContext* fmt=avformat_alloc_context();AVCodecContext* dec=nullptr;AVPacket* pk=av_packet_alloc();AVFrame* fr=av_frame_alloc();
 auto clip=std::make_shared<Clip>();bool ok=false;
 do{
  if(!fmt||!pk||!fr){why="memory";break;}
  fmt->pb=io;fmt->flags|=AVFMT_FLAG_CUSTOM_IO;
  if(avformat_open_input(&fmt,nullptr,nullptr,nullptr)<0){fmt=nullptr;why="unknown format";break;} // freed by FFmpeg on failure
  if(avformat_find_stream_info(fmt,nullptr)<0){why="no stream info";break;}
  const AVCodec* codec=nullptr;int si=av_find_best_stream(fmt,AVMEDIA_TYPE_AUDIO,-1,-1,&codec,0);
  if(si<0||!codec){why="no audio stream / unsupported codec";break;}
  dec=avcodec_alloc_context3(codec);if(!dec||avcodec_parameters_to_context(dec,fmt->streams[si]->codecpar)<0||avcodec_open2(dec,codec,nullptr)<0){why="decoder failed";break;}
  int rate=dec->sample_rate>0?dec->sample_rate:fmt->streams[si]->codecpar->sample_rate;if(rate<4000||rate>192000){why="bad sample rate";break;}
  clip->rate=rate;const size_t cap=size_t(rate)*180; // 3 minutes at most
  bool bad=false;
  auto drainFrames=[&](){for(;;){int r=avcodec_receive_frame(dec,fr);if(r<0)break;if(!appendMono(fr,clip->pcm,cap))bad=true;av_frame_unref(fr);}};
  while(av_read_frame(fmt,pk)>=0){
   if(pk->stream_index==si&&avcodec_send_packet(dec,pk)>=0)drainFrames();
   av_packet_unref(pk);if(clip->pcm.size()>=cap||bad)break;
  }
  if(avcodec_send_packet(dec,nullptr)>=0)drainFrames();
  if(bad||clip->pcm.empty()){why=bad?"sample format":"no samples";break;}
  if(fmt->iformat&&fmt->iformat->name&&std::strstr(fmt->iformat->name,"wav")){int64_t l=wavLoop(bytes);if(l>=0&&l<int64_t(clip->pcm.size())-16)clip->loopStart=l;}
  ok=true;
 }while(false);
 av_frame_free(&fr);av_packet_free(&pk);avcodec_free_context(&dec);
 if(fmt)avformat_close_input(&fmt); // custom IO: the context below stays ours
 av_freep(&io->buffer);avio_context_free(&io);
 if(!ok)return nullptr;
 clip->pcm.shrink_to_fit();return clip;
}
// all decoded files of this game session (kept between recordings, oldest unused dropped first)
class Bank{
public:
 ~Bank(){ // static object: at process exit the thread is gone already; never join under the loader lock
  {std::lock_guard<std::mutex> lock(mutex_);stop_=true;}cv_.notify_all();if(thread_.joinable())thread_.detach();
 }
 // 1 queued (or decoding), 2 already known, 3 refused (bad / known bad), 4 busy (queue full: send again later)
 int add(const std::string& key,std::string bytes){
  {std::lock_guard<std::mutex> lock(mutex_);auto& e=map_[key];
   if(e.state==Ready||e.state==Decoding)return e.state==Ready?2:1;
   if(e.state==Failed)return 3;
   if(key.empty()||bytes.size()<16||bytes.size()>MaxFile){map_.erase(key);return 3;}
   if(queuedBytes_+bytes.size()>MaxQueue){map_.erase(key);return 4;}
   e.state=Decoding;queuedBytes_+=bytes.size();queue_.push_back({key,std::move(bytes)});
   if(!thread_.joinable()){stop_=false;thread_=std::thread([this]{loop();});}
  }
  cv_.notify_one();return 1;
 }
 int state(const std::string& key,double* seconds=nullptr,bool* loop=nullptr){
  std::lock_guard<std::mutex> lock(mutex_);auto it=map_.find(key);if(it==map_.end())return Unknown;
  if(it->second.clip){if(seconds)*seconds=it->second.clip->seconds();if(loop)*loop=it->second.clip->loopStart>=0;it->second.used=++tick_;}
  return it->second.state;
 }
 ClipPtr find(const std::string& key){std::lock_guard<std::mutex> lock(mutex_);auto it=map_.find(key);if(it==map_.end()||!it->second.clip)return nullptr;it->second.used=++tick_;return it->second.clip;}
 void stats(int& ready,int& failed,int& queued,double& megabytes){std::lock_guard<std::mutex> lock(mutex_);ready=ready_;failed=failed_;queued=int(queue_.size());megabytes=double(decoded_)/1048576.0;}
 // module unload: the decoder thread ends (an unfinished file is dropped), then everything is forgotten
 void shutdown(){
  {std::lock_guard<std::mutex> lock(mutex_);stop_=true;queue_.clear();queuedBytes_=0;}cv_.notify_all();
  if(thread_.joinable())thread_.join();
  std::lock_guard<std::mutex> lock(mutex_);map_.clear();decoded_=0;ready_=failed_=0;
 }
 void clear(){std::lock_guard<std::mutex> lock(mutex_);map_.clear();queue_.clear();queuedBytes_=0;decoded_=0;ready_=failed_=0;}
private:
 static constexpr size_t MaxFile=16u<<20,MaxQueue=48u<<20,MaxDecoded=192u<<20;
 struct Entry{int state=Unknown;ClipPtr clip;uint64_t used=0;};
 struct Job{std::string key,bytes;};
 std::mutex mutex_;std::condition_variable cv_;std::thread thread_;bool stop_=false;
 std::unordered_map<std::string,Entry> map_;std::deque<Job> queue_;size_t queuedBytes_=0,decoded_=0;uint64_t tick_=0;int ready_=0,failed_=0;
 void loop(){
  SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
  for(;;){
   Job job;
   {std::unique_lock<std::mutex> lock(mutex_);cv_.wait(lock,[&]{return stop_||!queue_.empty();});if(stop_)return;
    job=std::move(queue_.front());queue_.pop_front();queuedBytes_-=std::min(queuedBytes_,job.bytes.size());}
   std::string why;ClipPtr clip;
   try{clip=decode(job.bytes,why);}catch(...){clip=nullptr;}
   std::lock_guard<std::mutex> lock(mutex_);if(stop_)return;
   auto it=map_.find(job.key);if(it==map_.end())continue; // forgotten meanwhile (clear)
   auto& e=it->second;
   if(clip){e.clip=clip;e.state=Ready;e.used=++tick_;decoded_+=clip->pcm.size()*sizeof(float);++ready_;trim();}
   else{e.state=Failed;++failed_;}
  }
 }
 void trim(){ // under the lock: drop the least recently used clips nobody plays
  while(decoded_>MaxDecoded){
   auto victim=map_.end();
   for(auto it=map_.begin();it!=map_.end();++it)if(it->second.clip&&it->second.clip.use_count()==1&&(victim==map_.end()||it->second.used<victim->second.used))victim=it;
   if(victim==map_.end())return;
   decoded_-=std::min(decoded_,victim->second.clip->pcm.size()*sizeof(float));--ready_;map_.erase(victim);
  }
 }
};
inline Bank& bank(){static Bank b;return b;}
}
