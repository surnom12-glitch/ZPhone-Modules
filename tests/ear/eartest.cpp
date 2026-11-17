// fix63/fix64 ear test: bank decoding (fix64: on the bank's own thread) (WAV PCM / cue loop / MS-ADPCM, MP3, OGG) + voices in the recorder
#include "windows.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>
std::atomic<int64_t> g_qpcOffset{0};
static int64_t T0;
static int64_t q100(){LARGE_INTEGER c;QueryPerformanceCounter(&c);return c.QuadPart;}
#include "../../src/audio_capture.h"
HRESULT CoCreateInstance(REFIID,void*,DWORD,REFIID,void** out){*out=nullptr;return E_FAIL;} // no devices: ear only
static std::string slurp(const char* p){std::ifstream f(p,std::ios::binary);std::stringstream s;s<<f.rdbuf();return s.str();}
int main(int argc,char** argv){
 T0=q100();std::string mode=argc>1?argv[1]:"";
 auto& B=PhoneEar::bank();
 const char* files[]={"click.wav","loop.wav","adpcm.wav","tone.mp3","tone.ogg"};
 if(mode=="bank"){
  for(auto f:files)printf("add %s -> %d\n",f,B.add(f,slurp(f)));
  printf("add junk -> %d\n",B.add("junk.wav",std::string(100,'x')));
  printf("add again -> %d\n",B.add("click.wav",slurp("click.wav")));
  // fix64: the bank decodes on its own thread: wait until nothing is queued / decoding (5 s at most)
  for(int i=0;i<500;++i){int r,fl,q;double mb;B.stats(r,fl,q,mb);bool busy=q>0;for(auto f:files)if(B.state(f)==PhoneEar::Decoding)busy=true;if(!busy)break;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
  for(auto f:files){double s=0;bool l=false;int st=B.state(f,&s,&l);auto c=B.find(f);double rms=0;if(c){for(float v:c->pcm)rms+=v*v;rms=std::sqrt(rms/c->pcm.size());}
   printf("%s state=%d sec=%.3f loop=%d rate=%d rms=%.4f loopStart=%lld\n",f,st,s,l,c?c->rate:0,rms,c?(long long)c->loopStart:-1LL);}
  printf("junk state=%d\n",B.state("junk.wav"));
  int r,fl,q;double mb;B.stats(r,fl,q,mb);printf("stats ready=%d failed=%d queued=%d MB=%.3f\n",r,fl,q,mb);
  B.clear();return 0;
 }
 // timeline (seconds after T0); video pts 0 = 0.2 s
 for(auto f:{"click.wav","adpcm.wav","tone.mp3"})B.add(f,slurp(f)); // decoded by the bank thread (fix64)
 auto rec=std::make_unique<PhoneAudio::Recorder>(8,"ear.wav");
 bool zero=false;int step=0;
 for(;;){double t=(q100()-T0)/1e7;if(t>4.2)break;
  double master=t<3.3?1.0:0.0;
  rec->control(0,0,0,master);
  if(!zero&&t>=0.25){rec->zero(T0+2000000);zero=true;}
  if(zero&&t<3.9)rec->seen(t-0.2-0.3);
  if(step==0&&t>=0.5){rec->earPlay(1,"click.wav",0.5,20000,1,true);step=1;printf("play1 at %.4f\n",t-0.2);}
  if(step==1&&t>=1.0){rec->earPlay(2,"adpcm.wav",1.0,20000,1,true);step=2;printf("play2 at %.4f\n",t-0.2);}
  if(step==2&&t>=1.6){B.add("loop.wav",slurp("loop.wav"));rec->earPlay(3,"loop.wav",0.5,20000,1,true);step=3;printf("play3 at %.4f (file still decoding)\n",t-0.2);}
  if(step==3&&t>=2.0){rec->earPlay(4,"tone.mp3",1.0,500,1,true);step=4;printf("play4 at %.4f lp 500\n",t-0.2);}
  if(step==4&&t>=2.3){rec->earSet(4,1.0,20000,1);step=5;printf("set4 open at %.4f\n",t-0.2);}
  if(step==5&&t>=2.6){rec->earStop(3);step=6;printf("stop3 at %.4f\n",t-0.2);}
  if(step==6&&t>=2.8){rec->earPlay(5,"click.wav",0.5,20000,2,true);step=7;printf("play5 pitch2 at %.4f\n",t-0.2);}
  if(step==7&&t>=3.1){rec->earPlay(7,"click.wav",0.5,20000,1,true,0.25);step=8;printf("play7 age 0.25 at %.4f -> onset %.4f\n",t-0.2,t-0.2-0.25);}
  if(step==8&&t>=3.5){rec->earPlay(6,"click.wav",0.5,20000,1,true);step=9;printf("play6 (master 0) at %.4f\n",t-0.2);}
  std::this_thread::sleep_for(std::chrono::milliseconds(16));
 }
 rec->finish(int64_t(3.8*90000));bool ok=rec->waitDone(20000);auto st=rec->stats();
 printf("done=%d wav=%d aac=%d played=%d voices=%d err='%s'\n",ok,st.wav,st.aac,st.earPlayed,st.earVoices,st.error.c_str());
 int n=0;while(AVPacket* p=rec->pop()){av_packet_free(&p);++n;}printf("aac packets=%d\n",n);
 rec.reset();B.clear();return 0;
}
