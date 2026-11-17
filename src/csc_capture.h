// fix74: the uplink of a Cross Server Call - what YOUR phone sends to the other server, built like the
// clip sound of the recorder (audio_capture.h): your microphone (Lua opens it with the voice key and
// lowers it by the distance phone <-> mouth), the sound of THIS game process ("what you hear": the
// voices of the players around you, every game / mod sound, only while the phone is at your head)
// and the phone's own ear (game sounds re-played at the phone, phone_ear.h) when it is away from you.
// The phone line plays the other side in this same process, so the process capture contains it: it is
// taken out again (csc_audio.h Echo, the line hands every block it plays to refTap()).
// 48 kHz timeline (QPC), 50 ms behind real time, out as 16 kHz IMA-ADPCM frames of 20 ms.
#pragma once
#include "audio_capture.h"
#include "csc_audio.h"
namespace Csc {
struct UpFrame{uint16_t seq=0;std::array<uint8_t,FrameBytes> data{};};
struct UplinkStats{int mic=0,game=0,frames=0,sent=0,earVoices=0;double micPeak=0,gamePeak=0,outPeak=0;EchoStats echo;std::string error;bool ended=false;};
class Uplink{
public:
 static constexpr int Rate=PhoneAudio::Rate;static constexpr int64_t Ring=int64_t(Rate)*4;
 Uplink(bool mic,bool game):wantMic_(mic),wantGame_(game){lastControl_=PhoneAudio::now100();wake_=CreateEventW(nullptr,FALSE,FALSE,nullptr);thread_=std::thread([this]{run();});}
 void requestStop(){quit_=true;if(wake_)SetEvent(wake_);} // the thread ends by itself (no wait here)
 ~Uplink(){quit_=true;if(wake_)SetEvent(wake_);if(thread_.joinable())thread_.join();if(wake_)CloseHandle(wake_);}
 Uplink(const Uplink&)=delete;Uplink& operator=(const Uplink&)=delete;
 // every frame from Lua (watchdog 4 s): microphone, "what you hear", pocket, the phone's ear
 void control(double mic,double game,double muffle,double ear){
  auto c=[](double v,double hi){return float(std::min(hi,std::max(0.0,std::isfinite(v)?v:0.0)));};
  micGain_=c(mic,4);gameGain_=c(game,4);muffle_=c(muffle,1);earGain_=c(ear,4);lastControl_=PhoneAudio::now100();
 }
 void earPlay(int id,const std::string& key,double gain,double lp,double pitch,bool loop,double age){
  if(!std::isfinite(age)||age<0)age=0;age=std::min(age,30.0);
  PhoneAudio::EarCommand c;c.kind=0;c.id=id;c.key=key;c.gain=cg(gain);c.lp=float(lp);c.pitch=cp(pitch);c.loop=loop;c.born=PhoneAudio::now100();c.at=c.born-int64_t(age*1e7);
  std::lock_guard<std::mutex> lock(earMutex_);if(earCommands_.size()<4096)earCommands_.push_back(std::move(c));
 }
 void earSet(int id,double gain,double lp,double pitch){PhoneAudio::EarCommand c;c.kind=1;c.id=id;c.gain=cg(gain);c.lp=float(lp);c.pitch=cp(pitch);std::lock_guard<std::mutex> lock(earMutex_);if(earCommands_.size()<4096)earCommands_.push_back(std::move(c));}
 void earStop(int id){PhoneAudio::EarCommand c;c.kind=id<0?3:2;c.id=id;std::lock_guard<std::mutex> lock(earMutex_);if(earCommands_.size()<4096)earCommands_.push_back(std::move(c));}
 std::vector<UpFrame> take(){std::lock_guard<std::mutex> lock(outMutex_);std::vector<UpFrame> o;o.swap(out_);return o;}
 bool ended()const{return ended_.load();}
 UplinkStats stats(){UplinkStats s;s.mic=micState_;s.game=gameState_;s.frames=frames_;s.micPeak=micPeak_;s.gamePeak=gamePeak_;s.outPeak=outPeak_;s.earVoices=earVoices_;s.ended=ended_;
  {std::lock_guard<std::mutex> lock(statMutex_);s.echo=echoStats_;s.error=error_;}return s;}
private:
 bool wantMic_,wantGame_;std::thread thread_;HANDLE wake_=nullptr;std::atomic<bool> quit_{false},ended_{false};
 std::atomic<float> micGain_{0},gameGain_{0},muffle_{0},earGain_{0},micPeak_{0},gamePeak_{0},outPeak_{0};std::atomic<int64_t> lastControl_{0};
 std::atomic<int> micState_{0},gameState_{0},frames_{0},earVoices_{0};
 std::mutex earMutex_,outMutex_,statMutex_;std::vector<PhoneAudio::EarCommand> earCommands_;std::vector<PhoneAudio::Voice> voices_;std::vector<UpFrame> out_;
 std::string error_;EchoStats echoStats_;
 PhoneAudio::Source mic_,game_;int64_t base_=0,consumed_=0,refNext_=-1;double refPhase_=0;float refPrev_=0;
 std::vector<float> main_,loop_,ref_;
 Decimate3 dMain_,dLoop_,dRef_;Echo echo_;std::array<float,Echo::Lead> delay_{};int delayPos_=0;
 float gGame_=0,hp_=0,lp1_=0,lp2_=0,muff_=0;std::array<int16_t,Frame> pcm_{};int pcmN_=0;Adpcm adpcm_;uint16_t seq_=0;
 float earFrom_=0;int64_t earMark_=-1;
 static float cg(double g){return float(std::min(4.0,std::max(0.0,std::isfinite(g)?g:0.0)));}
 static float cp(double p){return float(std::min(4.0,std::max(0.1,std::isfinite(p)?p:1.0)));}
 bool refOn_=false;void ref(bool on){if(on!=refOn_){refOn_=on;refTap().enable(on);}}
 void fail(const std::string& t){std::lock_guard<std::mutex> lock(statMutex_);if(error_.empty())error_=t;}
 // ---- devices (the recorder's way)
 static bool parse(const WAVEFORMATEX* f,PhoneAudio::Source& s){
  if(!f||f->nChannels<1||f->nChannels>16||f->nSamplesPerSec<8000||f->nSamplesPerSec>384000)return false;
  int tag=f->wFormatTag;
  if(tag==WAVE_FORMAT_EXTENSIBLE&&f->cbSize>=22){auto x=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);tag=int(x->SubFormat.Data1);}
  s.channels=f->nChannels;s.rate=int(f->nSamplesPerSec);s.block=f->nBlockAlign;s.bytes=f->wBitsPerSample/8;
  if(tag==3&&s.bytes==4)s.kind=3;else if(tag==1&&s.bytes==2)s.kind=0;else if(tag==1&&s.bytes==3)s.kind=1;else if(tag==1&&s.bytes==4)s.kind=2;else return false;
  return s.block==s.channels*s.bytes;
 }
 static bool startClient(PhoneAudio::Source& s,const WAVEFORMATEX* f,DWORD flags){
  if(!parse(f,s))return false;
  s.event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
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
 static bool openMic(PhoneAudio::Source& s){
  IMMDeviceEnumerator* list=nullptr;IMMDevice* device=nullptr;WAVEFORMATEX* mix=nullptr;bool ok=false;
  if(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&list)))&&
     SUCCEEDED(list->GetDefaultAudioEndpoint(eCapture,eCommunications,&device))&&
     SUCCEEDED(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(&s.client)))&&
     SUCCEEDED(s.client->GetMixFormat(&mix)))ok=startClient(s,mix,0);
  if(mix)CoTaskMemFree(mix);PhoneAudio::drop(device);PhoneAudio::drop(list);if(!ok)s.close();return ok;
 }
 static bool openProcess(PhoneAudio::Source& s){
  using Activate=HRESULT(WINAPI*)(LPCWSTR,REFIID,PROPVARIANT*,IActivateAudioInterfaceCompletionHandler*,IActivateAudioInterfaceAsyncOperation**);
  HMODULE lib=LoadLibraryW(L"mmdevapi.dll");auto activate=lib?reinterpret_cast<Activate>(GetProcAddress(lib,"ActivateAudioInterfaceAsync")):nullptr;
  if(!activate)return false;
  PhoneAudio::ActivationParams params{1,{GetCurrentProcessId(),0}};
  PROPVARIANT value;PropVariantInit(&value);value.vt=VT_BLOB;value.blob.cbSize=sizeof params;value.blob.pBlobData=reinterpret_cast<BYTE*>(&params);
  auto handler=new PhoneAudio::Completion;IActivateAudioInterfaceAsyncOperation* op=nullptr;bool ok=false;
  if(handler->done&&SUCCEEDED(activate(L"VAD\\Process_Loopback",__uuidof(IAudioClient),&value,handler,&op))&&op&&WaitForSingleObject(handler->done,4000)==WAIT_OBJECT_0){
   HRESULT result=E_FAIL;IUnknown* unknown=nullptr;
   if(SUCCEEDED(op->GetActivateResult(&result,&unknown))&&SUCCEEDED(result)&&unknown)
    ok=SUCCEEDED(unknown->QueryInterface(__uuidof(IAudioClient),reinterpret_cast<void**>(&s.client)));
   PhoneAudio::drop(unknown);
  }
  PhoneAudio::drop(op);handler->Release();
  if(ok){WAVEFORMATEX f{};f.wFormatTag=WAVE_FORMAT_PCM;f.nChannels=2;f.nSamplesPerSec=Rate;f.wBitsPerSample=16;f.nBlockAlign=4;f.nAvgBytesPerSec=Rate*4;ok=startClient(s,&f,AUDCLNT_STREAMFLAGS_LOOPBACK);}
  if(!ok)s.close();return ok;
 }
 static float sampleAt(const BYTE* p,int kind){
  switch(kind){
   case 0:{int16_t v;std::memcpy(&v,p,2);return v/32768.f;}
   case 1:{int32_t v=int32_t(uint32_t(p[0])<<8|uint32_t(p[1])<<16|uint32_t(p[2])<<24);return float(v/2147483648.0);}
   case 2:{int32_t v;std::memcpy(&v,p,4);return float(v/2147483648.0);}
   default:{float v;std::memcpy(&v,p,4);return std::isfinite(v)?v:0.f;}
  }
 }
 // a capture packet onto the timeline (by its QPC time, contiguous within jitter), times target gain
 void ingest(PhoneAudio::Source& s,std::vector<float>& ring,const BYTE* data,UINT32 frames,DWORD flags,UINT64 qpc,float want,std::atomic<float>& peak){
  s.mono.resize(frames);
  if((flags&AUDCLNT_BUFFERFLAGS_SILENT)||!data)std::fill(s.mono.begin(),s.mono.end(),0.f);
  else for(UINT32 i=0;i<frames;++i){float sum=0;const BYTE* f=data+size_t(i)*s.block;for(int c=0;c<s.channels;++c)sum+=sampleAt(f+c*s.bytes,s.kind);s.mono[i]=sum/s.channels;}
  s.out.clear();
  if(s.rate==Rate)s.out=s.mono;
  else if(frames){
   if(!s.primed){s.previous=s.mono[0];s.phase=0;s.primed=true;}
   double step=double(s.rate)/Rate;double p=s.phase;
   while(p<double(frames)-1.0){long i=long(std::floor(p));double f=p-i;float a=i<0?s.previous:s.mono[size_t(i)];float b=s.mono[size_t(i+1)];s.out.push_back(float(a+(b-a)*f));p+=step;}
   s.phase=p-frames;s.previous=s.mono[frames-1];
  }
  int64_t now=PhoneAudio::now100();int64_t at=int64_t(qpc);
  if((flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)||at<=0||std::llabs(at-now)>5000000)at=now-int64_t(frames)*10000000LL/std::max(1,s.rate);
  int64_t pos=(at-base_)*Rate/10000000LL;
  if(s.next<0||pos>s.next+2400||pos<s.next-4800)s.next=pos;
  float g=s.gain,k=1.f-std::exp(-1.f/(0.012f*Rate)),hi=0;const int64_t limit=consumed_+Ring;
  for(size_t i=0;i<s.out.size();++i){
   g+=(want-g)*k;int64_t q=s.next+int64_t(i);float v=s.out[i]*g;
   if(q>=consumed_&&q<limit&&g>1e-5f)ring[size_t(q%Ring)]+=v;
   hi=std::max(hi,std::fabs(v));
  }
  s.gain=g;s.next+=int64_t(s.out.size());peak=std::max(hi,peak.load()*0.9f);
 }
 bool drain(PhoneAudio::Source& s,std::vector<float>& ring,float want,std::atomic<float>& peak){
  if(!s.capture)return true;
  for(int guard=0;guard<64;++guard){
   UINT32 n=0;HRESULT r=s.capture->GetNextPacketSize(&n);if(FAILED(r))return false;if(!n)return true;
   BYTE* data=nullptr;UINT32 frames=0;DWORD flags=0;UINT64 device=0,qpc=0;
   r=s.capture->GetBuffer(&data,&frames,&flags,&device,&qpc);
   if(r==AUDCLNT_S_BUFFER_EMPTY)return true;if(FAILED(r))return false;
   ingest(s,ring,data,frames,flags,qpc,want,peak);s.capture->ReleaseBuffer(frames);
  }
  return true;
 }
 // what the line played (the echo reference) onto the same timeline
 void reference(){
  auto blocks=refTap().take();const int64_t limit=consumed_+Ring;
  for(auto& b:blocks){
   if(b.s.empty()||b.rate<8000)continue;
   int64_t pos=(b.at-base_)*Rate/10000000LL;
   if(refNext_>=0&&std::llabs(pos-refNext_)<960)pos=refNext_;else{refPhase_=0;refPrev_=b.s[0];}
   if(b.rate==Rate){for(size_t i=0;i<b.s.size();++i){int64_t q=pos+int64_t(i);if(q>=consumed_&&q<limit)ref_[size_t(q%Ring)]=b.s[i];}refNext_=pos+int64_t(b.s.size());}
   else{ // 44.1 / 96 kHz devices: linear to 48 kHz, the phase carried on
    const double step=double(b.rate)/Rate;double p=refPhase_;int64_t q=pos;const long n=long(b.s.size());
    while(p<double(n)-1.0){long i=long(std::floor(p));double f=p-i;float a=i<0?refPrev_:b.s[size_t(i)];float c=b.s[size_t(i+1)];
     if(q>=consumed_&&q<limit)ref_[size_t(q%Ring)]=float(a+(c-a)*f);++q;p+=step;}
    refPhase_=p-double(n);refPrev_=b.s.back();refNext_=q;
   }
  }
 }
 // the phone's ear: every voice rendered into the main ring up to nowPos (the recorder's ear())
 void ear(int64_t nowPos){
  std::vector<PhoneAudio::EarCommand> cmds;{std::lock_guard<std::mutex> lock(earMutex_);cmds.swap(earCommands_);}
  for(auto& c:cmds){
   if(c.kind==0){
    if(voices_.size()>=96){size_t q=0;for(size_t i=1;i<voices_.size();++i)if(voices_[i].gainT<voices_[q].gainT)q=i;if(voices_[q].gainT>c.gain)continue;voices_.erase(voices_.begin()+std::ptrdiff_t(q));}
    PhoneAudio::Voice v;v.id=c.id;v.key=c.key;v.at=c.at;v.born=c.born;v.gain=v.gainT=c.gain;v.a=v.aT=PhoneAudio::lowpassCoef(c.lp);v.pitch=v.pitchT=c.pitch;v.loop=c.loop;voices_.push_back(std::move(v));
   }else if(c.kind==3){for(auto& v:voices_)v.stop=true;}
   else for(auto& v:voices_)if(v.id==c.id){if(c.kind==1){v.gainT=c.gain;v.aT=PhoneAudio::lowpassCoef(c.lp);v.pitchT=c.pitch;}else v.stop=true;}
  }
  const float master=earGain_.load();
  const int64_t from=earMark_<0?nowPos:earMark_;const float m0=earMark_<0?master:earFrom_;
  auto masterAt=[&](int64_t q)->float{if(q<=from||nowPos<=from)return q>=nowPos?master:m0;return m0+(master-m0)*float(double(q-from)/double(nowPos-from));};
  const float k=1.f-std::exp(-1.f/(0.015f*Rate));
  const int64_t limit=std::min(nowPos,consumed_+Ring-1);
  for(size_t i=0;i<voices_.size();){
   auto& v=voices_[i];bool done=false;
   if(!v.clip){v.clip=PhoneEar::bank().find(v.key);
    if(!v.clip){if(PhoneAudio::now100()-v.born>30000000LL||PhoneEar::bank().state(v.key)==PhoneEar::Failed){voices_.erase(voices_.begin()+std::ptrdiff_t(i));continue;}++i;continue;}
    v.step=double(v.clip->rate)/Rate;}
   const auto& pcm=v.clip->pcm;const double len=double(pcm.size());const int64_t loopAt=v.loop?v.clip->loopStart:-1;
   if(v.pos<0)v.pos=(v.at-base_)*Rate/10000000LL;
   if(v.pos<consumed_){v.cursor+=double(consumed_-v.pos)*v.step*v.pitch;v.pos=consumed_;}
   auto wrap=[&]()->bool{if(v.cursor<len)return true;if(loopAt>=0&&double(loopAt)<len-1){double span=len-double(loopAt);v.cursor=double(loopAt)+std::fmod(v.cursor-len,span);return true;}return false;};
   if(!wrap())done=true;
   else if(!v.stop&&v.gain<1e-5f&&v.gainT<1e-5f){if(limit>v.pos){v.cursor+=double(limit-v.pos)*v.step*v.pitch;v.pos=limit;}if(!wrap())done=true;}
   else for(int64_t q=v.pos;q<limit;++q){
    v.gain+=(v.gainT-v.gain)*k;v.a+=(v.aT-v.a)*k;v.pitch+=(v.pitchT-v.pitch)*k;
    if(v.stop){v.fade-=1.f/(0.03f*Rate);if(v.fade<=0.f){done=true;v.pos=q;break;}}
    if(!wrap()){done=true;v.pos=q;break;}
    float x=PhoneAudio::hermite(pcm,v.cursor,loopAt);v.y1+=v.a*(x-v.y1);v.y2+=v.a*(v.y1-v.y2);
    main_[size_t(q%Ring)]+=v.y2*v.gain*v.fade*masterAt(q);
    v.cursor+=v.step*double(v.pitch);v.pos=q+1;
   }
   if(done){voices_.erase(voices_.begin()+std::ptrdiff_t(i));continue;}
   ++i;
  }
  earFrom_=master;earMark_=nowPos;earVoices_=int(voices_.size());
 }
 // one 16 kHz sample of the call: phone mic band (80 Hz high-pass), pocket, limiter, ADPCM frames
 void emit16(float x){
  hp_+=0.0309f*(x-hp_);x-=hp_;
  float m=muffle_.load();muff_+=(m-muff_)*0.0024f;const float a=0.297f;lp1_+=a*(x-lp1_);lp2_+=a*(lp1_-lp2_);x+=(lp2_-x)*muff_;
  float ax=std::fabs(x);if(ax>0.85f)x=std::copysign(0.85f+0.15f*std::tanh((ax-0.85f)/0.15f),x);
  if(!std::isfinite(x))x=0;
  outPeak_=std::max(std::fabs(x),outPeak_.load()*0.9995f);
  pcm_[size_t(pcmN_++)]=int16_t(std::lround(std::max(-1.f,std::min(1.f,x))*32767.f));
  if(pcmN_==Frame){
   pcmN_=0;UpFrame f;f.seq=seq_++;encodeFrame(adpcm_,pcm_.data(),f.data.data());++frames_;
   std::lock_guard<std::mutex> lock(outMutex_);if(out_.size()<100)out_.push_back(f);
  }
 }
 void consume(int64_t limit){
  const bool game=game_.capture!=nullptr;const float want=gameGain_.load();const float k=1.f-std::exp(-1.f/(0.02f*NetRate));
  while(consumed_<limit){
   const size_t i=size_t(consumed_%Ring);float m=main_[i],y=loop_[i],r=ref_[i];main_[i]=loop_[i]=ref_[i]=0;++consumed_;
   float m16,y16,r16;bool a=dMain_.push(m,m16);dLoop_.push(y,y16);dRef_.push(r,r16);
   if(!a)continue;
   // the main path waits for the echo canceller (Lead samples late)
   float md=delay_[size_t(delayPos_)];delay_[size_t(delayPos_)]=m16;delayPos_=(delayPos_+1)%Echo::Lead;
   float e=game?echo_.process(y16,r16):0.f;
   gGame_+=(want-gGame_)*k;
   emit16(md+e*gGame_);
  }
 }
 void run(){
  bool com=SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
  base_=PhoneAudio::now100();main_.assign(size_t(Ring),0.f);loop_.assign(size_t(Ring),0.f);ref_.assign(size_t(Ring),0.f);
  try{
   if(wantMic_){mic_.name="microphone";micState_=openMic(mic_)?1:2;if(micState_==2)fail("microphone unavailable (Windows privacy setting / no device)");}
   if(wantGame_){game_.name="game";if(openProcess(game_)){gameState_=1;ref(true);}else{gameState_=3;fail("the game sound of calls needs Windows 10 2004+ (process loopback)");}}
  }catch(...){fail("audio device setup failed");}
  consumed_=(PhoneAudio::now100()-base_)*Rate/10000000LL;
  int retry=0,statN=0;
  while(!quit_.load()){
   HANDLE waits[3];DWORD n=0;if(wake_)waits[n++]=wake_;if(mic_.event)waits[n++]=mic_.event;if(game_.event)waits[n++]=game_.event;
   if(n)WaitForMultipleObjects(n,waits,FALSE,10);else Sleep(10);
   if(quit_.load())break;
   if(PhoneAudio::now100()-lastControl_.load()>40000000LL){fail("call audio watchdog: the game stopped steering it");break;} // never a hot microphone
   if(!drain(mic_,main_,micGain_.load(),micPeak_)){mic_.close();micState_=2;}
   if(!drain(game_,loop_,1.f,gamePeak_)){game_.close();gameState_=3;ref(false);}
   reference();
   if(++retry>=200){retry=0; // a headset plugged in / out: open again (every 2 s)
    if(wantMic_&&!mic_.capture&&micState_==2&&openMic(mic_))micState_=1;
    if(wantGame_&&!game_.capture&&gameState_==3&&openProcess(game_)){gameState_=1;ref(true);}
   }
   const int64_t nowPos=(PhoneAudio::now100()-base_)*Rate/10000000LL;
   ear(nowPos);
   if(consumed_<nowPos-Rate)consumed_=nowPos-Rate; // a stall (the PC hung): no backlog
   consume(nowPos-2400);
   if(++statN>=50){statN=0;std::lock_guard<std::mutex> lock(statMutex_);echoStats_=echo_.stats();}
  }
  ref(false);mic_.close();game_.close();
  if(com)CoUninitialize();
  ended_=true;
 }
};
}
