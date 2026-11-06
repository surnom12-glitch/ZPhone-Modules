// fix73: + the phone line (phone_line.h): during a call the sounds around the OTHER phone (reported by
//        the server, shaped in Lua like the clip ear) are mixed and played on this PC's default output
//        device like the phone's speaker: line_open / line_close / line_gain / line_play / line_set /
//        line_stop / line_poll, line_api = 1. Build "fix73-phone-line"; recording is unchanged.
// fix70: no H.264 GPU encoder at all (old / integrated GPUs, drivers without NVENC/AMF/MF): the
//        frame is read back on the encoder worker, written as MJPEG (CPU) while filming, and after
//        the stop converted to a normal H.264 MP4 (Media Foundation software encoder, +faststart)
//        - sound as before. zhphone.encoder(4) = this path first. D3D11 without video support too.
// fix69: + AMD (h264_amf, the same BGRA GPU frames as NVENC) and every other DX11 GPU (h264_mf,
//        Media Foundation, BGRA->NV12 on the GPU in gpu_nv12.h); 32-bit build gmcl_zhphone_win32.dll;
//        zhphone.encoder([0 auto/1 nvenc/2 amf/3 mf]) and poll().encoder.
// fix66: + export_start / export_poll / export_cancel: Save writes a real .mp4 into data/zhunter_saved.
// fix64: + FXAA 3.11 on the recorded video (gpu_fxaa.h, encoder worker, zhphone.fxaa(0/1/2));
//        + the record press adopts a session prepared while the camera was idle (prepare51:
//          shared textures one per frame, D3D11 + NVENC opened on the worker beforehand);
//        + sound files decoded on their own thread, ear_play(..., age) keeps late files in sync;
//        + status49 (no per-frame stats table), cheaper D3D9 Clear hook.
// fix62: + real clip audio (microphone + this game's own sound, see audio_capture.h).
// fix63: + the phone's own ear: game sounds re-played at the phone's position (phone_ear.h).
// fix36: D3D9 command-stream bridge; NO Source interface/vtable calls.
// Gallery JPEG proxies are encoded on the worker only, never on the Lua/render thread.
// Experimental: compiled Windows x64, not validated against a running GMod.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <GarrysMod/Lua/Interface.h>
#include <MinHook.h>
#include "gpu_marker_bridge.h"
#include <cstring>
#include "gpu_thumbnail.h"
#include "gpu_fxaa.h"
#include "gpu_nv12.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
}
#include "audio_capture.h"
#include "phone_line.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <chrono>
static_assert(AV_PIX_FMT_RGB32 == AV_PIX_FMT_BGRA,"Windows little-endian BGRA/NVENC mapping");
using GarrysMod::Lua::ILuaBase;
namespace fs=std::filesystem;
namespace {
constexpr size_t Count=8;
std::atomic<unsigned> hookUsers{0};
struct HookScope {HookScope(){++hookUsers;}~HookScope(){--hookUsers;}};
std::atomic<uint32_t> ticketSerial{1};
std::atomic<IDirect3DDevice9*> observed{nullptr};
std::atomic<bool> unloading{false};
using EndSceneFn=HRESULT(WINAPI*)(IDirect3DDevice9*);
using ResetFn=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
using ResetExFn=HRESULT(WINAPI*)(IDirect3DDevice9Ex*,D3DPRESENT_PARAMETERS*,D3DDISPLAYMODEEX*);
EndSceneFn originalEnd=nullptr;ResetFn originalReset=nullptr;ResetExFn originalResetEx=nullptr;
void* endAddress=nullptr;void* resetAddress=nullptr;void* resetExAddress=nullptr;
using SetTargetFn=HRESULT(WINAPI*)(IDirect3DDevice9*,DWORD,IDirect3DSurface9*);
using ClearFn=HRESULT(WINAPI*)(IDirect3DDevice9*,DWORD,const D3DRECT*,DWORD,D3DCOLOR,float,DWORD);
SetTargetFn originalTarget=nullptr;ClearFn originalClear=nullptr;
void* targetAddress=nullptr;void* clearAddress=nullptr;
HWND dummyWindow=nullptr;
std::atomic<int> encoderPref{0};     // fix69: 0 auto (NVENC, AMF, Media Foundation), 1/2/3 = that one first; fix70: 4 = CPU MJPEG -> H.264 first
std::atomic<int> fxaaMode{1};        // fix64: 0 off, 1 quality, 2 strong; read by the worker per frame
std::atomic<bool> markerBound{false}; // fix64: render target 0 is a 64x64 (marker) surface right now

template<class T>void release(T*& p){if(p){p->Release();p=nullptr;}}
void hr(HRESULT r,const char* op){if(FAILED(r))throw std::runtime_error(std::string(op)+" HRESULT="+std::to_string(uint32_t(r)));}
void check(int r,const char* op){if(r<0){char text[256];av_strerror(r,text,sizeof text);throw std::runtime_error(std::string(op)+": "+text);}}
struct Slot {
 // 0 free, 1 GPU copy outstanding, 2 encoder queued/retained.
 std::atomic<int> state{0},users{0};IDirect3DTexture9* texture=nullptr;IDirect3DSurface9* surface=nullptr;
 IDirect3DQuery9* fence=nullptr;HANDLE handle=nullptr;int64_t pts=0;ULONGLONG issuedAt=0;
};
struct Job {size_t index;int64_t pts;};
struct Ticket {uint32_t color;int64_t pts;ULONGLONG at;};
struct Session {
 int width,height,fps,bitrate;fs::path output,partial;LUID adapter{};
 std::array<Slot,Count> slots;std::mutex mutex;std::condition_variable cv;PhoneGPU::Ring<Job,Count> jobs;std::thread thread;
 bool stopping=false,cancelled=false;std::string error,encoder; // fix69: encoder = the H.264 encoder in use (mutex)
 PhoneGPU::Ring<Ticket,Count*2> tickets; // mutex protected, bounded CPU metadata only
 PhoneGPU::Images<IDirect3DSurface9,Count> images;
 std::atomic<int> initState{0}; // 0 idle, 1 initializing, 2 initialized, 3 cancelled before init
 std::atomic<bool> initialized{false},ready{false},workerDone{false},gpuCleaned{true},finishRequested{false};
 std::atomic<uint64_t> submitted{0},encoded{0},dropped{0},copiesReady{0};
 // fix70: CPU path: the clip is converted to H.264 after the stop (processing = that conversion runs)
 std::atomic<bool> processing{false};std::atomic<uint64_t> processed{0},processTotal{0};
 std::atomic<int> galleryMode{0};std::atomic<bool> galleryDirect{false};std::atomic<uint64_t> galleryReadbackBytes{0};
 // fix64: prepared = made while the camera is idle; the worker waits (all GPU/NVENC work done) for go
 bool preparedMode=false,go=false;size_t made=0;std::atomic<bool> fxaaReady{false};
 std::atomic<double> maxSubmitMs{0};IDirect3DDevice9* device=nullptr;
 // fix62: optional clip audio; owned here, its own capture thread
 std::unique_ptr<PhoneAudio::Recorder> audio;std::atomic<bool> audioMuxBroken{false};
 Session(int w,int h,int f,int b,fs::path path):width(w),height(h),fps(f),bitrate(b),output(std::move(path)){partial=output;partial+=L".partial";}
 void endAudio(){if(audio)audio->cancel();}
 void fail(const std::string& text){std::lock_guard<std::mutex> lock(mutex);if(error.empty())error=text;}
 void stop(bool cancel){ {std::lock_guard<std::mutex> lock(mutex);stopping=true;cancelled|=cancel;}cv.notify_all();}
 void cleanGPU(){images.clear();for(auto& slot:slots){release(slot.fence);release(slot.surface);release(slot.texture);}release(device);gpuCleaned=true;}
 ~Session(){stop(true);if(thread.joinable())thread.join();cleanGPU();}
};
std::shared_ptr<Session> current;
std::shared_ptr<Session> getCurrent(){return std::atomic_load(&current);}
// fix64: the session prepared for the next record press, and sessions being torn down on the render thread
std::shared_ptr<Session> prepared;
std::mutex retiredMutex;std::vector<std::shared_ptr<Session>> retired;
ULONGLONG prepareRetryAt=0;
void retire(std::shared_ptr<Session> s){
 if(!s)return;s->finishRequested=true;s->stop(true);
 std::lock_guard<std::mutex> lock(retiredMutex);retired.push_back(std::move(s));
}
fs::path outputPath(const std::string& name){
 if(name.size()<5||name.size()>120||name.find("..")!=std::string::npos||name.substr(name.size()-4)!=".mp4")throw std::runtime_error("invalid filename");
 for(char c:name)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))throw std::runtime_error("invalid filename");
 wchar_t exe[32768];if(!GetModuleFileNameW(nullptr,exe,32768))throw std::runtime_error("cannot locate game");
 fs::path p=fs::path(exe).parent_path();for(int n=0;n<5;++n,p=p.parent_path())if(fs::is_regular_file(p/L"garrysmod"/L"gameinfo.txt")){
  auto dir=p/L"garrysmod"/L"data"/L"stts_phone_tmp";fs::create_directories(dir);return dir/name;
 }
 throw std::runtime_error("garrysmod/gameinfo.txt not found near executable");
}
// Slots outlive codec and all AVBufferRefs. Buffer data is the stable Slot,
// not a per-frame heap allocation. Callback releases only the slot's GPU lease.
void unlockSlot(void* opaque,uint8_t*){auto slot=static_cast<Slot*>(opaque);if(--slot->users==0)slot->state=0;}
// COM and WIC are private to the encoding worker. Gallery failure must not
// discard a valid MP4. Full-size frames remain GPU-resident for NVENC.
void galleryJPEG(IWICImagingFactory* factory,const D3D11_MAPPED_SUBRESOURCE& mapped,int w,int h,const fs::path& path){
 IWICBitmap* bitmap=nullptr;IWICBitmapScaler* scaler=nullptr;IWICFormatConverter* convert=nullptr;
 IWICStream* stream=nullptr;IWICBitmapEncoder* encoder=nullptr;IWICBitmapFrameEncode* frame=nullptr;IPropertyBag2* props=nullptr;
 try{
  hr(factory->CreateBitmapFromMemory(w,h,GUID_WICPixelFormat32bppBGRA,mapped.RowPitch,mapped.RowPitch*h,static_cast<BYTE*>(mapped.pData),&bitmap),"gallery bitmap");
  if(w!=192||h!=336){hr(factory->CreateBitmapScaler(&scaler),"gallery scaler");hr(scaler->Initialize(bitmap,192,336,WICBitmapInterpolationModeLinear),"gallery resize");}
  hr(factory->CreateFormatConverter(&convert),"gallery converter");hr(convert->Initialize(scaler?static_cast<IWICBitmapSource*>(scaler):static_cast<IWICBitmapSource*>(bitmap),GUID_WICPixelFormat24bppBGR,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"gallery format");
  hr(factory->CreateStream(&stream),"gallery stream");hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"gallery output");
  hr(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&encoder),"gallery JPEG");hr(encoder->Initialize(stream,WICBitmapEncoderNoCache),"gallery JPEG init");
  hr(encoder->CreateNewFrame(&frame,&props),"gallery frame");
  PROPBAG2 option{};option.pstrName=const_cast<LPOLESTR>(L"ImageQuality");VARIANT quality{};quality.vt=VT_R4;quality.fltVal=0.86f;
  hr(props->Write(1,&option,&quality),"gallery quality");hr(frame->Initialize(props),"gallery frame init");hr(frame->SetSize(192,336),"gallery dimensions");
  WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;hr(frame->SetPixelFormat(&format),"gallery pixel format");
  hr(frame->WriteSource(convert,nullptr),"gallery write");hr(frame->Commit(),"gallery frame commit");hr(encoder->Commit(),"gallery commit");
 }catch(...){release(props);release(frame);release(encoder);release(stream);release(convert);release(scaler);release(bitmap);throw;}
 release(props);release(frame);release(encoder);release(stream);release(convert);release(scaler);release(bitmap);
}
// fix70: BGRA (game picture) -> YUVJ420P, BT.601 full range = what every JPEG decoder expects
void bgraToJ420(const uint8_t* src,size_t pitch,int w,int h,AVFrame* f){
 for(int y=0;y<h;y+=2){
  const uint8_t* a=src+size_t(y)*pitch;const uint8_t* b=y+1<h?a+pitch:a;
  uint8_t* ya=f->data[0]+size_t(y)*f->linesize[0];uint8_t* yb=y+1<h?ya+f->linesize[0]:ya;
  uint8_t* u=f->data[1]+size_t(y/2)*f->linesize[1];uint8_t* v=f->data[2]+size_t(y/2)*f->linesize[2];
  for(int x=0;x<w;x+=2){
   const uint8_t* p0=a+4*x;const uint8_t* p1=x+1<w?p0+4:p0;const uint8_t* p2=b+4*x;const uint8_t* p3=x+1<w?p2+4:p2;
   auto Y=[](const uint8_t* p){return uint8_t((19595*p[2]+38470*p[1]+7471*p[0]+32768)>>16);};
   ya[x]=Y(p0);if(x+1<w)ya[x+1]=Y(p1);yb[x]=Y(p2);if(x+1<w)yb[x+1]=Y(p3);
   int R=p0[2]+p1[2]+p2[2]+p3[2],G=p0[1]+p1[1]+p2[1]+p3[1],B=p0[0]+p1[0]+p2[0]+p3[0];
   int cb=(-11059*R-21709*G+32768*B+(128<<18)+(1<<17))>>18,cr=(32768*R-27439*G-5329*B+(128<<18)+(1<<17))>>18;
   u[x/2]=uint8_t(std::clamp(cb,0,255));v[x/2]=uint8_t(std::clamp(cr,0,255));
  }
 }
}
// fix70: decoded JPEG (BT.601 full range) -> YUV420P BT.709 limited range = what players assume for HD H.264
struct To709{int ya,yb,yr,ub,ur,vb,vr;To709(){
  const double kb=-0.7152*0.344136+0.0722*1.772,kr=0.2126*1.402-0.7152*0.714136,ys=219.0/255,cs=224.0/255,S=65536;
  ya=int(std::lround(ys*S));yb=int(std::lround(kb*ys*S));yr=int(std::lround(kr*ys*S));
  ub=int(std::lround((1.772-kb)/1.8556*cs*S));ur=int(std::lround(-kr/1.8556*cs*S));
  vb=int(std::lround(-kb/1.5748*cs*S));vr=int(std::lround((1.402-kr)/1.5748*cs*S));}};
