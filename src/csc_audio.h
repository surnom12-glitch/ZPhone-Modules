// fix74: Cross Server Calls (CSC) - the device independent sound of a call between two GMod servers
// (Linux-tested, tests/csc74): 16 kHz mono in 20 ms frames, IMA-ADPCM (4 bit, 64 kbit/s: the shipped
// FFmpeg has no voice decoder), 48 -> 16 kHz decimation, the receive side (jitter buffer, loss
// concealment, clock drift, resampling to the output device) and the echo canceller of the uplink
// (the "what you hear" capture of GMod also contains the line that plays the other side).
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
namespace Csc {
constexpr int NetRate=16000;
constexpr int Frame=320;              // 20 ms
constexpr int FrameBytes=4+Frame/2;   // predictor (int16 LE), step index, flags, 320 x 4 bit
// ---------------------------------------------------------------- IMA-ADPCM (every frame self-contained)
static const int16_t kStep[89]={7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
static const int8_t kIndex[16]={-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
struct Adpcm{int pred=0,index=0;};
inline void adpcmApply(Adpcm& s,int code){
 int step=kStep[s.index];int d=step>>3;if(code&4)d+=step;if(code&2)d+=step>>1;if(code&1)d+=step>>2;
 s.pred+=(code&8)?-d:d;s.pred=std::max(-32768,std::min(32767,s.pred));
 s.index=std::max(0,std::min(88,s.index+kIndex[code&15]));
}
inline int adpcmCode(Adpcm& s,int x){
 int step=kStep[s.index];int diff=x-s.pred;int code=0;if(diff<0){code=8;diff=-diff;}
 if(diff>=step){code|=4;diff-=step;}step>>=1;
 if(diff>=step){code|=2;diff-=step;}step>>=1;
 if(diff>=step)code|=1;
 adpcmApply(s,code);return code;
}
// 320 samples -> 164 bytes; the state goes on from frame to frame, the header makes a lost frame harmless
inline void encodeFrame(Adpcm& s,const int16_t* in,uint8_t* out){
 out[0]=uint8_t(s.pred&255);out[1]=uint8_t((s.pred>>8)&255);out[2]=uint8_t(s.index);out[3]=0;
 for(int i=0;i<Frame;i+=2){int a=adpcmCode(s,in[i]),b=adpcmCode(s,in[i+1]);out[4+i/2]=uint8_t(a|(b<<4));}
}
inline bool decodeFrame(const uint8_t* in,size_t len,float* out){
 if(len<size_t(FrameBytes))return false;
 Adpcm s;s.pred=int16_t(uint16_t(in[0])|uint16_t(in[1])<<8);s.index=in[2];if(s.index>88)return false;
 for(int i=0;i<Frame;i+=2){uint8_t b=in[4+i/2];adpcmApply(s,b&15);out[i]=float(s.pred)/32768.f;adpcmApply(s,b>>4);out[i+1]=float(s.pred)/32768.f;}
 return true;
}
// ---------------------------------------------------------------- 48 kHz -> 16 kHz (windowed sinc, 7.2 kHz)
class Decimate3{
public:
 Decimate3(){
  const int N=Taps;double sum=0;
  for(int i=0;i<N;++i){double m=i-(N-1)/2.0;double x=2*0.15*m;double s=std::fabs(m)<1e-9?1.0:std::sin(3.141592653589793*x)/(3.141592653589793*x);
   double w=0.42-0.5*std::cos(6.283185307179586*i/(N-1))+0.08*std::cos(12.566370614359172*i/(N-1));h_[i]=float(2*0.15*s*w);sum+=h_[i];}
  for(auto& v:h_)v=float(v/sum);
 }
 // one 48 kHz sample in; true = a 16 kHz sample came out
 bool push(float x,float& y){
  hist_[pos_]=x;pos_=(pos_+1)%Taps;
  if(++phase_<3)return false;phase_=0;
  double acc=0;int p=pos_;for(int i=0;i<Taps;++i){acc+=double(h_[i])*hist_[p];p=(p+1)%Taps;}
  y=float(acc);return true;
 }
private:
 static constexpr int Taps=63;std::array<float,Taps> h_{},hist_{};int pos_=0,phase_=0;
};
// ---------------------------------------------------------------- the receive side of one stream
inline int64_t extendSeq(int64_t last,uint16_t seq){
 if(last<0)return int64_t(seq)+65536*16; // room below for an early reorder
 return last+int16_t(uint16_t(seq-uint16_t(last&65535)));
}
struct StreamStats{int frames=0,lost=0,late=0,concealed=0,dropped=0,underruns=0;double bufferMs=0,targetMs=0;bool playing=false;};
// frames arrive in any order on the network thread, the output device pulls any number of samples
// at any rate; ~40..240 ms of buffer (by the measured jitter), drift is followed by a tiny speed change
class StreamIn{
public:
 explicit StreamIn(double startGain=1):gain_(float(startGain)),gainT_(float(startGain)){}
 // at = arrival time in seconds (any clock that runs on)
 void push(uint16_t seq,const uint8_t* data,size_t len,double at){
  std::array<float,Frame> f;if(!decodeFrame(data,len,f.data()))return;
  std::lock_guard<std::mutex> lock(m_);
  const int64_t ext=extendSeq(lastSeq_,seq);
  if(lastSeq_>=0&&(ext>lastSeq_+3000||ext<lastSeq_-3000)){reset();return push2(extendSeq(-1,seq),f,at);} // the other side restarted
  push2(ext,f,at);
 }
 void setGain(double g){gainT_=float(std::min(4.0,std::max(0.0,std::isfinite(g)?g:0.0)));}
 double lastPush()const{return lastAt_.load();}
 StreamStats stats(){std::lock_guard<std::mutex> lock(m_);StreamStats s=stats_;s.playing=playing_;s.bufferMs=bufferedLocked()*1000.0/NetRate;s.targetMs=target_*1000.0/NetRate;return s;}
 // adds n samples at rate into out
 void render(float* out,int n,int rate){
  if(n<=0||rate<=0)return;
  std::lock_guard<std::mutex> lock(m_);
  if(rate!=rate_){rate_=rate;lp1_.design(rate);lp2_.design(rate);}
  const double base=double(NetRate)/double(rate);
  if(!playing_){
   if(!frames_.empty()&&bufferedLocked()>=target_){playing_=true;next_=frames_.begin()->first;pos_=1.0;}
   else{decay(out,n);return;}
  }
  const double buf=bufferedLocked();
  double corr=std::max(-1.0,std::min(1.0,(buf-target_)/std::max(1.0,target_)))*0.012; // +-1.2 % speed
  if(buf>target_+NetRate*0.12)skipLocked(int64_t(target_+NetRate*0.02)); // far too much: jump
  const double step=base*(1.0+corr);
  const float k=1.f-std::exp(-1.f/(0.02f*float(rate)));
  for(int i=0;i<n;++i){
   while(fifo_.size()<size_t(pos_)+4){if(!refill())break;}
   if(fifo_.size()<size_t(pos_)+3){ // starved even after concealment: stop, buffer again
    playing_=false;++stats_.underruns;fifo_.clear();pos_=1.0;concealed_=0;decay(out+i,n-i);return;}
   size_t ip=size_t(pos_);float t=float(pos_-double(ip));
   float xm1=fifo_[ip-1],x0=fifo_[ip],x1=fifo_[ip+1],x2=fifo_[ip+2];
   float c1=0.5f*(x1-xm1),c2=xm1-2.5f*x0+2.f*x1-0.5f*x2,c3=0.5f*(x2-xm1)+1.5f*(x0-x1);
   float y=((c3*t+c2)*t+c1)*t+x0;
   if(rate>NetRate*5/4)y=lp2_.run(lp1_.run(y)); // the images above 8 kHz
   gain_+=(gainT_-gain_)*k;y*=gain_;last_=y;
   if(std::isfinite(y))out[i]+=y;
   pos_+=step;
   size_t drop=size_t(pos_)>2?size_t(pos_)-2:0;
   if(drop>=Frame){fifo_.erase(fifo_.begin(),fifo_.begin()+std::ptrdiff_t(drop));pos_-=double(drop);}
  }
 }
private:
 struct LP{float b0=1,b1=0,b2=0,a1=0,a2=0,z1=0,z2=0;
  void design(int rate){double hz=7300;if(hz>=rate*0.45){b0=1;b1=b2=a1=a2=0;return;}
   const double w=6.283185307179586*hz/rate,c=std::cos(w),al=std::sin(w)/(2*0.7071067811865476),a0=1+al;
   b0=float((1-c)/2/a0);b1=float((1-c)/a0);b2=b0;a1=float(-2*c/a0);a2=float((1-al)/a0);z1=z2=0;}
  float run(float x){float y=b0*x+z1;z1=b1*x-a1*y+z2;z2=b2*x-a2*y;if(!std::isfinite(z1)||!std::isfinite(z2)){z1=z2=0;y=0;}return y;}};
 std::mutex m_;std::map<int64_t,std::array<float,Frame>> frames_;std::deque<float> fifo_;
 int64_t lastSeq_=-1,next_=-1;bool playing_=false;double pos_=1.0;int concealed_=0;int rate_=0;
 float gain_=1,gainT_=1,last_=0;LP lp1_,lp2_;std::array<float,Frame> lastFrame_{};
 double target_=NetRate*0.06,jit_=0,minLate_=1e9;double minAt_=0;std::atomic<double> lastAt_{-1};StreamStats stats_;
 void reset(){frames_.clear();fifo_.clear();lastSeq_=-1;next_=-1;playing_=false;pos_=1.0;concealed_=0;jit_=0;minLate_=1e9;}
 double bufferedLocked()const{double s=double(fifo_.size())-pos_;if(!frames_.empty()){int64_t from=playing_?next_:frames_.begin()->first;s+=double(std::max<int64_t>(0,frames_.rbegin()->first+1-from))*Frame;}return std::max(0.0,s);}
 void push2(int64_t ext,const std::array<float,Frame>& f,double at){
  lastAt_=at;++stats_.frames;
  if(playing_&&ext<next_){++stats_.late;return;}
  if(frames_.size()>200)frames_.erase(frames_.begin());
  frames_[ext]=f;if(ext>lastSeq_)lastSeq_=ext;
  // jitter: lateness against the best arrival (sliding), target = jitter + 30 ms
  double late=at-double(ext)*0.02;
  if(late<minLate_||at-minAt_>8.0){minLate_=late;minAt_=at;}
  double j=late-minLate_;jit_=std::max(j,jit_*0.995);
  target_=std::max(NetRate*0.04,std::min(NetRate*0.24,(jit_+0.03)*NetRate));
 }
 void declick(std::array<float,Frame>& f){ // a jump at the joint: shifted back to the last sample
  if(fifo_.empty())return;float off=fifo_.back()-f[0];for(int i=0;i<32;++i)f[size_t(i)]+=off*(1.f-float(i)/32.f);
 }
 bool refill(){
  auto it=frames_.find(next_);
  if(it!=frames_.end()){
   std::array<float,Frame> f=it->second;frames_.erase(it);
   if(concealed_)declick(f);concealed_=0;lastFrame_=f;
   fifo_.insert(fifo_.end(),f.begin(),f.end());++next_;return true;
  }
  while(!frames_.empty()&&frames_.begin()->first<next_)frames_.erase(frames_.begin());
  if(concealed_>=4)return false;            // 80 ms of nothing: silence, buffering again
  // lost (later ones are here) or late: the last frame again, quieter each time
  ++concealed_;++stats_.concealed;if(!frames_.empty())++stats_.lost;
  std::array<float,Frame> f=lastFrame_;const float g=concealed_==1?0.7f:concealed_==2?0.4f:0.15f;
  for(auto& v:f)v*=g;declick(f);
  fifo_.insert(fifo_.end(),f.begin(),f.end());++next_;return true;
 }
 void skipLocked(int64_t keep){
  while(!frames_.empty()&&bufferedLocked()>double(keep)){
   auto it=frames_.begin();if(it->first>=next_){next_=it->first+1;}frames_.erase(it);++stats_.dropped;
  }
 }
 void decay(float* out,int n){for(int i=0;i<n;++i){last_*=0.995f;out[i]+=last_;}}
};
// ---------------------------------------------------------------- echo canceller (16 kHz)
// y = the GMod "what you hear" capture, x = what the phone line played (the reference, placed on the
// same clock). The echo is digital (same process, the Windows mixer): a delayed, scaled copy, maybe
// smeared a little by a sample rate converter. So: the delay is found by correlation (coarse at 4 kHz
// over -10 .. +300 ms, then to the sample), a short adaptive filter (48 taps = 3 ms) sits on it.
// Two filters (Ochiai): the background one always adapts, the foreground one (= the output) takes its
// weights only while they are clearly better - talking at this end ("double talk") can't spoil it.
struct EchoStats{double erleDb=0,delayMs=0,refPeak=0;int resets=0,moves=0;bool active=false;};
class Echo{
public:
 static constexpr int L=48,Lead=160,Hist=32768,Block=160;
 Echo(){wb_.assign(L,0.f);wf_.assign(L,0.f);x_.assign(size_t(Hist)*2,0.f);y_.assign(Hist,0.f);}
 // one sample of each in, the echo-free y of Lead samples ago out
 float process(float y,float x){
  if(!std::isfinite(x))x=0;if(!std::isfinite(y))y=0;
  const size_t i=size_t(t_%Hist);x_[i]=x;x_[i+Hist]=x;y_[i]=y; // x mirrored: any L in a row are contiguous
  const int64_t ty=t_-Lead;float out=0;
  // tap k = echo delay c - L/2 + k: x[ty - c + L/2 - k]
  const int64_t newest=ty-c_+L/2;
  if(ty>=0&&newest-(L-1)>=0&&newest<=t_){
   const float yy=y_[size_t(ty%Hist)];
   const float* xp=&x_[size_t((newest-(L-1))%Hist)]; // xp[L-1-k] = x[newest-k]
   double eb=0,ef=0,px=0;
   for(int k=0;k<L;++k){float v=xp[L-1-k];eb+=double(wb_[size_t(k)])*v;ef+=double(wf_[size_t(k)])*v;px+=double(v)*v;}
   float errB=yy-float(eb),errF=yy-float(ef);
   if(px>L*1e-7){float g=float(mu_*errB/(px+L*1e-5));for(int k=0;k<L;++k)wb_[size_t(k)]+=g*xp[L-1-k];}
   if(!std::isfinite(errB)||std::fabs(errB)>8.f){std::fill(wb_.begin(),wb_.end(),0.f);++resets_;}
   if(!std::isfinite(errF)||std::fabs(errF)>8.f){std::fill(wf_.begin(),wf_.end(),0.f);errF=yy;++resets_;}
   bEb_+=double(errB)*errB;bEf_+=double(errF)*errF;bEy_+=double(yy)*yy;bEx_+=double(x)*x;
   if(++bN_>=Block){
    if(bEx_>Block*1e-7){
     if(bEb_<0.5*bEf_&&bEb_<0.5*bEy_){if(++better_>=2)wf_=wb_;}  // the background is clearly better (twice): output takes it
     else{better_=0;if(bEb_>8*bEf_)wb_=wf_;}                      // the background diverged: back to the good one
    }
    if(bEf_>1.5*bEy_+1e-9)std::fill(wf_.begin(),wf_.end(),0.f);  // worse than nothing (the path changed)
    pY_+=0.05*(bEy_-pY_);pErr_+=0.05*(bEf_-pErr_);pX_+=0.05*(bEx_/Block-pX_);
    bEb_=bEf_=bEy_=bEx_=0;bN_=0;
   }
   out=errF;
  }else if(ty>=0)out=y_[size_t(ty%Hist)];
  ++t_;
  if(t_-lastSearch_>=NetRate){lastSearch_=t_;search();}
  return out;
 }
 EchoStats stats()const{EchoStats s;s.erleDb=10*std::log10((pY_+1e-12)/(pErr_+1e-12));s.delayMs=double(c_-L/2+peakTap())*1000.0/NetRate;s.refPeak=std::sqrt(pX_);s.resets=resets_;s.moves=moves_;s.active=pX_>1e-7;return s;}
private:
 std::vector<float> wb_,wf_,x_,y_;int64_t t_=0,lastSearch_=0;int c_=0,resets_=0,moves_=0,bN_=0,lastD_=-100000,better_=0;
 double mu_=0.05; // slow: the path is fixed, a voice at this end must not be learned
 double bEb_=0,bEf_=0,bEy_=0,bEx_=0,pErr_=0,pY_=0,pX_=0;
 float xAt(int64_t q)const{return q<0||q>t_?0.f:x_[size_t(q%Hist)];}
 int peakTap()const{int b=L/2;float m=0;for(int k=0;k<L;++k)if(std::fabs(wf_[size_t(k)])>m){m=std::fabs(wf_[size_t(k)]);b=k;}return b;}
 void search(){
  constexpr int N=NetRate/4,D0=40,D1=1200;
  const int64_t yEnd=t_-Lead;if(yEnd-int64_t(N+D1)*4-8<0)return;
  // coarse: 4 kHz sums, pre-emphasized (the low notes do not dominate)
  std::vector<float> ys((size_t)N),xs((size_t)(N+D1+D0));
  double ey=0,ex=0;float py=0,px=0;
  for(int i=0;i<N;++i){int64_t q=yEnd-int64_t(N-i)*4;float a=0;for(int j=0;j<4;++j)a+=y_[size_t((q+j)%Hist)];float d=a-0.9f*py;py=a;ys[size_t(i)]=d;ey+=double(d)*d;}
  for(int i=0;i<N+D1+D0;++i){int64_t q=yEnd-int64_t(N+D1-i)*4;float a=0;for(int j=0;j<4;++j)a+=xAt(q+j);float d=a-0.9f*px;px=a;xs[size_t(i)]=d;ex+=double(d)*d;}
  if(ex<N*1e-6||ey<N*1e-7)return; // the line or the capture was quiet
  double best=0;int bestD=0;
  for(int d=-D0;d<=D1;++d){double c=0,e=0;const int off=D1-d;for(int i=0;i<N;i+=2){float v=xs[size_t(i+off)];c+=double(ys[size_t(i)])*v;e+=double(v)*v;}
   if(e<=0)continue;double r=c/std::sqrt(e*ey*0.5);if(r>best){best=r;bestD=d;}}
  if(best<0.3)return;
  // fine: 16 kHz, +-8 samples around it, over the last 0.5 s
  const int M=NetRate/2;int fine=bestD*4;double fb=-1e300;
  for(int d=bestD*4-8;d<=bestD*4+8;++d){
   double c=0,e=0;for(int i=0;i<M;++i){int64_t q=yEnd-M+i;float v=xAt(q-d),w=xAt(q-d-1),u=y_[size_t(q%Hist)],uu=y_[size_t((q-1)%Hist)];float dv=v-0.9f*w,du=u-0.9f*uu;c+=double(du)*dv;e+=double(dv)*dv;}
   if(e>0){double r=c/std::sqrt(e);if(r>fb){fb=r;fine=d;}}}
  const bool agree=std::abs(fine-lastD_)<=2;lastD_=fine;
  if(std::abs(fine-c_)<=L/4||!agree)return; // inside the filter / not sure yet
  if(fine<L/2-Lead)return;                 // earlier than the reference can be read
  const int delta=fine-c_;std::vector<float> nb(L,0.f),nf(L,0.f);
  for(int k=0;k<L;++k){int o=k+delta;if(o>=0&&o<L){nb[size_t(k)]=wb_[size_t(o)];nf[size_t(k)]=wf_[size_t(o)];}}
  wb_.swap(nb);wf_.swap(nf);c_=fine;++moves_;
 }
};
// ---------------------------------------------------------------- the line's echo reference
// the phone line (phone_line.h) hands every block it writes to the device here (when to be heard,
// QPC 100 ns), the CSC uplink takes them on its own clock
struct RefBlock{int64_t at=0;int rate=0;std::vector<float> s;};
class RefTap{
public:
 void enable(bool on){std::lock_guard<std::mutex> lock(m_);users_=std::max(0,users_+(on?1:-1));on_=users_>0;if(!on_)blocks_.clear();} // counted (an old uplink may still end)
 bool on(){return on_.load();}
 void put(int64_t at,int rate,const float* s,int n){
  if(!on_.load()||n<=0)return;
  std::lock_guard<std::mutex> lock(m_);
  if(!on_)return;
  if(blocks_.size()>400)blocks_.pop_front();
  RefBlock b;b.at=at;b.rate=rate;b.s.assign(s,s+n);blocks_.push_back(std::move(b));
 }
 std::deque<RefBlock> take(){std::lock_guard<std::mutex> lock(m_);std::deque<RefBlock> o;o.swap(blocks_);return o;}
private:
 int users_=0;
 std::mutex m_;std::atomic<bool> on_{false};std::deque<RefBlock> blocks_;
};
inline RefTap& refTap(){static RefTap t;return t;}
}
