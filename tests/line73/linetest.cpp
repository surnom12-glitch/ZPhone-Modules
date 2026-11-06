// fix73 phone-line test (Linux, no Windows needed): the real src/phone_line.h (Mixer + Player) with
// the real PhoneEar bank (FFmpeg decoding) and a fake WASAPI render device (shim/ + below).
//   ./linetest mixer  : band filters, pocket muffle, pitch, loops + stop fade, age skip, voice cap,
//                       limiter, master, unknown / unusable files
//   ./linetest player : float / int16 5.1 / 24-bit output formats (only front L/R carry the line),
//                       device lost -> reopened, default device changed -> reopened, watchdog,
//                       no audio device at all, close without waiting
#include "windows.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>
#include <string>
std::atomic<int64_t> g_qpcOffset{0};
#include "../../src/phone_line.h"
static int fails=0;
#define CHECK(c,...) do{if(!(c)){++fails;printf("FAIL %s:%d: ",__FILE__,__LINE__);printf(__VA_ARGS__);printf("\n");}else{printf("ok   ");printf(__VA_ARGS__);printf("\n");}}while(0)
static double nowS(){return double(PhoneLine::now100())/1e7;}
// ---------- test sounds: PCM16 mono WAV in memory
static std::string wav(int rate,const std::vector<float>& s){
 std::string o;auto u32=[&](uint32_t v){for(int i=0;i<4;++i)o.push_back(char((v>>(8*i))&255));};auto u16=[&](uint16_t v){o.push_back(char(v&255));o.push_back(char(v>>8));};
 o+="RIFF";u32(uint32_t(36+s.size()*2));o+="WAVEfmt ";u32(16);u16(1);u16(1);u32(uint32_t(rate));u32(uint32_t(rate*2));u16(2);u16(16);o+="data";u32(uint32_t(s.size()*2));
 for(float v:s){int x=int(std::lround(std::max(-1.f,std::min(1.f,v))*32767));u16(uint16_t(int16_t(x)));}
 return o;
}
static std::vector<float> sine(int rate,double hz,double sec,double amp){std::vector<float> s(size_t(rate*sec));for(size_t i=0;i<s.size();++i)s[i]=float(amp*std::sin(6.283185307179586*hz*double(i)/rate));return s;}
static void addWait(const std::string& key,const std::string& bytes){
 auto& B=PhoneEar::bank();B.add(key,bytes);
 for(int i=0;i<500&&B.state(key)==PhoneEar::Decoding;++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));
}
static double rms(const std::vector<float>& v,size_t a,size_t b){double s=0;b=std::min(b,v.size());if(b<=a)return 0;for(size_t i=a;i<b;++i)s+=double(v[i])*v[i];return std::sqrt(s/double(b-a));}
static double peakOf(const std::vector<float>& v,size_t a,size_t b){double p=0;b=std::min(b,v.size());for(size_t i=a;i<b;++i)p=std::max(p,double(std::fabs(v[i])));return p;}
static int crossings(const std::vector<float>& v,size_t a,size_t b){int n=0;b=std::min(b,v.size());for(size_t i=a+1;i<b;++i)if((v[i-1]<0)!=(v[i]<0))++n;return n;}
// renders sec seconds of the mixer in 10 ms blocks (like the device would)
static std::vector<float> run(PhoneLine::Mixer& m,double sec,std::vector<float>* into=nullptr){
 std::vector<float> out;size_t n=size_t(sec*m.rate());std::vector<float> b(480);
 while(out.size()<n){m.render(b.data(),480);out.insert(out.end(),b.begin(),b.end());}
 if(into)into->insert(into->end(),out.begin(),out.end());
 return out;
}
static PhoneLine::Mixer makeMixer(){
 return PhoneLine::Mixer([](const std::string& k){return PhoneEar::bank().find(k);},[](const std::string& k){return PhoneEar::bank().state(k)==PhoneEar::Failed;});
}
static void mixerTests(){
 addWait("t100.wav",wav(44100,sine(44100,100,2.0,0.5)));
 addWait("t1k.wav",wav(44100,sine(44100,1000,2.0,0.5)));
 addWait("t10k.wav",wav(44100,sine(44100,10000,2.0,0.5)));
 addWait("t1k_short.wav",wav(22050,sine(22050,1000,0.2,0.5)));
 PhoneEar::bank().add("junk.wav",std::string(100,'x'));
 for(int i=0;i<200&&PhoneEar::bank().state("junk.wav")==PhoneEar::Decoding;++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));
 CHECK(PhoneEar::bank().state("t1k.wav")==PhoneEar::Ready,"test tones decoded by the bank");
 // full band: the tone as it is (gain 1, 0.5 amplitude -> rms 0.354)
 for(int band=0;band<3;++band){
  double r[3];const char* keys[3]={"t100.wav","t1k.wav","t10k.wav"};
  for(int k=0;k<3;++k){auto m=makeMixer();m.setRate(48000);m.control(1,0,band);m.play(1,keys[k],1,20000,1,false,0);auto o=run(m,0.6);r[k]=rms(o,48000*3/10,48000*55/100);}
  printf("     band %d: 100 Hz %.3f, 1 kHz %.3f, 10 kHz %.3f\n",band,r[0],r[1],r[2]);
  if(band==0)CHECK(std::fabs(r[0]-0.354)<0.02&&std::fabs(r[1]-0.354)<0.02&&std::fabs(r[2]-0.354)<0.03,"band 0 (full): every tone passes");
  if(band==1)CHECK(r[1]>0.33&&r[0]<0.25&&r[2]<0.22,"band 1 (HD 120-7000 Hz): 1 kHz passes, 100 Hz and 10 kHz are cut");
  if(band==2)CHECK(r[1]>0.30&&r[0]<0.02&&r[2]<0.02,"band 2 (old phone 300-3400 Hz): 100 Hz / 10 kHz almost gone");
 }
 { // per-voice low-pass (a far / occluded sound): 10 kHz through lp 1300
  auto m=makeMixer();m.setRate(48000);m.control(1,0,0);m.play(1,"t10k.wav",1,1300,1,false,0);auto o=run(m,0.5);
  CHECK(rms(o,14400,24000)<0.03,"voice low-pass 1300 Hz cuts a 10 kHz tone (%.4f)",rms(o,14400,24000));
 }
 { // pocket muffle (0 -> 1, 120 ms crossfade): 10 kHz goes, 1 kHz is quieter
  auto m=makeMixer();m.setRate(48000);m.control(1,0,0);m.play(1,"t10k.wav",1,20000,1,false,0);m.play(2,"t1k.wav",0,20000,1,false,0);
  auto a=run(m,0.3);m.control(1,1,0);auto b=run(m,0.8);
  CHECK(rms(a,4800,14400)>0.33&&rms(b,28800,38400)<0.02,"muffle: 10 kHz %.3f -> %.4f",rms(a,4800,14400),rms(b,28800,38400));
 }
 { // pitch 2 = twice the frequency
  auto m=makeMixer();m.setRate(48000);m.control(1,0,0);m.play(1,"t1k.wav",1,20000,2,false,0);auto o=run(m,0.5);
  int c=crossings(o,4800,4800+48000/10);CHECK(c>=395&&c<=405,"pitch 2: %d zero crossings in 100 ms (2 kHz = 400)",c);
 }
 { // a short sound ends by itself; the voice is removed
  auto m=makeMixer();m.setRate(48000);m.control(1,0,0);m.play(1,"t1k_short.wav",1,20000,1,false,0);auto o=run(m,0.5);
  CHECK(rms(o,2400,7200)>0.3&&peakOf(o,48000*25/100,o.size())<1e-4&&m.stats().voices==0,"short sound: plays 0.2 s, then silent, voice gone (voices %d)",m.stats().voices);
 }
 { // loop (loop flag, the bank's loop point: none -> loop = no loop) - stop fades out in 30 ms
  auto m=makeMixer();m.setRate(48000);m.control(1,0,0);m.play(7,"t1k.wav",1,20000,1,true,0);auto o=run(m,0.3);m.stop(7);auto p=run(m,0.1);
  CHECK(rms(o,4800,14400)>0.33&&peakOf(p,48000*4/100,p.size())<1e-4&&m.stats().voices==0,"stop: faded out within 40 ms (%.5f), voice removed",peakOf(p,48000*4/100,p.size()));
  m.play(8,"t1k.wav",1,20000,1,true,0);m.play(9,"t100.wav",1,20000,1,true,0);run(m,0.05);m.stopAll();auto q=run(m,0.1);
  CHECK(peakOf(q,48000*4/100,q.size())<1e-4&&m.stats().voices==0,"stopAll: every voice faded out");
 }
 { // gain changes are smoothed (no click): set 1 -> 0.25
  auto m=makeMixer();m.setRate(48000);m.control(1,0,0);m.play(1,"t1k.wav",1,20000,1,false,0);auto a=run(m,0.2);m.set(1,0.25,20000,1);auto b=run(m,0.3);
  double before=rms(a,4800,9600),after=rms(b,9600,24000);double jump=0;for(size_t i=1;i<240;++i)jump=std::max(jump,double(std::fabs(b[i]-b[i-1])));
  CHECK(std::fabs(after/before-0.25)<0.02&&jump<0.08,"set gain 0.25: ratio %.3f, largest step %.3f",after/before,jump);
 }
 { // age: a sound started 0.5 s ago starts in the middle (the second half of a 1 s ramp)
  std::vector<float> ramp(44100);for(size_t i=0;i<ramp.size();++i)ramp[i]=float(i)/44100.f*0.8f;addWait("ramp.wav",wav(44100,ramp));
  auto m=makeMixer();m.setRate(48000);m.control(1,0,0);run(m,0.3); // the line is already open (master faded in)
  m.play(1,"ramp.wav",1,20000,1,false,0.5);auto o=run(m,0.2);
  double first=0;for(size_t i=200;i<400;++i)first+=o[i];first/=200;
  CHECK(std::fabs(first-0.4)<0.03,"age 0.5 s: starts at the middle of the file (level %.3f, expected 0.40)",first);
 }
 { // a file still decoding waits; an unusable one is dropped; an unknown key drops after 10 s
  auto m=makeMixer();m.setRate(48000);m.control(1,0,0);m.play(1,"junk.wav",1,20000,1,false,0);m.play(2,"never.wav",1,20000,1,false,0);run(m,0.05);
  CHECK(m.stats().voices==1,"unusable file dropped at once, unknown file waits (voices %d)",m.stats().voices);
  g_qpcOffset+=110000000LL;run(m,0.02);g_qpcOffset-=110000000LL;
  CHECK(m.stats().voices==0,"unknown file given up after 10 s");
 }
 { // at most 96 voices; the quietest goes
  auto m=makeMixer();m.setRate(48000);m.control(1,0,0);for(int i=0;i<120;++i)m.play(100+i,"t1k.wav",0.001*(i+1),20000,1,true,0);run(m,0.05);
  CHECK(m.stats().voices==96&&m.stats().played==120,"voice cap: %d voices (each louder one replaced the quietest: played %d)",m.stats().voices,m.stats().played);
  m.play(999,"t1k.wav",0.0001,20000,1,true,0);run(m,0.02);
  CHECK(m.stats().voices==96&&m.stats().played==120,"voice cap: a quieter one than all 96 is not started");
 }
 { // limiter: way too loud = never above 1, never NaN; master 0 = silent; master smooths
  auto m=makeMixer();m.setRate(48000);m.control(4,0,0);for(int i=0;i<8;++i)m.play(i,"t1k.wav",4,20000,1,true,0);auto o=run(m,0.3);
  bool finite=true;for(float v:o)if(!std::isfinite(v))finite=false;
  CHECK(finite&&peakOf(o,0,o.size())<=1.0001,"limiter: peak %.4f with 128x the level",peakOf(o,0,o.size()));
  m.control(0,0,0);auto z=run(m,0.5);CHECK(peakOf(z,48000*4/10,z.size())==0,"master 0: silent (%.6f)",peakOf(z,48000*4/10,z.size()));
  auto n=makeMixer();n.setRate(48000);n.control(1,0,0);n.play(1,"t1k.wav",1,20000,1,true,0);run(n,0.2);n.control(0,0,0);auto y=run(n,0.4);
  double jump=0;for(size_t i=1;i<y.size();++i)jump=std::max(jump,double(std::fabs(y[i]-y[i-1])));
  CHECK(jump<0.07&&peakOf(y,48000*3/10,y.size())==0,"master 1 -> 0 smoothed (largest step %.3f; 1 kHz itself: 0.065), then silent",jump);
 }
 { // bad input from Lua: NaN / inf / out of range never break the mix
  auto m=makeMixer();m.setRate(48000);m.control(NAN,NAN,99);m.play(1,"t1k.wav",NAN,NAN,NAN,true,NAN);m.play(2,"t1k.wav",INFINITY,-5,-1,true,-3);run(m,0.1);m.control(1,0,-4);auto o=run(m,0.2);
  bool finite=true;for(float v:o)if(!std::isfinite(v))finite=false;
  CHECK(finite&&peakOf(o,0,o.size())<=1.0001,"NaN / inf arguments: output finite (peak %.3f)",peakOf(o,0,o.size()));
 }
 { // other output rates: 44.1 kHz and 96 kHz keep the pitch
  for(int rate:{44100,96000}){auto m=makeMixer();m.setRate(rate);m.control(1,0,0);m.play(1,"t1k.wav",1,20000,1,false,0);std::vector<float> o;size_t n=size_t(rate/2);std::vector<float> b(256);while(o.size()<n){m.render(b.data(),256);o.insert(o.end(),b.begin(),b.end());}
   int c=crossings(o,size_t(rate/10),size_t(rate/10+rate/10));CHECK(c>=198&&c<=202,"output %d Hz: 1 kHz stays 1 kHz (%d crossings / 100 ms)",rate,c);}
 }
}
// ---------- the fake Windows playback device
struct Fake{
 std::mutex m;int rate=48000,ch=2,kind=3;bool ext=true;std::wstring id=L"{speakers}";bool noCom=false,noDevice=false;int failPad=0;int opens=0;
 std::vector<std::vector<float>> got; // per channel, everything the line wrote
 void reset(int r,int c,int k,bool e){std::lock_guard<std::mutex> l(m);rate=r;ch=c;kind=k;ext=e;got.assign(size_t(c),{});id=L"{speakers}";noCom=noDevice=false;failPad=0;opens=0;}
} F;
template<class I>struct Com:I{std::atomic<ULONG> refs{1};HRESULT QueryInterface(REFIID,void**)override{return E_NOINTERFACE;}ULONG AddRef()override{return ++refs;}ULONG Release()override{ULONG r=--refs;if(!r)delete this;return r;}};
struct FakeClient;
struct FakeRender:Com<IAudioRenderClient>{
 FakeClient* c;std::vector<BYTE> buf;explicit FakeRender(FakeClient* x):c(x){}
 HRESULT GetBuffer(UINT32 n,BYTE** d)override;HRESULT ReleaseBuffer(UINT32 n,DWORD)override;
};
struct FakeClient:Com<IAudioClient>{
 int rate,ch,kind,bytes;bool ext,init=false,started=false;double pad=0,last=0;UINT32 frames=0;
 FakeClient(){std::lock_guard<std::mutex> l(F.m);rate=F.rate;ch=F.ch;kind=F.kind;ext=F.ext;bytes=kind==0?2:kind==1?3:4;}
 void drain(){double t=nowS();if(started)pad=std::max(0.0,pad-(t-last)*rate);last=t;}
 HRESULT Initialize(AUDCLNT_SHAREMODE,DWORD,REFERENCE_TIME dur,REFERENCE_TIME,const WAVEFORMATEX* f,const GUID*)override{
  if(!f||int(f->nSamplesPerSec)!=rate||f->nChannels!=ch)return E_FAIL;frames=UINT32(double(dur)/1e7*rate);init=true;return S_OK;}
 HRESULT GetBufferSize(UINT32* n)override{*n=frames;return init?S_OK:E_FAIL;}
 HRESULT GetCurrentPadding(UINT32* n)override{
  {std::lock_guard<std::mutex> l(F.m);if(F.failPad>0){--F.failPad;return E_FAIL;}}
  drain();*n=UINT32(pad);return S_OK;}
 HRESULT GetMixFormat(WAVEFORMATEX** out)override{
  auto x=static_cast<WAVEFORMATEXTENSIBLE*>(calloc(1,sizeof(WAVEFORMATEXTENSIBLE)));
  x->Format.wFormatTag=ext?WAVE_FORMAT_EXTENSIBLE:(kind==3?3:WAVE_FORMAT_PCM);x->Format.nChannels=WORD(ch);x->Format.nSamplesPerSec=DWORD(rate);
  x->Format.wBitsPerSample=WORD(bytes*8);x->Format.nBlockAlign=WORD(bytes*ch);x->Format.nAvgBytesPerSec=DWORD(rate*bytes*ch);x->Format.cbSize=ext?22:0;
  x->wValidBitsPerSample=WORD(bytes*8);x->SubFormat.Data1=kind==3?3u:1u;*out=&x->Format;return S_OK;}
 HRESULT GetService(REFIID,void** out)override{*out=static_cast<IAudioRenderClient*>(new FakeRender(this));return S_OK;}
 HRESULT Start()override{if(started)return E_FAIL;started=true;last=nowS();return S_OK;}
 HRESULT Stop()override{started=false;return S_OK;}
};
HRESULT FakeRender::GetBuffer(UINT32 n,BYTE** d){c->drain();if(c->pad+n>c->frames+0.5)return E_FAIL;buf.assign(size_t(n)*size_t(c->bytes*c->ch),0xAB);*d=buf.data();return S_OK;}
HRESULT FakeRender::ReleaseBuffer(UINT32 n,DWORD){
 std::lock_guard<std::mutex> l(F.m);
 for(UINT32 i=0;i<n;++i)for(int k=0;k<c->ch&&k<int(F.got.size());++k){
  const BYTE* p=buf.data()+(size_t(i)*size_t(c->ch)+size_t(k))*size_t(c->bytes);float v=0;
  if(c->kind==0){int16_t s;std::memcpy(&s,p,2);v=s/32767.f;}
  else if(c->kind==1){int32_t s=int32_t(uint32_t(p[0])<<8|uint32_t(p[1])<<16|uint32_t(p[2])<<24)>>8;v=float(s/8388607.0);}
  else if(c->kind==2){int32_t s;std::memcpy(&s,p,4);v=float(s/2147483647.0);}
  else std::memcpy(&v,p,4);
  F.got[size_t(k)].push_back(v);
 }
 c->pad+=n;return S_OK;
}
struct FakeDevice:Com<IMMDevice>{
 HRESULT Activate(REFIID,DWORD,PROPVARIANT*,void** out)override{{std::lock_guard<std::mutex> l(F.m);++F.opens;}*out=static_cast<IAudioClient*>(new FakeClient());return S_OK;}
 HRESULT GetId(LPWSTR* out)override{std::lock_guard<std::mutex> l(F.m);size_t n=F.id.size()+1;auto s=static_cast<wchar_t*>(malloc(n*sizeof(wchar_t)));std::wmemcpy(s,F.id.c_str(),n);*out=s;return S_OK;}
};
struct FakeEnum:Com<IMMDeviceEnumerator>{
 HRESULT GetDefaultAudioEndpoint(EDataFlow,ERole,IMMDevice** out)override{{std::lock_guard<std::mutex> l(F.m);if(F.noDevice){*out=nullptr;return E_FAIL;}}*out=new FakeDevice();return S_OK;}
};
HRESULT CoCreateInstance(REFIID,void*,DWORD,REFIID,void** out){{std::lock_guard<std::mutex> l(F.m);if(F.noCom){*out=nullptr;return E_FAIL;}}*out=static_cast<IMMDeviceEnumerator*>(new FakeEnum());return S_OK;}
// keeps the player alive (like Lua's line_gain every frame) for sec seconds, calling at(t) on the way
template<class Fn>static void drive(PhoneLine::Player& p,double sec,Fn at){double t0=nowS();for(;;){double t=nowS()-t0;if(t>sec)break;p.keep();p.mixer().control(1,0,0);at(t);std::this_thread::sleep_for(std::chrono::milliseconds(16));}}
static std::vector<float> channel(int k){std::lock_guard<std::mutex> l(F.m);return F.got[size_t(k)];}
static void playerTests(){
 struct Fmt{int rate,ch,kind;bool ext;const char* name;};
 for(Fmt f:{Fmt{48000,2,3,true,"48 kHz stereo float"},Fmt{44100,6,0,false,"44.1 kHz 5.1 int16"},Fmt{96000,2,1,true,"96 kHz stereo 24-bit"},Fmt{48000,1,2,true,"48 kHz mono int32"}}){
  F.reset(f.rate,f.ch,f.kind,f.ext);
  {PhoneLine::Player p;bool played=false,stopped=false;
   drive(p,1.2,[&](double t){if(!played&&t>=0.3){p.mixer().play(1,"t1k.wav",0.5,20000,1,true,0);played=true;}if(!stopped&&t>=0.8){p.mixer().stop(1);stopped=true;}});
   auto s=p.stats();auto L=channel(0);size_t r=size_t(f.rate);
   // the device got ~what was played: 0.5 * 0.354 rms in the middle, silence after the stop
   double mid=rms(L,size_t(0.45*r),size_t(0.7*r)),tail=L.size()>size_t(1.0*r)?peakOf(L,size_t(0.95*r),L.size()):1;
   CHECK(s.state==1&&s.rate==f.rate&&s.channels==f.ch,"%s: playing at %d Hz x%d",f.name,s.rate,s.channels);
   CHECK(std::fabs(mid-0.177)<0.02&&tail<2e-3,"%s: line level %.3f (expected 0.177), after the stop %.5f",f.name,mid,tail);
   CHECK(s.underruns<=1&&s.queuedMs>20&&s.queuedMs<=110,"%s: underruns %d, queued %.0f ms",f.name,s.underruns,s.queuedMs);
   if(f.ch>=2){auto R=channel(1);double d=0;for(size_t i=0;i<std::min(L.size(),R.size());++i)d=std::max(d,double(std::fabs(L[i]-R[i])));CHECK(d<1e-6,"%s: left = right",f.name);}
   double other=0;for(int k=2;k<f.ch;++k){auto c=channel(k);other=std::max(other,peakOf(c,0,c.size()));}
   if(f.ch>2)CHECK(other==0,"%s: centre / LFE / rear silent (%.6f)",f.name,other);
   double t0=nowS();p.requestClose();p.join();CHECK(p.ended()&&nowS()-t0<0.1,"%s: closed in %.0f ms",f.name,(nowS()-t0)*1000);}
 }
 { // the device is lost for a moment (unplugged): reopened, the sound goes on
  F.reset(48000,2,3,true);PhoneLine::Player p;bool a=false,b=false;
  drive(p,1.6,[&](double t){if(!a&&t>=0.2){p.mixer().play(1,"t1k.wav",0.5,20000,1,true,0);a=true;}if(!b&&t>=0.5){std::lock_guard<std::mutex> l(F.m);F.failPad=1;b=true;}});
  auto s=p.stats();auto L=channel(0);
  CHECK(s.reopened>=1&&s.state==1&&F.opens>=2,"device lost: reopened %d, state %d",s.reopened,s.state);
  CHECK(rms(L,L.size()-48000*3/10,L.size())>0.15,"device lost: the line plays again (%.3f)",rms(L,L.size()-48000*3/10,L.size()));
 }
 { // another default output (headset plugged in): moved to it within ~2 s
  F.reset(48000,2,3,true);PhoneLine::Player p;bool a=false;
  drive(p,2.8,[&](double t){if(!a&&t>=0.3){std::lock_guard<std::mutex> l(F.m);F.id=L"{headset}";a=true;}});
  auto s=p.stats();CHECK(s.reopened==1&&s.state==1&&F.opens==2,"default device changed: reopened %d, opens %d",s.reopened,F.opens);
 }
 { // watchdog: Lua stopped calling (the game froze / the addon broke): the line ends by itself
  F.reset(48000,2,3,true);PhoneLine::Player p;p.mixer().control(1,0,0);p.mixer().play(1,"t1k.wav",0.5,20000,1,true,0);
  double t0=nowS();while(!p.ended()&&nowS()-t0<5)std::this_thread::sleep_for(std::chrono::milliseconds(20));
  auto s=p.stats();CHECK(p.ended()&&s.state==3&&nowS()-t0>2.9&&s.error.find("watchdog")!=std::string::npos,"watchdog: ended after %.2f s (%s)",nowS()-t0,s.error.c_str());
 }
 { // no Windows audio at all / no playback device: state 2, nothing breaks, time passes for the voices
  F.reset(48000,2,3,true);{std::lock_guard<std::mutex> l(F.m);F.noDevice=true;}
  PhoneLine::Player p;p.mixer().play(1,"t1k_short.wav",1,20000,1,false,0);drive(p,0.6,[](double){});
  auto s=p.stats();CHECK(s.state==2&&!s.error.empty()&&s.mix.voices==0,"no device: state %d (%s), the short sound passed silently (voices %d)",s.state,s.error.c_str(),s.mix.voices);
  {std::lock_guard<std::mutex> l(F.m);F.noDevice=false;}drive(p,2.3,[](double){});
  CHECK(p.stats().state==1,"no device -> a device appears: playing (state %d)",p.stats().state);
  F.reset(48000,2,3,true);{std::lock_guard<std::mutex> l(F.m);F.noCom=true;}
  PhoneLine::Player q;drive(q,0.2,[](double){});CHECK(q.stats().state==2,"no Windows audio: state %d (%s)",q.stats().state,q.stats().error.c_str());
  double t0=nowS();q.requestClose();q.join();CHECK(nowS()-t0<0.1,"closed in %.0f ms",(nowS()-t0)*1000);
 }
 { // destroyed while playing: no wait, no leak (ASan)
  F.reset(48000,2,3,true);auto p=std::make_unique<PhoneLine::Player>();drive(*p,0.2,[&](double){});p->mixer().play(1,"t1k.wav",1,20000,1,true,0);
  double t0=nowS();p.reset();CHECK(nowS()-t0<0.1,"destructor while playing: %.0f ms",(nowS()-t0)*1000);
 }
}
int main(int argc,char** argv){
 std::string mode=argc>1?argv[1]:"all";
 if(mode=="mixer"||mode=="all")mixerTests();
 if(mode=="player"||mode=="all"){if(mode=="player")mixerTests();playerTests();}
 PhoneEar::bank().clear();
 printf(fails?"FAILED %d\n":"ALL OK\n",fails);
 return fails?1:0;
}
