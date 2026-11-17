#include "windows.h"
#include <cmath>
#include <cstdio>
#include <vector>
std::atomic<int64_t> g_qpcOffset{0};
static int64_t T0;
static int64_t q100(){LARGE_INTEGER c;QueryPerformanceCounter(&c);return c.QuadPart;}
#include "../../src/audio_capture.h"
extern "C"{
#include <libavformat/avformat.h>
}
struct FakeCapture;
template<class Base>struct Ref:Base{std::atomic<ULONG> refs{1};HRESULT QueryInterface(REFIID,void** o)override{*o=this;AddRef();return S_OK;}ULONG AddRef()override{return ++refs;}ULONG Release()override{ULONG n=--refs;if(!n)delete this;return n;}};
struct FakeCapture:Ref<IAudioCaptureClient>{
 int rate,ch,kind;double latency;bool game;int64_t start=0,produced=0;std::vector<BYTE> buf;int packet;
 FakeCapture(int r,int c,int k,bool g):rate(r),ch(c),kind(k),latency(0.012),game(g),packet(r/100){}
 double sig(double t){ // t = seconds since T0
  if(!game){double f=std::fmod(t,1.0);return (t>0.5&&f<0.001)?0.9:0.0;}
  return 0.3*std::sin(2*M_PI*440*t);
 }
 bool window(double t){return !game||(t>=1.8&&t<3.0);}
 HRESULT GetNextPacketSize(UINT32* n)override{
  for(;;){double t=double(produced+packet)/rate;double now=(q100()-start)/1e7;
   if(now<t+latency){*n=0;return S_OK;}
   double abs=(start-T0)/1e7+double(produced)/rate;
   if(!window(abs)){produced+=packet;continue;} // a silent loopback sends nothing
   *n=packet;return S_OK;}
 }
 HRESULT GetBuffer(BYTE** d,UINT32* frames,DWORD* flags,UINT64* dev,UINT64* qpc)override{
  int bytes=kind==3?4:2;buf.assign(size_t(packet)*ch*bytes,0);
  for(int i=0;i<packet;++i){double abs=(start-T0)/1e7+double(produced+i)/rate;double v=sig(abs);
   for(int c=0;c<ch;++c){BYTE* p=buf.data()+(size_t(i)*ch+c)*bytes;if(kind==3){float f=float(v);std::memcpy(p,&f,4);}else{int16_t s=int16_t(std::lround(v*32767));std::memcpy(p,&s,2);}}}
  *d=buf.data();*frames=packet;*flags=0;*dev=produced;*qpc=UINT64(start+int64_t(double(produced)*1e7/rate));return S_OK;
 }
 HRESULT ReleaseBuffer(UINT32 n)override{produced+=n;return S_OK;}
};
struct FakeClient:Ref<IAudioClient>{
 bool game;FakeCapture* cap=nullptr;FakeClient(bool g):game(g){}
 ~FakeClient(){if(cap)cap->Release();}
 HRESULT Initialize(AUDCLNT_SHAREMODE,DWORD flags,REFERENCE_TIME,REFERENCE_TIME,const WAVEFORMATEX* f,const GUID*)override{
  printf("Initialize %s flags=%x rate=%u ch=%u bits=%u\n",game?"game":"mic",flags,f->nSamplesPerSec,f->nChannels,f->wBitsPerSample);
  cap=new FakeCapture(f->nSamplesPerSec,f->nChannels,f->wBitsPerSample==32?3:0,game);return S_OK;}
 HRESULT GetMixFormat(WAVEFORMATEX** out)override{
  auto f=static_cast<WAVEFORMATEXTENSIBLE*>(malloc(sizeof(WAVEFORMATEXTENSIBLE)));std::memset(f,0,sizeof *f);
  if(game){f->Format.wFormatTag=WAVE_FORMAT_PCM;f->Format.nChannels=2;f->Format.nSamplesPerSec=44100;f->Format.wBitsPerSample=16;f->Format.nBlockAlign=4;f->Format.cbSize=0;}
  else{f->Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE;f->Format.nChannels=2;f->Format.nSamplesPerSec=48000;f->Format.wBitsPerSample=32;f->Format.nBlockAlign=8;f->Format.cbSize=22;f->SubFormat.Data1=3;}
  *out=&f->Format;return S_OK;}
 HRESULT SetEventHandle(HANDLE)override{return S_OK;}
 HRESULT GetService(REFIID,void** o)override{cap->AddRef();*o=static_cast<IAudioCaptureClient*>(cap);return S_OK;}
 HRESULT Start()override{cap->start=q100();return S_OK;}
 HRESULT Stop()override{return S_OK;}
};
struct FakeDevice:Ref<IMMDevice>{bool game;FakeDevice(bool g):game(g){}
 HRESULT Activate(REFIID,DWORD,PROPVARIANT*,void** o)override{*o=static_cast<IAudioClient*>(new FakeClient(game));return S_OK;}};