void j420To709(const AVFrame* in,AVFrame* out){
 static const To709 k;const int w=out->width,h=out->height;
 for(int y=0;y<h;++y){
  const uint8_t* ys=in->data[0]+size_t(y)*in->linesize[0];
  const uint8_t* us=in->data[1]+size_t(y/2)*in->linesize[1];const uint8_t* vs=in->data[2]+size_t(y/2)*in->linesize[2];
  uint8_t* yd=out->data[0]+size_t(y)*out->linesize[0];
  for(int x=0;x<w;++x){int cb=us[x/2]-128,cr=vs[x/2]-128;
   yd[x]=uint8_t(std::clamp(((k.ya*ys[x]+k.yb*cb+k.yr*cr+(16<<16)+32768+(256<<16))>>16)-256,16,235));}
  if(!(y&1)){
   uint8_t* ud=out->data[1]+size_t(y/2)*out->linesize[1];uint8_t* vd=out->data[2]+size_t(y/2)*out->linesize[2];
   for(int x=0;x<(w+1)/2;++x){int cb=us[x]-128,cr=vs[x]-128;
    ud[x]=uint8_t(std::clamp(((k.ub*cb+k.ur*cr+(128<<16)+32768+(256<<16))>>16)-256,16,240));
    vd[x]=uint8_t(std::clamp(((k.vb*cb+k.vr*cr+(128<<16)+32768+(256<<16))>>16)-256,16,240));}
  }
 }
}
struct FrameRef{AVFrame* f=nullptr;~FrameRef(){av_frame_free(&f);}};
int cpuThreads(int most){unsigned n=std::thread::hardware_concurrency();return std::max(1,std::min(most,int(n>2?n/2:1)));}
// fix70: the finished MJPEG clip -> H.264 (Media Foundation software encoder = in every Windows
// except N editions without the Media Feature Pack) + the same AAC track, moov first (faststart)
bool transcodeH264(Session* s,const fs::path& in,const fs::path& out,std::string& why){
 AVFormatContext* ic=nullptr;AVFormatContext* oc=nullptr;AVCodecContext* dec=nullptr;AVCodecContext* enc=nullptr;
 AVPacket* pkt=nullptr;AVPacket* opkt=nullptr;AVFrame* df=nullptr;bool ok=false;
 auto aborted=[&]{if(unloading)return true;std::lock_guard<std::mutex> lock(s->mutex);return s->cancelled;};
 try{
  check(avformat_open_input(&ic,in.u8string().c_str(),nullptr,nullptr),"open MJPEG clip");
  int vi=-1,ai=-1;
  for(unsigned n=0;n<ic->nb_streams;++n){auto par=ic->streams[n]->codecpar;
   if(vi<0&&par->codec_type==AVMEDIA_TYPE_VIDEO&&par->codec_id==AV_CODEC_ID_MJPEG)vi=int(n);
   else if(ai<0&&par->codec_type==AVMEDIA_TYPE_AUDIO&&par->codec_id==AV_CODEC_ID_AAC)ai=int(n);}
  if(vi<0)throw std::runtime_error("MJPEG clip has no video track");
  AVStream* ivs=ic->streams[vi];
  const AVCodec* dc=avcodec_find_decoder(AV_CODEC_ID_MJPEG);if(!dc)throw std::runtime_error("FFmpeg lacks the MJPEG decoder");
  dec=avcodec_alloc_context3(dc);if(!dec)throw std::bad_alloc();
  check(avcodec_parameters_to_context(dec,ivs->codecpar),"MJPEG parameters");dec->pkt_timebase=ivs->time_base;dec->thread_count=cpuThreads(4);
  check(avcodec_open2(dec,dc,nullptr),"open MJPEG decoder");
  const AVCodec* ec=avcodec_find_encoder_by_name("h264_mf");if(!ec)throw std::runtime_error("FFmpeg lacks h264_mf");
  enc=avcodec_alloc_context3(ec);if(!enc)throw std::bad_alloc();
  enc->width=s->width;enc->height=s->height;enc->pix_fmt=AV_PIX_FMT_YUV420P;enc->time_base=ivs->time_base;enc->framerate={s->fps,1};
  enc->bit_rate=s->bitrate;enc->gop_size=s->fps*2;enc->max_b_frames=0;
  enc->color_range=AVCOL_RANGE_MPEG;enc->colorspace=AVCOL_SPC_BT709;enc->color_primaries=AVCOL_PRI_BT709;enc->color_trc=AVCOL_TRC_BT709;
  enc->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
  {AVDictionary* opts=nullptr;av_dict_set(&opts,"rate_control","u_vbr",0);av_dict_set(&opts,"scenario","archive",0);av_dict_set(&opts,"hw_encoding","0",0);
   int r=avcodec_open2(enc,ec,&opts);av_dict_free(&opts);check(r,"Media Foundation H.264 software encoder (Windows N: install the Media Feature Pack)");}
  check(avformat_alloc_output_context2(&oc,nullptr,"mp4",out.u8string().c_str()),"MP4 context");if(!oc)throw std::bad_alloc();
  AVStream* ovs=avformat_new_stream(oc,nullptr);if(!ovs)throw std::bad_alloc();
  check(avcodec_parameters_from_context(ovs->codecpar,enc),"H.264 parameters");ovs->time_base=enc->time_base;
  AVStream* oas=nullptr;
  if(ai>=0){oas=avformat_new_stream(oc,nullptr);if(!oas)throw std::bad_alloc();
   check(avcodec_parameters_copy(oas->codecpar,ic->streams[ai]->codecpar),"AAC parameters");oas->codecpar->codec_tag=0;if(oas->codecpar->frame_size<=0)oas->codecpar->frame_size=1024;oas->time_base=ic->streams[ai]->time_base;}
  check(avio_open(&oc->pb,out.u8string().c_str(),AVIO_FLAG_WRITE),"open H.264 MP4");
  {AVDictionary* mo=nullptr;av_dict_set(&mo,"movflags","+faststart",0);int r=avformat_write_header(oc,&mo);av_dict_free(&mo);check(r,"H.264 MP4 header");}
  pkt=av_packet_alloc();opkt=av_packet_alloc();df=av_frame_alloc();if(!pkt||!opkt||!df)throw std::bad_alloc();
  int64_t lastPts=-1;
  auto drainEnc=[&](){for(;;){int rc=avcodec_receive_packet(enc,opkt);if(rc==AVERROR(EAGAIN)||rc==AVERROR_EOF)break;check(rc,"receive H.264 packet");
   av_packet_rescale_ts(opkt,enc->time_base,ovs->time_base);opkt->stream_index=ovs->index;check(av_interleaved_write_frame(oc,opkt),"write H.264 packet");}};
  auto feed=[&](AVFrame* f){
   if((f->format!=AV_PIX_FMT_YUVJ420P&&f->format!=AV_PIX_FMT_YUV420P)||f->width<enc->width||f->height<enc->height)throw std::runtime_error("unexpected MJPEG frame format");
   FrameRef o;o.f=av_frame_alloc();if(!o.f)throw std::bad_alloc();
   o.f->format=AV_PIX_FMT_YUV420P;o.f->width=enc->width;o.f->height=enc->height;check(av_frame_get_buffer(o.f,32),"H.264 frame");
   j420To709(f,o.f);
   int64_t p=f->best_effort_timestamp;if(p==AV_NOPTS_VALUE)p=f->pts;if(p==AV_NOPTS_VALUE||p<=lastPts)p=lastPts+1;lastPts=p;o.f->pts=p;
   int rc=avcodec_send_frame(enc,o.f);if(rc==AVERROR(EAGAIN)){drainEnc();rc=avcodec_send_frame(enc,o.f);}check(rc,"submit H.264 frame");drainEnc();
   ++s->processed;
  };
  auto drainDec=[&](){for(;;){int rc=avcodec_receive_frame(dec,df);if(rc==AVERROR(EAGAIN)||rc==AVERROR_EOF)break;
   if(rc<0)break; // a damaged JPEG is skipped, the clip goes on
   feed(df);av_frame_unref(df);}};
  for(;;){
   if(aborted())throw std::runtime_error("cancelled");
   int rc=av_read_frame(ic,pkt);if(rc==AVERROR_EOF)break;check(rc,"read MJPEG clip");
   if(pkt->stream_index==vi){
    rc=avcodec_send_packet(dec,pkt);if(rc==AVERROR(EAGAIN)){drainDec();rc=avcodec_send_packet(dec,pkt);}
    av_packet_unref(pkt);drainDec();
   }else if(oas&&pkt->stream_index==ai){
    av_packet_rescale_ts(pkt,ic->streams[ai]->time_base,oas->time_base);pkt->stream_index=oas->index;pkt->pos=-1;
    check(av_interleaved_write_frame(oc,pkt),"write AAC packet");
   }else av_packet_unref(pkt);
  }
  avcodec_send_packet(dec,nullptr);drainDec();
  check(avcodec_send_frame(enc,nullptr),"flush H.264 encoder");drainEnc();
  if(s->processed==0)throw std::runtime_error("no frame could be converted");
  check(av_write_trailer(oc),"H.264 MP4 trailer");avio_closep(&oc->pb);
  ok=true;
 }catch(const std::exception& e){why=e.what();}
 av_frame_free(&df);av_packet_free(&pkt);av_packet_free(&opkt);avcodec_free_context(&dec);avcodec_free_context(&enc);
 if(oc){if(oc->pb)avio_closep(&oc->pb);avformat_free_context(oc);}
 avformat_close_input(&ic);
 if(!ok){std::error_code ec;fs::remove(out,ec);}
 return ok;
}
void encode(Session* s){
 AVFormatContext* fmt=nullptr;AVCodecContext* codec=nullptr;AVPacket* packet=nullptr;AVFrame* frame=nullptr;AVStream* stream=nullptr;AVStream* astream=nullptr;
 AVBufferRef* deviceRef=nullptr;AVBufferRef* framesRef=nullptr;
 IDXGIFactory1* factory=nullptr;IDXGIAdapter1* adapter=nullptr;ID3D11Device* d11=nullptr;ID3D11DeviceContext* context=nullptr;
 std::array<ID3D11Texture2D*,Count> images{};
 PhoneGPU::Thumbnail thumbnail;PhoneGPU::FXAA fxaa;PhoneGPU::NV12 nv12;AVBufferRef* framesNV=nullptr;bool useNV12=false;
 ID3D11Texture2D* cpuStage=nullptr;bool cpuMode=false,needTranscode=false;int cpuQuality=0; // fix70
 ID3D11Texture2D* galleryStage=nullptr;IWICImagingFactory* imaging=nullptr;bool com=SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
 fs::path galleryDir;std::ofstream galleryIndex;const AVOutputFormat* mp4=nullptr;
 int64_t nextGallery=0,galleryPTS=0;unsigned galleryCount=0;bool galleryEnabled=false,galleryReady=false,adopted=true;
 size_t galleryPending=Count;
 auto releaseGallery=[&](){if(galleryPending!=Count){auto& slot=s->slots[galleryPending];if(--slot.users==0)slot.state=0;galleryPending=Count;}};
 auto pollGallery=[&](){
  if(galleryPending==Count)return;
  D3D11_MAPPED_SUBRESOURCE mapped{};
  HRESULT rc=context->Map(galleryStage,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
  if(rc==DXGI_ERROR_WAS_STILL_DRAWING)return; // retry once on a later frame; never wait/spin
  if(SUCCEEDED(rc)){
   s->galleryReadbackBytes+=uint64_t(mapped.RowPitch)*(s->galleryMode==2?336:s->height);
   try{std::ostringstream name;name<<std::setw(6)<<std::setfill('0')<<galleryCount<<".jpg";
    galleryJPEG(imaging,mapped,s->galleryMode==2?192:s->width,s->galleryMode==2?336:s->height,galleryDir/name.str());
    galleryIndex<<galleryPTS<<" "<<name.str()<<"\n";++galleryCount;
   }catch(...){galleryEnabled=false;}
   context->Unmap(galleryStage,0);
  }else galleryEnabled=false;
  releaseGallery();
 };
 auto drain=[&](){for(;;){int rc=avcodec_receive_packet(codec,packet);if(rc==AVERROR(EAGAIN)||rc==AVERROR_EOF)break;check(rc,"receive H.264 packet");
  av_packet_rescale_ts(packet,codec->time_base,stream->time_base);packet->stream_index=stream->index;
  // with an audio track: written in arrival order (MP4 tracks are independent), never
  // held back by the interleaver while the audio of the last second is still pending
  check(astream?av_write_frame(fmt,packet):av_interleaved_write_frame(fmt,packet),"write MP4 packet");av_packet_unref(packet);++s->encoded;
 }};
 auto writeAudio=[&](){
  if(!astream)return;
  while(AVPacket* a=s->audio->pop()){
   if(!s->audioMuxBroken){
    av_packet_rescale_ts(a,AVRational{1,PhoneAudio::Rate},astream->time_base);a->stream_index=astream->index;
    if(av_write_frame(fmt,a)<0)s->audioMuxBroken=true; // the video is never lost over its sound
   }
   av_packet_free(&a);
  }
 };
 try{
  if(s->preparedMode){std::lock_guard<std::mutex> lock(s->mutex);if(s->stopping&&!s->go)adopted=false;} // retired before it even began
  if(adopted){
  hr(CreateDXGIFactory1(__uuidof(IDXGIFactory1),reinterpret_cast<void**>(&factory)),"DXGI factory");
  for(UINT n=0;;++n){IDXGIAdapter1* candidate=nullptr;if(factory->EnumAdapters1(n,&candidate)==DXGI_ERROR_NOT_FOUND)break;
   DXGI_ADAPTER_DESC1 desc{};candidate->GetDesc1(&desc);
   if(desc.AdapterLuid.HighPart==s->adapter.HighPart&&desc.AdapterLuid.LowPart==s->adapter.LowPart){adapter=candidate;break;}candidate->Release();
  }
  if(!adapter)throw std::runtime_error("D3D9Ex adapter has no matching DXGI adapter");
  // fix70: old GPUs / drivers have no D3D11 video support: then without it (NVENC/AMF/CPU path still work)
  D3D_FEATURE_LEVEL level;
  if(FAILED(D3D11CreateDevice(adapter,D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_VIDEO_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&d11,&level,&context)))
   hr(D3D11CreateDevice(adapter,D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&d11,&level,&context),"D3D11 device");
  for(size_t n=0;n<Count;++n){hr(d11->OpenSharedResource(s->slots[n].handle,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&images[n])),"OpenSharedResource D3D9Ex/D3D11");
   D3D11_TEXTURE2D_DESC desc{};images[n]->GetDesc(&desc);if(desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM||desc.Width!=UINT(s->width)||desc.Height!=UINT(s->height))throw std::runtime_error("shared GPU texture format mismatch");}
  deviceRef=av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);if(!deviceRef)throw std::bad_alloc();
  auto hwdev=reinterpret_cast<AVHWDeviceContext*>(deviceRef->data);auto dctx=static_cast<AVD3D11VADeviceContext*>(hwdev->hwctx);
  dctx->device=d11;d11->AddRef();dctx->device_context=context;context->AddRef();check(av_hwdevice_ctx_init(deviceRef),"FFmpeg D3D11 device");
  framesRef=av_hwframe_ctx_alloc(deviceRef);if(!framesRef)throw std::bad_alloc();
  auto frames=reinterpret_cast<AVHWFramesContext*>(framesRef->data);frames->format=AV_PIX_FMT_D3D11;frames->sw_format=AV_PIX_FMT_BGRA;
  frames->width=s->width;frames->height=s->height;frames->initial_pool_size=0;check(av_hwframe_ctx_init(framesRef),"D3D11 frame description");
  mp4=av_guess_format("mp4",nullptr,nullptr);if(!mp4)throw std::runtime_error("FFmpeg lacks the MP4 muxer");
  packet=av_packet_alloc();if(!packet)throw std::bad_alloc();
  // fix69: the first H.264 GPU encoder that opens: NVENC (NVIDIA), AMF (AMD) - both read the BGRA
  // frame like before - then Media Foundation (Intel / any DX11 GPU, NV12 made on the GPU)
  {ID3D10Multithread* mt=nullptr;if(SUCCEEDED(d11->QueryInterface(__uuidof(ID3D10Multithread),reinterpret_cast<void**>(&mt)))){mt->SetMultithreadProtected(TRUE);mt->Release();}}
  struct Try{const char* name;int kind;int hw;};
  // fix70: the Media Foundation software encoder is no longer tried live (too slow next to the game);
  // without a GPU encoder the clip is MJPEG while filming and becomes H.264 after the stop
  std::vector<Try> order={{"h264_nvenc",1,-1},{"h264_amf",2,-1},{"h264_mf",3,1},{"mjpeg",4,0}};
  int pref=encoderPref.load();
  if(pref>=1&&pref<=4)std::stable_partition(order.begin(),order.end(),[&](const Try& t){return t.kind==pref;});
  std::string tried;const char* chosen=nullptr;
  for(const Try& t:order){
   const AVCodec* enc=avcodec_find_encoder_by_name(t.name);if(!enc)continue;
   if(t.kind==4){
    // fix70: CPU path. one staging texture: the worker copies the frame into it and reads it back
    if(!cpuStage){D3D11_TEXTURE2D_DESC desc{};images[0]->GetDesc(&desc);
     desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MipLevels=1;desc.ArraySize=1;desc.SampleDesc.Count=1;desc.SampleDesc.Quality=0;
     HRESULT r=d11->CreateTexture2D(&desc,nullptr,&cpuStage);
     if(FAILED(r)){tried+=std::string(tried.empty()?"":"; ")+"mjpeg: CPU readback texture HRESULT="+std::to_string(uint32_t(r));continue;}}
    codec=avcodec_alloc_context3(enc);if(!codec)throw std::bad_alloc();
    codec->width=s->width;codec->height=s->height;codec->pix_fmt=AV_PIX_FMT_YUVJ420P;
    codec->time_base={1,90000};codec->framerate={s->fps,1};
    codec->color_range=AVCOL_RANGE_JPEG;codec->colorspace=AVCOL_SPC_BT470BG;codec->color_primaries=AVCOL_PRI_BT709;codec->color_trc=AVCOL_TRC_BT709;
    cpuQuality=(s->bitrate>=14000000?3:4)*FF_QP2LAMBDA;codec->flags|=AV_CODEC_FLAG_QSCALE;codec->global_quality=cpuQuality;
    codec->thread_count=cpuThreads(4);codec->thread_type=FF_THREAD_SLICE;
    if(mp4->flags&AVFMT_GLOBALHEADER)codec->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
    int result=avcodec_open2(codec,enc,nullptr);
    if(result>=0){chosen=t.name;cpuMode=true;break;}
    char text[256];av_strerror(result,text,sizeof text);
    tried+=std::string(tried.empty()?"":"; ")+"mjpeg: "+text;
    avcodec_free_context(&codec);continue;
   }
   if(t.kind==3&&!framesNV){
    try{nv12.init(d11,context,s->width,s->height);
     framesNV=av_hwframe_ctx_alloc(deviceRef);if(!framesNV)throw std::bad_alloc();
     auto f=reinterpret_cast<AVHWFramesContext*>(framesNV->data);f->format=AV_PIX_FMT_D3D11;f->sw_format=AV_PIX_FMT_NV12;
     f->width=s->width;f->height=s->height;f->initial_pool_size=0;check(av_hwframe_ctx_init(framesNV),"D3D11 NV12 frame description");
    }catch(const std::exception& e){av_buffer_unref(&framesNV);tried+=std::string(tried.empty()?"":"; ")+"mf: "+e.what();continue;}
   }
   codec=avcodec_alloc_context3(enc);if(!codec)throw std::bad_alloc();
   codec->width=s->width;codec->height=s->height;codec->pix_fmt=AV_PIX_FMT_D3D11;
   codec->hw_frames_ctx=av_buffer_ref(t.kind==3?framesNV:framesRef);if(!codec->hw_frames_ctx)throw std::bad_alloc();
   codec->time_base={1,90000};codec->framerate={s->fps,1};codec->bit_rate=s->bitrate;codec->gop_size=s->fps*2;codec->max_b_frames=0;
   codec->color_range=AVCOL_RANGE_MPEG;codec->colorspace=AVCOL_SPC_BT709;codec->color_primaries=AVCOL_PRI_BT709;codec->color_trc=AVCOL_TRC_BT709;
   if(mp4->flags&AVFMT_GLOBALHEADER)codec->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
   AVDictionary* opts=nullptr;
   if(t.kind==1){av_dict_set(&opts,"preset","p4",0);av_dict_set(&opts,"tune","ll",0);av_dict_set(&opts,"rc","vbr",0);
    av_dict_set(&opts,"cq","20",0);av_dict_set(&opts,"rc-lookahead","0",0);av_dict_set(&opts,"delay","0",0);av_dict_set(&opts,"zerolatency","1",0);av_dict_set(&opts,"surfaces","8",0);}
   else if(t.kind==2){codec->rc_max_rate=int64_t(s->bitrate)*3/2;codec->rc_buffer_size=s->bitrate;
    av_dict_set(&opts,"usage","lowlatency",0);av_dict_set(&opts,"quality","balanced",0);av_dict_set(&opts,"rc","vbr_peak",0);}
   else{codec->rc_max_rate=int64_t(s->bitrate)*3/2;codec->rc_buffer_size=s->bitrate;
    av_dict_set(&opts,"rate_control","pc_vbr",0);av_dict_set(&opts,"scenario","camera_record",0);av_dict_set(&opts,"hw_encoding",t.hw?"1":"0",0);}
   int result=avcodec_open2(codec,enc,&opts);av_dict_free(&opts);
   if(result>=0){chosen=t.name;useNV12=t.kind==3;break;}
   char text[256];av_strerror(result,text,sizeof text);
   tried+=std::string(tried.empty()?"":"; ")+t.name+(t.kind==3?(t.hw?" hw":" sw"):"")+": "+text;
   avcodec_free_context(&codec);
  }
  if(!codec)throw std::runtime_error("no video encoder opened (NVENC / AMF / Media Foundation / CPU): "+tried);
  {std::lock_guard<std::mutex> lock(s->mutex);s->encoder=cpuMode?std::string("CPU MJPEG, H.264 after the stop"):std::string(chosen)+(useNV12?" (nv12)":"");
   if(!tried.empty()&&cpuMode)s->encoder+=" [no GPU encoder: "+tried+"]";}
  // Optional, bounded 15 FPS browsing proxy. CPU readback/resize/JPEG occurs
  // only here on this worker; a slow worker drops capture slots, never waits
  // for itself from the render thread. There is no screenshot/backbuffer path.
  try{
   hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&imaging)),"WIC");
   try{thumbnail.init(d11,images,&galleryStage);s->galleryMode=2;s->galleryDirect=thumbnail.directSource();}
   catch(...){
    // Gallery must survive a missing compiler/unsupported shader path. This
    // compatibility fallback is the old worker-only readback, never Lua/GPU hooks.
    release(galleryStage);D3D11_TEXTURE2D_DESC desc{};images[0]->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    hr(d11->CreateTexture2D(&desc,nullptr,&galleryStage),"gallery compatibility staging");s->galleryMode=1;
   }
   galleryReady=true;
  }catch(...){galleryReady=false;}
  // fix64: FXAA is optional: without the shader compiler / on failure the clip is recorded as before
  try{fxaa.init(d11,images,s->width,s->height);s->fxaaReady=true;}catch(...){s->fxaaReady=false;}
  frame=av_frame_alloc();if(!frame)throw std::bad_alloc();
  if(!cpuMode){frame->format=AV_PIX_FMT_D3D11;frame->width=s->width;frame->height=s->height;frame->hw_frames_ctx=av_buffer_ref(useNV12?framesNV:framesRef);if(!frame->hw_frames_ctx)throw std::bad_alloc();}
  // fix64: a prepared session is complete up to here (GPU, NVENC, shaders); it waits for the record
  // press, which gives it the file name. Then only the MP4 header remains: no "starting" frames.
  if(s->preparedMode){std::unique_lock<std::mutex> lock(s->mutex);s->cv.wait(lock,[&]{return s->go||s->stopping;});adopted=s->go;}
  }
  if(adopted){
  galleryDir=s->output;galleryDir.replace_extension(L".proxy");
  check(avformat_alloc_output_context2(&fmt,mp4,nullptr,s->partial.u8string().c_str()),"MP4 context");if(!fmt)throw std::bad_alloc();
  stream=avformat_new_stream(fmt,nullptr);if(!stream)throw std::bad_alloc();
  stream->time_base=codec->time_base;check(avcodec_parameters_from_context(stream->codecpar,codec),"stream parameters");
  // fix62: AAC track next to the video (absent = old FFmpeg DLLs / audio off)
  if(s->audio&&s->audio->params()){
   astream=avformat_new_stream(fmt,nullptr);
   if(astream&&avcodec_parameters_copy(astream->codecpar,s->audio->params())>=0)astream->time_base={1,PhoneAudio::Rate};
   else{astream=nullptr;s->audioMuxBroken=true;}
  }
  check(avio_open(&fmt->pb,s->partial.u8string().c_str(),AVIO_FLAG_WRITE),"open MP4");check(avformat_write_header(fmt,nullptr),"MP4 header");
  if(galleryReady){try{fs::create_directories(galleryDir);galleryIndex.open(galleryDir/L"index.txt",std::ios::trunc);galleryEnabled=bool(galleryIndex);}catch(...){galleryEnabled=false;}}
  s->ready=true;int64_t last=-1;
  for(;;){Job job;
   {std::unique_lock<std::mutex> lock(s->mutex);s->cv.wait(lock,[&]{return s->stopping||!s->jobs.empty();});
    if(s->cancelled)break;if(s->jobs.empty()){if(s->stopping)break;else continue;}job=s->jobs.front();s->jobs.pop_front();}
   s->slots[job.index].users=1; // encoder ownership, plus an optional gallery lease
   pollGallery();
   if(galleryEnabled&&galleryPending==Count&&job.pts>=nextGallery){
    ++s->slots[job.index].users;galleryPending=job.index;galleryPTS=job.pts;nextGallery=job.pts+6000;
    if(s->galleryMode==2)thumbnail.copy(context,job.index,images[job.index],galleryStage);
    else context->CopyResource(galleryStage,images[job.index]);
    context->Flush(); // submit this WORKER's copy; does not wait for GPU completion
   }
   auto& slot=s->slots[job.index];
   // fix64: FXAA into this slot's private texture (free again with the slot, i.e. after NVENC read it)
   ID3D11Texture2D* picture=images[job.index];
   int fx=fxaaMode.load(std::memory_order_relaxed);
   if(fx>0&&fxaa.ready()){picture=fxaa.apply(context,job.index,images[job.index],fx);context->Flush();}
   if(cpuMode){
    // fix70: copy + read back on this worker (Map waits for the copy), the slot is free right after
    context->CopyResource(cpuStage,picture);
    D3D11_MAPPED_SUBRESOURCE mapped{};HRESULT mr=context->Map(cpuStage,0,D3D11_MAP_READ,0,&mapped);
    if(--slot.users==0)slot.state=0;
    hr(mr,"read back the frame");
    FrameRef sw;sw.f=av_frame_alloc();
    if(!sw.f){context->Unmap(cpuStage,0);throw std::bad_alloc();}
    sw.f->format=AV_PIX_FMT_YUVJ420P;sw.f->width=s->width;sw.f->height=s->height;
    int gb=av_frame_get_buffer(sw.f,32);
    if(gb>=0)bgraToJ420(static_cast<const uint8_t*>(mapped.pData),mapped.RowPitch,s->width,s->height,sw.f);
    context->Unmap(cpuStage,0);check(gb,"CPU frame");
    sw.f->color_range=AVCOL_RANGE_JPEG;sw.f->quality=cpuQuality;
    sw.f->pts=std::max(last+1,job.pts);last=sw.f->pts;
    int rc=avcodec_send_frame(codec,sw.f);if(rc==AVERROR(EAGAIN)){drain();rc=avcodec_send_frame(codec,sw.f);}check(rc,"submit CPU frame");drain();
    writeAudio();
    continue;
   }
   if(useNV12){picture=nv12.convert(job.index,picture);context->Flush();} // fix69: Media Foundation takes NV12
   frame->buf[0]=av_buffer_create(reinterpret_cast<uint8_t*>(&slot),sizeof(Slot),unlockSlot,&slot,0);
   if(!frame->buf[0]){if(--slot.users==0)slot.state=0;throw std::bad_alloc();}
   frame->data[0]=reinterpret_cast<uint8_t*>(picture);frame->data[1]=nullptr;frame->linesize[0]=useNV12?s->width:s->width*4;
   frame->pts=std::max(last+1,job.pts);last=frame->pts;
   int rc=avcodec_send_frame(codec,frame);if(rc==AVERROR(EAGAIN)){drain();rc=avcodec_send_frame(codec,frame);}av_buffer_unref(&frame->buf[0]);check(rc,"submit GPU frame");drain();
   writeAudio();
  }
  pollGallery();
  check(avcodec_send_frame(codec,nullptr),"flush H.264 encoder");drain();
  bool cancelled;{std::lock_guard<std::mutex> lock(s->mutex);cancelled=s->cancelled;}
  if(s->audio){
   // exact clip length = last frame + one frame; the audio thread cuts/pads to it
   if(!cancelled&&s->encoded>0&&last>=0){s->audio->finish(last+90000/std::max(1,s->fps));s->audio->waitDone(20000);}
   else s->audio->cancel();
   writeAudio();
  }
  check(av_write_trailer(fmt),"MP4 trailer");avio_closep(&fmt->pb);
  if(!cancelled&&s->encoded>0){if(cpuMode)needTranscode=true;else fs::rename(s->partial,s->output);}
  }
 }catch(const std::exception& e){s->fail(e.what());}
 s->endAudio();
 releaseGallery();
 av_frame_free(&frame);av_packet_free(&packet);avcodec_free_context(&codec);av_buffer_unref(&framesNV);av_buffer_unref(&framesRef);av_buffer_unref(&deviceRef);
 if(fmt){if(fmt->pb)avio_closep(&fmt->pb);avformat_free_context(fmt);}
 galleryIndex.close();release(imaging);release(galleryStage);
 release(cpuStage);
 for(auto& image:images)release(image);release(context);release(d11);release(adapter);release(factory);
 // fix70: the CPU clip becomes H.264 now (GPU/D3D11 already released); a failure keeps the MJPEG clip
 if(needTranscode){
  s->processTotal=s->encoded.load();s->processed=0;s->processing=true;
  fs::path tmp=s->output;tmp+=L".h264.partial";std::string why;
  bool done=transcodeH264(s,s->partial,tmp,why);
  bool cancelled;{std::lock_guard<std::mutex> lock(s->mutex);cancelled=s->cancelled;}
  std::error_code ec;
  if(done){fs::rename(tmp,s->output,ec);if(ec){done=false;why="cannot publish the H.264 clip";fs::remove(tmp,ec);}}
  if(done){std::lock_guard<std::mutex> lock(s->mutex);s->encoder="CPU MJPEG -> H.264 (Media Foundation software)";}
  else if(!cancelled&&!unloading){
   std::error_code e2;fs::rename(s->partial,s->output,e2);
   if(e2)s->fail("cannot publish the clip");
   else{std::lock_guard<std::mutex> lock(s->mutex);s->encoder="CPU MJPEG (H.264 conversion failed: "+why+")";}
  }
  s->processing=false;
 }
 if(com)CoUninitialize();
 if(adopted&&!s->partial.empty()){std::error_code ec;fs::remove(s->partial,ec);}s->ready=false;s->workerDone=true;
}
// fix64: GPU setup in slices instead of everything on the record press: first frame the device
// checks, then the shared textures (1 per frame for a prepared session, 2 for a direct start),
// then the worker thread opens D3D11 + NVENC on its own
void initStep(Session* s){
 if(s->finishRequested){s->workerDone=true;s->endAudio();s->initState=2;return;}
 if(!s->device){
  auto d=observed.load();if(!d)throw std::runtime_error("real D3D9 device not observed yet");
  s->device=d;d->AddRef();s->gpuCleaned=false;
  IDirect3DDevice9Ex* ex=nullptr;hr(d->QueryInterface(__uuidof(IDirect3DDevice9Ex),reinterpret_cast<void**>(&ex)),"D3D9Ex required (plain D3D9 unsupported)");ex->Release();
  IDirect3D9* base=nullptr;hr(d->GetDirect3D(&base),"GetDirect3D");IDirect3D9Ex* api=nullptr;
  auto r=base->QueryInterface(__uuidof(IDirect3D9Ex),reinterpret_cast<void**>(&api));base->Release();hr(r,"D3D9Ex API");
  D3DDEVICE_CREATION_PARAMETERS params{};r=d->GetCreationParameters(&params);if(SUCCEEDED(r))r=api->GetAdapterLUID(params.AdapterOrdinal,&s->adapter);api->Release();hr(r,"GetAdapterLUID");
  if(s->preparedMode)return;
 }
 for(size_t k=s->preparedMode?1:2;k>0&&s->made<Count;--k){
  auto& slot=s->slots[s->made];auto d=s->device;
  hr(d->CreateTexture(s->width,s->height,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&slot.texture,&slot.handle),"create shared GPU texture");
  if(!slot.handle)throw std::runtime_error("driver did not return shared GPU handle");hr(slot.texture->GetSurfaceLevel(0,&slot.surface),"shared surface");hr(d->CreateQuery(D3DQUERYTYPE_EVENT,&slot.fence),"GPU event query");
  ++s->made;
 }
 if(s->made<Count)return;
 s->initialized=true;
 s->thread=std::thread(encode,s);
 s->initState=2;
}
void pump(Session* s){
 if(!s->initialized||s->workerDone||s->gpuCleaned)return;
 for(size_t n=0;n<Count;++n){auto& slot=s->slots[n];if(slot.state!=1)continue;
  // NO FLUSH, NO spin: unfinished work is retried next game frame.
  HRESULT r=slot.fence->GetData(nullptr,0,0);
  if(r==S_OK){{std::lock_guard<std::mutex> lock(s->mutex);if(s->cancelled){slot.state=0;continue;}s->jobs.push_back({n,slot.pts});slot.state=2;}++s->copiesReady;s->cv.notify_one();}
  else if(r==S_FALSE&&GetTickCount64()-slot.issuedAt>5000){s->fail("GPU copy did not complete within 5 seconds");s->stop(true);return;}
  else if(FAILED(r)){s->fail("GPU event query failed (device lost)");s->stop(true);return;}
 }
 if(s->finishRequested){
  bool pending=!s->images.empty();for(auto& slot:s->slots)pending|=slot.state==1;
  {std::lock_guard<std::mutex> lock(s->mutex);pending|=!s->tickets.empty();}
  if(!pending)s->stop(false);
 }
}
// The exact completed phone surface is retained by a tokened command on a
// separate 64x64 marker RT. No marker color is ever drawn into the phone image.
// StretchRect runs AFTER original EndScene, not inside BeginScene/EndScene.
void submit(Session* s,IDirect3DSurface9* source,int64_t pts){
 if(!s->ready||s->workerDone||s->gpuCleaned||!s->device)return;
 size_t n=Count;for(size_t i=0;i<Count;++i){int expected=0;if(s->slots[i].state.compare_exchange_strong(expected,1)){n=i;break;}}
 if(n==Count){++s->dropped;return;}
 auto& slot=s->slots[n];D3DSURFACE_DESC desc{};
 HRESULT rc=source?source->GetDesc(&desc):E_POINTER;
 if(FAILED(rc)||desc.Width!=UINT(s->width)||desc.Height!=UINT(s->height)||desc.MultiSampleType!=D3DMULTISAMPLE_NONE){slot.state=0;s->fail("GPU marker source is not the phone output");s->stop(true);return;}
 LARGE_INTEGER a,b,freq;QueryPerformanceFrequency(&freq);QueryPerformanceCounter(&a);
 rc=s->device->StretchRect(source,nullptr,slot.surface,nullptr,D3DTEXF_NONE);
 if(SUCCEEDED(rc))rc=slot.fence->Issue(D3DISSUE_END);QueryPerformanceCounter(&b);
 s->maxSubmitMs=std::max(s->maxSubmitMs.load(),1000.0*(b.QuadPart-a.QuadPart)/freq.QuadPart);
 if(FAILED(rc)){slot.state=0;s->fail("GPU copy submission failed");s->stop(true);return;}
 slot.pts=pts;slot.issuedAt=GetTickCount64();++s->submitted;
}
HRESULT WINAPI targetHook(IDirect3DDevice9* device,DWORD index,IDirect3DSurface9* surface){
 HookScope guard;
 if(index==0&&surface&&device==observed.load()&&!unloading){
  // fix64: remember whether a 64x64 (marker) target is bound: Clear no longer asks the device
  D3DSURFACE_DESC to{};bool small=SUCCEEDED(surface->GetDesc(&to))&&to.Width==64&&to.Height==64;
  markerBound.store(small,std::memory_order_relaxed);
  auto s=small?getCurrent():std::shared_ptr<Session>{};if(s&&s->ready&&!s->gpuCleaned){
   {
    IDirect3DSurface9* from=nullptr;D3DSURFACE_DESC desc{};
    if(SUCCEEDED(device->GetRenderTarget(0,&from))&&from&&SUCCEEDED(from->GetDesc(&desc))&&desc.Width==UINT(s->width)&&desc.Height==UINT(s->height))s->images.remember(from);
    else if(from!=surface)s->images.remember(nullptr); // preserve source on redundant marker rebind
    release(from);
   }
  }
 }
 return originalTarget(device,index,surface);
}
HRESULT WINAPI clearHook(IDirect3DDevice9* device,DWORD count,const D3DRECT* rects,DWORD flags,D3DCOLOR color,float z,DWORD stencil){
 HookScope guard;
 if((flags&D3DCLEAR_TARGET)&&markerBound.load(std::memory_order_relaxed)&&device==observed.load()&&!unloading){
  auto s=getCurrent();if(s&&s->ready&&!s->gpuCleaned){
   {
    int64_t pts=-1;
    {std::lock_guard<std::mutex> lock(s->mutex);
     for(size_t i=0;i<s->tickets.size();++i)if(PhoneGPU::matches(s->tickets[i].color,color&0xffffff)){pts=s->tickets[i].pts;s->tickets.erase(i);break;}}
    if(pts>=0){
     try{
      int result=s->images.commit(pts);
      if(result==0)throw std::runtime_error("GPU marker has no preceding phone surface");
      if(result>=2)++s->dropped;
     }catch(const std::exception& e){s->images.remember(nullptr);s->fail(e.what());s->stop(true);}
    }
   }
  }
 }
 return originalClear(device,count,rects,flags,color,z,stencil);
}
void renderTick(IDirect3DDevice9* device){
 if(unloading)return;
 // fix64: the prepared session is built in slices while the camera is idle
 if(auto p=std::atomic_load(&prepared)){
  try{
   int expected=0;
   if(!p->finishRequested&&!p->workerDone)p->initState.compare_exchange_strong(expected,1);
   if(p->initState==1)initStep(p.get());
   if(p->workerDone&&!p->gpuCleaned)p->cleanGPU();
  }catch(const std::exception& e){p->fail(e.what());p->stop(true);if(!p->thread.joinable())p->workerDone=true;}
 }
 // fix64: finished/cancelled prepared sessions are released here, never on the Lua thread
 std::vector<std::shared_ptr<Session>> gone;
 {std::lock_guard<std::mutex> lock(retiredMutex);
  for(auto it=retired.begin();it!=retired.end();){
   if((*it)->thread.joinable()&&!(*it)->workerDone){++it;continue;}
   gone.push_back(std::move(*it));it=retired.erase(it);
  }}
 for(auto& r:gone){if(r->thread.joinable())r->thread.join();r->workerDone=true;if(!r->gpuCleaned)r->cleanGPU();}
 gone.clear();
 auto s=getCurrent();if(!s)return;
 try{
  int expected=0;
  if(!s->finishRequested&&!s->workerDone)s->initState.compare_exchange_strong(expected,1);
  if(s->initState==1)initStep(s.get());
  if(s->initialized&&!s->workerDone&&!s->gpuCleaned){
   bool cancelled;{std::lock_guard<std::mutex> lock(s->mutex);
    cancelled=s->cancelled;auto now=GetTickCount64();
    while(!s->tickets.empty()&&now-s->tickets.front().at>3000){s->tickets.pop_front();++s->dropped;if(s->error.empty())s->error="phone GPU marker was not observed";s->stopping=s->cancelled=true;cancelled=true;}}
   if(cancelled)s->cv.notify_all();
   s->images.drain([&](IDirect3DSurface9* source,int64_t pts){if(!cancelled)submit(s.get(),source,pts);});
   pump(s.get());
  }
  if(s->workerDone&&!s->gpuCleaned)s->cleanGPU();
 }catch(const std::exception& e){s->fail(e.what());s->stop(true);if(!s->thread.joinable()){s->workerDone=true;s->endAudio();}}
}
HRESULT WINAPI endHook(IDirect3DDevice9* device){
 HookScope guard;
 if(!observed&&!unloading){D3DDEVICE_CREATION_PARAMETERS p{};
  if(SUCCEEDED(device->GetCreationParameters(&p))&&p.hFocusWindow!=dummyWindow){DWORD process=0;GetWindowThreadProcessId(p.hFocusWindow,&process);
   RECT bounds{};GetClientRect(p.hFocusWindow,&bounds);
   if(process==GetCurrentProcessId()&&IsWindowVisible(p.hFocusWindow)&&bounds.right>=320&&bounds.bottom>=200){IDirect3DDevice9* expected=nullptr;device->AddRef();if(!observed.compare_exchange_strong(expected,device))device->Release();}
  }
 }
 HRESULT result=originalEnd(device);
 if(SUCCEEDED(result)&&device==observed.load()&&!unloading)renderTick(device);
 return result;
}
void beforeReset(IDirect3DDevice9* device){
 markerBound=false;
 auto s=getCurrent();if(s&&s->device==device&&!s->gpuCleaned){s->fail("D3D9 device reset; recording cancelled");s->finishRequested=true;s->stop(true);s->cleanGPU();}
 // fix64: a prepared session is dropped (made again later); retired ones release their textures now
 if(auto p=std::atomic_exchange(&prepared,std::shared_ptr<Session>{})){p->fail("D3D9 device reset");if(!p->gpuCleaned)p->cleanGPU();retire(p);}
 std::lock_guard<std::mutex> lock(retiredMutex);for(auto& r:retired)if(r->device==device&&!r->gpuCleaned)r->cleanGPU();
}
HRESULT WINAPI resetHook(IDirect3DDevice9* device,D3DPRESENT_PARAMETERS* params){HookScope guard;beforeReset(device);return originalReset(device,params);}
HRESULT WINAPI resetExHook(IDirect3DDevice9Ex* device,D3DPRESENT_PARAMETERS* params,D3DDISPLAYMODEEX* mode){HookScope guard;beforeReset(device);return originalResetEx(device,params,mode);}
void installHooks(){
 HMODULE d3d=GetModuleHandleW(L"d3d9.dll");if(!d3d)throw std::runtime_error("D3D9 backend not loaded (DXVK/other renderer not supported)");
 using Create=HRESULT(WINAPI*)(UINT,IDirect3D9Ex**);auto create=reinterpret_cast<Create>(GetProcAddress(d3d,"Direct3DCreate9Ex"));if(!create)throw std::runtime_error("Direct3D9Ex unavailable");
 dummyWindow=CreateWindowExW(0,L"STATIC",L"zhphone probe",WS_POPUP,0,0,16,16,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);if(!dummyWindow)throw std::runtime_error("D3D probe window failed");
 IDirect3D9Ex* api=nullptr;IDirect3DDevice9Ex* dummy=nullptr;HRESULT r=create(D3D_SDK_VERSION,&api);
 if(SUCCEEDED(r)){D3DPRESENT_PARAMETERS p{};p.Windowed=TRUE;p.SwapEffect=D3DSWAPEFFECT_DISCARD;p.hDeviceWindow=dummyWindow;p.BackBufferFormat=D3DFMT_UNKNOWN;
  r=api->CreateDeviceEx(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,dummyWindow,D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_MULTITHREADED,&p,nullptr,&dummy);}
 if(FAILED(r)){release(api);DestroyWindow(dummyWindow);dummyWindow=nullptr;hr(r,"probe D3D9Ex device");}
 auto table=*reinterpret_cast<void***>(dummy);endAddress=table[42];resetAddress=table[16];resetExAddress=table[132];targetAddress=table[37];clearAddress=table[43]; // documented IDirect3DDevice9 COM order
 auto code=MH_Initialize();if(code!=MH_OK&&code!=MH_ERROR_ALREADY_INITIALIZED){release(dummy);release(api);throw std::runtime_error("MinHook initialization failed");}
 code=MH_CreateHook(endAddress,reinterpret_cast<void*>(endHook),reinterpret_cast<void**>(&originalEnd));
 if(code==MH_OK)code=MH_CreateHook(resetAddress,reinterpret_cast<void*>(resetHook),reinterpret_cast<void**>(&originalReset));
 if(code==MH_OK)code=MH_CreateHook(resetExAddress,reinterpret_cast<void*>(resetExHook),reinterpret_cast<void**>(&originalResetEx));
 if(code==MH_OK)code=MH_CreateHook(targetAddress,reinterpret_cast<void*>(targetHook),reinterpret_cast<void**>(&originalTarget));
 if(code==MH_OK)code=MH_CreateHook(clearAddress,reinterpret_cast<void*>(clearHook),reinterpret_cast<void**>(&originalClear));
 if(code==MH_OK)code=MH_EnableHook(targetAddress);if(code==MH_OK)code=MH_EnableHook(clearAddress);
 if(code==MH_OK)code=MH_EnableHook(endAddress);if(code==MH_OK)code=MH_EnableHook(resetAddress);if(code==MH_OK)code=MH_EnableHook(resetExAddress);
 release(dummy);release(api);DestroyWindow(dummyWindow);dummyWindow=nullptr;
 if(code!=MH_OK)throw std::runtime_error("D3D9 observer hook installation failed (conflicting capture module?)");
}
bool removeHooks(){
 void* addresses[]={endAddress,resetAddress,resetExAddress,targetAddress,clearAddress};
 for(auto p:addresses)if(p)MH_DisableHook(p);
 auto deadline=GetTickCount64()+5000;
 while(hookUsers&&GetTickCount64()<deadline)Sleep(1); // unload only, NEVER recording
 if(hookUsers){HMODULE pinned=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&endHook),&pinned);return false;}
 for(auto p:addresses)if(p)MH_RemoveHook(p);
 endAddress=resetAddress=resetExAddress=targetAddress=clearAddress=nullptr;
 auto d=observed.exchange(nullptr);if(d)d->Release();return true;
}
int answer(ILuaBase* L,bool ok,const char* why=nullptr){L->PushBool(ok);if(why){L->PushString(why);return 2;}return 1;}
LUA_FUNCTION(start){
 const char* name=LUA->CheckString(1);int w=int(LUA->CheckNumber(2)),h=int(LUA->CheckNumber(3)),fps=int(LUA->CheckNumber(4)),bitrate=int(LUA->CheckNumber(5));
 int audioFlags=LUA->IsType(6,GarrysMod::Lua::Type::Number)?int(LUA->GetNumber(6))&15:0; // fix62: 1 mic, 2 game, 4 whole-device fallback; fix63: 8 the phone's ear
 auto previous=getCurrent();if(previous&&(!previous->workerDone||!previous->gpuCleaned))return answer(LUA,false,"recorder busy");
 if(w<64||h<64||w>1920||h>1920||(w&1)||(h&1)||fps<1||fps>60||bitrate<500000||bitrate>50000000)return answer(LUA,false,"invalid dimensions/rate");
 std::shared_ptr<Session> s;
 try{
  fs::path path=outputPath(name);
  // fix64: adopt the session prepared while the camera was idle (same size/rate, healthy)
  if(auto p=std::atomic_exchange(&prepared,std::shared_ptr<Session>{})){
   bool usable;{std::lock_guard<std::mutex> lock(p->mutex);usable=p->error.empty()&&!p->stopping;}
   usable=usable&&!p->workerDone&&!p->finishRequested&&p->width==w&&p->height==h&&p->fps==fps&&p->bitrate==bitrate;
   if(usable)s=p;else retire(p);
  }
  if(s){s->output=path;s->partial=path;s->partial+=L".partial";}
  else s=std::make_shared<Session>(w,h,fps,bitrate,path);
  if(audioFlags){fs::path wav=s->output;wav.replace_extension(L".wav");std::error_code ec;fs::remove(wav,ec);s->audio=std::make_unique<PhoneAudio::Recorder>(audioFlags,wav);}
  if(s->preparedMode){{std::lock_guard<std::mutex> lock(s->mutex);s->go=true;}s->cv.notify_all();}
  std::atomic_store(&current,s);return answer(LUA,true);
 }catch(const std::exception& e){if(s&&s->preparedMode)retire(s);return answer(LUA,false,e.what());}
}
// fix64: prepare51(w,h,fps,bitrate) while the camera is idle -> true = a matching session is (being) prepared.
// Shared textures are made one per frame on the render thread, D3D11/NVENC/shaders on its worker.
LUA_FUNCTION(prepare51){
 int w=int(LUA->CheckNumber(1)),h=int(LUA->CheckNumber(2)),fps=int(LUA->CheckNumber(3)),bitrate=int(LUA->CheckNumber(4));
 if(unloading||w<64||h<64||w>1920||h>1920||(w&1)||(h&1)||fps<1||fps>60||bitrate<500000||bitrate>50000000){LUA->PushBool(false);return 1;}
 if(auto p=std::atomic_load(&prepared)){
  bool bad;{std::lock_guard<std::mutex> lock(p->mutex);bad=!p->error.empty()||p->stopping;}
  bad=bad||p->workerDone||p->finishRequested;
  if(!bad&&p->width==w&&p->height==h&&p->fps==fps&&p->bitrate==bitrate){LUA->PushBool(true);return 1;}
  if(bad)prepareRetryAt=GetTickCount64()+10000; // a failing driver is not retried every half second
  retire(std::atomic_exchange(&prepared,std::shared_ptr<Session>{}));
 }
 auto c=getCurrent();if(c&&(!c->workerDone||!c->gpuCleaned)){LUA->PushBool(false);return 1;} // still recording / finishing
 if(GetTickCount64()<prepareRetryAt||!observed.load()){LUA->PushBool(false);return 1;}
 try{auto s=std::make_shared<Session>(w,h,fps,bitrate,fs::path{});s->preparedMode=true;std::atomic_store(&prepared,s);}
 catch(...){LUA->PushBool(false);return 1;}
 LUA->PushBool(true);return 1;
}
LUA_FUNCTION(cancel_prepare51){retire(std::atomic_exchange(&prepared,std::shared_ptr<Session>{}));return 0;}
// fix64: status49() -> ready, done, error: what the recorder needs every frame, without poll()'s table
LUA_FUNCTION(status49){
 auto s=getCurrent();std::string error;if(s){std::lock_guard<std::mutex> lock(s->mutex);error=s->error;}
 LUA->PushBool(s&&s->ready);LUA->PushBool(!s||(s->workerDone&&s->gpuCleaned));LUA->PushString(error.c_str());
 // fix70: 4th/5th = frames converted to H.264 / of (-1 = no conversion running)
 bool proc=s&&s->processing;LUA->PushNumber(proc?double(s->processed.load()):-1);LUA->PushNumber(proc?double(s->processTotal.load()):0);return 5;
}
// fix70: pref 4 = CPU MJPEG -> H.264 first
// fix69: encoder([pref]) -> pref, name of the encoder of the current/last session ("" = none yet)
LUA_FUNCTION(encoder){
 if(LUA->IsType(1,GarrysMod::Lua::Type::Number)){double v=LUA->GetNumber(1);encoderPref=std::isfinite(v)?std::max(0,std::min(4,int(v))):0;}
 auto s=getCurrent();std::string name;if(s){std::lock_guard<std::mutex> lock(s->mutex);name=s->encoder;}
 LUA->PushNumber(encoderPref.load());LUA->PushString(name.c_str());return 2;
}
// fix64: fxaa([mode]) -> mode. 0 off, 1 quality (default), 2 strong. Takes effect on the next frame.
LUA_FUNCTION(fxaa){
 if(LUA->IsType(1,GarrysMod::Lua::Type::Number)){double v=LUA->GetNumber(1);fxaaMode=std::isfinite(v)?std::max(0,std::min(2,int(v))):1;}
 LUA->PushNumber(fxaaMode.load());return 1;
}
LUA_FUNCTION(capture){
 double t=LUA->CheckNumber(1);auto s=getCurrent();if(!s||s->workerDone)return answer(LUA,false,"not recording");
 if(!std::isfinite(t)||t<0||t>600)return answer(LUA,false,"invalid timestamp");
 if(s->audio&&!s->finishRequested){
  // fix62: arg 2 = how old this image is (s); pts 0 on the QPC clock, set once
  double age=LUA->IsType(2,GarrysMod::Lua::Type::Number)?LUA->GetNumber(2):0;if(!std::isfinite(age)||age<0||age>5)age=0;
  s->audio->zero(PhoneAudio::now100()-int64_t(std::llround((age+t)*1e7)));s->audio->seen(t);
 }
 if(!s->ready)return answer(LUA,false,"starting");if(s->finishRequested)return answer(LUA,false,"stopping");
 uint32_t color=PhoneGPU::markerColor(ticketSerial.fetch_add(1));
 {std::lock_guard<std::mutex> lock(s->mutex);
  if(s->tickets.size()>=Count*2){++s->dropped;return answer(LUA,false,"queue full");}
  s->tickets.push_back({color,int64_t(std::llround(t*90000)),GetTickCount64()});}
 LUA->PushBool(true);LUA->PushNumber(color);return 2;
}
void stopEarly(std::shared_ptr<Session> s,bool cancel){
 s->finishRequested=true;
 if(cancel||!s->initialized)s->stop(true);
 int expected=0;if(s->initState.compare_exchange_strong(expected,3)){s->workerDone=true;s->endAudio();}
 if(cancel)s->endAudio();
}
LUA_FUNCTION(finish){if(auto s=getCurrent())stopEarly(s,false);return 0;}
// fix62: audio(micGain, gameGain, muffle[, earGain]) every frame while filming. No I/O, atomics only.
LUA_FUNCTION(audio){
 auto s=getCurrent();if(!s||!s->audio||s->workerDone){LUA->PushBool(false);return 1;}
 s->audio->control(LUA->CheckNumber(1),LUA->CheckNumber(2),LUA->IsType(3,GarrysMod::Lua::Type::Number)?LUA->GetNumber(3):0,
  LUA->IsType(4,GarrysMod::Lua::Type::Number)?LUA->GetNumber(4):0);
 LUA->PushBool(true);return 1;
}
// fix63: the phone's own ear (phone_ear.h). ear_load(key, fileBytes) -> 1 queued, 2 known, 3 refused, (fix64) 4 busy: again later
LUA_FUNCTION(ear_load){
 unsigned int len=0;const char* key=LUA->CheckString(1);const char* data=LUA->GetString(2,&len);
 if(!data){LUA->PushNumber(3);return 1;}
 LUA->PushNumber(PhoneEar::bank().add(key,std::string(data,len)));return 1;
}
// ear_state(key) -> 0 unknown / 1 decoding / 2 ready / 3 unusable, seconds, loops
LUA_FUNCTION(ear_state){
 double seconds=0;bool loop=false;int st=PhoneEar::bank().state(LUA->CheckString(1),&seconds,&loop);
 LUA->PushNumber(st);LUA->PushNumber(seconds);LUA->PushBool(loop);return 3;
}
// ear_play(id, key, gain, lowpassHz, pitch[, loop[, age]]) -> 0 not filming, 1 playing, 2 send the file first, 3 unusable file
// fix64: age = seconds since the game started the sound (its file was still being read)
LUA_FUNCTION(ear_play){
 auto s=getCurrent();if(!s||!s->audio||s->workerDone){LUA->PushNumber(0);return 1;}
 int id=int(LUA->CheckNumber(1));std::string key=LUA->CheckString(2);
 int st=PhoneEar::bank().state(key);
 if(st==PhoneEar::Unknown){LUA->PushNumber(2);return 1;}
 if(st==PhoneEar::Failed){LUA->PushNumber(3);return 1;}
 bool loop=!LUA->IsType(6,GarrysMod::Lua::Type::Bool)||LUA->GetBool(6);
 double age=LUA->IsType(7,GarrysMod::Lua::Type::Number)?LUA->GetNumber(7):0;
 s->audio->earPlay(id,key,LUA->CheckNumber(3),LUA->CheckNumber(4),LUA->CheckNumber(5),loop,age);
 LUA->PushNumber(1);return 1;
}
LUA_FUNCTION(ear_set){
 auto s=getCurrent();if(!s||!s->audio||s->workerDone)return 0;
 s->audio->earSet(int(LUA->CheckNumber(1)),LUA->CheckNumber(2),LUA->CheckNumber(3),LUA->CheckNumber(4));return 0;
}
LUA_FUNCTION(ear_stop){
 auto s=getCurrent();if(!s||!s->audio||s->workerDone)return 0;
 s->audio->earStop(int(LUA->CheckNumber(1)));return 0;
}
// fix73: the phone line. One Player at a time; a closed one finishes on its own thread and is joined
// later (never a wait on the game thread). Lua calls line_gain every frame while a line is heard.
std::mutex line73Mutex;std::shared_ptr<PhoneLine::Player> line73;std::vector<std::shared_ptr<PhoneLine::Player>> line73Old;
void line73Reap(){for(auto it=line73Old.begin();it!=line73Old.end();){if((*it)->ended()){(*it)->join();it=line73Old.erase(it);}else ++it;}}
std::shared_ptr<PhoneLine::Player> line73Get(){std::lock_guard<std::mutex> lock(line73Mutex);if(line73&&line73->ended())return nullptr;return line73;}
// line_open() -> 1 (the line plays, the device opens on its thread), 0 failed
LUA_FUNCTION(line_open){
 std::lock_guard<std::mutex> lock(line73Mutex);line73Reap();
 if(line73&&line73->ended()){line73->join();line73.reset();}
 if(!line73){try{line73=std::make_shared<PhoneLine::Player>();}catch(...){line73.reset();LUA->PushNumber(0);return 1;}}
 line73->keep();LUA->PushNumber(1);return 1;
}
LUA_FUNCTION(line_close){
 std::lock_guard<std::mutex> lock(line73Mutex);
 if(line73){line73->requestClose();line73Old.push_back(line73);line73.reset();}
 line73Reap();return 0;
}
// line_gain(master[, muffle[, band]]): every frame while the line is heard (watchdog: 3 s)
LUA_FUNCTION(line_gain){
 auto p=line73Get();if(!p){LUA->PushBool(false);return 1;}
 double muffle=LUA->IsType(2,GarrysMod::Lua::Type::Number)?LUA->GetNumber(2):0;
 int band=LUA->IsType(3,GarrysMod::Lua::Type::Number)?int(LUA->GetNumber(3)):1;
 p->mixer().control(LUA->CheckNumber(1),muffle,band);p->keep();LUA->PushBool(true);return 1;
}
// line_play(id, key, gain, lowpassHz, pitch[, loop[, age]]) -> 0 no line, 1 playing, 2 send the file first, 3 unusable file
LUA_FUNCTION(line_play){
 auto p=line73Get();if(!p){LUA->PushNumber(0);return 1;}
 int id=int(LUA->CheckNumber(1));std::string key=LUA->CheckString(2);
 int st=PhoneEar::bank().state(key);
 if(st==PhoneEar::Unknown){LUA->PushNumber(2);return 1;}
 if(st==PhoneEar::Failed){LUA->PushNumber(3);return 1;}
 bool loop=!LUA->IsType(6,GarrysMod::Lua::Type::Bool)||LUA->GetBool(6);
 double age=LUA->IsType(7,GarrysMod::Lua::Type::Number)?LUA->GetNumber(7):0;
 p->mixer().play(id,key,LUA->CheckNumber(3),LUA->CheckNumber(4),LUA->CheckNumber(5),loop,age);
 LUA->PushNumber(1);return 1;
}
LUA_FUNCTION(line_set){
 auto p=line73Get();if(!p)return 0;
 p->mixer().set(int(LUA->CheckNumber(1)),LUA->CheckNumber(2),LUA->CheckNumber(3),LUA->CheckNumber(4));return 0;
}
LUA_FUNCTION(line_stop){
 auto p=line73Get();if(!p)return 0;
 if(LUA->IsType(1,GarrysMod::Lua::Type::Number))p->mixer().stop(int(LUA->GetNumber(1)));else p->mixer().stopAll();return 0;
}
// line_poll() -> {open, state (0 starting, 1 playing, 2 no device, 3 ended), error, rate, channels, queuedMs,
//                 underruns, reopened, voices, played, peak, seconds}
LUA_FUNCTION(line_poll){
 auto p=line73Get();LUA->CreateTable();
 LUA->PushBool(p!=nullptr);LUA->SetField(-2,"open");
 if(p){auto s=p->stats();
  auto number=[&](const char* k,double v){LUA->PushNumber(v);LUA->SetField(-2,k);};
  number("state",s.state);number("rate",s.rate);number("channels",s.channels);number("queuedMs",s.queuedMs);
  number("underruns",s.underruns);number("reopened",s.reopened);number("voices",s.mix.voices);number("played",s.mix.played);
  number("peak",s.mix.peak);number("seconds",s.mix.rendered);LUA->PushString(s.error.c_str());LUA->SetField(-2,"error");}
 return 1;
}
// fix66: export_start(src, name) copies a finished clip garrysmod/data/<src> to
// garrysmod/data/zhunter_saved/<name> (.mp4) on its own thread: Lua may not write .mp4 files
// (GMod's extension whitelist), without this a saved clip was "<name>.mp4.dat".
// export_poll() -> {done, bytes, total, error}, export_cancel().
struct Export66{std::thread thread;std::atomic<bool> cancel{false},done{false};std::atomic<uint64_t> bytes{0};uint64_t total=0;std::mutex m;std::string error;};
std::shared_ptr<Export66> export66;
fs::path dataDir66(){
 wchar_t exe[32768];if(!GetModuleFileNameW(nullptr,exe,32768))throw std::runtime_error("cannot locate game");
 fs::path p=fs::path(exe).parent_path();for(int n=0;n<5;++n,p=p.parent_path())if(fs::is_regular_file(p/L"garrysmod"/L"gameinfo.txt"))return p/L"garrysmod"/L"data";
 throw std::runtime_error("garrysmod/gameinfo.txt not found near executable");
}
bool safeName66(const std::string& s,bool slash){
 if(s.empty()||s.size()>200||s.find("..")!=std::string::npos||s[0]=='/')return false;
 for(char c:s)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'||(slash&&c=='/')))return false;
 return true;
}
LUA_FUNCTION(export_start){
 std::string src=LUA->CheckString(1),name=LUA->CheckString(2);
 if(export66&&!export66->done.load())return answer(LUA,false,"export busy");
 if(export66&&export66->thread.joinable())export66->thread.join();
 try{
  if(!safeName66(src,true))throw std::runtime_error("invalid source");
  if(!safeName66(name,false)||name.size()<5||name.substr(name.size()-4)!=".mp4")throw std::runtime_error("invalid filename");
  fs::path data=dataDir66(),from=data,to=data/L"zhunter_saved";
  {std::stringstream ss(src);std::string part;while(std::getline(ss,part,'/'))if(!part.empty())from/=part;}
  std::error_code ec;fs::create_directories(to,ec);to/=name;
  if(!fs::is_regular_file(from))throw std::runtime_error("source clip missing");
  if(fs::exists(to))throw std::runtime_error("destination exists");
  auto e=std::make_shared<Export66>();e->total=uint64_t(fs::file_size(from));
  e->thread=std::thread([e,from,to]{
   fs::path tmp=to;tmp+=L".partial";std::string why;
   try{
    std::ifstream in(from,std::ios::binary);std::ofstream out(tmp,std::ios::binary|std::ios::trunc);
    if(!in)throw std::runtime_error("cannot read the clip");if(!out)throw std::runtime_error("cannot write zhunter_saved");
    std::vector<char> buf(1<<20);
    while(!e->cancel.load()){
     in.read(buf.data(),std::streamsize(buf.size()));std::streamsize n=in.gcount();if(n<=0)break;
     out.write(buf.data(),n);if(!out)throw std::runtime_error("write failed (disk full?)");
     e->bytes+=uint64_t(n);
    }
    out.close();in.close();
    if(e->cancel.load())throw std::runtime_error("cancelled");
    if(e->bytes.load()!=e->total)throw std::runtime_error("clip changed while saving");
    std::error_code ec;fs::rename(tmp,to,ec);if(ec)throw std::runtime_error("cannot publish the mp4");
   }catch(const std::exception& x){why=x.what();std::error_code ec;fs::remove(tmp,ec);}
   {std::lock_guard<std::mutex> lock(e->m);e->error=why;}
   e->done=true;
  });
  export66=e;return answer(LUA,true);
 }catch(const std::exception& x){return answer(LUA,false,x.what());}
}
LUA_FUNCTION(export_poll){
 auto e=export66;LUA->CreateTable();
 LUA->PushBool(!e||e->done.load());LUA->SetField(-2,"done");
 LUA->PushNumber(e?double(e->bytes.load()):0);LUA->SetField(-2,"bytes");
 LUA->PushNumber(e?double(e->total):0);LUA->SetField(-2,"total");
 std::string error;if(e&&e->done.load()){std::lock_guard<std::mutex> lock(e->m);error=e->error;}
 LUA->PushString(error.c_str());LUA->SetField(-2,"error");return 1;
}
LUA_FUNCTION(export_cancel){if(auto e=export66)e->cancel=true;return 0;}
LUA_FUNCTION(cancel){if(auto s=getCurrent())stopEarly(s,true);return 0;}
LUA_FUNCTION(poll){
 // Plain statistics snapshot. NEVER calls any Source/renderer interface.
 auto s=getCurrent();
 LUA->CreateTable();auto boolean=[&](const char* k,bool v){LUA->PushBool(v);LUA->SetField(-2,k);};auto number=[&](const char* k,double v){LUA->PushNumber(v);LUA->SetField(-2,k);};
 boolean("ready",s&&s->ready);boolean("done",!s||(s->workerDone&&s->gpuCleaned));boolean("async_gpu",true);
 number("submitted",s?double(s->submitted.load()):0);number("encoded",s?double(s->encoded.load()):0);number("dropped",s?double(s->dropped.load()):0);
 boolean("galleryDirect",s&&s->galleryDirect);number("galleryMode",s?double(s->galleryMode.load()):0);number("galleryReadbackBytes",s?double(s->galleryReadbackBytes.load()):0);
 number("gpuCopiesReady",s?double(s->copiesReady.load()):0);number("maxSubmitMs",s?s->maxSubmitMs.load():0);
 std::string error,encoder;if(s){std::lock_guard<std::mutex> lock(s->mutex);error=s->error;encoder=s->encoder;number("pending",double(s->jobs.size()));number("pendingMarkers",double(s->tickets.size()));}
 LUA->PushString(encoder.c_str());LUA->SetField(-2,"encoder"); // fix69
 boolean("processing",s&&s->processing);number("processed",s?double(s->processed.load()):0);number("processTotal",s?double(s->processTotal.load()):0); // fix70
 boolean("audio",s&&s->audio);
 boolean("fxaa",s&&s->fxaaReady&&fxaaMode.load()>0);number("fxaaMode",fxaaMode.load());boolean("prepared",s&&s->preparedMode);
 if(s&&s->audio){auto a=s->audio->stats();
  number("audioMic",a.mic);number("audioGame",a.game);number("audioSamples",a.samples);number("audioMicPeak",a.micPeak);number("audioGamePeak",a.gamePeak);
  boolean("audioWav",a.wav);boolean("audioAAC",a.aac&&!s->audioMuxBroken);LUA->PushString(a.error.c_str());LUA->SetField(-2,"audioError");
  number("earVoices",a.earVoices);number("earPlayed",a.earPlayed);number("earPeak",a.earPeak);}
 {int ready=0,failed=0,queued=0;double mb=0;PhoneEar::bank().stats(ready,failed,queued,mb);
  number("earFiles",ready);number("earBadFiles",failed);number("earQueued",queued);number("earMB",mb);}
 LUA->PushString(error.c_str());LUA->SetField(-2,"error");return 1;
}
}
GMOD_MODULE_OPEN(){
 try{
  unloading=false;av_log_set_level(AV_LOG_QUIET);
  installHooks();
 }catch(const std::exception& e){removeHooks();LUA->ThrowError(e.what());return 0;}
 LUA->CreateTable();LUA->PushNumber(3);LUA->SetField(-2,"api_version");LUA->PushBool(true);LUA->SetField(-2,"async_gpu");LUA->PushString("fix73-phone-line");LUA->SetField(-2,"build");LUA->PushNumber(2);LUA->SetField(-2,"encoder_api");LUA->PushNumber(sizeof(void*)*8);LUA->SetField(-2,"bits");LUA->PushNumber(1);LUA->SetField(-2,"export_api");LUA->PushNumber(2);LUA->SetField(-2,"audio_api");LUA->PushNumber(2);LUA->SetField(-2,"ear_api");LUA->PushNumber(1);LUA->SetField(-2,"fxaa_api");LUA->PushNumber(1);LUA->SetField(-2,"line_api");
 #define FN(name) LUA->PushCFunction(name);LUA->SetField(-2,#name)
 FN(start);FN(capture);FN(finish);FN(cancel);FN(poll);FN(audio);FN(ear_load);FN(ear_state);FN(ear_play);FN(ear_set);FN(ear_stop);
 FN(prepare51);FN(cancel_prepare51);FN(status49);FN(fxaa);FN(export_start);FN(export_poll);FN(export_cancel);FN(encoder);
 FN(line_open);FN(line_close);FN(line_gain);FN(line_play);FN(line_set);FN(line_stop);FN(line_poll); // fix73
 LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);LUA->Push(-2);LUA->SetField(-2,"zhphone");LUA->Pop();
 #undef FN
 return 1;
}
GMOD_MODULE_CLOSE(){
 if(auto e=export66){e->cancel=true;if(e->thread.joinable())e->thread.join();} // fix66
 {std::lock_guard<std::mutex> lock(line73Mutex); // fix73: the line ends before the sound bank goes
  if(line73){line73->requestClose();line73Old.push_back(line73);line73.reset();}
  for(auto& p:line73Old){p->requestClose();p->join();}line73Old.clear();}
 unloading=true;auto s=getCurrent();if(s)stopEarly(s,true);
 auto p=std::atomic_exchange(&prepared,std::shared_ptr<Session>{});if(p){p->finishRequested=true;p->stop(true);}
 std::vector<std::shared_ptr<Session>> old;{std::lock_guard<std::mutex> lock(retiredMutex);old.swap(retired);}
 for(auto& r:old)r->stop(true);
 if(p)old.push_back(p);
 if(!removeHooks()){std::lock_guard<std::mutex> lock(retiredMutex);for(auto& r:old)retired.push_back(r);return 0;} // pinned until process exit if a hook is still active
 if(s){if(s->thread.joinable())s->thread.join();s->workerDone=true;s->cleanGPU();}
 for(auto& r:old){if(r->thread.joinable())r->thread.join();r->workerDone=true;r->cleanGPU();}
 old.clear();
 std::atomic_store(&current,std::shared_ptr<Session>{});PhoneEar::bank().shutdown();return 0;
}
