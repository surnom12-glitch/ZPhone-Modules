#pragma once
#include "windows.h"
// fix73 line test: IAudioClient + IAudioRenderClient (shared-mode render)
typedef int64_t REFERENCE_TIME;
enum AUDCLNT_SHAREMODE{AUDCLNT_SHAREMODE_SHARED};
struct IAudioRenderClient:IUnknown{virtual HRESULT GetBuffer(UINT32,BYTE**)=0;virtual HRESULT ReleaseBuffer(UINT32,DWORD)=0;};
struct IAudioClient:IUnknown{virtual HRESULT Initialize(AUDCLNT_SHAREMODE,DWORD,REFERENCE_TIME,REFERENCE_TIME,const WAVEFORMATEX*,const GUID*)=0;
 virtual HRESULT GetBufferSize(UINT32*)=0;virtual HRESULT GetCurrentPadding(UINT32*)=0;
 virtual HRESULT GetMixFormat(WAVEFORMATEX**)=0;virtual HRESULT GetService(REFIID,void**)=0;virtual HRESULT Start()=0;virtual HRESULT Stop()=0;};
