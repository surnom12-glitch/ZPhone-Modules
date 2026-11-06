
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <filesystem>
#include <stdexcept>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}
namespace fs=std::filesystem;
std::atomic<bool> unloading{false};
void check(int r,const char* op){if(r<0){char text[256];av_strerror(r,text,sizeof text);throw std::runtime_error(std::string(op)+": "+text);}}
struct Session{int width,height,fps,bitrate;std::mutex mutex;bool cancelled=false;std::atomic<uint64_t> processed{0};};
#include <chrono>
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
  const AVCodec* ec=avcodec_find_encoder_by_name(getenv("ENC")?getenv("ENC"):"mpeg4");if(!ec)throw std::runtime_error("FFmpeg lacks h264_mf");
  enc=avcodec_alloc_context3(ec);if(!enc)throw std::bad_alloc();
  enc->width=s->width;enc->height=s->height;enc->pix_fmt=AV_PIX_FMT_YUV420P;enc->time_base={1,30000};enc->framerate={s->fps,1};
  enc->bit_rate=s->bitrate;enc->gop_size=s->fps*2;enc->max_b_frames=0;
  enc->color_range=AVCOL_RANGE_MPEG;enc->colorspace=AVCOL_SPC_BT709;enc->color_primaries=AVCOL_PRI_BT709;enc->color_trc=AVCOL_TRC_BT709;
  enc->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
  {AVDictionary* opts=nullptr;
   int r=avcodec_open2(enc,ec,&opts);av_dict_free(&opts);check(r,"Media Foundation H.264 software encoder (Windows N: install the Media Feature Pack)");}
  check(avformat_alloc_output_context2(&oc,nullptr,"mp4",out.u8string().c_str()),"MP4 context");if(!oc)throw std::bad_alloc();
  AVStream* ovs=avformat_new_stream(oc,nullptr);if(!ovs)throw std::bad_alloc();
  check(avcodec_parameters_from_context(ovs->codecpar,enc),"H.264 parameters");ovs->time_base=enc->time_base;
  AVStream* oas=nullptr;
  if(ai>=0){oas=avformat_new_stream(oc,nullptr);if(!oas)throw std::bad_alloc();
   check(avcodec_parameters_copy(oas->codecpar,ic->streams[ai]->codecpar),"AAC parameters");oas->codecpar->codec_tag=0;oas->time_base=ic->streams[ai]->time_base;}
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
   int64_t p=f->best_effort_timestamp;if(p==AV_NOPTS_VALUE)p=f->pts;if(p==AV_NOPTS_VALUE||p<=lastPts)p=lastPts+1;lastPts=p;o.f->pts=av_rescale_q(p,ivs->time_base,enc->time_base);
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

