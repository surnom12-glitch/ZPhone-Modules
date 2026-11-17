// fix74: the real Steam behind csc_net.h - the steam_api(64).dll GMod already loaded (no own copy, no
// own app id, nothing initialised here: the game's Steam session is used as it is).
// Two ways, picked at csc_init:
//  flat : the "flat" C exports of newer steam_api DLLs (SteamAPI_ISteamMatchmaking_* ...) with the
//         interface from SteamAPI_SteamMatchmaking_vNNN() / the old SteamMatchmaking() exports
//  vtable: older DLLs without them: the interfaces of a fixed version straight from the Steam client
//         (SteamInternal_FindOrCreateUserInterface / SteamClient()->GetISteamGenericInterface) and the
//         C++ methods by their slot, called like MSVC does (thiscall on x86; a CSteamID comes back
//         through a hidden pointer after "this")
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include "csc_net.h"
namespace Csc {
#if defined(__i386__)
#define CSC_THIS __attribute__((thiscall))
#else
#define CSC_THIS
#endif
class SteamBackend:public Backend{
public:
 bool init(){
  dll_=GetModuleHandleA(sizeof(void*)==8?"steam_api64.dll":"steam_api.dll");
  if(!dll_)dll_=GetModuleHandleA(sizeof(void*)==8?"steam_api.dll":"steam_api64.dll");
  if(!dll_){error_="steam_api DLL not loaded (Steam not running?)";return false;}
  if(initFlat())return ok_=true;
  if(initVtable())return ok_=true;
  if(error_.empty())error_="this steam_api DLL offers neither the flat API nor the interfaces";
  return false;
 }
 bool ok()const{return ok_;}
 const std::string& how()const{return how_;}
 const std::string& error()const{return error_;}
 // ---- Backend
 uint64_t requestLobbyList()override{
  if(!ok_)return 0;
  if(flat_){fStrF_(mm_,"zphcsc","1",0);fIntF_(mm_,3);fCount_(mm_,50);return fReq_(mm_);}
  reinterpret_cast<VStrF>(slot(mm_,5))(mm_,"zphcsc","1",0);reinterpret_cast<VIntF>(slot(mm_,9))(mm_,3);reinterpret_cast<VIntF>(slot(mm_,10))(mm_,50);
  return reinterpret_cast<VReq>(slot(mm_,4))(mm_);
 }
 uint64_t lobbyByIndex(int i)override{if(!ok_)return 0;if(flat_)return fByIdx_(mm_,i);uint64_t r=0;reinterpret_cast<VByIdx>(slot(mm_,12))(mm_,&r,i);return r;}
 uint64_t createLobby(int maxMembers)override{if(!ok_)return 0;return flat_?fCreate_(mm_,3,maxMembers):reinterpret_cast<VCreate>(slot(mm_,13))(mm_,3,maxMembers);}
 uint64_t joinLobby(uint64_t lobby)override{if(!ok_)return 0;return flat_?fJoin_(mm_,lobby):reinterpret_cast<VJoin>(slot(mm_,14))(mm_,lobby);}
 void leaveLobby(uint64_t lobby)override{if(!ok_||!lobby)return;if(flat_)fLeave_(mm_,lobby);else reinterpret_cast<VLeave>(slot(mm_,15))(mm_,lobby);}
 int memberCount(uint64_t lobby)override{if(!ok_)return 0;return flat_?fNum_(mm_,lobby):reinterpret_cast<VNum>(slot(mm_,17))(mm_,lobby);}
 uint64_t memberByIndex(uint64_t lobby,int i)override{if(!ok_)return 0;if(flat_)return fMem_(mm_,lobby,i);uint64_t r=0;reinterpret_cast<VMem>(slot(mm_,18))(mm_,&r,lobby,i);return r;}
 std::string lobbyData(uint64_t lobby,const char* key)override{
  if(!ok_)return "";const char* v=flat_?fGetD_(mm_,lobby,key):reinterpret_cast<VGetD>(slot(mm_,19))(mm_,lobby,key);
  if(!v)return "";size_t n=strnlen(v,512);return std::string(v,n);
 }
 bool setLobbyData(uint64_t lobby,const char* key,const char* value)override{if(!ok_)return false;return flat_?fSetD_(mm_,lobby,key,value):reinterpret_cast<VSetD>(slot(mm_,20))(mm_,lobby,key,value);}
 uint64_t lobbyOwner(uint64_t lobby)override{if(!ok_)return 0;if(flat_)return fOwner_(mm_,lobby);uint64_t r=0;reinterpret_cast<VOwner>(slot(mm_,35))(mm_,&r,lobby);return r;}
 int listResult(uint64_t call,uint32_t& count)override{uint8_t b[4]={};int r=result(call,b,4,510);if(r>0)std::memcpy(&count,b,4);return r;}
 int createResult(uint64_t call,int& res,uint64_t& lobby)override{uint8_t b[16]={};int r=result(call,b,16,513);if(r>0){int32_t e;std::memcpy(&e,b,4);res=e;std::memcpy(&lobby,b+8,8);}return r;}
 int enterResult(uint64_t call,uint64_t& lobby,uint32_t& resp)override{uint8_t b[24]={};int r=result(call,b,24,504);if(r>0){std::memcpy(&lobby,b,8);std::memcpy(&resp,b+16,4);}return r;}
 bool send(uint64_t to,const void* data,uint32_t len,int mode)override{if(!ok_)return false;return flat_?fSend_(nw_,to,data,len,mode,P2PChannel):reinterpret_cast<VSend>(slot(nw_,0))(nw_,to,data,len,mode,P2PChannel);}
 bool available(uint32_t& size)override{if(!ok_)return false;size=0;return flat_?fAvail_(nw_,&size,P2PChannel):reinterpret_cast<VAvail>(slot(nw_,1))(nw_,&size,P2PChannel);}
 bool read(void* buf,uint32_t cap,uint32_t& size,uint64_t& from)override{if(!ok_)return false;size=0;from=0;return flat_?fRead_(nw_,buf,cap,&size,&from,P2PChannel):reinterpret_cast<VRead>(slot(nw_,2))(nw_,buf,cap,&size,&from,P2PChannel);}
 void accept(uint64_t user)override{if(!ok_||!user)return;if(flat_)fAcc_(nw_,user);else reinterpret_cast<VAcc>(slot(nw_,3))(nw_,user);}
 void close(uint64_t user)override{if(!ok_||!user)return;if(flat_)fClose_(nw_,user);else reinterpret_cast<VAcc>(slot(nw_,4))(nw_,user);}
private:
 HMODULE dll_=nullptr;bool ok_=false,flat_=false;void* mm_=nullptr;void* nw_=nullptr;void* ut_=nullptr;std::string how_,error_;
 // flat: S_CALLTYPE = __cdecl, the interface first, CSteamID = uint64
 using FReq=uint64_t(*)(void*);using FStrF=void(*)(void*,const char*,const char*,int);using FIntF=void(*)(void*,int);
 using FByIdx=uint64_t(*)(void*,int);using FCreate=uint64_t(*)(void*,int,int);using FJoin=uint64_t(*)(void*,uint64_t);using FLeave=void(*)(void*,uint64_t);
 using FNum=int(*)(void*,uint64_t);using FMem=uint64_t(*)(void*,uint64_t,int);using FGetD=const char*(*)(void*,uint64_t,const char*);
 using FSetD=bool(*)(void*,uint64_t,const char*,const char*);using FOwner=uint64_t(*)(void*,uint64_t);
 using FDone=bool(*)(void*,uint64_t,bool*);using FRes=bool(*)(void*,uint64_t,void*,int,int,bool*);
 using FSend=bool(*)(void*,uint64_t,const void*,uint32_t,int,int);using FAvail=bool(*)(void*,uint32_t*,int);
 using FRead=bool(*)(void*,void*,uint32_t,uint32_t*,uint64_t*,int);using FAcc=bool(*)(void*,uint64_t);
 using FIface=void*(*)();using FUser=int32_t(*)();using FFind=void*(*)(int32_t,const char*);
 FReq fReq_=nullptr;FStrF fStrF_=nullptr;FIntF fIntF_=nullptr,fCount_=nullptr;FByIdx fByIdx_=nullptr;FCreate fCreate_=nullptr;FJoin fJoin_=nullptr;FLeave fLeave_=nullptr;
 FNum fNum_=nullptr;FMem fMem_=nullptr;FGetD fGetD_=nullptr;FSetD fSetD_=nullptr;FOwner fOwner_=nullptr;FDone fDone_=nullptr;FRes fRes_=nullptr;
 FSend fSend_=nullptr;FAvail fAvail_=nullptr;FRead fRead_=nullptr;FAcc fAcc_=nullptr,fClose_=nullptr;
 // vtable (MSVC C++ ABI of the Steam client)
 using VReq=uint64_t(CSC_THIS*)(void*);using VStrF=void(CSC_THIS*)(void*,const char*,const char*,int);using VIntF=void(CSC_THIS*)(void*,int);
 using VByIdx=uint64_t*(CSC_THIS*)(void*,uint64_t*,int);using VCreate=uint64_t(CSC_THIS*)(void*,int,int);using VJoin=uint64_t(CSC_THIS*)(void*,uint64_t);
 using VLeave=void(CSC_THIS*)(void*,uint64_t);using VNum=int(CSC_THIS*)(void*,uint64_t);using VMem=uint64_t*(CSC_THIS*)(void*,uint64_t*,uint64_t,int);
 using VGetD=const char*(CSC_THIS*)(void*,uint64_t,const char*);using VSetD=bool(CSC_THIS*)(void*,uint64_t,const char*,const char*);
 using VOwner=uint64_t*(CSC_THIS*)(void*,uint64_t*,uint64_t);
 using VDone=bool(CSC_THIS*)(void*,uint64_t,bool*);using VRes=bool(CSC_THIS*)(void*,uint64_t,void*,int,int,bool*);
 using VSend=bool(CSC_THIS*)(void*,uint64_t,const void*,uint32_t,int,int);using VAvail=bool(CSC_THIS*)(void*,uint32_t*,int);
 using VRead=bool(CSC_THIS*)(void*,void*,uint32_t,uint32_t*,uint64_t*,int);using VAcc=bool(CSC_THIS*)(void*,uint64_t);
 using VGeneric=void*(CSC_THIS*)(void*,int32_t,int32_t,const char*);
 static void* slot(void* iface,int i){return (*reinterpret_cast<void***>(iface))[i];}
 template<class T>bool get(T& f,const char* name){f=reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(dll_,name)));return f!=nullptr;}
 // the newest SteamAPI_<prefix>_vNNN export (the PE export table of steam_api)
 void* versioned(const char* prefix){
  auto base=reinterpret_cast<const uint8_t*>(dll_);auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return nullptr;auto nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
  if(nt->Signature!=IMAGE_NT_SIGNATURE)return nullptr;
  const auto& dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];if(!dir.VirtualAddress||!dir.Size)return nullptr;
  auto ex=reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base+dir.VirtualAddress);auto names=reinterpret_cast<const DWORD*>(base+ex->AddressOfNames);
  const size_t pl=std::strlen(prefix);int best=-1;std::string bestName;
  for(DWORD i=0;i<ex->NumberOfNames&&i<20000;++i){const char* n=reinterpret_cast<const char*>(base+names[i]);
   if(std::strncmp(n,prefix,pl)!=0)continue;const char* v=n+pl;int ver=0,digits=0;while(*v>='0'&&*v<='9'&&digits<6){ver=ver*10+(*v-'0');++v;++digits;}
   if(*v||!digits)continue;if(ver>best){best=ver;bestName=n;}}
  if(best<0)return nullptr;FIface f=nullptr;if(!get(f,bestName.c_str()))return nullptr;void* r=f();if(r)how_+=bestName+" ";return r;
 }
 void* oldAccessor(const char* name){FIface f=nullptr;if(!get(f,name))return nullptr;void* r=f();if(r)how_+=std::string(name)+"() ";return r;}
 void* byVersion(const char* ver){
  FUser user=nullptr,pipe=nullptr;get(user,"SteamAPI_GetHSteamUser");get(pipe,"SteamAPI_GetHSteamPipe");
  FFind find=nullptr;
  if(user&&get(find,"SteamInternal_FindOrCreateUserInterface")){void* r=find(user(),ver);if(r){how_+=std::string(ver)+" ";return r;}}
  FIface client=nullptr;
  if(user&&pipe&&get(client,"SteamClient")){void* c=client();if(c){void* r=reinterpret_cast<VGeneric>(slot(c,12))(c,user(),pipe(),ver);if(r){how_+=std::string(ver)+"(client) ";return r;}}}
  return nullptr;
 }
 bool initFlat(){
  bool all=get(fReq_,"SteamAPI_ISteamMatchmaking_RequestLobbyList")&&get(fStrF_,"SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter")&&
   get(fIntF_,"SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter")&&get(fCount_,"SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter")&&
   get(fByIdx_,"SteamAPI_ISteamMatchmaking_GetLobbyByIndex")&&get(fCreate_,"SteamAPI_ISteamMatchmaking_CreateLobby")&&get(fJoin_,"SteamAPI_ISteamMatchmaking_JoinLobby")&&
   get(fLeave_,"SteamAPI_ISteamMatchmaking_LeaveLobby")&&get(fNum_,"SteamAPI_ISteamMatchmaking_GetNumLobbyMembers")&&get(fMem_,"SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex")&&
   get(fGetD_,"SteamAPI_ISteamMatchmaking_GetLobbyData")&&get(fSetD_,"SteamAPI_ISteamMatchmaking_SetLobbyData")&&get(fOwner_,"SteamAPI_ISteamMatchmaking_GetLobbyOwner")&&
   get(fDone_,"SteamAPI_ISteamUtils_IsAPICallCompleted")&&get(fRes_,"SteamAPI_ISteamUtils_GetAPICallResult")&&
   get(fSend_,"SteamAPI_ISteamNetworking_SendP2PPacket")&&get(fAvail_,"SteamAPI_ISteamNetworking_IsP2PPacketAvailable")&&get(fRead_,"SteamAPI_ISteamNetworking_ReadP2PPacket")&&
   get(fAcc_,"SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser")&&get(fClose_,"SteamAPI_ISteamNetworking_CloseP2PSessionWithUser");
  if(!all)return false;
  how_="flat: ";
  mm_=versioned("SteamAPI_SteamMatchmaking_v");if(!mm_)mm_=oldAccessor("SteamMatchmaking");if(!mm_)mm_=byVersion("SteamMatchMaking009");
  nw_=versioned("SteamAPI_SteamNetworking_v");if(!nw_)nw_=oldAccessor("SteamNetworking");if(!nw_)nw_=byVersion("SteamNetworking005");
  ut_=versioned("SteamAPI_SteamUtils_v");if(!ut_)ut_=oldAccessor("SteamUtils");if(!ut_)ut_=byVersion("SteamUtils009");
  if(!mm_||!nw_||!ut_){error_="Steam interfaces not available (flat)";return false;}
  flat_=true;return true;
 }
 bool initVtable(){
  how_="vtable: ";
  mm_=byVersion("SteamMatchMaking009");
  nw_=byVersion("SteamNetworking005");if(!nw_)nw_=byVersion("SteamNetworking006");
  ut_=byVersion("SteamUtils009");if(!ut_)ut_=byVersion("SteamUtils008");if(!ut_)ut_=byVersion("SteamUtils007");
  if(!mm_||!nw_||!ut_){error_="Steam interfaces not available (no flat API, no SteamInternal / SteamClient)";return false;}
  flat_=false;return true;
 }
 // 0 pending, 1 done, -1 failed
 int result(uint64_t call,void* out,int size,int id){
  if(!ok_||!call)return -1;bool failed=false;
  bool done=flat_?fDone_(ut_,call,&failed):reinterpret_cast<VDone>(slot(ut_,11))(ut_,call,&failed);
  if(!done)return failed?-1:0;
  failed=false;bool got=flat_?fRes_(ut_,call,out,size,id,&failed):reinterpret_cast<VRes>(slot(ut_,13))(ut_,call,out,size,id,&failed);
  return got&&!failed?1:-1;
 }
};
}