struct FakeEnum:Ref<IMMDeviceEnumerator>{
 HRESULT GetDefaultAudioEndpoint(EDataFlow f,ERole,IMMDevice** o)override{*o=new FakeDevice(f==eRender);return S_OK;}};
HRESULT CoCreateInstance(REFIID,void*,DWORD,REFIID,void** out){*out=static_cast<IMMDeviceEnumerator*>(new FakeEnum);return S_OK;}
int main(int argc,char** argv){
 T0=q100();
 std::string mode=argc>1?argv[1]:"";
 if(mode=="cancel"||mode=="nozero"||mode=="watchdog"||mode=="destroy"){
  auto rec=std::make_unique<PhoneAudio::Recorder>(7,"x.wav");
  for(int i=0;i<60;++i){if(mode!="watchdog")rec->control(1,1,0);std::this_thread::sleep_for(std::chrono::milliseconds(16));}
  if(mode=="cancel")rec->cancel();
  if(mode=="nozero")rec->finish(90000);
  if(mode=="watchdog"){bool d=rec->waitDone(12000);printf("watchdog done=%d err=%s\n",d,rec->stats().error.c_str());}
  else if(mode!="destroy"){bool d=rec->waitDone(5000);printf("%s done=%d wav=%d err=%s\n",mode.c_str(),d,rec->stats().wav,rec->stats().error.c_str());}
  int n=0;while(AVPacket* p=rec->pop()){av_packet_free(&p);++n;}
  rec.reset();printf("packets=%d destroyed\n",n);return 0;
 }
 auto rec=std::make_unique<PhoneAudio::Recorder>(7,"out.wav");
 bool zeroSet=false;
 for(;;){double t=(q100()-T0)/1e7;if(t>4.3)break;
  rec->control(1.0,1.0,0.0);
  if(t>=0.4){if(!zeroSet){rec->zero(q100()-1000000);zeroSet=true;}if(t<3.6)rec->seen(t-0.3);}
  std::this_thread::sleep_for(std::chrono::milliseconds(16));}
 rec->finish(int64_t(3.0*90000));
 bool ok=rec->waitDone(20000);auto st=rec->stats();
 printf("done=%d mic=%d game=%d samples=%.0f wav=%d aac=%d err='%s'\n",ok,st.mic,st.game,st.samples,st.wav,st.aac,st.error.c_str());
 // mux like module.cpp: video first (during the clip), the audio tail later, av_write_frame
 AVFormatContext* fmt=nullptr;avformat_alloc_output_context2(&fmt,nullptr,"mp4","out.mp4");
 const AVCodec* venc=avcodec_find_encoder(AV_CODEC_ID_MPEG4);AVCodecContext* vc=avcodec_alloc_context3(venc);
 vc->width=64;vc->height=112;vc->pix_fmt=AV_PIX_FMT_YUV420P;vc->time_base={1,30};vc->framerate={30,1};vc->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
 if(avcodec_open2(vc,venc,nullptr)<0){puts("venc");return 1;}
 AVStream* vs=avformat_new_stream(fmt,nullptr);vs->time_base=vc->time_base;avcodec_parameters_from_context(vs->codecpar,vc);
 AVStream* as=avformat_new_stream(fmt,nullptr);avcodec_parameters_copy(as->codecpar,rec->params());as->time_base={1,48000};
 avio_open(&fmt->pb,"out.mp4",AVIO_FLAG_WRITE);if(avformat_write_header(fmt,nullptr)<0){puts("hdr");return 1;}
 AVFrame* fr=av_frame_alloc();fr->format=vc->pix_fmt;fr->width=64;fr->height=112;av_frame_get_buffer(fr,0);AVPacket* pk=av_packet_alloc();
 int na=0;auto audio=[&](){while(AVPacket* a=rec->pop()){av_packet_rescale_ts(a,AVRational{1,48000},as->time_base);a->stream_index=as->index;if(av_write_frame(fmt,a)<0)puts("awrite");av_packet_free(&a);++na;}};
 for(int i=0;i<=90;++i){
  if(i<90){av_frame_make_writable(fr);for(int y=0;y<112;++y)memset(fr->data[0]+y*fr->linesize[0],(i*3)&255,64);fr->pts=i;avcodec_send_frame(vc,fr);}else avcodec_send_frame(vc,nullptr);
  while(avcodec_receive_packet(vc,pk)>=0){av_packet_rescale_ts(pk,vc->time_base,vs->time_base);pk->stream_index=vs->index;if(av_write_frame(fmt,pk)<0)puts("vwrite");av_packet_unref(pk);}
  if(i==45)audio();
 }
 audio();printf("audio packets=%d\n",na);
 av_write_trailer(fmt);avio_closep(&fmt->pb);avformat_free_context(fmt);
 rec.reset();puts("OK");return 0;
}
