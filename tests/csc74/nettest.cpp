// fix74: Cross Server Calls signalling test (Linux): src/csc_net.h against a fake Steam (lobbies with
// owner / members / data, call results that take time, P2P with latency, jitter, loss of unreliable
// packets, sessions that have to be accepted). Build: g++ -std=c++17 -O1 -g -fsanitize=address,undefined nettest.cpp -o nettest
// fix77: protocol 2 (end-to-end encryption, replay window, owner hash, rate limits) at the end
#include <cstdio>
#include <deque>
#include <random>
#include "../../src/csc_net.h"
static int fails=0;
#define CHECK(c,...) do{if(!(c)){++fails;printf("FAIL %s:%d: ",__FILE__,__LINE__);printf(__VA_ARGS__);printf("\n");}else{printf("ok   ");printf(__VA_ARGS__);printf("\n");}}while(0)
struct World;
struct Pkt{double at;uint64_t from;std::vector<uint8_t> d;};
struct Lobby{uint64_t owner=0;std::vector<uint64_t> members;std::map<std::string,std::string> data;int max=2;};
struct Pending{int kind;double at;uint64_t lobby;int result;uint32_t count;};
struct World{double now=0;std::map<uint64_t,Lobby> lobbies;uint64_t nextLobby=109775240000000001ull;uint64_t nextCall=1;std::mt19937 rng{7};double loss=0,latency=0.06,jitter=0.02;
 std::map<uint64_t,struct Fake*> users;
 std::vector<std::vector<uint8_t>> wire; // fix77: everything anybody sent (what an eavesdropper sees)
 std::function<int(uint64_t,uint64_t,std::vector<uint8_t>&)> tamper;}; // fix77: a man in the middle: 0 drop, 1 deliver, 2 deliver twice (replay)
