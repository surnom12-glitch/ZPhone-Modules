#pragma once
#include "windows.h"
// fix73 line test: the render endpoint (fake devices in linetest.cpp)
enum EDataFlow{eRender,eCapture};enum ERole{eConsole};
struct IMMDevice:IUnknown{virtual HRESULT Activate(REFIID,DWORD,PROPVARIANT*,void**)=0;virtual HRESULT GetId(LPWSTR*)=0;};
struct IMMDeviceEnumerator:IUnknown{virtual HRESULT GetDefaultAudioEndpoint(EDataFlow,ERole,IMMDevice**)=0;};
struct MMDeviceEnumerator{};
