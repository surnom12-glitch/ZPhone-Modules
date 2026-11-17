#pragma once
#include "windows.h"
enum EDataFlow{eRender,eCapture};enum ERole{eConsole};
struct IActivateAudioInterfaceAsyncOperation:IUnknown{virtual HRESULT GetActivateResult(HRESULT*,IUnknown**)=0;};
struct IActivateAudioInterfaceCompletionHandler:IUnknown{virtual HRESULT ActivateCompleted(IActivateAudioInterfaceAsyncOperation*)=0;};
struct IMMDevice:IUnknown{virtual HRESULT Activate(REFIID,DWORD,PROPVARIANT*,void**)=0;};
struct IMMDeviceEnumerator:IUnknown{virtual HRESULT GetDefaultAudioEndpoint(EDataFlow,ERole,IMMDevice**)=0;};
struct MMDeviceEnumerator{};