struct Fake:Csc::Backend{
 World& w;uint64_t me;std::map<uint64_t,Pending> calls;std::vector<uint64_t> lastList;std::deque<Pkt> inbox;std::set<uint64_t> acceptedFrom,sentTo;int sent=0,dropped=0;bool dead=false;std::set<int> dropSet;
 Fake(World& w_,uint64_t id):w(w_),me(id){w.users[id]=this;}
 uint64_t requestLobbyList()override{uint64_t c=w.nextCall++;Pending p{0,w.now+0.3,0,1,0};calls[c]=p;return c;}
 uint64_t lobbyByIndex(int i)override{return i>=0&&size_t(i)<lastList.size()?lastList[size_t(i)]:0;}
 uint64_t createLobby(int max)override{uint64_t c=w.nextCall++;uint64_t id=w.nextLobby++;Lobby l;l.owner=me;l.members={me};l.max=max;w.lobbies[id]=l;calls[c]=Pending{1,w.now+0.2,id,1,0};return c;}
 uint64_t joinLobby(uint64_t lobby)override{uint64_t c=w.nextCall++;calls[c]=Pending{2,w.now+0.3,lobby,0,0};return c;}
 void leaveLobby(uint64_t lobby)override{auto it=w.lobbies.find(lobby);if(it==w.lobbies.end())return;auto& m=it->second.members;m.erase(std::remove(m.begin(),m.end(),me),m.end());
  if(m.empty())w.lobbies.erase(it);else if(it->second.owner==me)it->second.owner=m[0];}
 Lobby* get(uint64_t id){auto it=w.lobbies.find(id);return it==w.lobbies.end()?nullptr:&it->second;}
 bool inLobby(uint64_t id){auto l=get(id);return l&&std::find(l->members.begin(),l->members.end(),me)!=l->members.end();}
 int memberCount(uint64_t lobby)override{auto l=get(lobby);return l&&inLobby(lobby)?int(l->members.size()):0;}
 uint64_t memberByIndex(uint64_t lobby,int i)override{auto l=get(lobby);return l&&inLobby(lobby)&&i>=0&&size_t(i)<l->members.size()?l->members[size_t(i)]:0;}
 std::string lobbyData(uint64_t lobby,const char* k)override{auto l=get(lobby);if(!l)return "";auto it=l->data.find(k);return it==l->data.end()?"":it->second;}
 bool setLobbyData(uint64_t lobby,const char* k,const char* v)override{auto l=get(lobby);if(!l||l->owner!=me)return false;l->data[k]=v;return true;}
 uint64_t lobbyOwner(uint64_t lobby)override{auto l=get(lobby);return l&&inLobby(lobby)?l->owner:0;}
 int ready(uint64_t c,Pending*& p){auto it=calls.find(c);if(it==calls.end())return -1;if(w.now<it->second.at)return 0;p=&it->second;return 1;}
 int listResult(uint64_t c,uint32_t& n)override{Pending* p;int r=ready(c,p);if(r<=0)return r;lastList.clear();
  for(auto& l:w.lobbies)if(l.second.data.count("zphcsc")&&l.second.data["zphcsc"]=="1")lastList.push_back(l.first);n=uint32_t(lastList.size());calls.erase(c);return 1;}
 int createResult(uint64_t c,int& res,uint64_t& lobby)override{Pending* p;int r=ready(c,p);if(r<=0)return r;res=1;lobby=p->lobby;calls.erase(c);return 1;}
 int enterResult(uint64_t c,uint64_t& lobby,uint32_t& resp)override{Pending* p;int r=ready(c,p);if(r<=0)return r;lobby=p->lobby;auto l=get(p->lobby);
  if(!l)resp=2;else if(int(l->members.size())>=l->max)resp=4;else{resp=1;l->members.push_back(me);}calls.erase(c);return 1;}
 bool send(uint64_t to,const void* d,uint32_t n,int mode)override{
  auto it=w.users.find(to);if(it==w.users.end())return false;Fake* t=it->second;sentTo.insert(to);++sent;
  std::vector<uint8_t> data(static_cast<const uint8_t*>(d),static_cast<const uint8_t*>(d)+n);w.wire.push_back(data);
  int copies=w.tamper?w.tamper(me,to,data):1;
  if(t->dead)return true;
  if(!t->acceptedFrom.count(me)&&!t->sentTo.count(me)){++dropped;return true;} // no session yet: lost
  if(mode!=Csc::SendReliable){std::uniform_real_distribution<double> u(0,1);if(u(w.rng)<w.loss||dropSet.count(sent)){++dropped;return true;}}
  std::uniform_real_distribution<double> j(0,w.jitter);double at=w.now+w.latency+j(w.rng);
  if(mode==Csc::SendReliable&&!t->inbox.empty())at=std::max(at,t->inbox.back().at); // in order
  Pkt p{at,me,data};
  for(int c=0;c<copies;++c){auto pos=t->inbox.end();while(pos!=t->inbox.begin()&&(pos-1)->at>at)--pos;t->inbox.insert(pos,p);}return true;}
 bool available(uint32_t& s)override{if(inbox.empty()||inbox.front().at>w.now)return false;s=uint32_t(inbox.front().d.size());return true;}
 bool read(void* b,uint32_t cap,uint32_t& s,uint64_t& from)override{if(!available(s))return false;auto p=inbox.front();inbox.pop_front();s=std::min<uint32_t>(cap,uint32_t(p.d.size()));std::memcpy(b,p.d.data(),s);from=p.from;return true;}
 void accept(uint64_t u)override{acceptedFrom.insert(u);}
 void close(uint64_t u)override{acceptedFrom.erase(u);sentTo.erase(u);}
};
struct Phone{Fake f;Csc::Net net;std::vector<Csc::Event> ev;int audio=0;std::string name;
 Phone(World& w,uint64_t id,const std::string& n,const std::string& server):f(w,id),net(f),name(n){net.setMe(id);net.info(n,server,"gm_construct",false,true);net.onAudio=[this](uint16_t,const uint8_t*,size_t){++audio;};}
 void pump(World& w){if(f.dead)return;net.pump(w.now,1700000000+int64_t(w.now));for(auto& e:net.events())ev.push_back(e);}
 bool has(const std::string& k,const std::string& reason=""){for(auto& e:ev)if(e.kind==k&&(reason.empty()||e.reason==reason))return true;return false;}
 const Csc::Event* last(const std::string& k){const Csc::Event* r=nullptr;for(auto& e:ev)if(e.kind==k)r=&e;return r;}
};
static void run(World& w,std::vector<Phone*> ps,double sec,std::function<void()> each=nullptr){
 for(double t=0;t<sec;t+=0.016){w.now+=0.016;for(auto p:ps)p->pump(w);if(each)each();}
}
static void talk(World& w,Phone& a,Phone& b,double sec){ // both sides send 20 ms frames
 uint16_t sa=0,sb=0;uint8_t fr[Csc::FrameBytes]={};double acc=0;
 run(w,{&a,&b},sec,[&]{acc+=0.016;while(acc>=0.02){acc-=0.02;a.net.sendAudio(sa++,fr);b.net.sendAudio(sb++,fr);}});
}
int main(){
 World w;
 Phone A(w,76561198000000001ull,"Alice","Z-City #1"),B(w,76561198000000002ull,"Bob","Z-City EU"),C(w,76561198000000003ull,"Carl","Other server");
 A.net.publish(true);B.net.publish(true);C.net.publish(true);
 run(w,{&A,&B,&C},1.0);
 CHECK(A.net.published()&&B.net.published()&&C.net.published(),"three published entries");
 CHECK(w.lobbies[A.net.myLobby()].data["n"]=="Alice"&&w.lobbies[A.net.myLobby()].data["s"]=="Z-City #1"&&w.lobbies[A.net.myLobby()].data["b"]=="0","lobby data: name, server, not busy");
 // ---- list
 B.net.list(w.now);run(w,{&A,&B,&C},0.5);
 auto l=B.last("list");CHECK(l&&l->list.size()==2,"B sees 2 others (not himself): %d",l?int(l->list.size()):-1);
 uint64_t aLobby=0;if(l)for(auto& e:l->list)if(e.name=="Alice"){aLobby=e.lobby;CHECK(e.server=="Z-City #1"&&e.map=="gm_construct"&&!e.busy,"entry: Alice / server / map / free");}
 CHECK(!B.net.list(w.now),"a second list within 2 s is refused");
 // ---- call, ring, answer, talk, hang up
 CHECK(B.net.call(aLobby,w.now),"B calls Alice");
 run(w,{&A,&B,&C},1.5);
 CHECK(A.has("ring")&&A.last("ring")->name=="Bob"&&A.last("ring")->server=="Z-City EU","Alice's phone rings: Bob from Z-City EU");
 CHECK(B.has("ringing")&&B.net.state()==Csc::Alerting,"B hears it ringing");
 CHECK(w.lobbies[A.net.myLobby()].data["b"]=="1","Alice's entry is busy while ringing");
 run(w,{&A,&B,&C},3.0);
 CHECK(A.net.state()==Csc::RingIn&&B.net.state()==Csc::Alerting,"still ringing after 3 s (keepalive)");
 A.net.answer(w.now);run(w,{&A,&B,&C},0.3);
 CHECK(A.net.state()==Csc::Active&&B.net.state()==Csc::Active&&B.has("accepted"),"answered: both active");
 w.loss=0.2;talk(w,A,B,10);w.loss=0;
 CHECK(A.audio>350&&A.audio<450&&B.audio>350&&B.audio<450,"voice both ways with 20%% loss: %d / %d frames of 500",A.audio,B.audio);
 CHECK(A.net.state()==Csc::Active,"20%% loss for 10 s does not end the call");
 B.net.sendLua(3,std::string(3000,'x'),false);B.net.sendLua(4,"pose",false);run(w,{&A,&B,&C},0.3);
 int msgs=0;size_t big=0;for(auto& e:A.ev)if(e.kind=="msg"){++msgs;if(e.msgKind==3)big=e.data.size();}
 CHECK(msgs==2&&big==3000,"Lua messages: a big one (reliable) and a small one: %d, %d bytes",msgs,int(big));
 // ---- fix75: the picture (JPEG frames, binary, split into packets)
 {
  auto frameOf=[](int i,size_t n){std::string f(n,'\0');f[0]=char(0xFF);f[1]=char(0xD8);for(size_t k=2;k<n;++k)f[k]=char((k*31+size_t(i)*7)%251);
   f[2]=char(i&255);f[3]=char(i>>8);return f;}; // zeros, 0xFF ... anything a JPEG has
  std::string got;uint16_t gid=0;
  CHECK(!A.net.takeVideo(got,gid),"video: nothing before the first picture");
  std::string f1=frameOf(1,25000);
  CHECK(B.net.sendVideo(f1),"video: a 25000 byte picture goes out (24 packets)");
  run(w,{&A,&B,&C},0.2);
  CHECK(A.net.takeVideo(got,gid)&&got==f1,"video: arrives byte for byte (%d bytes)",int(got.size()));
  CHECK(!A.net.takeVideo(got,gid),"video: taken once");
  B.net.sendVideo(frameOf(2,9000));B.net.sendVideo(frameOf(3,9000));B.net.sendVideo(frameOf(4,700));
  run(w,{&A,&B,&C},0.2);
  CHECK(A.net.takeVideo(got,gid)&&got==frameOf(4,700),"video: three quickly -> the newest one is shown (%d bytes)",int(got.size()));
  CHECK(!B.net.sendVideo(std::string(Csc::VideoMax+1,'x'))&&B.net.sendVideo(std::string(Csc::VideoMax,'x')),"video: %d bytes max",int(Csc::VideoMax));
  run(w,{&A,&B,&C},0.2);A.net.takeVideo(got,gid);
  // 12 pictures a second, 8 packets each, 10 %% of the packets lost, packets reordered (jitter 30 ms)
  w.loss=0.1;w.jitter=0.03;int sentN=0,shown=0,bad=0,last=-1,stale=0;double acc=0;
  run(w,{&A,&B,&C},5.0,[&]{acc+=0.016;if(acc>=1.0/12){acc-=1.0/12;++sentN;B.net.sendVideo(frameOf(100+sentN,8000));}
   if(A.net.takeVideo(got,gid)){++shown;int i=(unsigned char)got[2]|((unsigned char)got[3]<<8);if(got!=frameOf(i,8000))++bad;if(i<=last)++stale;last=i;}});
  w.loss=0;w.jitter=0.02;
  CHECK(shown>=sentN/2&&shown<sentN&&bad==0&&stale==0,"video: 10%% packet loss: %d of %d pictures shown (parity), %d broken, %d out of order",shown,sentN,bad,stale);
  CHECK(A.net.videoIn()>=shown&&A.net.videoLost()>0&&A.net.videoFixed()>0&&B.net.videoOut()>=sentN,"video: counters in %d lost %d rebuilt %d out %d",A.net.videoIn(),A.net.videoLost(),A.net.videoFixed(),B.net.videoOut());
  // one exact packet lost: the first, a middle one, the last (short) one, the parity; two lost = gone
  struct Case{const char* what;std::vector<int> drop;bool shows;};
  for(const Case& c:{Case{"the first part",{1},true},Case{"a middle part",{2},true},Case{"the last (short) part",{3},true},Case{"the parity",{4},true},Case{"two parts",{1,3},false}}){
   A.net.takeVideo(got,gid);const int base=B.f.sent;B.f.dropSet.clear();for(int k:c.drop)B.f.dropSet.insert(base+k);
   const std::string fr=frameOf(500,2500); // 3 parts (1060, 1060, 380) + parity
   B.net.sendVideo(fr);run(w,{&A,&B,&C},0.2);B.f.dropSet.clear();
   const bool ok=A.net.takeVideo(got,gid);
   CHECK(ok==c.shows&&(!ok||got==fr),"video: lost %s -> %s",c.what,c.shows?"still shown (parity)":"not shown");
  }
  A.net.sendVideo(frameOf(9,3000));run(w,{&A,&B,&C},0.2);
  CHECK(B.net.takeVideo(got,gid)&&got==frameOf(9,3000),"video: the other way too");
  CHECK(!C.net.sendVideo(frameOf(1,100)),"video: no call = nothing sent");
 }
 // ---- Carl: Alice is busy (entry full)
 C.net.list(w.now);run(w,{&A,&B,&C},0.5);uint64_t aL2=0;for(auto& e:C.last("list")->list)if(e.name=="Alice"){aL2=e.lobby;CHECK(e.busy,"Carl sees Alice busy");}
 C.net.call(aL2,w.now);run(w,{&A,&B,&C},1.0);
 CHECK(C.has("ended","busy"),"Carl calls anyway: busy");
 B.net.hangup();run(w,{&A,&B,&C},0.5);
 CHECK(A.has("ended","hangup")&&A.net.state()==Csc::Idle&&B.net.state()==Csc::Idle,"Bob hangs up: Alice ended (hangup)");
 CHECK(w.lobbies.count(A.net.myLobby())&&w.lobbies[A.net.myLobby()].members.size()==1,"Bob left Alice's entry");
 run(w,{&A,&B,&C},8);
 CHECK(w.lobbies[A.net.myLobby()].data["b"]=="0","Alice free again");
 // ---- decline
 A.ev.clear();C.ev.clear();C.net.list(w.now);run(w,{&A,&B,&C},0.5);
 C.net.call(A.net.myLobby(),w.now);run(w,{&A,&B,&C},1.5);
 CHECK(A.has("ring")&&A.last("ring")->name=="Carl","Carl rings Alice");
 A.net.decline(0);run(w,{&A,&B,&C},0.5);
 CHECK(C.has("ended","declined")&&C.net.state()==Csc::Idle,"Alice declines: Carl gets declined");
 // ---- no answer (45 s)
 A.ev.clear();C.ev.clear();run(w,{&A,&B,&C},1.0);
 C.net.call(A.net.myLobby(),w.now);run(w,{&A,&B,&C},1.5);
 CHECK(C.has("ended","busy")&&!A.has("ring"),"fix77: declined -> the same caller can not ring again for 10 s");
 A.ev.clear();C.ev.clear();run(w,{&A,&B,&C},9.0);
 C.net.call(A.net.myLobby(),w.now);run(w,{&A,&B,&C},47);
 CHECK(C.has("ended","noanswer")&&A.has("ended","missed"),"nobody answers: noanswer / missed");
 // ---- caller cancels while ringing
 A.ev.clear();C.ev.clear();run(w,{&A,&B,&C},10.5); // fix77: 10 s pause after a missed call
 C.net.call(A.net.myLobby(),w.now);run(w,{&A,&B,&C},2);C.net.hangup();run(w,{&A,&B,&C},0.5);
 CHECK(A.has("ended","cancel")&&A.net.state()==Csc::Idle,"Carl gives up while it rings: Alice ended (cancel)");
 // ---- a stranger rings without joining the entry: nothing
 A.ev.clear();run(w,{&A,&B,&C},1);
 {Fake& d=C.f;uint8_t p[11]={'Z','P',1,Csc::PRing,1,2,3,4,0,0,0};A.f.accept(d.me);d.send(A.f.me,p,sizeof p,Csc::SendReliable);}
 run(w,{&A,&B,&C},0.5);CHECK(!A.has("ring"),"RING from someone not in the entry is ignored");
 // ---- lost: Bob's game freezes during a call
 A.ev.clear();B.ev.clear();run(w,{&A,&B,&C},4);
 B.net.call(A.net.myLobby(),w.now);run(w,{&A,&B,&C},1.5);A.net.answer(w.now);run(w,{&A,&B,&C},0.5);
 CHECK(A.net.state()==Csc::Active,"Alice and Bob talk again");
 B.f.dead=true;run(w,{&A,&C},13);
 CHECK(A.has("ended","lost"),"Bob's game is gone: Alice ended (lost) after 12 s");
 uint64_t old=A.net.myLobby();run(w,{&A,&C},8);
 CHECK(A.net.published()&&A.net.myLobby()!=old,"Bob still sits in the entry: Alice gets a fresh one");
 C.ev.clear();A.ev.clear();C.net.call(A.net.myLobby(),w.now);run(w,{&A,&C},1.5);
 CHECK(A.has("ring"),"and can be called again");
 C.net.hangup();run(w,{&A,&C},0.5);
 // ---- unpublish
 A.net.publish(false);run(w,{&A,&C},0.5);C.ev.clear();C.net.list(w.now);run(w,{&A,&C},0.5);
 bool seen=false;for(auto& e:C.last("list")->list)if(e.name=="Alice")seen=true;
 CHECK(!A.net.published()&&!seen,"Alice turned it off: gone from the list");
 // ---- joining a lobby that vanished
 C.ev.clear();C.net.call(old,w.now);run(w,{&A,&C},1);CHECK(C.has("ended","unavailable"),"a gone entry: unavailable");
 // ---- fix77: protocol 2 - nobody can listen, fake, replay or block a call
 {
  World w2;w2.latency=0.03;
  Phone A2(w2,76561198000000011ull,"AliceTheCallee","Z-City #1"),B2(w2,76561198000000012ull,"BobTheCaller","Z-City EU"),E(w2,76561198000000013ull,"EveEvil","Evil server");
  A2.net.publish(true);B2.net.publish(true);E.net.publish(true);run(w2,{&A2,&B2,&E},1.0);
  {auto& d=w2.lobbies[A2.net.myLobby()].data;
   CHECK(!d.count("sid")&&d["v"]=="2"&&d["h"].size()==16&&d["h"]==Csc::sidHash(A2.net.myLobby(),A2.f.me),"fix77 lobby: no SteamID, protocol 2, owner hash salted with the lobby");}
  B2.net.list(w2.now);run(w2,{&A2,&B2,&E},0.5);
  {auto l2=B2.last("list");bool leak=false;if(l2)for(auto& e:l2->list)if(e.sid)leak=true;
   CHECK(l2&&l2->list.size()==2&&!leak,"fix77 list: 2 entries, no SteamID in it");}
  // a protocol 1 entry (old DLL / somebody faking one) is not listed
  {Fake old(w2,76561198000000099ull);uint64_t c=old.createLobby(2);(void)c;uint64_t id=w2.nextLobby-1;auto& l=w2.lobbies[id];l.data["zphcsc"]="1";l.data["v"]="1";l.data["sid"]="76561198000000099";l.data["n"]="OldPhone";
   run(w2,{&A2,&B2,&E},2.1);B2.ev.clear();B2.net.list(w2.now);run(w2,{&A2,&B2,&E},0.5);bool seenOld=false;if(auto l3=B2.last("list"))for(auto& e:l3->list)if(e.name=="OldPhone")seenOld=true;
   CHECK(!seenOld,"fix77: a protocol 1 entry is not listed (no downgrade)");
   // a fake entry: data copied from Alice's, owned by someone else -> the owner hash does not fit
   auto& a=w2.lobbies[A2.net.myLobby()].data;l.data=a;l.data["n"]="FakeAlice";
   B2.ev.clear();B2.net.call(id,w2.now);run(w2,{&A2,&B2,&E},1.0);
   CHECK(B2.has("ended","unavailable"),"fix77: a copied entry of somebody else (fake caller ID / redirect): unavailable");
   w2.lobbies.erase(id);w2.users.erase(old.me);}
  w2.wire.clear();B2.ev.clear();
  B2.net.call(A2.net.myLobby(),w2.now);run(w2,{&A2,&B2,&E},1.5);
  auto r=A2.last("ring");
  CHECK(r&&r->name=="BobTheCaller"&&r->sid==B2.f.me&&r->tag==Csc::sidTag(B2.f.me)&&r->tag.size()==4,"fix77 ring: name, the SteamID Steam gave, tag %s",r?r->tag.c_str():"-");
  CHECK(A2.net.secure()&&B2.net.secure()&&A2.net.safetyCode().size()==7&&A2.net.safetyCode()==B2.net.safetyCode()&&r&&r->code==B2.net.safetyCode(),"fix77 keys: both secure, the same safety code %s",A2.net.safetyCode().c_str());
  A2.net.answer(w2.now);run(w2,{&A2,&B2,&E},0.3);
  CHECK(A2.net.state()==Csc::Active&&B2.net.state()==Csc::Active,"fix77: answered");
  B2.net.sendLua(5,"secret-pose-XYZ",true);B2.net.sendVideo(std::string(3000,'Q')+"JPEGSECRET");
  {uint16_t sq=0;uint8_t fr[Csc::FrameBytes];std::memset(fr,'V',sizeof fr);for(int i=0;i<20;++i)B2.net.sendAudio(sq++,fr);}
  run(w2,{&A2,&B2,&E},0.3);
  {auto contains=[&](const std::string& s){for(auto& p:w2.wire)if(std::search(p.begin(),p.end(),s.begin(),s.end())!=p.end())return true;return false;};
   CHECK(!contains("Z-City EU")&&!contains("BobTheCaller")&&!contains("AliceTheCallee")&&!contains("secret-pose")&&!contains("JPEGSECRET")&&!contains("QQQQQQQQQQQQ")&&!contains("VVVVVVVVVVVV")&&!contains("gm_construct"),
    "fix77 on the wire (%d packets): no name, server, map, voice, Lua message or picture in clear",int(w2.wire.size()));}
  // tampering: one bit of every packet Bob -> Alice flipped (payload, then the header)
  for(int at:{30,3,9}){
   int before=A2.net.rejected();size_t msgs=A2.ev.size();
   w2.tamper=[&](uint64_t f,uint64_t t,std::vector<uint8_t>& d){if(f==B2.f.me&&t==A2.f.me&&d.size()>size_t(at))d[size_t(at)]^=1;return 1;};
   B2.net.sendLua(6,"tampered",true);run(w2,{&A2,&B2,&E},0.2);w2.tamper=nullptr;
   bool got=false;for(size_t i=msgs;i<A2.ev.size();++i)if(A2.ev[i].kind=="msg")got=true;
   CHECK(!got&&A2.net.rejected()>before&&A2.net.state()==Csc::Active,"fix77: a changed packet (byte %d) is dropped (%d rejected), the call goes on",at,A2.net.rejected()-before);
  }
  // replay: every packet Bob -> Alice delivered twice
  {int before=A2.net.rejected();w2.tamper=[&](uint64_t f,uint64_t t,std::vector<uint8_t>&){return f==B2.f.me&&t==A2.f.me?2:1;};
   B2.net.sendLua(7,"once",true);run(w2,{&A2,&B2,&E},0.2);w2.tamper=nullptr;int n=0;for(auto& e:A2.ev)if(e.kind=="msg"&&e.data=="once")++n;
   CHECK(n==1&&A2.net.rejected()>before,"fix77: a replayed packet is dropped (message once, %d rejected)",A2.net.rejected()-before);}
  // forged hang up: a sealed-looking BYE without the key, from Bob's own account and from Eve
  {std::vector<uint8_t> h;for(auto& p:w2.wire)if(p.size()>=8&&p[2]==2&&p[3]==Csc::PKeep){h.assign(p.begin(),p.begin()+8);}
   CHECK(h.size()==8,"fix77: the call id is visible (header) - but useless without the key");
   std::vector<uint8_t> bye=h;bye[3]=Csc::PBye;bye.resize(8+8+16,0);bye[8]=200;
   run(w2,{&A2,&B2,&E},0.4); // late copies of the replay test are through
   int before=A2.net.rejected();B2.f.send(A2.f.me,bye.data(),uint32_t(bye.size()),Csc::SendReliable);
   A2.f.accept(E.f.me);E.f.send(A2.f.me,bye.data(),uint32_t(bye.size()),Csc::SendReliable);
   std::vector<uint8_t> v1=h;v1[2]=1;v1[3]=Csc::PBye;E.f.send(A2.f.me,v1.data(),uint32_t(v1.size()),Csc::SendReliable);B2.f.send(A2.f.me,v1.data(),uint32_t(v1.size()),Csc::SendReliable);
   std::vector<uint8_t> big(200000,0);big[0]='Z';big[1]='P';big[2]=2;big[3]=Csc::PLua;E.f.send(A2.f.me,big.data(),uint32_t(big.size()),Csc::SendReliable);
   run(w2,{&A2,&B2,&E},0.3);
   CHECK(A2.net.state()==Csc::Active&&B2.net.state()==Csc::Active&&A2.net.rejected()==before+1,"fix77: forged BYE (no key / Eve / protocol 1) and a 200 KB packet: ignored, the call goes on");
   B2.net.sendLua(8,"after",true);run(w2,{&A2,&B2,&E},0.2);bool after=false;for(auto& e:A2.ev)if(e.kind=="msg"&&e.data=="after")after=true;
   CHECK(after,"fix77: the receive queue is not stuck after a huge packet");}
  A2.net.hangup();run(w2,{&A2,&B2,&E},0.5);
  CHECK(B2.has("ended","hangup")&&A2.net.state()==Csc::Idle&&!A2.net.secure()&&A2.net.safetyCode().empty(),"fix77: hang up, the keys are wiped");
  // a man in the middle swaps the callee's key in the hello: the confirmation fails, no call, no ring
  {run(w2,{&A2,&B2,&E},2);A2.ev.clear();B2.ev.clear();uint8_t ep[32],epub[32];CscCrypto::random(ep,32);CscCrypto::x25519Base(epub,ep);
   w2.tamper=[&](uint64_t f,uint64_t,std::vector<uint8_t>& d){if(f==A2.f.me&&d.size()==64&&d[3]==Csc::PHello)std::memcpy(d.data()+8,epub,32);return 1;};
   B2.net.call(A2.net.myLobby(),w2.now);run(w2,{&A2,&B2,&E},12);w2.tamper=nullptr;
   CHECK(B2.has("ended","unavailable")&&!B2.has("ringing")&&!A2.has("ring")&&B2.net.rejected()>0&&A2.net.state()==Csc::Idle,"fix77: a swapped key (man in the middle): no call (%d rejected)",B2.net.rejected());}
  // somebody sits in the entry without ringing: a fresh entry, he is blocked for 10 minutes
  {run(w2,{&A2,&B2,&E},3);uint64_t old=A2.net.myLobby();uint64_t c=E.f.joinLobby(old);run(w2,{&A2,&B2,&E},0.4);uint64_t lb=0;uint32_t resp=0;E.f.enterResult(c,lb,resp);
   run(w2,{&A2,&B2,&E},6.5);
   CHECK(resp==1&&A2.net.myLobby()!=old&&A2.net.published()&&A2.net.blocked(E.f.me,w2.now),"fix77: a squatter: Alice gets a fresh entry, Eve is blocked");
   E.ev.clear();A2.ev.clear();E.net.call(A2.net.myLobby(),w2.now);run(w2,{&A2,&B2,&E},2);
   CHECK(E.has("ended","busy")&&!A2.has("ring"),"fix77: the blocked squatter can not ring");}
  // ring spam: a 5th call within a minute from the same account is refused
  {Phone S(w2,76561198000000014ull,"Spammer","x");int rings=0;
   for(int i=0;i<5;++i){S.ev.clear();A2.ev.clear();S.net.call(A2.net.myLobby(),w2.now);run(w2,{&A2,&B2,&E,&S},1.2);if(A2.has("ring"))++rings;S.net.hangup();run(w2,{&A2,&B2,&E,&S},0.6);
    // the 10 s pause after a missed call is waited out
    run(w2,{&A2,&B2,&E,&S},10.2);}
   CHECK(rings==4,"fix77: ring spam: 4 calls a minute ring, the 5th not (%d rang)",rings);}
  CHECK(Csc::sidTag(76561198000000012ull)==Csc::sidTag(76561198000000012ull)&&Csc::sidTag(76561198000000012ull)!=Csc::sidTag(76561198000000013ull),"fix77: tag: the same account = the same tag");
 }
 // ---- clip(): control bytes, long names, broken UTF-8
 CHECK(Csc::clip("a\x01""b",10)=="a b"&&Csc::clip(std::string(200,'x'),64).size()==64&&Csc::clip("\xd0\x9f\xd1",10)=="\xd0\x9f","clip: control / length / UTF-8");
 CHECK(Csc::parseU64("76561198000000001")==76561198000000001ull&&Csc::parseU64("12a")==0&&Csc::parseU64("99999999999999999999")==0,"parseU64");
 printf(fails?"\n%d FAILED\n":"\nALL OK\n",fails);return fails?1:0;
}
