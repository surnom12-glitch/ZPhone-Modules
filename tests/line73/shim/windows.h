// Linux test shim: just enough Win32/COM for audio_capture.h (fake devices in test.cpp)
#pragma once
#include <cstdint>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
typedef int32_t HRESULT;typedef uint32_t ULONG,DWORD,UINT32,UINT;typedef uint64_t UINT64;typedef uint8_t BYTE;typedef uint16_t WORD;
typedef void* HANDLE;typedef void* HMODULE;typedef const wchar_t* LPCWSTR;typedef int BOOL;
#define WINAPI
#define STDMETHODCALLTYPE
#define TRUE 1
#define FALSE 0
#define S_OK HRESULT(0)
#define S_FALSE HRESULT(1)
#define E_FAIL HRESULT(0x80004005)
#define E_POINTER HRESULT(0x80004003)
#define E_NOINTERFACE HRESULT(0x80004002)
#define SUCCEEDED(x) (HRESULT(x)>=0)
#define FAILED(x) (HRESULT(x)<0)
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
struct GUID{uint32_t Data1;uint16_t Data2,Data3;uint8_t Data4[8];};
typedef const GUID& REFIID;
inline bool IsEqualIID(REFIID a,REFIID b){return std::memcmp(&a,&b,sizeof(GUID))==0;}
template<class T>inline const GUID& shim_iid(){static GUID g{uint32_t(reinterpret_cast<uintptr_t>(&g)),0,0,{0}};return g;}
#define __uuidof(x) shim_iid<x>()
union LARGE_INTEGER{int64_t QuadPart;};
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f){f->QuadPart=10000000;return 1;}
extern std::atomic<int64_t> g_qpcOffset;
inline BOOL QueryPerformanceCounter(LARGE_INTEGER* c){c->QuadPart=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()/100+g_qpcOffset.load();return 1;}
struct ShimEvent{std::atomic<bool> set{false};bool manual=false;};
inline HANDLE CreateEventW(void*,BOOL manual,BOOL init,void*){auto e=new ShimEvent;e->manual=manual;e->set=init;return e;}
inline BOOL SetEvent(HANDLE h){static_cast<ShimEvent*>(h)->set=true;return 1;}
inline BOOL CloseHandle(HANDLE h){delete static_cast<ShimEvent*>(h);return 1;}
inline DWORD WaitForMultipleObjects(DWORD n,const HANDLE* hs,BOOL,DWORD ms){
 auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
 for(;;){for(DWORD i=0;i<n;++i){auto e=static_cast<ShimEvent*>(hs[i]);if(e->set){if(!e->manual)e->set=false;return WAIT_OBJECT_0+i;}}
  if(std::chrono::steady_clock::now()>=end)return WAIT_TIMEOUT;std::this_thread::sleep_for(std::chrono::microseconds(500));}
}
inline DWORD WaitForSingleObject(HANDLE h,DWORD ms){return WaitForMultipleObjects(1,&h,0,ms);}
inline void Sleep(DWORD ms){std::this_thread::sleep_for(std::chrono::milliseconds(ms));}
#define COINIT_MULTITHREADED 0
#define CLSCTX_ALL 0
inline HRESULT CoInitializeEx(void*,DWORD){return S_OK;}
inline void CoUninitialize(){}
inline void CoTaskMemFree(void* p){free(p);}
HRESULT CoCreateInstance(REFIID clsid,void*,DWORD,REFIID iid,void** out);
inline HMODULE LoadLibraryW(LPCWSTR){return nullptr;}
inline void* GetProcAddress(HMODULE,const char*){return nullptr;}
inline DWORD GetCurrentProcessId(){return 1;}
struct IUnknown{virtual HRESULT QueryInterface(REFIID,void**)=0;virtual ULONG AddRef()=0;virtual ULONG Release()=0;virtual ~IUnknown(){}};
#define VT_BLOB 65
struct BLOB{ULONG cbSize;BYTE* pBlobData;};
struct PROPVARIANT{uint16_t vt;BLOB blob;};
inline void PropVariantInit(PROPVARIANT* p){std::memset(p,0,sizeof *p);}
#define WAVE_FORMAT_PCM 1
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE
#pragma pack(push,1)
struct WAVEFORMATEX{WORD wFormatTag,nChannels;DWORD nSamplesPerSec,nAvgBytesPerSec;WORD nBlockAlign,wBitsPerSample,cbSize;};
struct WAVEFORMATEXTENSIBLE{WAVEFORMATEX Format;WORD wValidBitsPerSample;DWORD dwChannelMask;GUID SubFormat;};
#pragma pack(pop)
// fix64: the phone-ear bank decodes on a below-normal thread
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
inline HANDLE GetCurrentThread(){return nullptr;}
inline BOOL SetThreadPriority(HANDLE,int){return 1;}
// fix73 line test: the render side of WASAPI
typedef wchar_t* LPWSTR;
