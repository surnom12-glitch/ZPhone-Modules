// fix69: BGRA -> NV12 on the GPU (D3D11 video processor, every vendor) for encoders that only take
// NV12 GPU frames: h264_mf (Media Foundation: Intel Quick Sync, AMD/NVIDIA MFTs, any DX11 GPU).
// NVENC and AMF read the BGRA frame directly and never use this. Encoder worker only; one NV12
// texture per capture slot (same lifetime as the slot), BT.709 limited range like NVENC/AMF output.
#pragma once
#include <d3d11.h>
#include <array>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
namespace PhoneGPU {
class NV12 {
 template<class T>static void drop(T*& p){if(p){p->Release();p=nullptr;}}
 static void check(HRESULT r,const char* what){if(FAILED(r))throw std::runtime_error(std::string("NV12: ")+what);}
 ID3D11VideoDevice* vd=nullptr;ID3D11VideoContext* vc=nullptr;
 ID3D11VideoProcessorEnumerator* en=nullptr;ID3D11VideoProcessor* vp=nullptr;
 std::array<ID3D11Texture2D*,8> out{};std::array<ID3D11VideoProcessorOutputView*,8> views{};
 std::vector<std::pair<ID3D11Texture2D*,ID3D11VideoProcessorInputView*>> inputs; // shared images + FXAA results
 int width=0,height=0;bool ok=false;
 void clear(){for(auto& p:inputs){drop(p.second);drop(p.first);}inputs.clear();for(auto& v:views)drop(v);for(auto& t:out)drop(t);drop(vp);drop(en);drop(vc);drop(vd);ok=false;}
 ID3D11VideoProcessorInputView* input(ID3D11Texture2D* t){
  for(auto& p:inputs)if(p.first==t)return p.second;
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC d{};d.FourCC=0;d.ViewDimension=D3D11_VPIV_DIMENSION_TEXTURE2D;d.Texture2D.MipSlice=0;d.Texture2D.ArraySlice=0;
  ID3D11VideoProcessorInputView* v=nullptr;check(vd->CreateVideoProcessorInputView(t,en,&d,&v),"input view");
  t->AddRef();inputs.emplace_back(t,v);return v;
 }
 public:
 ~NV12(){clear();}
 bool ready()const{return ok;}
 void init(ID3D11Device* device,ID3D11DeviceContext* context,int w,int h){
  clear();width=w;height=h;
  try{
   if(w<2||h<2||(w&1)||(h&1))throw std::runtime_error("NV12: odd video size");
   check(device->QueryInterface(__uuidof(ID3D11VideoDevice),reinterpret_cast<void**>(&vd)),"video device");
   check(context->QueryInterface(__uuidof(ID3D11VideoContext),reinterpret_cast<void**>(&vc)),"video context");
   D3D11_VIDEO_PROCESSOR_CONTENT_DESC c{};c.InputFrameFormat=D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
   c.InputWidth=UINT(w);c.InputHeight=UINT(h);c.OutputWidth=UINT(w);c.OutputHeight=UINT(h);
   c.InputFrameRate={30,1};c.OutputFrameRate={30,1};c.Usage=D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
   check(vd->CreateVideoProcessorEnumerator(&c,&en),"processor enumerator");
   UINT f=0;check(en->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM,&f),"BGRA support");
   if(!(f&D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT))throw std::runtime_error("NV12: BGRA input unsupported");
   f=0;check(en->CheckVideoProcessorFormat(DXGI_FORMAT_NV12,&f),"NV12 support");
   if(!(f&D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))throw std::runtime_error("NV12: NV12 output unsupported");
   check(vd->CreateVideoProcessor(en,0,&vp),"processor");
   D3D11_TEXTURE2D_DESC t{};t.Width=UINT(w);t.Height=UINT(h);t.MipLevels=1;t.ArraySize=1;t.Format=DXGI_FORMAT_NV12;
   t.SampleDesc.Count=1;t.Usage=D3D11_USAGE_DEFAULT;t.BindFlags=D3D11_BIND_RENDER_TARGET;
   D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC o{};o.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;o.Texture2D.MipSlice=0;
   for(size_t n=0;n<out.size();++n){check(device->CreateTexture2D(&t,nullptr,&out[n]),"NV12 texture");check(vd->CreateVideoProcessorOutputView(out[n],en,&o,&views[n]),"output view");}
   D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};in.Usage=0;in.RGB_Range=0;in.YCbCr_Matrix=1;in.Nominal_Range=D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
   D3D11_VIDEO_PROCESSOR_COLOR_SPACE os{};os.Usage=0;os.RGB_Range=1;os.YCbCr_Matrix=1;os.Nominal_Range=D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
   vc->VideoProcessorSetStreamColorSpace(vp,0,&in);vc->VideoProcessorSetOutputColorSpace(vp,&os);
   vc->VideoProcessorSetStreamFrameFormat(vp,0,D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
   vc->VideoProcessorSetStreamAutoProcessingMode(vp,0,FALSE);
   RECT r{0,0,LONG(w),LONG(h)};
   vc->VideoProcessorSetStreamSourceRect(vp,0,TRUE,&r);vc->VideoProcessorSetStreamDestRect(vp,0,TRUE,&r);vc->VideoProcessorSetOutputTargetRect(vp,TRUE,&r);
   ok=true;
  }catch(...){clear();throw;}
 }
 // the NV12 picture of capture slot `slot` (valid until the slot is free again)
 ID3D11Texture2D* convert(size_t slot,ID3D11Texture2D* source){
  if(!ok||slot>=out.size())throw std::runtime_error("NV12: not ready");
  D3D11_VIDEO_PROCESSOR_STREAM s{};s.Enable=TRUE;s.pInputSurface=input(source);
  check(vc->VideoProcessorBlt(vp,views[slot],0,1,&s),"blit");
  return out[slot];
 }
};
}
