// fix62: real phone-clip audio. Microphone (default Windows capture device) plus
// the sound of THIS game process only (Windows 10 2004+ process loopback;
// optional whole-device loopback fallback). Mixed to 48 kHz mono, encoded to
// AAC for the MP4 and kept as a 24 kHz PCM WAV for in-game playback.
// Everything runs on its own thread; the render/Lua thread only stores gains.
// fix63: + the phone's own ear (phone_ear.h): game sounds re-played at the phone's position
// (gain / low-pass / pitch per sound from Lua), mixed here sample-accurately by start time.
#pragma once
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}
#include "phone_ear.h"
namespace PhoneAudio {
namespace fs=std::filesystem;
constexpr int Rate=48000;          // mix / AAC rate
constexpr int WavRate=24000;       // in-game playback copy (2:1)
constexpr int64_t RingSize=Rate*12;
enum Flags{Mic=1,Game=2,SystemFallback=4,Ear=8}; // Ear: no device, the sounds come from Lua
inline int64_t now100(){
 LARGE_INTEGER c,f;QueryPerformanceCounter(&c);QueryPerformanceFrequency(&f);
 return (c.QuadPart/f.QuadPart)*10000000LL+(c.QuadPart%f.QuadPart)*10000000LL/f.QuadPart;
}
// {94ea2b94-e9cc-49e0-c0ff-ee64ca8f5b90}: marker that the handler is free-threaded
static const GUID AgileIID={0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
struct LoopbackParams{DWORD pid;int mode;};          // AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS
struct ActivationParams{int type;LoopbackParams loop;}; // AUDIOCLIENT_ACTIVATION_PARAMS
class Completion final:public IActivateAudioInterfaceCompletionHandler{
 std::atomic<ULONG> refs{1};
public:
 HANDLE done=CreateEventW(nullptr,TRUE,FALSE,nullptr);
 ~Completion(){if(done)CloseHandle(done);}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{
  if(!out)return E_POINTER;
  if(IsEqualIID(id,__uuidof(IUnknown))||IsEqualIID(id,__uuidof(IActivateAudioInterfaceCompletionHandler))||IsEqualIID(id,AgileIID)){*out=this;AddRef();return S_OK;}
  *out=nullptr;return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
 ULONG STDMETHODCALLTYPE Release()override{ULONG n=--refs;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*)override{SetEvent(done);return S_OK;}
};
template<class T>inline void drop(T*& p){if(p){p->Release();p=nullptr;}}
struct Source{
 const char* name="";IAudioClient* client=nullptr;IAudioCaptureClient* capture=nullptr;HANDLE event=nullptr;
 int channels=0,rate=0,bytes=0,kind=0,block=0; // kind 0 int16, 1 int24, 2 int32, 3 float32
 int64_t next=-1;double phase=0;float previous=0;bool primed=false;
 float gain=0;std::vector<float> mono,out;
 void close(){if(client)client->Stop();drop(capture);drop(client);if(event){CloseHandle(event);event=nullptr;}next=-1;primed=false;}
};
struct Stats{int mic=0,game=0;double samples=0,micPeak=0,gamePeak=0;bool wav=false,aac=false;std::string error;
 int earVoices=0,earPlayed=0;double earPeak=0;};
// fix63: one game sound heard by the phone
struct Voice{
 int id=0;std::string key;PhoneEar::ClipPtr clip;int64_t at=0,born=0;int64_t pos=-1;
 double cursor=0,step=1;float gain=0,gainT=0,a=1,aT=1,pitch=1,pitchT=1,fade=1,y1=0,y2=0;bool loop=true,stop=false;
};
struct EarCommand{int kind=0;int id=0;std::string key;float gain=0,lp=20000,pitch=1;bool loop=true;int64_t at=0,born=0;};
inline float lowpassCoef(float hz){if(!(hz<19000.f))return 1.f;hz=std::max(150.f,hz);return 1.f-std::exp(-6.2831853f*hz/float(Rate));}
// 4-point Hermite; past the end: the loop (or silence)
inline float hermite(const std::vector<float>& p,double c,int64_t loopStart){
 int64_t n=int64_t(p.size());int64_t i=int64_t(std::floor(c));float t=float(c-double(i));
 auto at=[&](int64_t k)->float{if(k<0)return 0.f;if(k>=n){if(loopStart<0||loopStart>=n)return 0.f;k=loopStart+(k-n)%(n-loopStart);}return p[size_t(k)];};
 float xm1=at(i-1),x0=at(i),x1=at(i+1),x2=at(i+2);
 float c1=0.5f*(x1-xm1),c2=xm1-2.5f*x0+2.f*x1-0.5f*x2,c3=0.5f*(x2-xm1)+1.5f*(x0-x1);
 return ((c3*t+c2)*t+c1)*t+x0;
}
class Recorder{
public:
 Recorder(int flags,fs::path wavPath):flags_(flags),wav_(std::move(wavPath)){
  lastControl_=now100();
  openEncoder();
  thread_=std::thread([this]{run();});
 }
 ~Recorder(){cancel();if(thread_.joinable())thread_.join();if(wakeEvent_)CloseHandle(wakeEvent_);
  for(auto p:packets_)av_packet_free(&p);packets_.clear();
  av_packet_free(&packet_);av_frame_free(&frame_);avcodec_free_context(&aac_);avcodec_parameters_free(&params_);}
 Recorder(const Recorder&)=delete;Recorder& operator=(const Recorder&)=delete;
 const AVCodecParameters* params()const{return params_;}
 // gains from Lua, every rendered frame while filming
 void control(double mic,double game,double muffle,double ear=0){
  micGain_=float(std::min(4.0,std::max(0.0,mic)));gameGain_=float(std::min(4.0,std::max(0.0,game)));
  muffle_=float(std::min(1.0,std::max(0.0,muffle)));earGain_=float(std::min(4.0,std::max(0.0,ear)));lastControl_=now100();
 }
 // fix63: the phone's ear. play = the sound started now (its file in PhoneEar::bank());
 // gain / low-pass (Hz) / pitch (1 = normal) follow the phone<->source geometry from Lua
 // fix64: age = how long ago (s) the game started it (its file was still being read): it is placed
 // where it began, the part already mixed is skipped, the rest stays in sync with the picture
 void earPlay(int id,const std::string& key,double gain,double lp,double pitch,bool loop,double age=0){
  if(!std::isfinite(age)||age<0)age=0;age=std::min(age,30.0);
  EarCommand c;c.kind=0;c.id=id;c.key=key;c.gain=clampGain(gain);c.lp=float(lp);c.pitch=clampPitch(pitch);c.loop=loop;c.born=now100();c.at=c.born-int64_t(age*1e7);
  std::lock_guard<std::mutex> lock(earMutex_);if(earCommands_.size()<4096)earCommands_.push_back(std::move(c));
 }
 void earSet(int id,double gain,double lp,double pitch){
  EarCommand c;c.kind=1;c.id=id;c.gain=clampGain(gain);c.lp=float(lp);c.pitch=clampPitch(pitch);
  std::lock_guard<std::mutex> lock(earMutex_);if(earCommands_.size()<4096)earCommands_.push_back(std::move(c));
 }
 void earStop(int id){EarCommand c;c.kind=2;c.id=id;std::lock_guard<std::mutex> lock(earMutex_);if(earCommands_.size()<4096)earCommands_.push_back(std::move(c));}
 // QPC time (100 ns) of video pts 0; set once by the first capture() call
 void zero(int64_t at){int64_t none=INT64_MIN;zero_.compare_exchange_strong(none,at);}
 // latest video timestamp handed to capture(): AAC is encoded up to 1 s before it,
 // the rest after finish() once the exact video length is known
 void seen(double t){int64_t v=int64_t(t*Rate);int64_t old=seen_.load();while(v>old&&!seen_.compare_exchange_weak(old,v)){}}
 // video length in 90 kHz units: the clip audio is cut / padded to exactly that
 void finish(int64_t end90k){end90k_=end90k;finishing_=true;wake();}
 void cancel(){cancelled_=true;wake();}
 bool waitDone(unsigned ms){std::unique_lock<std::mutex> lock(mutex_);return cv_.wait_for(lock,std::chrono::milliseconds(ms),[&]{return done_;});}
 bool done(){std::lock_guard<std::mutex> lock(mutex_);return done_;}
 AVPacket* pop(){std::lock_guard<std::mutex> lock(mutex_);if(packets_.empty())return nullptr;auto p=packets_.front();packets_.pop_front();return p;}
 Stats stats(){Stats s;s.mic=micState_;s.game=gameState_;s.samples=double(emitted_.load());s.micPeak=micPeak_;s.gamePeak=gamePeak_;s.wav=wavReady_;s.aac=params_!=nullptr;
  s.earVoices=earVoices_;s.earPlayed=earPlayed_;s.earPeak=earPeak_;
  std::lock_guard<std::mutex> lock(mutex_);s.error=error_;return s;}
private:
 int flags_;fs::path wav_;std::thread thread_;std::mutex mutex_;std::condition_variable cv_;bool done_=false;std::string error_;
 std::atomic<float> micGain_{0},gameGain_{0},muffle_{0},micPeak_{0},gamePeak_{0},earGain_{0},earPeak_{0};
 std::atomic<int> earVoices_{0},earPlayed_{0};std::mutex earMutex_;std::vector<EarCommand> earCommands_;std::vector<Voice> voices_;
 float earFrom_=0;int64_t earMark_=-1;float hp_=0;
 static float clampGain(double g){return float(std::min(4.0,std::max(0.0,std::isfinite(g)?g:0.0)));}
 static float clampPitch(double p){return float(std::min(4.0,std::max(0.1,std::isfinite(p)?p:1.0)));}
 std::atomic<int64_t> zero_{INT64_MIN},end90k_{-1},lastControl_{0},emitted_{0},seen_{0};
 std::atomic<bool> finishing_{false},cancelled_{false},wavReady_{false};std::atomic<int> micState_{0},gameState_{0};
 HANDLE wakeEvent_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
 Source mic_,game_;int64_t base_=0,consumed_=0,zeroPos_=0,endPos_=-1;bool zeroKnown_=false;std::vector<float> ring_;
 AVCodecContext* aac_=nullptr;AVCodecParameters* params_=nullptr;AVFrame* frame_=nullptr;AVPacket* packet_=nullptr;bool aacOK_=false;
 std::vector<float> mix_;int64_t encoded_=0;float lp1_=0,lp2_=0,muff_=0;std::deque<AVPacket*> packets_;
 void wake(){if(wakeEvent_)SetEvent(wakeEvent_);}
 void fail(const std::string& text){std::lock_guard<std::mutex> lock(mutex_);if(error_.empty())error_=text;}
 void openEncoder(){
  const AVCodec* codec=avcodec_find_encoder(AV_CODEC_ID_AAC);if(!codec){fail("FFmpeg has no AAC encoder (old av*-zhphone32 DLLs): MP4 without sound");return;}
  aac_=avcodec_alloc_context3(codec);if(!aac_)return;
  aac_->sample_fmt=AV_SAMPLE_FMT_FLTP;aac_->sample_rate=Rate;av_channel_layout_default(&aac_->ch_layout,1);
  aac_->bit_rate=128000;aac_->time_base={1,Rate};aac_->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
  if(avcodec_open2(aac_,codec,nullptr)<0){avcodec_free_context(&aac_);fail("AAC encoder failed to open");return;}
  params_=avcodec_parameters_alloc();frame_=av_frame_alloc();packet_=av_packet_alloc();
  if(!params_||!frame_||!packet_||avcodec_parameters_from_context(params_,aac_)<0){avcodec_parameters_free(&params_);avcodec_free_context(&aac_);return;}
  frame_->format=AV_SAMPLE_FMT_FLTP;frame_->sample_rate=Rate;frame_->nb_samples=aac_->frame_size>0?aac_->frame_size:1024;
  av_channel_layout_default(&frame_->ch_layout,1);
  if(av_frame_get_buffer(frame_,0)<0){avcodec_parameters_free(&params_);avcodec_free_context(&aac_);return;}
  aacOK_=true;
 }
 static bool parse(const WAVEFORMATEX* f,Source& s){
  if(!f||f->nChannels<1||f->nChannels>16||f->nSamplesPerSec<8000||f->nSamplesPerSec>384000)return false;
  int tag=f->wFormatTag;
  if(tag==WAVE_FORMAT_EXTENSIBLE&&f->cbSize>=22){auto x=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);tag=int(x->SubFormat.Data1);}
  s.channels=f->nChannels;s.rate=int(f->nSamplesPerSec);s.block=f->nBlockAlign;s.bytes=f->wBitsPerSample/8;
  if(tag==3&&s.bytes==4)s.kind=3;else if(tag==1&&s.bytes==2)s.kind=0;else if(tag==1&&s.bytes==3)s.kind=1;else if(tag==1&&s.bytes==4)s.kind=2;else return false;
  return s.block==s.channels*s.bytes;
 }
 bool startClient(Source& s,const WAVEFORMATEX* f,DWORD flags){
  if(!parse(f,s))return false;
  s.event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  // event-driven if allowed, else plain polling (the thread looks every 10 ms anyway);
  // the 16-bit request of the process loopback may need the Windows converter
  const DWORD convert=AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  const DWORD tries[4]={flags|AUDCLNT_STREAMFLAGS_EVENTCALLBACK,flags,flags|AUDCLNT_STREAMFLAGS_EVENTCALLBACK|convert,flags|convert};
  HRESULT r=E_FAIL;bool events=false;
  for(DWORD t:tries){r=s.client->Initialize(AUDCLNT_SHAREMODE_SHARED,t,2000000,0,f,nullptr);if(SUCCEEDED(r)){events=(t&AUDCLNT_STREAMFLAGS_EVENTCALLBACK)!=0;break;}}
  if(FAILED(r))return false;
  if(events&&FAILED(s.client->SetEventHandle(s.event))){CloseHandle(s.event);s.event=nullptr;}
  if(!events&&s.event){CloseHandle(s.event);s.event=nullptr;}
  if(FAILED(s.client->GetService(__uuidof(IAudioCaptureClient),reinterpret_cast<void**>(&s.capture))))return false;
  if(FAILED(s.client->Start()))return false;
  s.mono.reserve(4096);s.out.reserve(8192);return true;
 }
 bool openEndpoint(Source& s,EDataFlow flow,bool loopback){
  IMMDeviceEnumerator* list=nullptr;IMMDevice* device=nullptr;WAVEFORMATEX* mix=nullptr;bool ok=false;
  if(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&list)))&&
     SUCCEEDED(list->GetDefaultAudioEndpoint(flow,eConsole,&device))&&
     SUCCEEDED(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(&s.client)))&&
     SUCCEEDED(s.client->GetMixFormat(&mix)))ok=startClient(s,mix,loopback?AUDCLNT_STREAMFLAGS_LOOPBACK:0);
  if(mix)CoTaskMemFree(mix);drop(device);drop(list);if(!ok)s.close();return ok;
 }
 bool openProcess(Source& s){
  using Activate=HRESULT(WINAPI*)(LPCWSTR,REFIID,PROPVARIANT*,IActivateAudioInterfaceCompletionHandler*,IActivateAudioInterfaceAsyncOperation**);
  HMODULE lib=LoadLibraryW(L"mmdevapi.dll");auto activate=lib?reinterpret_cast<Activate>(GetProcAddress(lib,"ActivateAudioInterfaceAsync")):nullptr;
  if(!activate)return false;
  ActivationParams params{1,{GetCurrentProcessId(),0}}; // PROCESS_LOOPBACK, INCLUDE_TARGET_PROCESS_TREE
  PROPVARIANT value;PropVariantInit(&value);value.vt=VT_BLOB;value.blob.cbSize=sizeof params;value.blob.pBlobData=reinterpret_cast<BYTE*>(&params);
  auto handler=new Completion;IActivateAudioInterfaceAsyncOperation* op=nullptr;bool ok=false;
  if(handler->done&&SUCCEEDED(activate(L"VAD\\Process_Loopback",__uuidof(IAudioClient),&value,handler,&op))&&op&&WaitForSingleObject(handler->done,4000)==WAIT_OBJECT_0){
   HRESULT result=E_FAIL;IUnknown* unknown=nullptr;
   if(SUCCEEDED(op->GetActivateResult(&result,&unknown))&&SUCCEEDED(result)&&unknown)
    ok=SUCCEEDED(unknown->QueryInterface(__uuidof(IAudioClient),reinterpret_cast<void**>(&s.client)));
   drop(unknown);
  }
  drop(op);handler->Release();
  if(ok){ // the virtual device has no mix format: ask for 48 kHz stereo 16-bit
   WAVEFORMATEX f{};f.wFormatTag=WAVE_FORMAT_PCM;f.nChannels=2;f.nSamplesPerSec=Rate;f.wBitsPerSample=16;f.nBlockAlign=4;f.nAvgBytesPerSec=Rate*4;
   ok=startClient(s,&f,AUDCLNT_STREAMFLAGS_LOOPBACK);
  }
  if(!ok)s.close();return ok;
 }
 void openSources(){
  if(flags_&Mic){mic_.name="microphone";micState_=openEndpoint(mic_,eCapture,false)?1:2;if(micState_==2)fail("microphone unavailable (Windows privacy setting / no device)");}
  if(flags_&Game){game_.name="game";
   if(openProcess(game_))gameState_=1;
   else if((flags_&SystemFallback)&&openEndpoint(game_,eRender,true))gameState_=2;
   else{gameState_=3;fail("game sound capture needs Windows 10 2004+ (process loopback)");}
  }
 }
 static float sampleAt(const BYTE* p,int kind){
  switch(kind){
   case 0:{int16_t v;std::memcpy(&v,p,2);return v/32768.f;}
   case 1:{int32_t v=int32_t(uint32_t(p[0])<<8|uint32_t(p[1])<<16|uint32_t(p[2])<<24);return float(v/2147483648.0);}
   case 2:{int32_t v;std::memcpy(&v,p,4);return float(v/2147483648.0);}
   default:{float v;std::memcpy(&v,p,4);return std::isfinite(v)?v:0.f;}
  }
 }
 void ingest(Source& s,const BYTE* data,UINT32 frames,DWORD flags,UINT64 qpc,std::atomic<float>& target,std::atomic<float>& peak){
  s.mono.resize(frames);
  if((flags&AUDCLNT_BUFFERFLAGS_SILENT)||!data)std::fill(s.mono.begin(),s.mono.end(),0.f);
  else for(UINT32 i=0;i<frames;++i){float sum=0;const BYTE* f=data+size_t(i)*s.block;for(int c=0;c<s.channels;++c)sum+=sampleAt(f+c*s.bytes,s.kind);s.mono[i]=sum/s.channels;}
  // to 48 kHz (linear; capture devices are 44.1/48 kHz, rarely 16 kHz headsets)
  s.out.clear();
  if(s.rate==Rate)s.out=s.mono;
  else if(frames){
   if(!s.primed){s.previous=s.mono[0];s.phase=0;s.primed=true;}
   double step=double(s.rate)/Rate;double p=s.phase;
   while(p<double(frames)-1.0){long i=long(std::floor(p));double f=p-i;float a=i<0?s.previous:s.mono[size_t(i)];float b=s.mono[size_t(i+1)];s.out.push_back(float(a+(b-a)*f));p+=step;}
   s.phase=p-frames;s.previous=s.mono[frames-1];
  }
  // place by capture time, keep contiguous within jitter
  int64_t now=now100();int64_t at=int64_t(qpc);
  if((flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)||at<=0||std::llabs(at-now)>5000000)at=now-int64_t(frames)*10000000LL/std::max(1,s.rate);
  int64_t pos=(at-base_)*Rate/10000000LL;
  if(s.next<0||pos>s.next+2400||pos<s.next-4800)s.next=pos;
  float want=target.load(),g=s.gain,k=1.f-std::exp(-1.f/(0.012f*Rate)),hi=0;
  int64_t limit=consumed_+RingSize;
  for(size_t i=0;i<s.out.size();++i){
   g+=(want-g)*k;int64_t q=s.next+int64_t(i);float v=s.out[i]*g;
   if(q>=consumed_&&q<limit&&g>1e-5f)ring_[size_t(q%RingSize)]+=v;
   hi=std::max(hi,std::fabs(v));
  }
  s.gain=g;s.next+=int64_t(s.out.size());
  peak=std::max(hi,peak.load()*0.9f);
 }
 bool drain(Source& s,std::atomic<float>& target,std::atomic<float>& peak){
  if(!s.capture)return true;
  for(int guard=0;guard<64;++guard){
   UINT32 n=0;HRESULT r=s.capture->GetNextPacketSize(&n);if(FAILED(r))return false;if(!n)return true;
   BYTE* data=nullptr;UINT32 frames=0;DWORD flags=0;UINT64 device=0,qpc=0;
   r=s.capture->GetBuffer(&data,&frames,&flags,&device,&qpc);
   if(r==AUDCLNT_S_BUFFER_EMPTY)return true;if(FAILED(r))return false;
   ingest(s,data,frames,flags,qpc,target,peak);s.capture->ReleaseBuffer(frames);
  }
  return true;
 }
 void collect(){
  for(;;){int r=avcodec_receive_packet(aac_,packet_);if(r<0)break;
   AVPacket* p=av_packet_alloc();if(!p){av_packet_unref(packet_);break;}av_packet_move_ref(p,packet_);
   std::lock_guard<std::mutex> lock(mutex_);packets_.push_back(p);}
 }
 // AAC frames for mix_[encoded_ .. upTo); last=true also sends the short tail + flush
 void encode(int64_t upTo,bool last){
  if(!aacOK_)return;
  upTo=std::min<int64_t>(upTo,int64_t(mix_.size()));
  const int size=aac_->frame_size>0?aac_->frame_size:1024;
  while(aacOK_&&(upTo-encoded_>=size||(last&&upTo>encoded_))){
   int count=int(std::min<int64_t>(size,upTo-encoded_));
   if(av_frame_make_writable(frame_)<0){aacOK_=false;fail("AAC frame buffer");return;}
   std::memcpy(frame_->data[0],mix_.data()+encoded_,size_t(count)*sizeof(float));
   frame_->nb_samples=count;frame_->pts=encoded_;encoded_+=count;
   if(avcodec_send_frame(aac_,frame_)<0){aacOK_=false;fail("AAC encoding failed");return;}
   collect();
  }
  if(last&&aacOK_&&avcodec_send_frame(aac_,nullptr)>=0)collect();
 }
 void emit(float x){
  // phone in a pocket / bag: muffled (2-pole low-pass ~900 Hz), crossfaded
  hp_+=0.0104f*(x-hp_);x-=hp_; // fix63: a phone microphone has no deep bass (1-pole high-pass ~80 Hz)
  float m=muffle_.load();muff_+=(m-muff_)*0.0008f;
  const float a=0.111f;lp1_+=a*(x-lp1_);lp2_+=a*(lp1_-lp2_);x+=(lp2_-x)*muff_;
  float ax=std::fabs(x);if(ax>0.85f)x=std::copysign(0.85f+0.15f*std::tanh((ax-0.85f)/0.15f),x); // soft limiter, no clipping
  if(mix_.size()<size_t(Rate)*130)mix_.push_back(x);
  ++emitted_;
 }
 // before the first video frame: keep 3 s, throw older audio away; true once pts 0 is known
 bool ensureZero(int64_t limit){
  if(zeroKnown_)return true;
  int64_t keep=limit-Rate*3;while(consumed_<keep){ring_[size_t(consumed_%RingSize)]=0;++consumed_;}
  int64_t z=zero_.load();if(z==INT64_MIN)return false;
  zeroKnown_=true;zeroPos_=(z-base_)*Rate/10000000LL;
  if(zeroPos_<consumed_){int64_t pad=std::min<int64_t>(consumed_-zeroPos_,Rate*5);for(int64_t i=0;i<pad;++i)emit(0.f);}
  else while(consumed_<zeroPos_){ring_[size_t(consumed_%RingSize)]=0;++consumed_;}
  return true;
 }
 void consume(int64_t limit){
  if(!ensureZero(limit))return;
  if(endPos_>=0)limit=std::min(limit,endPos_);
  while(consumed_<limit){float& v=ring_[size_t(consumed_%RingSize)];emit(v);v=0;++consumed_;}
 }
 void finalize(int64_t length){
  mix_.resize(size_t(std::max<int64_t>(0,std::min<int64_t>(length,int64_t(Rate)*130))),0.f); // exactly the video length
  encode(int64_t(mix_.size()),true);
  // 24 kHz copy for in-game playback: [1 2 1]/4 low-pass, every 2nd sample
  std::vector<int16_t> pcm_(mix_.size()/2);
  for(size_t i=0;i<pcm_.size();++i){float p=i?mix_[2*i-1]:0.f,c=mix_[2*i],n=mix_[2*i+1];float y=0.25f*p+0.5f*c+0.25f*n;
   pcm_[i]=int16_t(std::lround(std::max(-1.f,std::min(1.f,y))*32767.f));}
  // RIFF/WAVE, PCM 16-bit mono 24 kHz, published atomically
  std::error_code ec;fs::path partial=wav_;partial+=L".partial";
  {std::ofstream f(partial,std::ios::binary|std::ios::trunc);
   if(f){auto u32=[&](uint32_t v){f.write(reinterpret_cast<const char*>(&v),4);};auto u16=[&](uint16_t v){f.write(reinterpret_cast<const char*>(&v),2);};
    uint32_t bytes=uint32_t(pcm_.size()*2);f.write("RIFF",4);u32(36+bytes);f.write("WAVEfmt ",8);u32(16);u16(1);u16(1);u32(WavRate);u32(WavRate*2);u16(2);u16(16);f.write("data",4);u32(bytes);
    if(bytes)f.write(reinterpret_cast<const char*>(pcm_.data()),bytes);}
   if(!f){f.close();fs::remove(partial,ec);fail("cannot write clip WAV");return;}}
  fs::remove(wav_,ec);fs::rename(partial,wav_,ec);if(ec){fs::remove(partial,ec);fail("cannot publish clip WAV");return;}
  wavReady_=true;
 }
 // fix63: commands from Lua, then every voice rendered into the ring up to "now"
 void ear(int64_t nowPos){
  std::vector<EarCommand> cmds;{std::lock_guard<std::mutex> lock(earMutex_);cmds.swap(earCommands_);}
  for(auto& c:cmds){
   if(c.kind==0){
    if(voices_.size()>=96){ // too many at once: the quietest goes
     size_t q=0;for(size_t i=1;i<voices_.size();++i)if(voices_[i].gainT<voices_[q].gainT)q=i;
     if(voices_[q].gainT>c.gain)continue;voices_.erase(voices_.begin()+std::ptrdiff_t(q));
    }
    Voice v;v.id=c.id;v.key=c.key;v.at=c.at;v.born=c.born;v.gain=v.gainT=c.gain;v.a=v.aT=lowpassCoef(c.lp);v.pitch=v.pitchT=c.pitch;v.loop=c.loop;
    voices_.push_back(std::move(v));++earPlayed_;
   }else for(auto& v:voices_)if(v.id==c.id){
    if(c.kind==1){v.gainT=c.gain;v.aT=lowpassCoef(c.lp);v.pitchT=c.pitch;}else v.stop=true;
   }
  }
  // the ear's master gain (crossfade with "what you hear") ramps over the new block
  const float master=earGain_.load();
  const int64_t from=earMark_<0?nowPos:earMark_;const float m0=earMark_<0?master:earFrom_;
  auto masterAt=[&](int64_t q)->float{if(q<=from||nowPos<=from)return q>=nowPos?master:m0;return m0+(master-m0)*float(double(q-from)/double(nowPos-from));};
  const float k=1.f-std::exp(-1.f/(0.015f*Rate));float hi=0;
  const int64_t limit=std::min(nowPos,consumed_+RingSize-1);
  for(size_t i=0;i<voices_.size();){
   Voice& v=voices_[i];bool done=false;
   if(!v.clip){v.clip=PhoneEar::bank().find(v.key);
    if(!v.clip){if(now100()-v.born>30000000LL||PhoneEar::bank().state(v.key)==PhoneEar::Failed){voices_.erase(voices_.begin()+std::ptrdiff_t(i));continue;}++i;continue;} // still decoding (bank thread, 3 s at most)
    v.step=double(v.clip->rate)/Rate;
   }
   const auto& pcm=v.clip->pcm;const double len=double(pcm.size());const int64_t loopAt=v.loop?v.clip->loopStart:-1;
   if(v.pos<0)v.pos=(v.at-base_)*Rate/10000000LL;
   if(v.pos<consumed_){v.cursor+=double(consumed_-v.pos)*v.step*v.pitch;v.pos=consumed_;} // late file: the part already gone is skipped
   auto wrap=[&]()->bool{ // past the end: loop or finished
    if(v.cursor<len)return true;
    if(loopAt>=0&&double(loopAt)<len-1){double span=len-double(loopAt);v.cursor=double(loopAt)+std::fmod(v.cursor-len,span);return true;}
    return false;};
   if(!wrap())done=true;
   else if(!v.stop&&v.gain<1e-5f&&v.gainT<1e-5f){ // silent (too far): only time passes
    if(limit>v.pos){v.cursor+=double(limit-v.pos)*v.step*v.pitch;v.pos=limit;}
    if(!wrap())done=true;
   }else{
    for(int64_t q=v.pos;q<limit;++q){
     v.gain+=(v.gainT-v.gain)*k;v.a+=(v.aT-v.a)*k;v.pitch+=(v.pitchT-v.pitch)*k;
     if(v.stop){v.fade-=1.f/(0.03f*Rate);if(v.fade<=0.f){done=true;v.pos=q;break;}}
     if(!wrap()){done=true;v.pos=q;break;}
     float x=hermite(pcm,v.cursor,loopAt);
     v.y1+=v.a*(x-v.y1);v.y2+=v.a*(v.y1-v.y2);
     float out=v.y2*v.gain*v.fade*masterAt(q);
     ring_[size_t(q%RingSize)]+=out;hi=std::max(hi,std::fabs(out));
     v.cursor+=v.step*double(v.pitch);v.pos=q+1;
    }
   }
   if(done){voices_.erase(voices_.begin()+std::ptrdiff_t(i));continue;}
   ++i;
  }
  earFrom_=master;earMark_=nowPos;earVoices_=int(voices_.size());earPeak_=std::max(hi,earPeak_.load()*0.9f);
 }
 void run(){
  bool com=SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
  base_=now100();ring_.assign(size_t(RingSize),0.f);mix_.reserve(size_t(Rate)*30);
  try{openSources();}catch(...){fail("audio device setup failed");}
  int retryAt=0;int64_t finishSeen=0;
  for(;;){
   HANDLE waits[3];DWORD n=0;if(wakeEvent_)waits[n++]=wakeEvent_;if(mic_.event)waits[n++]=mic_.event;if(game_.event)waits[n++]=game_.event;
   if(n)WaitForMultipleObjects(n,waits,FALSE,10);else Sleep(10);
   if(cancelled_)break;
   if(!drain(mic_,micGain_,micPeak_)){mic_.close();micState_=2;fail("microphone disconnected");}
   if(!drain(game_,gameGain_,gamePeak_)){game_.close();gameState_=3;fail("game sound capture stopped");}
   // headset unplugged / default device changed: reopen at most every 2 s
   if(++retryAt>=200){retryAt=0;
    if((flags_&Mic)&&!mic_.capture&&micState_==2&&openEndpoint(mic_,eCapture,false))micState_=1;
    if((flags_&Game)&&!game_.capture&&gameState_==3){if(openProcess(game_))gameState_=1;else if((flags_&SystemFallback)&&openEndpoint(game_,eRender,true))gameState_=2;}
   }
   // fix64: new sound files are decoded by the bank's own thread (phone_ear.h), not here
   int64_t nowPos=(now100()-base_)*Rate/10000000LL;
   ear(nowPos);
   if(finishing_){
    if(!finishSeen)finishSeen=now100();
    int64_t end=end90k_.load();
    if(end<0||!ensureZero(nowPos-960))break; // no video: nothing to keep
    if(endPos_<0)endPos_=std::max(consumed_,zeroPos_+end*Rate/90000);
    consume(nowPos-960);
    if(consumed_>=endPos_||now100()-finishSeen>15000000LL){finalize(end*Rate/90000);break;} // late device: padded with silence
    continue;
   }
   // watchdog: Lua stopped steering the recorder (crash / lost session): never keep the mic open
   if(now100()-lastControl_.load()>80000000LL){fail("audio watchdog: recording no longer active");break;}
   consume(nowPos-24000); // fix64: 500 ms (was 200) for late packets / sound files still being read + decoded
   encode(seen_.load()-Rate,false);
  }
  mic_.close();game_.close();
  if(com)CoUninitialize();
  {std::lock_guard<std::mutex> lock(mutex_);done_=true;}cv_.notify_all();
 }
};
}