int main(int argc,char**argv){
 int W=720,H=1260,F=30,N=90;
 // color test
 {AVFrame* f=av_frame_alloc();f->format=AV_PIX_FMT_YUVJ420P;f->width=2;f->height=2;av_frame_get_buffer(f,32);
  struct C{int r,g,b;}cs[]={{255,0,0},{0,255,0},{0,0,255},{255,255,255},{0,0,0},{128,128,128},{200,100,50}};
  for(auto c:cs){uint8_t px[16];for(int i=0;i<4;i++){px[4*i]=c.b;px[4*i+1]=c.g;px[4*i+2]=c.r;px[4*i+3]=255;}
   bgraToJ420(px,8,2,2,f);
   AVFrame* o=av_frame_alloc();o->format=AV_PIX_FMT_YUV420P;o->width=2;o->height=2;av_frame_get_buffer(o,32);j420To709(f,o);
   // reference 709 limited
   double R=c.r/255.,G=c.g/255.,B=c.b/255.;double Y=0.2126*R+0.7152*G+0.0722*B;double Cb=(B-Y)/1.8556,Cr=(R-Y)/1.5748;
   printf("rgb(%3d,%3d,%3d) J:%3d %3d %3d -> 709L: %3d %3d %3d  ref %5.1f %5.1f %5.1f\n",c.r,c.g,c.b,f->data[0][0],f->data[1][0],f->data[2][0],o->data[0][0],o->data[1][0],o->data[2][0],16+219*Y,128+224*Cb,128+224*Cr);
   av_frame_free(&o);}
  av_frame_free(&f);}
 // write an MJPEG+AAC mp4 like the CPU path
 const char* part="clip.mp4.partial";
 AVFormatContext* fmt=nullptr;avformat_alloc_output_context2(&fmt,nullptr,"mp4",part);
 const AVCodec* mj=avcodec_find_encoder_by_name("mjpeg");AVCodecContext* c=avcodec_alloc_context3(mj);
 c->width=W;c->height=H;c->pix_fmt=AV_PIX_FMT_YUVJ420P;c->time_base={1,90000};c->framerate={F,1};c->color_range=AVCOL_RANGE_JPEG;
 int q=4*FF_QP2LAMBDA;c->flags|=AV_CODEC_FLAG_QSCALE|AV_CODEC_FLAG_GLOBAL_HEADER;c->global_quality=q;c->thread_count=cpuThreads(4);c->thread_type=FF_THREAD_SLICE;
 check(avcodec_open2(c,mj,nullptr),"open mjpeg");
 AVStream* vs=avformat_new_stream(fmt,nullptr);vs->time_base=c->time_base;avcodec_parameters_from_context(vs->codecpar,c);
 const AVCodec* ac=avcodec_find_encoder_by_name("aac");AVCodecContext* a=avcodec_alloc_context3(ac);a->sample_rate=48000;a->sample_fmt=AV_SAMPLE_FMT_FLTP;av_channel_layout_default(&a->ch_layout,2);a->bit_rate=128000;a->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;a->time_base={1,48000};
 check(avcodec_open2(a,ac,nullptr),"aac");
 AVStream* as=avformat_new_stream(fmt,nullptr);as->time_base={1,48000};avcodec_parameters_from_context(as->codecpar,a);
 check(avio_open(&fmt->pb,part,AVIO_FLAG_WRITE),"open");check(avformat_write_header(fmt,nullptr),"hdr");
 AVPacket* p=av_packet_alloc();std::vector<uint8_t> bgra(size_t(W)*H*4);
 auto drainv=[&]{while(avcodec_receive_packet(c,p)>=0){av_packet_rescale_ts(p,c->time_base,vs->time_base);p->stream_index=vs->index;check(av_write_frame(fmt,p),"wv");}};
 auto draina=[&]{while(avcodec_receive_packet(a,p)>=0){p->stream_index=as->index;check(av_write_frame(fmt,p),"wa");}};
 int64_t apts=0;
 auto t0=std::chrono::steady_clock::now();
 for(int n=0;n<N;n++){
  for(int y=0;y<H;y++)for(int x=0;x<W;x++){uint8_t* px=&bgra[(size_t(y)*W+x)*4];px[0]=uint8_t(x+n*4);px[1]=uint8_t(y);px[2]=uint8_t((x^y)+n);px[3]=255;}
  AVFrame* f=av_frame_alloc();f->format=AV_PIX_FMT_YUVJ420P;f->width=W;f->height=H;av_frame_get_buffer(f,32);
  bgraToJ420(bgra.data(),size_t(W)*4,W,H,f);f->quality=q;f->pts=int64_t(n)*3000+(n%3);f->color_range=AVCOL_RANGE_JPEG;
  check(avcodec_send_frame(c,f),"sf");av_frame_free(&f);drainv();
  while(apts<int64_t(n+1)*48000/F){AVFrame* af=av_frame_alloc();af->format=AV_SAMPLE_FMT_FLTP;af->nb_samples=a->frame_size;av_channel_layout_copy(&af->ch_layout,&a->ch_layout);af->sample_rate=48000;av_frame_get_buffer(af,0);
   for(int ch=0;ch<2;ch++){float* d=(float*)af->data[ch];for(int i=0;i<af->nb_samples;i++)d[i]=0.2f*sinf((apts+i)*0.05f);}
   af->pts=apts;apts+=af->nb_samples;check(avcodec_send_frame(a,af),"sa");av_frame_free(&af);draina();}
 }
 avcodec_send_frame(c,nullptr);drainv();avcodec_send_frame(a,nullptr);draina();
 check(av_write_trailer(fmt),"trailer");avio_closep(&fmt->pb);avformat_free_context(fmt);
 auto t1=std::chrono::steady_clock::now();
 printf("mjpeg write: %d frames in %.2fs, size %ld\n",N,std::chrono::duration<double>(t1-t0).count(),(long)fs::file_size(part));
 Session s;s.width=W;s.height=H;s.fps=F;s.bitrate=8000000;std::string why;
 bool ok=transcodeH264(&s,part,"clip.mp4",why);
 auto t2=std::chrono::steady_clock::now();
 printf("transcode ok=%d why=%s processed=%llu in %.2fs size %ld\n",ok,why.c_str(),(unsigned long long)s.processed.load(),std::chrono::duration<double>(t2-t1).count(),ok?(long)fs::file_size("clip.mp4"):0L);
 return ok?0:1;
}
