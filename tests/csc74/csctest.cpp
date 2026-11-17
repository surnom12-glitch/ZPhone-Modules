// fix74 CSC sound test (Linux): src/csc_audio.h - ADPCM, 48->16 kHz, the receive stream (jitter,
// loss, drift, DTX gaps), the echo canceller (delayed + scaled line in the capture)
//   c++ -std=c++17 -O2 -o csctest csctest.cpp -lpthread && ./csctest
#include "../../src/csc_audio.h"
#include <cstdio>
#include <random>
static int fails=0;
#define CHECK(c,...) do{if(!(c)){++fails;printf("FAIL %s:%d: ",__FILE__,__LINE__);printf(__VA_ARGS__);printf("\n");}else{printf("ok   ");printf(__VA_ARGS__);printf("\n");}}while(0)
static double rms(const std::vector<float>& v,size_t a,size_t b){double s=0;b=std::min(b,v.size());if(b<=a)return 0;for(size_t i=a;i<b;++i)s+=double(v[i])*v[i];return std::sqrt(s/double(b-a));}
// speech-like test signal: a few harmonics with a slow envelope + a bit of noise
static std::vector<float> voice(int rate,double sec,unsigned seed,double amp=0.3,double f0b=140,double f0d=30,double vib=2.1){
 std::mt19937 r(seed);std::normal_distribution<float> n(0,1);std::vector<float> s(size_t(rate*sec));
 for(size_t i=0;i<s.size();++i){double t=double(i)/rate;double f0=f0b+f0d*std::sin(t*vib);double env=0.5+0.5*std::sin(t*6.0+seed);
  double v=0;for(int h=1;h<=12;++h)v+=std::sin(6.283185307179586*f0*h*t+h)/h;s[i]=float(amp*env*(0.5*v+0.05*n(r)));}
 return s;
}
int main(){
 using namespace Csc;
 // ---- ADPCM
 {auto s=voice(NetRate,2,1);std::vector<float> out;Adpcm st;uint8_t buf[FrameBytes];int16_t pcm[Frame];float dec[Frame];
  for(size_t f=0;f+Frame<=s.size();f+=Frame){for(int i=0;i<Frame;++i)pcm[i]=int16_t(std::lround(s[f+i]*32767));encodeFrame(st,pcm,buf);decodeFrame(buf,FrameBytes,dec);out.insert(out.end(),dec,dec+Frame);}
  double e=0,p=0;for(size_t i=0;i<out.size();++i){double d=out[i]-s[i];e+=d*d;p+=double(s[i])*s[i];}
  double snr=10*std::log10(p/e);CHECK(snr>20,"ADPCM SNR %.1f dB (>20)",snr);
  uint8_t bad[FrameBytes]={0};bad[2]=200;CHECK(!decodeFrame(bad,FrameBytes,dec),"bad step index refused");
  CHECK(!decodeFrame(buf,10,dec),"short frame refused");}
 // ---- 48 -> 16 kHz
 {auto tone=[&](double hz){Decimate3 d;std::vector<float> o;for(int i=0;i<48000;++i){float y;if(d.push(float(0.5*std::sin(6.283185307179586*hz*i/48000)),y))o.push_back(y);}return rms(o,2000,o.size())/(0.5/std::sqrt(2));};
  double a=tone(1000),b=tone(6000),c=tone(12000),d=tone(20000);
  CHECK(std::fabs(a-1)<0.02,"1 kHz passes (%.3f)",a);CHECK(b>0.85,"6 kHz passes (%.3f)",b);CHECK(c<0.01,"12 kHz stopped (%.4f)",c);CHECK(d<0.01,"20 kHz stopped (%.4f)",d);}
 // ---- receive stream: jitter + loss + reorder, 48 kHz device in 10 ms blocks
 auto feed=[&](double clockRatio,double jitterMs,double loss,int gapFrom,int gapLen,int outRate,StreamStats& st,std::vector<float>& out,std::vector<float>& src){
  StreamIn s;src=voice(NetRate,12,3);std::mt19937 r(7);std::uniform_real_distribution<double> u(0,1);
  std::vector<std::pair<double,int>> arr; // arrival time, frame
  int frames=int(src.size()/Frame);
  for(int f=0;f<frames;++f){if(f>=gapFrom&&f<gapFrom+gapLen)continue;if(u(r)<loss)continue;arr.push_back({f*0.02*clockRatio+0.05+u(r)*jitterMs/1000.0,f});}
  std::sort(arr.begin(),arr.end());
  Adpcm enc;std::vector<std::vector<uint8_t>> pk((size_t)frames);int16_t pcm[Frame];
  for(int f=0;f<frames;++f){for(int i=0;i<Frame;++i)pcm[i]=int16_t(std::lround(src[size_t(f*Frame+i)]*32767));pk[size_t(f)].resize(FrameBytes);encodeFrame(enc,pcm,pk[size_t(f)].data());}
  size_t ai=0;const int blk=outRate/100;std::vector<float> b((size_t)blk);
  for(double t=0;t<frames*0.02+0.5;t+=0.01){
   while(ai<arr.size()&&arr[ai].first<=t){int f=arr[ai].second;s.push(uint16_t(f&65535),pk[size_t(f)].data(),FrameBytes,arr[ai].first);++ai;}
   std::fill(b.begin(),b.end(),0.f);s.render(b.data(),blk,outRate);out.insert(out.end(),b.begin(),b.end());
   if(std::fabs(t-6.0)<0.005)st=s.stats();
  }
 };
 {StreamStats st;std::vector<float> out,src;feed(1.0,30,0.03,-1,0,48000,st,out,src);
  double lvl=rms(out,48000*2,48000*10)/rms(src,NetRate*2,NetRate*10);
  CHECK(lvl>0.8&&lvl<1.2,"jitter 30 ms + 3 %% loss: level %.2f",lvl);
  CHECK(st.bufferMs<160,"buffer %.0f ms (target %.0f)",st.bufferMs,st.targetMs);
  CHECK(st.lost>0&&st.concealed>=st.lost,"lost %d concealed %d underruns %d",st.lost,st.concealed,st.underruns);
  double mx=0;for(size_t i=1;i<out.size();++i)mx=std::max(mx,double(std::fabs(out[i]-out[i-1])));CHECK(mx<0.35,"no clicks (max step %.3f)",mx);}
 {StreamStats st;std::vector<float> out,src;feed(0.99,5,0,-1,0,44100,st,out,src); // sender 1 % fast, 44.1 kHz output
  CHECK(st.bufferMs<150&&st.dropped<5,"drift 1 %% at 44.1 kHz: buffer %.0f ms, dropped %d, underruns %d",st.bufferMs,st.dropped,st.underruns);
  double lvl=rms(out,44100*2,44100*10)/rms(src,NetRate*2,NetRate*10);CHECK(lvl>0.8&&lvl<1.2,"level %.2f",lvl);}
 {StreamStats st;std::vector<float> out,src;feed(1.01,5,0,-1,0,48000,st,out,src); // sender 1 % slow
  CHECK(st.underruns<4,"slow sender: underruns %d, buffer %.0f ms",st.underruns,st.bufferMs);}
 {StreamStats st;std::vector<float> out,src;feed(1.0,5,0,100,150,48000,st,out,src); // 3 s gap (DTX / outage)
  double during=rms(out,size_t(48000*(0.05+2.6)),size_t(48000*(0.05+4.8)));double after=rms(out,48000*7,48000*10);
  CHECK(during<0.002&&after>0.05,"3 s gap: silent %.4f, sound again after %.3f",during,after);}
 // ---- echo canceller: capture = 0.6 x line delayed (fractional) + another voice at this end every other 2 s
 auto echoRun=[&](double delayMs,double& erle,double& keep,EchoStats& es,bool noise=false){
  auto x=voice(NetRate,16,5,0.4);auto loc=voice(NetRate,16,9,0.08,205,45,1.3);
  if(noise){std::mt19937 r(11);std::normal_distribution<float> g(0,0.05f);for(auto& v:loc)v=g(r);}for(size_t i=0;i<loc.size();++i)if((i/(NetRate*2))%2)loc[i]=0; // local sound every other 2 s
  Echo e;std::vector<float> out;const long D=long(std::lround(delayMs*NetRate/1000));const long n=long(x.size());
  auto xa=[&](long q){return (q>=0&&q<n)?x[size_t(q)]:0.f;};
  // a digital path: 0.6 x, delayed D + 0.37 samples (a resampler: linear interpolation smears it a little)
  auto echoAt=[&](long q){return 0.6f*(0.63f*xa(q-D)+0.37f*xa(q-D-1));};
  for(long i=0;i<n;++i)out.push_back(e.process(echoAt(i)+loc[size_t(i)],x[size_t(i)]));
  // out[i] = the capture of i - Lead: residual echo where loc = 0, the local sound kept where loc != 0
  double res=0,ech=0,kp=0,lv=0;
  for(long i=NetRate*8;i<n;++i){long q=i-Echo::Lead;float echo=echoAt(q);
   if(loc[size_t(q)]==0){res+=double(out[size_t(i)])*out[size_t(i)];ech+=double(echo)*echo;}else{double d=out[size_t(i)]-loc[size_t(q)];kp+=d*d;lv+=double(loc[size_t(q)])*loc[size_t(q)];}}
  erle=10*std::log10(ech/(res+1e-12));keep=10*std::log10(lv/(kp+1e-12));es=e.stats();
 };
 {double erle,keep;EchoStats es;echoRun(37,erle,keep,es);CHECK(erle>25,"echo 37 ms: removed %.1f dB",erle);CHECK(keep>6,"another voice at this end meanwhile: kept (residual -%.1f dB), delay est %.1f ms",keep,es.delayMs);}
 {double erle,keep;EchoStats es;echoRun(160,erle,keep,es);CHECK(erle>20,"echo 160 ms (bulk delay search): removed %.1f dB",erle);CHECK(keep>6,"voice at this end kept (residual -%.1f dB), delay est %.1f ms",keep,es.delayMs);}
 {double erle,keep;EchoStats es;echoRun(-6,erle,keep,es);CHECK(erle>20,"echo -6 ms (reference late): removed %.1f dB",erle);}
 {double erle,keep;EchoStats es;echoRun(25,erle,keep,es,true);CHECK(erle>25,"echo 25 ms with game noise at this end: removed %.1f dB",erle);CHECK(keep>15,"noise kept (residual -%.1f dB)",keep);}
 printf(fails?"\n%d FAILED\n":"\nall ok\n",fails);return fails?1:0;
}
