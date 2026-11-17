// fix74: Cross Server Calls (CSC) - the call itself, without any own server: a Steam lobby of GMod
// (app 4000) is the phone book entry of everyone who allowed it (Settings -> Experimental), Steam P2P
// (ISteamNetworking, through the Valve relay when needed) carries the signalling, the voice, the
// small Lua messages (camera state) and fix75 the picture of the video call (JPEG frames the game
// rendered on the other PC, split into packets: the newest complete frame wins, a lost packet costs
// one frame, never a delay). Independent of Windows (tests/csc74 runs
// it against a fake Steam); csc_steam.h is the real backend. Everything runs on the game thread
// (csc_poll every frame). fix77: protocol 2, end-to-end encrypted (see class Net).
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "csc_audio.h"
#include "csc_crypto.h"
namespace Csc {
struct UpFrame;
constexpr int P2PChannel=7740;
enum SendMode{SendUnreliable=0,SendUnreliableNoDelay=1,SendReliable=2};
// what the Steam side has to offer (csc_steam.h, tests: a fake one)
struct Backend{
 virtual ~Backend(){}
 virtual uint64_t requestLobbyList()=0;                        // filters: zphcsc = 1, worldwide, 50 results
 virtual uint64_t lobbyByIndex(int i)=0;
 virtual uint64_t createLobby(int maxMembers)=0;               // invisible (searchable, not shown to friends)
 virtual uint64_t joinLobby(uint64_t lobby)=0;
 virtual void leaveLobby(uint64_t lobby)=0;
 virtual int memberCount(uint64_t lobby)=0;
 virtual uint64_t memberByIndex(uint64_t lobby,int i)=0;
 virtual std::string lobbyData(uint64_t lobby,const char* key)=0;
 virtual bool setLobbyData(uint64_t lobby,const char* key,const char* value)=0;
 virtual uint64_t lobbyOwner(uint64_t lobby)=0;
 // call results: 0 = pending, 1 = done, -1 = failed
 virtual int listResult(uint64_t call,uint32_t& count)=0;
 virtual int createResult(uint64_t call,int& result,uint64_t& lobby)=0;
 virtual int enterResult(uint64_t call,uint64_t& lobby,uint32_t& response)=0;
 virtual bool send(uint64_t to,const void* data,uint32_t len,int mode)=0;
 virtual bool available(uint32_t& size)=0;
 virtual bool read(void* buf,uint32_t cap,uint32_t& size,uint64_t& from)=0;
 virtual void accept(uint64_t user)=0;
 virtual void close(uint64_t user)=0;
};
struct LobbyInfo{uint64_t lobby=0,sid=0;std::string name,server,map;bool busy=false,callable=true;};
struct Event{std::string kind,reason,name,server,map,data,tag,code;uint64_t sid=0,lobby=0;int msgKind=0;std::vector<LobbyInfo> list;};
enum CallState{Idle=0,Joining=1,RingOut=2,Alerting=3,RingIn=4,Active=5,Greeting=6}; // fix77: Greeting = keys being made for a call coming in (Lua sees 0)
enum PacketType:uint8_t{PRing=1,PRinging=2,PAccept=3,PDecline=4,PBye=5,PKeep=6,PAudio=7,PLua=8,PVideo=9,PHello=10,PRefuse=11,PInfo=12};
// fix75: a video frame = up to 64 packets of 4 bytes (frame id 16 bit, part, parts) + <= VideoPart bytes, and one
// parity packet (part = parts: the size (32 bit) + the XOR of all parts): any ONE lost packet is rebuilt
constexpr size_t VideoPart=1060,VideoParts=64,VideoMax=VideoPart*VideoParts;
enum DeclineReason{DeclDeclined=0,DeclBusy=1,DeclUnavailable=2,DeclNoAnswer=3};
inline const char* declineName(int r){switch(r){case DeclBusy:return "busy";case DeclUnavailable:return "unavailable";case DeclNoAnswer:return "noanswer";default:return "declined";}}
inline std::string u64s(uint64_t v){return std::to_string(static_cast<unsigned long long>(v));}
inline uint64_t parseU64(const std::string& s){if(s.empty()||s.size()>20)return 0;uint64_t v=0;for(char c:s){if(c<'0'||c>'9')return 0;uint64_t n=v*10+uint64_t(c-'0');if(n<v)return 0;v=n;}return v;}
inline std::string clip(const std::string& s,size_t n){ // a string from someone else: length, no control bytes, whole UTF-8
 std::string o;o.reserve(std::min(s.size(),n));
 for(size_t i=0;i<s.size()&&o.size()<n;){unsigned char c=static_cast<unsigned char>(s[i]);size_t k=c<0x80?1:(c>>5)==6?2:(c>>4)==14?3:(c>>3)==30?4:0;
  if(!k||i+k>s.size()||o.size()+k>n){++i;continue;}bool good=true;for(size_t j=1;j<k;++j)if((static_cast<unsigned char>(s[i+j])>>6)!=2)good=false;
  if(!good){++i;continue;}if(k==1&&(c<32||c==127)){o.push_back(' ');++i;continue;}o.append(s,i,k);i+=k;}
 return o;
}
// fix77: the call is end-to-end encrypted. PRing (caller -> callee) carries the caller's ephemeral X25519
// key A, PHello (callee -> caller) the callee's key B + a key confirmation; both sides derive two
// ChaCha20-Poly1305 keys (one per direction) with HKDF from X25519(A,B), the call id and both SteamIDs.
// Everything after that (caller ID, ringing, answer, decline, bye, keep alive, voice, video, Lua
// messages) is sealed: header + 64 bit counter as associated data, a sliding replay window. Packets of
// protocol 1 (fix74 / fix75, plain text) are never read or sent. A 6 digit safety code (the same on
// both phones unless somebody sits in the middle) and a 4 digit tag of the other side's Steam account
// (Steam authenticates the sender of every P2P packet) are shown to Lua.
constexpr uint8_t Proto=2;
constexpr size_t KeyBytes=32,SealOver=8+8+CscCrypto::TagBytes; // header + counter + tag
inline std::string hex(const uint8_t* d,size_t n){static const char* h="0123456789abcdef";std::string s;for(size_t i=0;i<n;i++){s.push_back(h[d[i]>>4]);s.push_back(h[d[i]&15]);}return s;}
// what the lobby shows instead of the SteamID: salted with the lobby id (no list of SteamIDs for everybody)
inline std::string sidHash(uint64_t lobby,uint64_t sid){
 uint8_t m[11+16],o[32];std::memcpy(m,"zphcsc2 sid",11);CscCrypto::st64(m+11,lobby);CscCrypto::st64(m+19,sid);CscCrypto::sha256(m,sizeof m,o);return hex(o,8);
}
// a 4 digit tag of a Steam account (the same account = the same tag, a number can not fake it)
inline std::string sidTag(uint64_t sid){
 if(!sid)return "";uint8_t m[11+8],o[32];std::memcpy(m,"zphcsc2 tag",11);CscCrypto::st64(m+11,sid);CscCrypto::sha256(m,sizeof m,o);
 char b[8];std::snprintf(b,sizeof b,"%04u",unsigned(CscCrypto::ld32(o)%10000u));return b;
}
class Net{
public:
 static constexpr int FrameAudio=8+2+FrameBytes;
 explicit Net(Backend& b):b_(b){}
 ~Net(){shutdown();}
 std::function<void(uint16_t,const uint8_t*,size_t)> onAudio; // the other side's voice (game thread)
 void setMe(uint64_t me){if(me)me_=me;}
 uint64_t me()const{return me_;}
 void publish(bool on){wantPublish_=on;}
 void info(const std::string& name,const std::string& server,const std::string& map,bool busy,bool callable){
  name_=clip(name,64);server_=clip(server,96);map_=clip(map,64);busyLua_=busy;callable_=callable;
 }
 // the phone book: false = a request is running / too soon (2 s)
 bool list(double now){if(listCall_||now-listAt_<2.0)return false;listCall_=b_.requestLobbyList();listAt_=now;if(!listCall_){fail("lobby list request failed");return false;}return true;}
 bool call(uint64_t lobby,double now){
  if(state_!=Idle||!lobby||lobby==myLobby_)return false;
  uint32_t id=0; // the entry is checked after joining (protocol 2, owner hash), the owner by the handshake
if(!CscCrypto::random(&id,sizeof id)){fail("no secure random numbers");return false;}
  joinCall_=b_.joinLobby(lobby);if(!joinCall_){push(ended("unavailable"));return false;}
  begin(Joining,now);caller_=true;lobby_=lobby;peer_=0;callId_=id|1u;
  for(auto& e:last_)if(e.lobby==lobby){peerName_=e.name;peerServer_=e.server;peerMap_=e.map;}
  return true;
 }
 bool answer(double now){if(state_!=RingIn||!keyed_)return false;seal(PAccept,nullptr,0,SendReliable);begin(Active,now);push(simple("accepted"));return true;}
 bool decline(int reason){if(state_!=RingIn)return false;uint8_t r=uint8_t(std::min(3,std::max(0,reason)));seal(PDecline,&r,1,SendReliable);finish(declineName(r),false);return true;}
 void hangup(){
  if(state_==Idle)return;
  if(state_==Greeting){uint8_t r=DeclUnavailable;seal(PDecline,&r,1,SendReliable);reset();return;}
  if(state_==RingIn){uint8_t r=DeclDeclined;seal(PDecline,&r,1,SendReliable);}
  else if(state_!=Joining&&keyed_)seal(PBye,nullptr,0,SendReliable);
  finish("local",false);
 }
 // a Lua message (camera pose, scene): <= 1100 bytes may go unreliable, bigger ones are reliable
 bool sendLua(int kind,const std::string& data,bool reliable){
  if(state_!=Active||data.size()>60000||kind<0||kind>255)return false;
  std::string p(1,char(kind));p+=data;return seal(PLua,p.data(),p.size(),(reliable||p.size()>1100)?SendReliable:SendUnreliable);
 }
 // fix75: one picture (a JPEG) to the other side, unreliable packets. false = no call / too big / Steam refused
 bool sendVideo(const std::string& data){
  if(state_!=Active||data.size()<4||data.size()>VideoMax)return false;
  const size_t count=(data.size()+VideoPart-1)/VideoPart;const uint16_t id=++vidSeq_;
  uint8_t p[8+VideoPart];uint8_t x[VideoPart]={};bool ok=true;
  for(size_t i=0;i<count;++i){
   const size_t off=i*VideoPart,len=std::min(VideoPart,data.size()-off);
   p[0]=uint8_t(id&255);p[1]=uint8_t(id>>8);p[2]=uint8_t(i);p[3]=uint8_t(count);std::memcpy(p+4,data.data()+off,len);
   for(size_t k=0;k<len;++k)x[k]^=p[4+k];
   if(!seal(PVideo,p,4+len,SendUnreliable))ok=false;
  }
  if(count>=2){ // the parity packet
   p[2]=uint8_t(count);const uint32_t n=uint32_t(data.size());for(int i=0;i<4;++i)p[4+i]=uint8_t((n>>(8*i))&255);std::memcpy(p+8,x,VideoPart);
   seal(PVideo,p,8+VideoPart,SendUnreliable);
  }
  if(ok){++vidOut_;vidBytesOut_+=data.size();}
  return ok;
 }
 // fix75: the newest complete picture of the other side not taken yet
 bool takeVideo(std::string& out,uint16_t& id){if(!vidReady_)return false;out.swap(vidDone_);vidDone_.clear();id=vidDoneId_;vidReady_=false;return true;}
 int videoIn()const{return vidIn_;}
 int videoOut()const{return vidOut_;}
 int videoLost()const{return vidLost_;}
 int videoFixed()const{return vidFixed_;}
 double videoBytesIn()const{return double(vidBytesIn_);}
 double videoBytesOut()const{return double(vidBytesOut_);}
 void sendAudio(uint16_t seq,const uint8_t* frame){
  if(state_!=Active)return;uint8_t p[2+FrameBytes];p[0]=uint8_t(seq&255);p[1]=uint8_t(seq>>8);std::memcpy(p+2,frame,FrameBytes);
  if(seal(PAudio,p,sizeof p,SendUnreliable))++audioOut_;
 }
 std::vector<Event> events(){std::vector<Event> e;e.swap(events_);return e;}
 // ---- status (csc_status). the handshake of a call coming in (not ringing yet) is 0 for Lua
 int state()const{return state_==Greeting?Idle:state_;}
 bool caller()const{return caller_;}
 uint64_t peer()const{return state_==Greeting?0:peer_;}
 uint64_t myLobby()const{return myLobby_;}
 uint64_t callLobby()const{return lobby_;}
 double since()const{return since_;}
 const std::string& peerName()const{return peerName_;}
 const std::string& peerServer()const{return peerServer_;}
 const std::string& peerMap()const{return peerMap_;}
 int audioIn()const{return audioIn_;}
 int audioOut()const{return audioOut_;}
 double lastRx()const{return lastRx_;}
 bool published()const{return myLobby_!=0;}
 bool secure()const{return keyed_&&state_!=Greeting;}           // fix77: the call is encrypted + authenticated
 const std::string& safetyCode()const{return code_;}           // fix77: the same on both phones = nobody in the middle
 std::string peerTag()const{return state_==Greeting?std::string():sidTag(peer_);}
 int rejected()const{return badPackets_;}                     // fix77: forged / replayed / damaged packets dropped
 bool blocked(uint64_t user,double now)const{auto it=block_.find(user);return it!=block_.end()&&now<it->second;}
 // every frame
 void pump(double now,int64_t unixNow){
  now_=now;
  results(now);
  receive(now);
  timers(now);
  lobby(now,unixNow);
  for(auto it=closeAt_.begin();it!=closeAt_.end();){if(now>=it->second&&it->first!=peer_){b_.close(it->first);it=closeAt_.erase(it);}else ++it;}
 }
 void shutdown(){
  if(state_!=Idle)hangup();
  retire();
  for(auto& c:closeAt_)b_.close(c.first);closeAt_.clear();
 }
private:
 Backend& b_;uint64_t me_=0;
 bool wantPublish_=false,busyLua_=false,callable_=true;std::string name_,server_,map_;
 uint64_t myLobby_=0,createCall_=0,listCall_=0,joinCall_=0;double createRetry_=0,listAt_=-100,scanAt_=0,beatAt_=-1e9;
 std::map<std::string,std::string> published_;std::map<uint64_t,double> seen_;std::set<uint64_t> accepted_,talked_;
 std::vector<uint64_t> orphanJoins_;std::map<uint64_t,double> closeAt_,block_;std::map<uint64_t,std::vector<double>> attempts_;std::vector<LobbyInfo> last_;
 int state_=Idle;bool caller_=false;uint64_t lobby_=0,peer_=0;uint32_t callId_=0;
 double since_=0,stateAt_=0,lastRx_=0,ringAt_=0,infoAt_=0,keepAt_=0,now_=0;std::string peerName_,peerServer_,peerMap_;
 int audioIn_=0,audioOut_=0,badPackets_=0;std::vector<Event> events_;
 // fix77: the keys of the call
 bool keyed_=false;uint8_t priv_[KeyBytes]={},pubMe_[KeyBytes]={},pubPeer_[KeyBytes]={},tx_[KeyBytes]={},rx_[KeyBytes]={};
 uint64_t txCtr_=0,rxMax_=0,rxBits_=0;std::string code_;std::vector<uint8_t> hello_;
 // fix75: pictures being put together (id -> parts), the last complete one
 struct VidFrame{size_t count=0,got=0,size=0,total=0;bool parity=false;std::vector<bool> have;std::vector<uint8_t> data,x;};
 std::map<uint16_t,VidFrame> vidParts_;std::string vidDone_;uint16_t vidSeq_=0,vidDoneId_=0,vidLast_=0;bool vidReady_=false,vidHave_=false;
 int vidIn_=0,vidOut_=0,vidLost_=0,vidFixed_=0;uint64_t vidBytesIn_=0,vidBytesOut_=0;
 void videoReset(){vidParts_.clear();vidDone_.clear();vidReady_=false;vidHave_=false;}
 void video(const uint8_t* d,size_t n){
  if(n<5)return;
  const uint16_t id=uint16_t(d[0]|(d[1]<<8));const size_t part=d[2],count=d[3],len=n-4;
  const bool par=part==count;
  if(!count||count>VideoParts||part>count||(par?(count<2||len!=4+VideoPart):(len>VideoPart||(part+1<count&&len!=VideoPart))))return;
  if(vidHave_&&int16_t(uint16_t(id-vidLast_))<=0)return; // not newer than the last complete one
  auto it=vidParts_.find(id);
  if(it==vidParts_.end()){
   if(vidParts_.size()>=4){ // the oldest one waiting is given up
    auto old=vidParts_.begin();for(auto j=vidParts_.begin();j!=vidParts_.end();++j)if(int16_t(uint16_t(j->first-id))<int16_t(uint16_t(old->first-id)))old=j;
    if(int16_t(uint16_t(old->first-id))>0)return; // every waiting one is newer than this packet
    vidParts_.erase(old);++vidLost_;
   }
   VidFrame f;f.count=count;f.have.assign(count,false);f.data.resize(count*VideoPart);it=vidParts_.emplace(id,std::move(f)).first;
  }
  VidFrame& f=it->second;
  if(f.count!=count)return;
  if(par){
   if(f.parity)return;
   const size_t total=size_t(d[4])|(size_t(d[5])<<8)|(size_t(d[6])<<16)|(size_t(d[7])<<24);
   if(total<=(count-1)*VideoPart||total>count*VideoPart)return;
   f.parity=true;f.total=total;f.x.assign(d+8,d+8+VideoPart);
  }else{
   if(f.have[part])return;
   f.have[part]=true;++f.got;std::memcpy(f.data.data()+part*VideoPart,d+4,len);if(part+1==count)f.size=part*VideoPart+len;
  }
  if(f.got+1==f.count&&f.parity){ // one part missing: parity XOR the others
   size_t miss=0;while(miss<f.count&&f.have[miss])++miss;
   uint8_t* m=f.data.data()+miss*VideoPart;std::memcpy(m,f.x.data(),VideoPart);
   for(size_t i=0;i<f.count;++i)if(i!=miss){const uint8_t* o=f.data.data()+i*VideoPart;const size_t ol=i+1==f.count?f.size-i*VideoPart:VideoPart;for(size_t k=0;k<ol;++k)m[k]^=o[k];}
   if(miss+1==f.count)f.size=f.total;
   if(f.size!=f.total)return; // the parity does not fit this frame
   f.have[miss]=true;++f.got;++vidFixed_;
  }
  if(f.got<f.count)return;
  vidDone_.assign(reinterpret_cast<const char*>(f.data.data()),f.size);vidDoneId_=id;vidReady_=true;vidHave_=true;vidLast_=id;
  ++vidIn_;vidBytesIn_+=f.size;
  for(auto j=vidParts_.begin();j!=vidParts_.end();){if(int16_t(uint16_t(j->first-id))<=0){if(j->first!=id)++vidLost_;j=vidParts_.erase(j);}else ++j;}
 }
 void push(Event&& e){if(events_.size()<256)events_.push_back(std::move(e));}
 static Event simple(const char* k){Event e;e.kind=k;return e;}
 Event ended(const std::string& reason){Event e;e.kind="ended";e.reason=reason;e.sid=peer_;e.name=peerName_;e.server=peerServer_;return e;}
 void fail(const std::string& t){Event e;e.kind="error";e.reason=t;push(std::move(e));}
 void begin(int s,double now){state_=s;stateAt_=now;lastRx_=now;if(s==Active){since_=now;videoReset();}}
 // ---- fix77 keys
 void wipeKeys(){CscCrypto::wipe(priv_,KeyBytes);CscCrypto::wipe(pubMe_,KeyBytes);CscCrypto::wipe(pubPeer_,KeyBytes);CscCrypto::wipe(tx_,KeyBytes);CscCrypto::wipe(rx_,KeyBytes);
  keyed_=false;txCtr_=0;rxMax_=0;rxBits_=0;code_.clear();hello_.clear();}
 bool newKey(){if(!CscCrypto::random(priv_,KeyBytes))return false;CscCrypto::x25519Base(pubMe_,priv_);return true;}
 // both sides: the caller's key A, the callee's key B, the call id and both accounts go into the keys
 bool derive(){
  uint8_t shared[32];if(!CscCrypto::x25519(shared,priv_,pubPeer_)){CscCrypto::wipe(shared,32);return false;}
  const uint8_t* A=caller_?pubMe_:pubPeer_;const uint8_t* B=caller_?pubPeer_:pubMe_;const uint64_t callerSid=caller_?me_:peer_,calleeSid=caller_?peer_:me_;
  uint8_t info[13+4+8+8+32+32];std::memcpy(info,"zphcsc2 keys:",13);CscCrypto::st32(info+13,callId_);CscCrypto::st64(info+17,callerSid);CscCrypto::st64(info+25,calleeSid);
  std::memcpy(info+33,A,32);std::memcpy(info+65,B,32);
  static const uint8_t salt[]={'Z','P','h','o','n','e',' ','C','S','C',' ','v','2'};
  uint8_t out[72];CscCrypto::hkdf(salt,sizeof salt,shared,32,info,sizeof info,out,sizeof out);CscCrypto::wipe(shared,32);
  std::memcpy(caller_?tx_:rx_,out,32);std::memcpy(caller_?rx_:tx_,out+32,32);
  char b[16];const unsigned c=unsigned(CscCrypto::ld64(out+64)%1000000ull);std::snprintf(b,sizeof b,"%03u %03u",c/1000,c%1000);code_=b;
  CscCrypto::wipe(out,sizeof out);CscCrypto::wipe(priv_,KeyBytes); // the private key is not needed any more (forward secrecy)
  txCtr_=0;rxMax_=0;rxBits_=0;return true;
 }
 static void nonce(uint8_t n[12],uint64_t ctr){std::memset(n,0,4);CscCrypto::st64(n+4,ctr);}
 static void header(uint8_t* p,uint8_t type,uint32_t id){p[0]='Z';p[1]='P';p[2]=Proto;p[3]=type;CscCrypto::st32(p+4,id);}
 bool sendPlain(uint64_t to,uint8_t type,uint32_t id,const void* data,size_t len){
  if(!to)return false;std::vector<uint8_t> p(8+len);header(p.data(),type,id);if(len)std::memcpy(p.data()+8,data,len);return b_.send(to,p.data(),uint32_t(p.size()),SendReliable);
 }
 // a sealed packet: header (8) + counter (8) + ciphertext + tag (16); header + counter are authenticated
 std::vector<uint8_t> sealPacket(uint8_t type,uint64_t ctr,const void* data,size_t len){
  std::vector<uint8_t> p(SealOver+len);header(p.data(),type,callId_);CscCrypto::st64(p.data()+8,ctr);uint8_t n[12];nonce(n,ctr);
  CscCrypto::seal(tx_,n,p.data(),16,static_cast<const uint8_t*>(data),len,p.data()+16);return p;
 }
 bool seal(uint8_t type,const void* data,size_t len,int mode){
  if(!keyed_||!peer_)return false;auto p=sealPacket(type,++txCtr_,len?data:nullptr,len);
  return b_.send(peer_,p.data(),uint32_t(p.size()),mode);
 }
 // false = forged / damaged / replayed (dropped)
 bool openPacket(const uint8_t* p,size_t n,std::vector<uint8_t>& out){
  if(!keyed_||n<SealOver)return false;const uint64_t ctr=CscCrypto::ld64(p+8);if(!ctr)return false;
  if(ctr<=rxMax_){const uint64_t off=rxMax_-ctr;if(off>=64||(rxBits_>>off)&1)return false;}
  out.resize(n-SealOver);uint8_t nn[12];nonce(nn,ctr);
  if(!CscCrypto::open(rx_,nn,p,16,p+16,n-16,out.data()))return false;
  if(ctr>rxMax_){const uint64_t s=ctr-rxMax_;rxBits_=s>=64?0:rxBits_<<s;rxBits_|=1;rxMax_=ctr;}else rxBits_|=uint64_t(1)<<(rxMax_-ctr);
  return true;
 }
 void reset(){ // the handshake of a call coming in failed / timed out: no call, no event
  if(peer_&&peer_!=me_)closeAt_[peer_]=now_+3.0;
  state_=Idle;caller_=false;lobby_=0;peer_=0;callId_=0;wipeKeys();
 }
 void finish(const std::string& reason,bool tell){
  {Event e=ended(reason);if(!tell)e.data="local";push(std::move(e));}
  if(peer_&&seen_.count(peer_))seen_[peer_]=now_; // the caller leaves the entry by himself: 6 s grace
  // a call coming in that was not talked (declined, missed, cancelled): the same caller waits 10 s
  if(!caller_&&peer_&&state_!=Active)block_[peer_]=std::max(block_[peer_],now_+10.0);
  if(caller_&&lobby_){if(state_==Joining&&joinCall_)orphanJoins_.push_back(joinCall_);else b_.leaveLobby(lobby_);}
  if(peer_&&peer_!=me_)closeAt_[peer_]=now_+3.0;
  state_=Idle;caller_=false;lobby_=0;peer_=0;callId_=0;joinCall_=0;peerName_.clear();peerServer_.clear();peerMap_.clear();videoReset();wipeKeys();
 }
 void sendRing(){sendPlain(peer_,PRing,callId_,pubMe_,KeyBytes);}
 void sendInfo(){
  std::string p(1,char(0));auto add=[&](const std::string& s){std::string c=clip(s,64);p.push_back(char(c.size()));p+=c;};add(name_);add(server_);add(map_);
  seal(PInfo,p.data(),p.size(),SendReliable);infoAt_=now_;
 }
 bool isMember(uint64_t user){
  if(!myLobby_)return false;int n=b_.memberCount(myLobby_);
  for(int i=0;i<n&&i<8;++i)if(b_.memberByIndex(myLobby_,i)==user)return true;return false;
 }
 void results(double now){
  uint32_t count=0;
  if(listCall_){int r=b_.listResult(listCall_,count);
   if(r!=0){listCall_=0;Event e;e.kind="list";
    if(r>0)for(uint32_t i=0;i<count&&i<64;++i){
     LobbyInfo l;l.lobby=b_.lobbyByIndex(int(i));if(!l.lobby||l.lobby==myLobby_)continue;
     if(b_.lobbyData(l.lobby,"v")!="2")continue; // protocol 2 only (no plain text calls)
     const std::string h=b_.lobbyData(l.lobby,"h");if(h.size()!=16||(me_&&h==sidHash(l.lobby,me_)))continue; // no hash / myself
     l.name=clip(b_.lobbyData(l.lobby,"n"),64);l.server=clip(b_.lobbyData(l.lobby,"s"),96);l.map=clip(b_.lobbyData(l.lobby,"m"),64);
     l.busy=b_.lobbyData(l.lobby,"b")=="1";l.callable=b_.lobbyData(l.lobby,"p")!="0";
     bool dup=false;for(auto& x:e.list)if(x.lobby==l.lobby)dup=true;if(!dup&&e.list.size()<50)e.list.push_back(l);
    }else e.reason="failed";
    last_=e.list;push(std::move(e));}}
  if(createCall_){int res=0;uint64_t lobby=0;int r=b_.createResult(createCall_,res,lobby);
   if(r!=0){createCall_=0;
    if(r>0&&res==1&&lobby){
     if(!wantPublish_){b_.leaveLobby(lobby);}
     else{myLobby_=lobby;published_.clear();seen_.clear();talked_.clear();beatAt_=-1e9;if(!me_)me_=b_.lobbyOwner(lobby);Event e;e.kind="published";e.lobby=lobby;push(std::move(e));}
    }else{createRetry_=now+30;fail("could not create the CSC lobby (Steam)");}
   }}
  if(joinCall_&&state_==Joining){uint64_t lobby=0;uint32_t resp=0;int r=b_.enterResult(joinCall_,lobby,resp);
   if(r!=0){joinCall_=0;
    if(r<0){lobby_=0;finish("unavailable",true);}
    else if(resp!=1){lobby_=0;finish(resp==4?"busy":"unavailable",true);}
    else{
     const uint64_t owner=b_.lobbyOwner(lobby_);
     // the entry must still be the one of its owner: retired entries / a new owner (somebody who sat in it
     // and kept the data: a fake phone book entry) = gone. the hash is salted with the lobby id
     if(b_.lobbyData(lobby_,"zphcsc")!="1"||b_.lobbyData(lobby_,"v")!="2"||!owner||owner==me_||b_.lobbyData(lobby_,"h")!=sidHash(lobby_,owner))finish("unavailable",true);
     else if(!newKey()){fail("no secure random numbers");finish("unavailable",true);}
     else{peer_=owner;b_.accept(peer_);accepted_.insert(peer_);sendRing();ringAt_=now;begin(RingOut,now);}
    }}}
  for(size_t i=0;i<orphanJoins_.size();){uint64_t lobby=0;uint32_t resp=0;int r=b_.enterResult(orphanJoins_[i],lobby,resp);
   if(r==0){++i;continue;}if(r>0&&resp==1&&lobby)b_.leaveLobby(lobby);orphanJoins_.erase(orphanJoins_.begin()+std::ptrdiff_t(i));}
 }
 void receive(double now){
  std::vector<uint8_t> buf,pt;uint32_t size=0;int guard=0;size_t luaBytes=0,opened=0;int luaN=0;
  while(guard++<600&&opened<(4u<<20)&&b_.available(size)){
   // always read the whole packet (a too small buffer would leave it at the head of the queue for ever)
   buf.resize(std::min<uint32_t>(std::max<uint32_t>(size,16),1u<<21));uint32_t got=0;uint64_t from=0;
   if(!b_.read(buf.data(),uint32_t(buf.size()),got,from))break;
   if(got>65600+SealOver||got<8||buf[0]!='Z'||buf[1]!='P'||buf[2]!=Proto||!from||from==me_)continue;
   const uint8_t type=buf[3];const uint32_t id=CscCrypto::ld32(buf.data()+4);
   const uint8_t* d=buf.data()+8;const size_t n=got-8;
   if(type==PRing){ring(from,id,d,n,now);continue;}
   if(from!=peer_||id!=callId_||state_==Idle||state_==Joining)continue; // not this call (an old one, a stranger)
   if(type==PHello){hello(d,n,now);continue;}
   if(type==PRefuse){if(state_==RingOut&&!keyed_&&caller_)finish(declineName(n?d[0]:0),true);continue;} // busy before any key: nothing to hide
   opened+=got;
   if(!openPacket(buf.data(),got,pt)){++badPackets_;continue;}
   lastRx_=now;const uint8_t* q=pt.data();const size_t m=pt.size();
   switch(type){
    case PInfo:
     if(state_==Greeting)ringing(q,m,now);
     else if(state_==RingIn)seal(PRinging,nullptr,0,SendReliable);
     break;
    case PRinging:if(state_==RingOut&&caller_){begin(Alerting,now);push(simple("ringing"));}break;
    case PAccept:if((state_==RingOut||state_==Alerting)&&caller_){begin(Active,now);push(simple("accepted"));}break;
    case PDecline:if(state_!=Active&&caller_)finish(declineName(m?q[0]:0),true);else if(state_==Greeting)reset();break;
    case PBye:if(state_==Greeting)reset();else finish(state_==Active?"hangup":"cancel",true);break;
    case PAudio:if(state_==Active&&m==2+size_t(FrameBytes)){++audioIn_;if(onAudio)onAudio(uint16_t(q[0]|(q[1]<<8)),q+2,FrameBytes);}break;
    case PLua:if(state_==Active&&m>=1&&luaN<200&&luaBytes<512*1024){++luaN;luaBytes+=m;Event e;e.kind="msg";e.msgKind=q[0];e.data.assign(reinterpret_cast<const char*>(q+1),m-1);push(std::move(e));}break;
    case PVideo:if(state_==Active)video(q,m);break;
    default:break;
   }
  }
 }
 // the caller: the callee's key + its key confirmation (a sealed empty packet, counter 0)
 void hello(const uint8_t* d,size_t n,double now){
  if(state_!=RingOut||!caller_||keyed_||n!=KeyBytes+8+CscCrypto::TagBytes)return;
  std::memcpy(pubPeer_,d,KeyBytes);
  uint8_t keep[KeyBytes];std::memcpy(keep,priv_,KeyBytes);
  if(!derive()){++badPackets_;std::memcpy(priv_,keep,KeyBytes);CscCrypto::wipe(keep,KeyBytes);return;}
  // check the confirmation: aad = header of the hello + counter 0
  uint8_t aad[16];header(aad,PHello,callId_);std::memcpy(aad+8,d+KeyBytes,8);uint8_t nn[12];nonce(nn,0);uint8_t none[1];
  if(CscCrypto::ld64(d+KeyBytes)!=0||!CscCrypto::open(rx_,nn,aad,16,d+KeyBytes+8,CscCrypto::TagBytes,none)){
   ++badPackets_;std::memcpy(priv_,keep,KeyBytes);CscCrypto::wipe(keep,KeyBytes);CscCrypto::wipe(tx_,KeyBytes);CscCrypto::wipe(rx_,KeyBytes);code_.clear();return;}
  CscCrypto::wipe(keep,KeyBytes);
  keyed_=true;rxBits_=1;rxMax_=0;lastRx_=now;sendInfo();
 }
 // the callee: the caller ID came (sealed): the phone rings now
 void ringing(const uint8_t* d,size_t n,double now){
  std::string f[3];size_t p=1;for(int i=0;i<3;++i){if(p>=n)break;size_t l=d[p++];if(p+l>n)break;f[i].assign(reinterpret_cast<const char*>(d+p),l);p+=l;}
  peerName_=clip(f[0],64);peerServer_=clip(f[1],96);peerMap_=clip(f[2],64);
  begin(RingIn,now);seal(PRinging,nullptr,0,SendReliable);
  Event e;e.kind="ring";e.sid=peer_;e.name=peerName_;e.server=peerServer_;e.map=peerMap_;e.tag=sidTag(peer_);e.code=code_;push(std::move(e));
 }
 void refuse(uint64_t to,uint32_t id,uint8_t reason,double now){sendPlain(to,PRefuse,id,&reason,1);if(accepted_.count(to))closeAt_[to]=now+3;}
 void ring(uint64_t from,uint32_t id,const uint8_t* d,size_t n,double now){
  // the same ring again (the hello was late): the same answer
  if(from==peer_&&id==callId_&&state_!=Idle&&!caller_){
   if(n==KeyBytes&&CscCrypto::equal(d,pubPeer_,KeyBytes)&&!hello_.empty())b_.send(peer_,hello_.data(),uint32_t(hello_.size()),SendReliable);
   return;
  }
  if(!isMember(from)||n!=KeyBytes)return; // only someone who joined the phone book entry may ring it
  if(blocked(from,now)){refuse(from,id,DeclBusy,now);return;}
  if(state_!=Idle||busyLua_||!callable_||!wantPublish_){refuse(from,id,callable_&&wantPublish_?DeclBusy:DeclUnavailable,now);return;}
  // no ring spam: 4 calls a minute from one account, then 2 minutes nothing
  auto& a=attempts_[from];a.erase(std::remove_if(a.begin(),a.end(),[&](double t){return now-t>60;}),a.end());
  if(a.size()>=4){block_[from]=now+120;refuse(from,id,DeclBusy,now);return;}
  a.push_back(now);
  if(!newKey()){fail("no secure random numbers");return;}
  caller_=false;peer_=from;callId_=id;lobby_=myLobby_;std::memcpy(pubPeer_,d,KeyBytes);
  if(!derive()){++badPackets_;block_[from]=now+600;reset();return;} // a broken key (low order point)
  keyed_=true;talked_.insert(from);begin(Greeting,now);
  // the hello: my key + a sealed empty packet with counter 0 (proves the keys)
  hello_.assign(8+KeyBytes,0);header(hello_.data(),PHello,callId_);std::memcpy(hello_.data()+8,pubMe_,KeyBytes);
  uint8_t aad[16];header(aad,PHello,callId_);CscCrypto::st64(aad+8,0);uint8_t nn[12];nonce(nn,0);uint8_t tag[CscCrypto::TagBytes];
  CscCrypto::seal(tx_,nn,aad,16,nullptr,0,tag);hello_.insert(hello_.end(),aad+8,aad+16);hello_.insert(hello_.end(),tag,tag+sizeof tag);
  b_.send(peer_,hello_.data(),uint32_t(hello_.size()),SendReliable);
 }
 void timers(double now){
  const double in=now-stateAt_;
  switch(state_){
   case Joining:if(in>12)finish("unavailable",true);break;
   case RingOut:
    if(in>10){if(keyed_)seal(PBye,nullptr,0,SendReliable);finish("unavailable",true);}
    else if(!keyed_&&now-ringAt_>=0.4){ringAt_=now;sendRing();} // until the hello comes (the first one may come before the session is accepted)
    else if(keyed_&&now-infoAt_>=2.0)sendInfo();
    break;
   case Greeting:if(in>8||!isMember(peer_))reset();break; // the caller ID never came / the caller left
   case Alerting:
    if(in>45){seal(PBye,nullptr,0,SendReliable);finish("noanswer",true);break;}
    if(now-lastRx_>15){seal(PBye,nullptr,0,SendReliable);finish("lost",true);break;}
    keep(now);break;
   case RingIn:
    if(in>45){uint8_t r=DeclNoAnswer;seal(PDecline,&r,1,SendReliable);finish("missed",true);break;}
    if(now-lastRx_>15){finish("cancel",true);break;}
    keep(now);break;
   case Active:
    if(now-lastRx_>12){seal(PBye,nullptr,0,SendReliable);finish("lost",true);break;}
    keep(now);break;
   default:break;
  }
 }
 // leave the own entry; somebody may still sit in it (he would own it then): first it stops being one
 void retire(){if(!myLobby_)return;b_.setLobbyData(myLobby_,"zphcsc","0");b_.setLobbyData(myLobby_,"v","0");b_.setLobbyData(myLobby_,"p","0");b_.setLobbyData(myLobby_,"h","");
  b_.leaveLobby(myLobby_);myLobby_=0;published_.clear();talked_.clear();}
 void keep(double now){if(now-keepAt_>=1.0){keepAt_=now;seal(PKeep,nullptr,0,SendUnreliable);}}
 void setData(const char* k,const std::string& v){auto it=published_.find(k);if(it!=published_.end()&&it->second==v)return;if(b_.setLobbyData(myLobby_,k,v.c_str()))published_[k]=v;}
 void lobby(double now,int64_t unixNow){
  if(block_.size()>512)for(auto it=block_.begin();it!=block_.end();){if(now>=it->second)it=block_.erase(it);else ++it;}
  if(attempts_.size()>512)attempts_.clear();
  if(!wantPublish_){if(myLobby_){retire();Event e;e.kind="unpublished";push(std::move(e));}return;}
  if(!myLobby_){if(!createCall_&&now>=createRetry_){createCall_=b_.createLobby(2);if(!createCall_){createRetry_=now+30;fail("could not create the CSC lobby (Steam)");}}return;}
  const bool busy=busyLua_||state_!=Idle;
  // fix77: no SteamID in the phone book (only a hash salted with the lobby id), protocol 2
  setData("zphcsc","1");setData("v","2");setData("h",sidHash(myLobby_,me_));setData("n",name_);setData("s",server_);setData("m",map_);
  setData("b",busy?"1":"0");setData("p",callable_?"1":"0");
  if(now-beatAt_>=60){beatAt_=now;published_.erase("t");setData("t",std::to_string(static_cast<long long>(unixNow)));}
  if(now<scanAt_)return;scanAt_=now+0.25;
  int n=b_.memberCount(myLobby_);std::set<uint64_t> here;
  for(int i=0;i<n&&i<8;++i){uint64_t u=b_.memberByIndex(myLobby_,i);if(!u||u==me_)continue;here.insert(u);
   if(!accepted_.count(u)){b_.accept(u);accepted_.insert(u);}
   if(!seen_.count(u))seen_[u]=now;
  }
  for(auto it=seen_.begin();it!=seen_.end();){if(!here.count(it->first))it=seen_.erase(it);else ++it;}
  for(auto it=accepted_.begin();it!=accepted_.end();){if(!here.count(*it)&&*it!=peer_)it=accepted_.erase(it);else ++it;}
  // somebody sits in the entry without a call (crashed caller, or somebody blocking the line on purpose):
  // a fresh entry, the old one dies with him; who never even rang is blocked for 10 minutes
  bool squat=false;
  for(auto& s:seen_)if(s.first!=peer_&&now-s.second>5){if(!talked_.count(s.first))block_[s.first]=now+600;squat=true;}
  if(squat){retire();createRetry_=now;if(state_==RingIn||state_==Active)lobby_=0;}
 }
};
}
