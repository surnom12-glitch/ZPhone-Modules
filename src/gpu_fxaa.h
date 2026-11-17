// fix64: FXAA 3.11 (NVIDIA, T. Lottes; "quality" PC version, preset 39 = 12 search steps, the
// widest) on the recorded video. Encoder worker only: one fullscreen pass on the GPU per frame,
// no CPU readback, nothing on the game/render thread. The phone image itself is untouched; the
// result goes to a private texture per capture slot (same lifetime as the slot) and NVENC reads it.
#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>
namespace PhoneGPU {
class FXAA {
 template<class T>static void drop(T*& p){if(p){p->Release();p=nullptr;}}
 static void check(HRESULT r,const char* what){if(FAILED(r))throw std::runtime_error(std::string("FXAA: ")+what);}
 ID3D11VertexShader* vs=nullptr;ID3D11PixelShader* ps=nullptr;ID3D11Buffer* params=nullptr;
 ID3D11SamplerState* sampler=nullptr;ID3D11RasterizerState* raster=nullptr;ID3D11DepthStencilState* depth=nullptr;
 std::array<ID3D11ShaderResourceView*,8> direct{};
 ID3D11Texture2D* input=nullptr;ID3D11ShaderResourceView* view=nullptr; // only when the shared images have no SRV
 std::array<ID3D11Texture2D*,8> out{};std::array<ID3D11RenderTargetView*,8> targets{};
 int width=0,height=0,mode=-1;bool ok=false;
 static std::vector<unsigned char> compile(const char* source,const char* entry,const char* profile){
  using Compile=HRESULT(WINAPI*)(LPCVOID,SIZE_T,LPCSTR,const D3D_SHADER_MACRO*,ID3DInclude*,LPCSTR,LPCSTR,UINT,UINT,ID3DBlob**,ID3DBlob**);
  HMODULE lib=LoadLibraryW(L"d3dcompiler_47.dll");if(!lib)throw std::runtime_error("FXAA: D3DCompiler 47 unavailable");
  auto fn=reinterpret_cast<Compile>(GetProcAddress(lib,"D3DCompile"));ID3DBlob* code=nullptr;ID3DBlob* error=nullptr;
  HRESULT result=fn?fn(source,strlen(source),"zhphone_fxaa",nullptr,nullptr,entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error):E_NOINTERFACE;
  drop(error);std::vector<unsigned char> bytes;
  if(SUCCEEDED(result)&&code)bytes.assign(static_cast<unsigned char*>(code->GetBufferPointer()),static_cast<unsigned char*>(code->GetBufferPointer())+code->GetBufferSize());
  drop(code);FreeLibrary(lib);if(bytes.empty())throw std::runtime_error("FXAA: shader compile failed");return bytes;
 }
 public:
 ~FXAA(){for(auto& v:targets)drop(v);for(auto& t:out)drop(t);for(auto& v:direct)drop(v);drop(view);drop(input);drop(depth);drop(raster);drop(sampler);drop(params);drop(ps);drop(vs);}
 bool ready()const{return ok;}
 void init(ID3D11Device* device,const std::array<ID3D11Texture2D*,8>& images,int w,int h){
  static const char source[]=R"(
cbuffer Knobs:register(b0){ float4 frame; float4 knobs; } // frame.xy = 1/size; knobs = subpix, edgeThreshold, edgeThresholdMin
struct Out { float4 p:SV_POSITION; float2 uv:TEXCOORD0; };
Out vert(uint id:SV_VertexID) { Out o; o.uv=float2((id<<1)&2,id&2); o.p=float4(o.uv.x*2-1,1-o.uv.y*2,0,1); return o; }
Texture2D image:register(t0); SamplerState lin:register(s0);
static const float P[12]={1.0,1.0,1.0,1.0,1.0,1.5,2.0,2.0,2.0,2.0,4.0,8.0}; // FXAA_QUALITY__PRESET 39
float Luma(float3 c){ return dot(c,float3(0.299,0.587,0.114)); }
#define TOP(p) image.SampleLevel(lin,p,0)
#define OFF(p,x,y) Luma(image.SampleLevel(lin,p,0,int2(x,y)).rgb)
float4 pixel(Out i):SV_TARGET {
 float2 posM=i.uv; float2 rcpFrame=frame.xy;
 float4 rgbyM=TOP(posM); float lumaM=Luma(rgbyM.rgb);
 float lumaS=OFF(posM,0,1), lumaE=OFF(posM,1,0), lumaN=OFF(posM,0,-1), lumaW=OFF(posM,-1,0);
 float maxSM=max(lumaS,lumaM), minSM=min(lumaS,lumaM);
 float maxESM=max(lumaE,maxSM), minESM=min(lumaE,minSM);
 float maxWN=max(lumaN,lumaW), minWN=min(lumaN,lumaW);
 float rangeMax=max(maxWN,maxESM), rangeMin=min(minWN,minESM);
 float range=rangeMax-rangeMin;
 if(range<max(knobs.z,rangeMax*knobs.y)) return float4(rgbyM.rgb,1);
 float lumaNW=OFF(posM,-1,-1), lumaSE=OFF(posM,1,1), lumaNE=OFF(posM,1,-1), lumaSW=OFF(posM,-1,1);
 float lumaNS=lumaN+lumaS, lumaWE=lumaW+lumaE;
 float subpixRcpRange=1.0/range;
 float subpixNSWE=lumaNS+lumaWE;
 float edgeHorz1=(-2.0*lumaM)+lumaNS, edgeVert1=(-2.0*lumaM)+lumaWE;
 float lumaNESE=lumaNE+lumaSE, lumaNWNE=lumaNW+lumaNE;
 float edgeHorz2=(-2.0*lumaE)+lumaNESE, edgeVert2=(-2.0*lumaN)+lumaNWNE;
 float lumaNWSW=lumaNW+lumaSW, lumaSWSE=lumaSW+lumaSE;
 float edgeHorz4=(abs(edgeHorz1)*2.0)+abs(edgeHorz2), edgeVert4=(abs(edgeVert1)*2.0)+abs(edgeVert2);
 float edgeHorz3=(-2.0*lumaW)+lumaNWSW, edgeVert3=(-2.0*lumaS)+lumaSWSE;
 float edgeHorz=abs(edgeHorz3)+edgeHorz4, edgeVert=abs(edgeVert3)+edgeVert4;
 float subpixNWSWNESE=lumaNWSW+lumaNESE;
 float lengthSign=rcpFrame.x;
 bool horzSpan=edgeHorz>=edgeVert;
 float subpixA=subpixNSWE*2.0+subpixNWSWNESE;
 if(!horzSpan) lumaN=lumaW;
 if(!horzSpan) lumaS=lumaE;
 if(horzSpan) lengthSign=rcpFrame.y;
 float subpixB=(subpixA*(1.0/12.0))-lumaM;
 float gradientN=lumaN-lumaM, gradientS=lumaS-lumaM;
 float lumaNN=lumaN+lumaM, lumaSS=lumaS+lumaM;
 bool pairN=abs(gradientN)>=abs(gradientS);
 float gradient=max(abs(gradientN),abs(gradientS));
 if(pairN) lengthSign=-lengthSign;
 float subpixC=saturate(abs(subpixB)*subpixRcpRange);
 float2 posB=posM;
 float2 offNP=float2(horzSpan?rcpFrame.x:0.0, horzSpan?0.0:rcpFrame.y);
 if(!horzSpan) posB.x+=lengthSign*0.5;
 if(horzSpan) posB.y+=lengthSign*0.5;
 float2 posN=posB-offNP*P[0], posP=posB+offNP*P[0];
 float subpixD=((-2.0)*subpixC)+3.0;
 float lumaEndN=Luma(TOP(posN).rgb);
 float subpixE=subpixC*subpixC;
 float lumaEndP=Luma(TOP(posP).rgb);
 if(!pairN) lumaNN=lumaSS;
 float gradientScaled=gradient*1.0/4.0;
 float lumaMM=lumaM-lumaNN*0.5;
 float subpixF=subpixD*subpixE;
 bool lumaMLTZero=lumaMM<0.0;
 lumaEndN-=lumaNN*0.5; lumaEndP-=lumaNN*0.5;
 bool doneN=abs(lumaEndN)>=gradientScaled, doneP=abs(lumaEndP)>=gradientScaled;
 if(!doneN) posN-=offNP*P[1];
 if(!doneP) posP+=offNP*P[1];
 [loop] for(int k=2;k<12;++k){
  if(doneN&&doneP) break;
  if(!doneN) lumaEndN=Luma(TOP(posN).rgb)-lumaNN*0.5;
  if(!doneP) lumaEndP=Luma(TOP(posP).rgb)-lumaNN*0.5;
  doneN=abs(lumaEndN)>=gradientScaled; doneP=abs(lumaEndP)>=gradientScaled;
  if(!doneN) posN-=offNP*P[k];
  if(!doneP) posP+=offNP*P[k];
 }
 float dstN=horzSpan?posM.x-posN.x:posM.y-posN.y;
 float dstP=horzSpan?posP.x-posM.x:posP.y-posM.y;
 bool goodSpanN=(lumaEndN<0.0)!=lumaMLTZero, goodSpanP=(lumaEndP<0.0)!=lumaMLTZero;
 float spanLengthRcp=1.0/(dstP+dstN);
 bool directionN=dstN<dstP;
 float dst=min(dstN,dstP);
 bool goodSpan=directionN?goodSpanN:goodSpanP;
 float subpixG=subpixF*subpixF;
 float pixelOffset=(dst*(-spanLengthRcp))+0.5;
 float subpixH=subpixG*knobs.x;
 float pixelOffsetGood=goodSpan?pixelOffset:0.0;
 float pixelOffsetSubpix=max(pixelOffsetGood,subpixH);
 if(!horzSpan) posM.x+=pixelOffsetSubpix*lengthSign;
 if(horzSpan) posM.y+=pixelOffsetSubpix*lengthSign;
 return float4(TOP(posM).rgb,1);
}
)";
  // Immutable bytecode compiled once per game session, reused by every clip.
  static const auto v=compile(source,"vert","vs_4_0");static const auto p=compile(source,"pixel","ps_4_0");
  width=w;height=h;
  check(device->CreateVertexShader(v.data(),v.size(),nullptr,&vs),"vertex shader");check(device->CreatePixelShader(p.data(),p.size(),nullptr,&ps),"pixel shader");
  bool all=true;
  for(size_t i=0;i<images.size();++i)if(FAILED(device->CreateShaderResourceView(images[i],nullptr,&direct[i]))){all=false;break;}
  if(!all){ // the shared D3D9 targets have no SRV on this driver: one private GPU copy per frame
   for(auto& d:direct)drop(d);
   D3D11_TEXTURE2D_DESC in{};images[0]->GetDesc(&in);in.Usage=D3D11_USAGE_DEFAULT;in.BindFlags=D3D11_BIND_SHADER_RESOURCE;in.CPUAccessFlags=0;in.MiscFlags=0;
   check(device->CreateTexture2D(&in,nullptr,&input),"input copy");check(device->CreateShaderResourceView(input,nullptr,&view),"input view");
  }
  D3D11_TEXTURE2D_DESC td{};td.Width=UINT(w);td.Height=UINT(h);td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;
  td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
  for(size_t i=0;i<out.size();++i){check(device->CreateTexture2D(&td,nullptr,&out[i]),"output texture");check(device->CreateRenderTargetView(out[i],nullptr,&targets[i]),"output view");}
  D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
  check(device->CreateSamplerState(&sd,&sampler),"sampler");
  D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;check(device->CreateRasterizerState(&rd,&raster),"rasterizer");
  D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=FALSE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;dd.DepthFunc=D3D11_COMPARISON_ALWAYS;check(device->CreateDepthStencilState(&dd,&depth),"depth state");
  D3D11_BUFFER_DESC bd{};bd.ByteWidth=32;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;check(device->CreateBuffer(&bd,nullptr,&params),"constants");
  ok=true;
 }
 // mode 1 = quality (subpix 0.50, edge 0.125, min 0.0312), 2 = strong (subpix 0.75, edge 0.063, min 0.0312)
 ID3D11Texture2D* apply(ID3D11DeviceContext* context,size_t index,ID3D11Texture2D* video,int wanted){
  if(!ok||index>=out.size())return video;
  if(wanted!=mode){
   float k[8]={1.f/float(width),1.f/float(height),0,0,wanted>=2?0.75f:0.50f,wanted>=2?0.063f:0.125f,0.0312f,0};
   context->UpdateSubresource(params,0,nullptr,k,0,0);mode=wanted;
  }
  if(input)context->CopyResource(input,video);
  auto source=direct[index]?direct[index]:view;
  D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&vp);context->RSSetState(raster);
  context->OMSetDepthStencilState(depth,0);context->OMSetBlendState(nullptr,nullptr,0xffffffff);context->OMSetRenderTargets(1,&targets[index],nullptr);
  context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(vs,nullptr,0);context->PSSetShader(ps,nullptr,0);context->PSSetConstantBuffers(0,1,&params);
  context->PSSetSamplers(0,1,&sampler);context->PSSetShaderResources(0,1,&source);context->Draw(3,0);
  ID3D11ShaderResourceView* empty=nullptr;context->PSSetShaderResources(0,1,&empty);context->OMSetRenderTargets(0,nullptr,nullptr);
  return out[index];
 }
};
}
