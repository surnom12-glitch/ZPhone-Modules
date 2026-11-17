#pragma once
#include <d3dcompiler.h>
#include <vector>
#include <array>
#include <stdexcept>
namespace PhoneGPU {
// Worker-only GPU reduction: never read full-resolution video back to the CPU
// just to build a small gallery proxy. No waits or render-thread operations.
class Thumbnail {
 template<class T>static void drop(T*& p){if(p){p->Release();p=nullptr;}}
 static void check(HRESULT r){if(FAILED(r))throw std::runtime_error("GPU thumbnail initialization failed");}
 ID3D11VertexShader* vs=nullptr;ID3D11PixelShader* ps=nullptr;
 ID3D11SamplerState* sampler=nullptr;ID3D11RasterizerState* raster=nullptr;ID3D11DepthStencilState* depth=nullptr;
 std::array<ID3D11ShaderResourceView*,8> direct{};
 ID3D11Texture2D* input=nullptr;ID3D11ShaderResourceView* view=nullptr;
 ID3D11Texture2D* color=nullptr;ID3D11RenderTargetView* target=nullptr;

 static std::vector<unsigned char> compile(const char* source,const char* entry,const char* profile){
  using Compile=HRESULT(WINAPI*)(LPCVOID,SIZE_T,LPCSTR,const D3D_SHADER_MACRO*,ID3DInclude*,LPCSTR,LPCSTR,UINT,UINT,ID3DBlob**,ID3DBlob**);
  HMODULE lib=LoadLibraryW(L"d3dcompiler_47.dll");if(!lib)throw std::runtime_error("D3DCompiler 47 unavailable for gallery thumbnail");
  auto fn=reinterpret_cast<Compile>(GetProcAddress(lib,"D3DCompile"));ID3DBlob* code=nullptr;ID3DBlob* error=nullptr;
  HRESULT result=fn?fn(source,strlen(source),nullptr,nullptr,nullptr,entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error):E_NOINTERFACE;
  drop(error);std::vector<unsigned char> bytes;
  try{check(result);bytes.assign(static_cast<unsigned char*>(code->GetBufferPointer()),static_cast<unsigned char*>(code->GetBufferPointer())+code->GetBufferSize());}
  catch(...){drop(code);FreeLibrary(lib);throw;}
  drop(code);FreeLibrary(lib);return bytes;
 }
 public:
 ~Thumbnail(){for(auto& v:direct)drop(v);drop(view);drop(input);drop(target);drop(color);drop(depth);drop(raster);drop(sampler);drop(ps);drop(vs);}
 void init(ID3D11Device* device,const std::array<ID3D11Texture2D*,8>& images,ID3D11Texture2D** staging){
  static const char source[]=R"(
struct Out { float4 p:SV_POSITION; float2 uv:TEXCOORD0; };
Out vert(uint id:SV_VertexID) { Out o; o.uv=float2((id<<1)&2,id&2); o.p=float4(o.uv.x*2-1,1-o.uv.y*2,0,1); return o; }
Texture2D image:register(t0); SamplerState linearClamp:register(s0);
float4 pixel(Out i):SV_TARGET { return image.SampleLevel(linearClamp,i.uv,0); }
)";
  // Immutable bytecode is reused between clips; no recurring shader compilation.
  static const auto v=compile(source,"vert","vs_4_0");static const auto p=compile(source,"pixel","ps_4_0");
  check(device->CreateVertexShader(v.data(),v.size(),nullptr,&vs));check(device->CreatePixelShader(p.data(),p.size(),nullptr,&ps));
  // D3D9 shared RTs need not expose D3D11_BIND_SHADER_RESOURCE. Use a private
  // GPU texture so thumbnail support never depends on that driver's bind flags.
  bool all=true;
  for(size_t i=0;i<images.size();++i)if(FAILED(device->CreateShaderResourceView(images[i],nullptr,&direct[i]))){all=false;break;}
  if(!all){
   for(auto& v:direct)drop(v);
   D3D11_TEXTURE2D_DESC in{};images[0]->GetDesc(&in);in.Usage=D3D11_USAGE_DEFAULT;in.BindFlags=D3D11_BIND_SHADER_RESOURCE;in.CPUAccessFlags=0;in.MiscFlags=0;
   check(device->CreateTexture2D(&in,nullptr,&input));check(device->CreateShaderResourceView(input,nullptr,&view));
  }
  D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
  check(device->CreateSamplerState(&sd,&sampler));D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;check(device->CreateRasterizerState(&rd,&raster));
  D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=FALSE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;dd.DepthFunc=D3D11_COMPARISON_ALWAYS;check(device->CreateDepthStencilState(&dd,&depth));
  D3D11_TEXTURE2D_DESC td{};td.Width=192;td.Height=336;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
  check(device->CreateTexture2D(&td,nullptr,&color));check(device->CreateRenderTargetView(color,nullptr,&target));
  td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateTexture2D(&td,nullptr,staging));
 }
 bool directSource()const{return direct[0]!=nullptr;}
 void copy(ID3D11DeviceContext* context,size_t index,ID3D11Texture2D* video,ID3D11Texture2D* staging){
  if(input)context->CopyResource(input,video);
  auto source=direct[index]?direct[index]:view;
  D3D11_VIEWPORT vp{0,0,192,336,0,1};context->RSSetViewports(1,&vp);context->RSSetState(raster);
  context->OMSetDepthStencilState(depth,0);context->OMSetBlendState(nullptr,nullptr,0xffffffff);context->OMSetRenderTargets(1,&target,nullptr);
  context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(vs,nullptr,0);context->PSSetShader(ps,nullptr,0);context->PSSetSamplers(0,1,&sampler);context->PSSetShaderResources(0,1,&source);context->Draw(3,0);
  ID3D11ShaderResourceView* empty=nullptr;context->PSSetShaderResources(0,1,&empty);context->OMSetRenderTargets(0,nullptr,nullptr);
  context->CopyResource(staging,color);
 }
};
}
