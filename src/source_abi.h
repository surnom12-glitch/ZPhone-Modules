// Generated from danielga/sourcesdk-minimal commit ab7be3ebe52c608bbf7b846f757fe18886b9f305.
// Windows PC only: excludes Xbox360 and DX_TO_GL_ABSTRACTION virtuals.
// Opaque non-owning interfaces; no MinGW C++ objects cross into the MSVC engine.
#pragma once
#include <cstddef>
struct IMaterialSystem;struct IMatRenderContext;struct ITexture;
template<class R,class... A> R sourceCall(void* self,size_t slot,A... args){
 using Fn=R(*)(void*,A...);return reinterpret_cast<Fn>((*reinterpret_cast<void***>(self))[slot])(self,args...);
}
struct IMaterialSystem {
 IMatRenderContext* GetRenderContext(){return sourceCall<IMatRenderContext*>(this,98);}
 void GMOD_FlushQueue(){return sourceCall<void>(this,120);}
};
struct IMatRenderContext {
 ITexture* GetRenderTarget(){return sourceCall<ITexture*>(this,5);}
 void* GetCallQueue(){return sourceCall<void*>(this,144);}
 void Flush(bool waitGPU = false){return sourceCall<void,bool>(this,2,waitGPU);}
};
struct ITexture {
 int GetActualWidth(){return sourceCall<int>(this,3);}
 int GetActualHeight(){return sourceCall<int>(this,4);}
};
