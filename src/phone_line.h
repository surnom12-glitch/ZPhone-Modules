// fix73: the phone line. During a call the sounds around the OTHER phone of the call (shots, steps,
// doors, cars... reported by the server, shaped in Lua exactly like the clip's phone ear: Source's
// fall-off at that phone, walls muffle, far sounds lose their highs, a pocket muffles) are mixed
// here and played on this PC's default Windows playback device, like the speaker of the phone:
// an own WASAPI shared render stream, a phone band filter, a soft limiter. The files are the ones
// of PhoneEar::bank() (decoded once, shared with the clip ear). The voices of the people at the
// other phone stay GMod voice chat (routed + leveled by the server from that phone's position).
// Mixer = device independent (Linux-tested, tests/line73); Player = the WASAPI part + its thread.
#pragma once
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "phone_ear.h"
namespace PhoneLine {
inline int64_t now100(){
 LARGE_INTEGER c,f;QueryPerformanceCounter(&c);QueryPerformanceFrequency(&f);
 return (c.QuadPart/f.QuadPart)*10000000LL+(c.QuadPart%f.QuadPart)*10000000LL/f.QuadPart;
}
struct Command{int kind=0;int id=0;std::string key;float gain=0,lp=20000,pitch=1;bool loop=true;double age=0;int64_t born=0;};
struct Voice{
 int id=0;std::string key;PhoneEar::ClipPtr clip;int64_t born=0;double cursor=0,step=1,age=0;
 float gain=0,gainT=0,a=1,aT=1,pitch=1,pitchT=1,fade=1,y1=0,y2=0;bool loop=true,stop=false,placed=false;
};
// 2nd order section (RBJ cookbook, Butterworth Q), transposed direct form II
struct Biquad{
 float b0=1,b1=0,b2=0,a1=0,a2=0,z1=0,z2=0;
 float run(float x){float y=b0*x+z1;z1=b1*x-a1*y+z2;z2=b2*x-a2*y;if(!std::isfinite(z1)||!std::isfinite(z2)){z1=z2=0;y=0;}return y;}
 void bypass(){b0=1;b1=b2=a1=a2=0;}
 void design(bool high,double hz,double rate){
  if(!(hz>0)||!(rate>0)||hz>=rate*0.45){bypass();return;}
  const double w=6.283185307179586*hz/rate,c=std::cos(w),al=std::sin(w)/(2*0.7071067811865476),a0=1+al;
  double nb0,nb1,nb2;if(high){nb0=(1+c)/2;nb1=-(1+c);nb2=(1+c)/2;}else{nb0=(1-c)/2;nb1=1-c;nb2=(1-c)/2;}
  b0=float(nb0/a0);b1=float(nb1/a0);b2=float(nb2/a0);a1=float(-2*c/a0);a2=float((1-al)/a0);
 }
};
inline float hermite(const std::vector<float>& p,double c,int64_t loopStart){
 int64_t n=int64_t(p.size());int64_t i=int64_t(std::floor(c));float t=float(c-double(i));
 auto at=[&](int64_t k)->float{if(k<0)return 0.f;if(k>=n){if(loopStart<0||loopStart>=n)return 0.f;k=loopStart+(k-n)%(n-loopStart);}return p[size_t(k)];};
 float xm1=at(i-1),x0=at(i),x1=at(i+1),x2=at(i+2);
 float c1=0.5f*(x1-xm1),c2=xm1-2.5f*x0+2.f*x1-0.5f*x2,c3=0.5f*(x2-xm1)+1.5f*(x0-x1);
 return ((c3*t+c2)*t+c1)*t+x0;
}
struct MixStats{int voices=0,played=0;double peak=0,rendered=0;};
// every line voice mixed to mono at the output rate. Lua thread: play/set/stop/control (queued),
// audio thread: render(). find(key) = decoded file or nullptr, bad(key) = file known unusable
class Mixer{
public:
 using Find=std::function<PhoneEar::ClipPtr(const std::string&)>;
 using Bad=std::function<bool(const std::string&)>;
 Mixer(Find find,Bad bad):find_(std::move(find)),bad_(std::move(bad)){setRate(48000);}
 void setRate(int rate){
  rate_=std::max(8000,std::min(384000,rate));k_=1.f-std::exp(-1.f/(0.015f*float(rate_)));
  masterK_=1.f-std::exp(-1.f/(0.03f*float(rate_)));muffK_=1.f-std::exp(-1.f/(0.12f*float(rate_)));
  muffA_=lowpassCoef(900.f);bandNow_=-1;
 }
 int rate()const{return rate_;}
 // age = seconds since the game started the sound (late file / network): the part already gone is skipped
 void play(int id,const std::string& key,double gain,double lp,double pitch,bool loop,double age){
  Command c;c.kind=0;c.id=id;c.key=key;c.gain=clampGain(gain);c.lp=float(lp);c.pitch=clampPitch(pitch);c.loop=loop;
  c.age=std::isfinite(age)&&age>0?std::min(age,30.0):0.0;c.born=now100();push(std::move(c));
 }
 void set(int id,double gain,double lp,double pitch){Command c;c.kind=1;c.id=id;c.gain=clampGain(gain);c.lp=float(lp);c.pitch=clampPitch(pitch);push(std::move(c));}
 void stop(int id){Command c;c.kind=2;c.id=id;push(std::move(c));}
 void stopAll(){Command c;c.kind=3;push(std::move(c));}
 // master = loudness of the line (0..4), muffle 0..1 (a phone in a pocket), band 0 full / 1 HD phone / 2 old phone
 void control(double master,double muffle,int band){
  master_=float(std::min(4.0,std::max(0.0,std::isfinite(master)?master:0.0)));
  muffle_=float(std::min(1.0,std::max(0.0,std::isfinite(muffle)?muffle:0.0)));
  band_=std::min(2,std::max(0,band));
 }
 MixStats stats()const{MixStats s;s.voices=voices__.load();s.played=played_.load();s.peak=peak_.load();s.rendered=seconds_.load();return s;}
 // n mono samples, the next part of the line
 void render(float* out,int n){
  if(n<=0)return;
  std::vector<Command> cmds;{std::lock_guard<std::mutex> lock(mutex_);cmds.swap(commands_);}
  for(auto& c:cmds){
   if(c.kind==0){
    if(voices_.size()>=96){ // too many at once: the quietest goes
     size_t q=0;for(size_t i=1;i<voices_.size();++i)if(voices_[i].gainT<voices_[q].gainT)q=i;
     if(voices_[q].gainT>c.gain)continue;voices_.erase(voices_.begin()+std::ptrdiff_t(q));
    }
    Voice v;v.id=c.id;v.key=c.key;v.born=c.born;v.age=c.age;v.gain=v.gainT=c.gain;v.a=v.aT=lowpassCoef(c.lp);v.pitch=v.pitchT=c.pitch;v.loop=c.loop;
    voices_.push_back(std::move(v));++played_;
   }else if(c.kind==3){for(auto& v:voices_)v.stop=true;}
   else for(auto& v:voices_)if(v.id==c.id){if(c.kind==1){v.gainT=c.gain;v.aT=lowpassCoef(c.lp);v.pitchT=c.pitch;}else v.stop=true;}
  }
  if(bandNow_!=band_.load()){
   bandNow_=band_.load();
   if(bandNow_==0){hp1_.bypass();hp2_.bypass();lp1_.bypass();lp2_.bypass();}
   else if(bandNow_==1){hp1_.design(true,120,rate_);hp2_.bypass();lp1_.design(false,7000,rate_);lp2_.bypass();}
   else{hp1_.design(true,300,rate_);hp2_.design(true,300,rate_);lp1_.design(false,3400,rate_);lp2_.design(false,3400,rate_);}
  }
  std::fill(out,out+n,0.f);
  const int64_t now=now100();
  for(size_t i=0;i<voices_.size();){
   Voice& v=voices_[i];
   if(!v.clip){
    v.clip=find_(v.key);
    if(!v.clip){if(v.stop||now-v.born>100000000LL||bad_(v.key)){voices_.erase(voices_.begin()+std::ptrdiff_t(i));continue;}++i;continue;} // still decoding
   }
   const auto& pcm=v.clip->pcm;const double len=double(pcm.size());const int64_t loopAt=v.loop?v.clip->loopStart:-1;
   if(!v.placed){ // starts in this block; the time it waited for its file (and its age) is skipped
    v.placed=true;v.step=double(v.clip->rate)/double(rate_);
    double late=v.age+double(now-v.born)/1e7;v.cursor=late>0.06?late*double(v.clip->rate)*double(v.pitch):0.0;
   }
   auto wrap=[&]()->bool{
    if(v.cursor<len)return true;
    if(loopAt>=0&&double(loopAt)<len-1){double span=len-double(loopAt);v.cursor=double(loopAt)+std::fmod(v.cursor-len,span);return true;}
    return false;};
   bool done=!wrap();
   if(!done&&!v.stop&&v.gain<1e-5f&&v.gainT<1e-5f){v.cursor+=double(n)*v.step*double(v.pitch);done=!wrap();} // silent: only time passes
   else if(!done){
    for(int q=0;q<n;++q){
     v.gain+=(v.gainT-v.gain)*k_;v.a+=(v.aT-v.a)*k_;v.pitch+=(v.pitchT-v.pitch)*k_;
     if(v.stop){v.fade-=1.f/(0.03f*float(rate_));if(v.fade<=0.f){done=true;break;}}
     if(!wrap()){done=true;break;}
     float x=hermite(pcm,v.cursor,loopAt);
     v.y1+=v.a*(x-v.y1);v.y2+=v.a*(v.y1-v.y2);
     out[q]+=v.y2*v.gain*v.fade;
     v.cursor+=v.step*double(v.pitch);
    }
   }
   if(done){voices_.erase(voices_.begin()+std::ptrdiff_t(i));continue;}
   ++i;
  }
  // phone band, pocket, line loudness, soft limiter (no clipping)
  const float master=master_.load(),muffle=muffle_.load();float hi=0;
  for(int q=0;q<n;++q){
   float x=out[q];
   x=lp2_.run(lp1_.run(hp2_.run(hp1_.run(x))));
   muff_+=(muffle-muff_)*muffK_;m1_+=muffA_*(x-m1_);m2_+=muffA_*(m1_-m2_);x+=(m2_-x)*muff_;
   gain_+=(master-gain_)*masterK_;if(std::fabs(master-gain_)<1e-4f)gain_=master;x*=gain_;
   float ax=std::fabs(x);if(ax>0.85f)x=std::copysign(0.85f+0.15f*std::tanh((ax-0.85f)/0.15f),x);
   if(!std::isfinite(x))x=0;
   out[q]=x;hi=std::max(hi,std::fabs(x));
  }
  voices__=int(voices_.size());peak_=std::max(hi,peak_.load()*0.9f);seconds_=seconds_.load()+double(n)/double(rate_);
 }
private:
 Find find_;Bad bad_;std::mutex mutex_;std::vector<Command> commands_;std::vector<Voice> voices_;
 int rate_=48000;float k_=0,masterK_=0,muffK_=0,muffA_=1,gain_=0,muff_=0,m1_=0,m2_=0;int bandNow_=-1;
 Biquad hp1_,hp2_,lp1_,lp2_;
 std::atomic<float> master_{0},muffle_{0},peak_{0};std::atomic<int> band_{1},voices__{0},played_{0};std::atomic<double> seconds_{0};
 void push(Command&& c){std::lock_guard<std::mutex> lock(mutex_);if(commands_.size()<4096)commands_.push_back(std::move(c));}
 float lowpassCoef(float hz)const{if(!(hz<19000.f))return 1.f;hz=std::max(150.f,hz);return 1.f-std::exp(-6.2831853f*hz/float(rate_));}
 static float clampGain(double g){return float(std::min(4.0,std::max(0.0,std::isfinite(g)?g:0.0)));}
 static float clampPitch(double p){return float(std::min(4.0,std::max(0.1,std::isfinite(p)?p:1.0)));}
};
template<class T>inline void release73(T*& p){if(p){p->Release();p=nullptr;}}
struct PlayerStats{int state=0;std::string error;int rate=0,channels=0,reopened=0,underruns=0;double queuedMs=0;MixStats mix;};
// the line's own playback stream on the default Windows output device (shared mode, ~60 ms queued).
// state: 0 starting, 1 playing, 2 no device (tried again every 2 s), 3 ended (closed / watchdog)
class Player{
public:
 Player():mixer_([](const std::string& k){return PhoneEar::bank().find(k);},[](const std::string& k){return PhoneEar::bank().state(k)==PhoneEar::Failed;}){
  lastKeep_=now100();wake_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  thread_=std::thread([this]{run();});
 }
 ~Player(){requestClose();join();if(wake_){CloseHandle(wake_);wake_=nullptr;}}
 Player(const Player&)=delete;Player& operator=(const Player&)=delete;
 Mixer& mixer(){return mixer_;}
 void keep(){lastKeep_=now100();}           // Lua steers the line (every frame): the watchdog stays calm
 void requestClose(){quit_=true;if(wake_)SetEvent(wake_);}
 void join(){if(thread_.joinable())thread_.join();}
 bool ended()const{return ended_.load();}
 PlayerStats stats(){PlayerStats s;s.state=state_.load();s.rate=rateOut_.load();s.channels=channelsOut_.load();s.reopened=reopened_.load();s.underruns=underruns_.load();s.queuedMs=queuedMs_.load();s.mix=mixer_.stats();
  std::lock_guard<std::mutex> lock(errorMutex_);s.error=error_;return s;}
private:
 Mixer mixer_;std::thread thread_;HANDLE wake_=nullptr;std::atomic<bool> quit_{false},ended_{false};std::atomic<int64_t> lastKeep_{0};
 std::atomic<int> state_{0},reopened_{0},underruns_{0},rateOut_{0},channelsOut_{0};std::atomic<double> queuedMs_{0};std::mutex errorMutex_;std::string error_;
 IMMDeviceEnumerator* list_=nullptr;IAudioClient* client_=nullptr;IAudioRenderClient* render_=nullptr;UINT32 bufferFrames_=0;
 int rate_=0,channels_=0,kind_=3,bytes_=4,block_=0;std::wstring device_;std::vector<float> mono_;bool started_=false;
 void fail(const std::string& t){std::lock_guard<std::mutex> lock(errorMutex_);error_=t;}
 bool parse(const WAVEFORMATEX* f){
  if(!f||f->nChannels<1||f->nChannels>16||f->nSamplesPerSec<8000||f->nSamplesPerSec>384000)return false;
  int tag=f->wFormatTag;
  if(tag==WAVE_FORMAT_EXTENSIBLE&&f->cbSize>=22){auto x=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);tag=int(x->SubFormat.Data1);}
  channels_=f->nChannels;rate_=int(f->nSamplesPerSec);block_=f->nBlockAlign;bytes_=f->wBitsPerSample/8;
  if(tag==3&&bytes_==4)kind_=3;else if(tag==1&&bytes_==2)kind_=0;else if(tag==1&&bytes_==3)kind_=1;else if(tag==1&&bytes_==4)kind_=2;else return false;
  return block_==channels_*bytes_;
 }
 void closeDevice(){if(client_&&started_)client_->Stop();started_=false;release73(render_);release73(client_);bufferFrames_=0;}
 std::wstring defaultId(){
  std::wstring id;IMMDevice* dev=nullptr;
  if(list_&&SUCCEEDED(list_->GetDefaultAudioEndpoint(eRender,eConsole,&dev))&&dev){LPWSTR s=nullptr;if(SUCCEEDED(dev->GetId(&s))&&s){id=s;CoTaskMemFree(s);}}
  release73(dev);return id;
 }
 bool open(){
  closeDevice();
  if(!list_&&FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&list_)))){list_=nullptr;fail("Windows audio unavailable");return false;}
  IMMDevice* dev=nullptr;WAVEFORMATEX* mix=nullptr;bool ok=false;
  if(SUCCEEDED(list_->GetDefaultAudioEndpoint(eRender,eConsole,&dev))&&dev&&
     SUCCEEDED(dev->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(&client_)))&&client_&&
     SUCCEEDED(client_->GetMixFormat(&mix))&&parse(mix)){
   LPWSTR s=nullptr;if(SUCCEEDED(dev->GetId(&s))&&s){device_=s;CoTaskMemFree(s);}
   if(SUCCEEDED(client_->Initialize(AUDCLNT_SHAREMODE_SHARED,0,1000000,0,mix,nullptr))&&SUCCEEDED(client_->GetBufferSize(&bufferFrames_))&&bufferFrames_>0&&
      SUCCEEDED(client_->GetService(__uuidof(IAudioRenderClient),reinterpret_cast<void**>(&render_)))&&render_){
    mixer_.setRate(rate_);rateOut_=rate_;channelsOut_=channels_;ok=true;
   }
  }
  if(mix)CoTaskMemFree(mix);release73(dev);
  if(!ok){closeDevice();fail("no playback device for the line (Windows sound output)");return false;}
  return true;
 }
 // tops the device buffer up to ~60 ms; false = the device is gone (unplugged / changed format)
 bool fill(){
  UINT32 pad=0;if(FAILED(client_->GetCurrentPadding(&pad)))return false;
  if(started_&&pad==0)++underruns_;
  queuedMs_=double(pad)*1000.0/double(std::max(1,rate_));
  UINT32 want=std::min<UINT32>(bufferFrames_,UINT32(rate_*0.06));
  if(pad>=want){if(!started_){if(FAILED(client_->Start()))return false;started_=true;}return true;}
  UINT32 n=want-pad;BYTE* data=nullptr;
  if(FAILED(render_->GetBuffer(n,&data))||!data)return false;
  mono_.resize(n);mixer_.render(mono_.data(),int(n));
  std::memset(data,0,size_t(n)*size_t(block_));
  const int out=std::min(2,channels_); // front left + right (mono: the one channel); others silent
  for(UINT32 i=0;i<n;++i){
   float v=std::max(-1.f,std::min(1.f,mono_[i]));BYTE* f=data+size_t(i)*size_t(block_);
   for(int c=0;c<out;++c){BYTE* p=f+size_t(c)*size_t(bytes_);
    switch(kind_){
     case 0:{int16_t s=int16_t(std::lround(v*32767.f));std::memcpy(p,&s,2);break;}
     case 1:{int32_t s=int32_t(std::lround(double(v)*8388607.0));p[0]=BYTE(s&255);p[1]=BYTE((s>>8)&255);p[2]=BYTE((s>>16)&255);break;}
     case 2:{int32_t s=int32_t(std::llround(double(v)*2147483647.0));std::memcpy(p,&s,4);break;}
     default:std::memcpy(p,&v,4);
    }
   }
  }
  if(FAILED(render_->ReleaseBuffer(n,0)))return false;
  if(!started_){if(FAILED(client_->Start()))return false;started_=true;}
  return true;
 }
 void run(){
  bool com=SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
  int64_t retryAt=0,checkAt=0;
  std::vector<float> idle;
  while(!quit_.load()){
   const int64_t now=now100();
   if(now-lastKeep_.load()>30000000LL){fail("line watchdog: the game stopped steering the line");break;} // never a stuck sound
   if(!client_&&now>=retryAt){
    if(open()){state_=1;checkAt=now+20000000LL;}
    else{state_=2;retryAt=now+20000000LL;}
   }
   if(client_){
    if(!fill()){closeDevice();state_=2;retryAt=now+5000000LL;++reopened_;}
    else if(now>=checkAt){checkAt=now+20000000LL;std::wstring id=defaultId();if(!id.empty()&&id!=device_){closeDevice();retryAt=0;++reopened_;}} // another default output (headset plugged in)
   }else{ // no device: time still passes for the voices (nothing piles up)
    idle.resize(size_t(std::max(1,mixer_.rate()/200)));mixer_.render(idle.data(),int(idle.size()));
   }
   if(wake_)WaitForSingleObject(wake_,5);else Sleep(5);
  }
  closeDevice();release73(list_);
  if(com)CoUninitialize();
  state_=3;ended_=true;
 }
};
}
