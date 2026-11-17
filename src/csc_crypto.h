#pragma once
// fix77: end-to-end encryption for Cross Server Calls (no third party library, portable C++17,
// also builds for 32 bit Windows). X25519 (key agreement, RFC 7748), ChaCha20-Poly1305 (AEAD,
// RFC 8439), SHA-256 / HMAC / HKDF (RFC 5869), a CSPRNG of the OS (BCryptGenRandom on Windows,
// /dev/urandom elsewhere). Checked against OpenSSL in tests/csc74/cryptotest.cpp.
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <string>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif
#else
#include <cstdio>
#endif

namespace CscCrypto {

// ---------------------------------------------------------------- helpers
inline uint32_t ld32(const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
inline void st32(uint8_t* p,uint32_t v){p[0]=uint8_t(v);p[1]=uint8_t(v>>8);p[2]=uint8_t(v>>16);p[3]=uint8_t(v>>24);}
inline void st64(uint8_t* p,uint64_t v){for(int i=0;i<8;i++)p[i]=uint8_t(v>>(8*i));}
inline uint64_t ld64(const uint8_t* p){uint64_t v=0;for(int i=7;i>=0;i--)v=(v<<8)|p[i];return v;}
// wipe secrets (volatile: the compiler may not drop it)
inline void wipe(void* p,size_t n){volatile uint8_t* v=static_cast<volatile uint8_t*>(p);while(n--)*v++=0;}
// constant time compare
inline bool equal(const uint8_t* a,const uint8_t* b,size_t n){uint8_t d=0;for(size_t i=0;i<n;i++)d|=uint8_t(a[i]^b[i]);return d==0;}

// ---------------------------------------------------------------- CSPRNG
inline bool random(void* out,size_t n){
#ifdef _WIN32
 auto* p=static_cast<uint8_t*>(out);
 while(n){ULONG c=ULONG(n>0x10000000?0x10000000:n);if(BCryptGenRandom(nullptr,p,c,BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)return false;p+=c;n-=c;}
 return true;
#else
 FILE* f=std::fopen("/dev/urandom","rb");if(!f)return false;size_t r=std::fread(out,1,n,f);std::fclose(f);return r==n;
#endif
}

// ---------------------------------------------------------------- SHA-256
struct Sha256{
 uint32_t h[8];uint64_t len=0;uint8_t buf[64];size_t used=0;
 Sha256(){static const uint32_t i[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};std::memcpy(h,i,sizeof h);}
 static uint32_t ror(uint32_t x,int n){return (x>>n)|(x<<(32-n));}
 void block(const uint8_t* p){
  static const uint32_t K[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
   0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
   0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
   0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  uint32_t w[64];for(int i=0;i<16;i++)w[i]=uint32_t(p[4*i])<<24|uint32_t(p[4*i+1])<<16|uint32_t(p[4*i+2])<<8|uint32_t(p[4*i+3]);
  for(int i=16;i<64;i++){uint32_t s0=ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3),s1=ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
  uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],k=h[7];
  for(int i=0;i<64;i++){uint32_t S1=ror(e,6)^ror(e,11)^ror(e,25),ch=(e&f)^(~e&g),t1=k+S1+ch+K[i]+w[i],S0=ror(a,2)^ror(a,13)^ror(a,22),mj=(a&b)^(a&c)^(b&c),t2=S0+mj;
   k=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
  h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=k;
 }
 void update(const void* d,size_t n){const uint8_t* p=static_cast<const uint8_t*>(d);len+=n;
  while(n){size_t c=64-used;if(c>n)c=n;std::memcpy(buf+used,p,c);used+=c;p+=c;n-=c;if(used==64){block(buf);used=0;}}}
 void final(uint8_t out[32]){uint64_t bits=len*8;uint8_t pad=0x80;update(&pad,1);uint8_t z=0;while(used!=56)update(&z,1);
  uint8_t l[8];for(int i=0;i<8;i++)l[i]=uint8_t(bits>>(56-8*i));update(l,8);
  for(int i=0;i<8;i++){out[4*i]=uint8_t(h[i]>>24);out[4*i+1]=uint8_t(h[i]>>16);out[4*i+2]=uint8_t(h[i]>>8);out[4*i+3]=uint8_t(h[i]);}
  wipe(buf,sizeof buf);wipe(h,sizeof h);}
};
inline void sha256(const void* d,size_t n,uint8_t out[32]){Sha256 s;s.update(d,n);s.final(out);}

inline void hmac256(const uint8_t* key,size_t kn,const void* d,size_t n,uint8_t out[32]){
 uint8_t k[64]={0};if(kn>64)sha256(key,kn,k);else if(kn)std::memcpy(k,key,kn);
 uint8_t ip[64],op[64];for(int i=0;i<64;i++){ip[i]=uint8_t(k[i]^0x36);op[i]=uint8_t(k[i]^0x5c);}
 uint8_t in[32];Sha256 a;a.update(ip,64);a.update(d,n);a.final(in);
 Sha256 b;b.update(op,64);b.update(in,32);b.final(out);
 wipe(k,64);wipe(ip,64);wipe(op,64);wipe(in,32);
}
// HKDF-SHA256 (extract + expand), out up to 255*32 bytes
inline void hkdf(const uint8_t* salt,size_t sn,const uint8_t* ikm,size_t in,const uint8_t* info,size_t infn,uint8_t* out,size_t on){
 uint8_t prk[32];hmac256(salt,sn,ikm,in,prk);
 uint8_t t[32];size_t tn=0;uint8_t ctr=1;std::string m;
 while(on){m.assign(reinterpret_cast<const char*>(t),tn);m.append(reinterpret_cast<const char*>(info),infn);m.push_back(char(ctr++));
  hmac256(prk,32,m.data(),m.size(),t);tn=32;size_t c=on<32?on:32;std::memcpy(out,t,c);out+=c;on-=c;}
 wipe(prk,32);wipe(t,32);if(!m.empty())wipe(&m[0],m.size());
}

// ---------------------------------------------------------------- X25519 (TweetNaCl field arithmetic, constant time)
namespace x25519_detail{
typedef int64_t gf[16];
inline void car(gf o){for(int i=0;i<16;i++){o[i]+=int64_t(1)<<16;int64_t c=o[i]>>16;if(i<15)o[i+1]+=c-1;else o[0]+=38*(c-1);o[i]-=c*65536;}}
inline void sel(gf p,gf q,int b){int64_t c=~(int64_t(b)-1);for(int i=0;i<16;i++){int64_t t=c&(p[i]^q[i]);p[i]^=t;q[i]^=t;}}
inline void pack(uint8_t* o,const gf n){gf m,t;for(int i=0;i<16;i++)t[i]=n[i];car(t);car(t);car(t);
 for(int j=0;j<2;j++){m[0]=t[0]-0xffed;for(int i=1;i<15;i++){m[i]=t[i]-0xffff-((m[i-1]>>16)&1);m[i-1]&=0xffff;}
  m[15]=t[15]-0x7fff-((m[14]>>16)&1);int b=int((m[15]>>16)&1);m[14]&=0xffff;sel(t,m,1-b);}
 for(int i=0;i<16;i++){o[2*i]=uint8_t(t[i]&0xff);o[2*i+1]=uint8_t((t[i]>>8)&0xff);}}
inline void unpack(gf o,const uint8_t* n){for(int i=0;i<16;i++)o[i]=n[2*i]+(int64_t(n[2*i+1])<<8);o[15]&=0x7fff;}
inline void A(gf o,const gf a,const gf b){for(int i=0;i<16;i++)o[i]=a[i]+b[i];}
inline void Z(gf o,const gf a,const gf b){for(int i=0;i<16;i++)o[i]=a[i]-b[i];}
inline void M(gf o,const gf a,const gf b){int64_t t[31];for(int i=0;i<31;i++)t[i]=0;for(int i=0;i<16;i++)for(int j=0;j<16;j++)t[i+j]+=a[i]*b[j];
 for(int i=0;i<15;i++)t[i]+=38*t[i+16];for(int i=0;i<16;i++)o[i]=t[i];car(o);car(o);}
inline void S(gf o,const gf a){M(o,a,a);}
inline void inv(gf o,const gf in){gf c;for(int a=0;a<16;a++)c[a]=in[a];for(int a=253;a>=0;a--){S(c,c);if(a!=2&&a!=4)M(c,c,in);}for(int a=0;a<16;a++)o[a]=c[a];}
}
// q = n * p ; returns false for an all zero result (a low order point from a bad peer)
inline bool x25519(uint8_t q[32],const uint8_t n[32],const uint8_t p[32]){
 using namespace x25519_detail;
 static const gf k121665={0xDB41,1};
 uint8_t z[32];gf x,a,b,c,d,e,f;
 for(int i=0;i<31;i++)z[i]=n[i];z[31]=uint8_t((n[31]&127)|64);z[0]&=248;
 unpack(x,p);for(int i=0;i<16;i++){b[i]=x[i];d[i]=a[i]=c[i]=0;}a[0]=d[0]=1;
 for(int i=254;i>=0;--i){int r=(z[i>>3]>>(i&7))&1;sel(a,b,r);sel(c,d,r);A(e,a,c);Z(a,a,c);A(c,b,d);Z(b,b,d);S(d,e);S(f,a);M(a,c,a);M(c,b,e);
  A(e,a,c);Z(a,a,c);S(b,a);Z(c,d,f);M(a,c,k121665);A(a,a,d);M(c,c,a);M(a,d,f);M(d,b,x);S(b,e);sel(a,b,r);sel(c,d,r);}
 inv(c,c);M(a,a,c);pack(q,a);
 wipe(z,32);wipe(a,sizeof a);wipe(b,sizeof b);wipe(c,sizeof c);wipe(d,sizeof d);wipe(e,sizeof e);wipe(f,sizeof f);
 uint8_t acc=0;for(int i=0;i<32;i++)acc|=q[i];return acc!=0;
}
inline void x25519Base(uint8_t pub[32],const uint8_t priv[32]){static const uint8_t base[32]={9};x25519(pub,priv,base);}

// ---------------------------------------------------------------- ChaCha20 + Poly1305 (RFC 8439)
inline void chachaBlock(const uint8_t key[32],uint32_t counter,const uint8_t nonce[12],uint8_t out[64]){
 uint32_t s[16]={0x61707865,0x3320646e,0x79622d32,0x6b206574};
 for(int i=0;i<8;i++)s[4+i]=ld32(key+4*i);s[12]=counter;s[13]=ld32(nonce);s[14]=ld32(nonce+4);s[15]=ld32(nonce+8);
 uint32_t x[16];std::memcpy(x,s,sizeof x);
 auto R=[](uint32_t v,int n){return (v<<n)|(v>>(32-n));};
 auto Q=[&](int a,int b,int c,int d){x[a]+=x[b];x[d]=R(x[d]^x[a],16);x[c]+=x[d];x[b]=R(x[b]^x[c],12);x[a]+=x[b];x[d]=R(x[d]^x[a],8);x[c]+=x[d];x[b]=R(x[b]^x[c],7);};
 for(int i=0;i<10;i++){Q(0,4,8,12);Q(1,5,9,13);Q(2,6,10,14);Q(3,7,11,15);Q(0,5,10,15);Q(1,6,11,12);Q(2,7,8,13);Q(3,4,9,14);}
 for(int i=0;i<16;i++)st32(out+4*i,x[i]+s[i]);
 wipe(x,sizeof x);wipe(s,sizeof s);
}
inline void chachaXor(const uint8_t key[32],uint32_t counter,const uint8_t nonce[12],const uint8_t* in,uint8_t* out,size_t n){
 uint8_t ks[64];while(n){chachaBlock(key,counter++,nonce,ks);size_t c=n<64?n:64;for(size_t i=0;i<c;i++)out[i]=uint8_t(in[i]^ks[i]);in+=c;out+=c;n-=c;}wipe(ks,64);
}
struct Poly1305{ // poly1305-donna, 26 bit limbs (no 128 bit integers needed)
 uint32_t r[5],h[5]={0,0,0,0,0},pad[4];uint8_t buf[16];size_t left=0;
 explicit Poly1305(const uint8_t key[32]){
  r[0]=(ld32(key+0))&0x3ffffff;r[1]=(ld32(key+3)>>2)&0x3ffff03;r[2]=(ld32(key+6)>>4)&0x3ffc0ff;r[3]=(ld32(key+9)>>6)&0x3f03fff;r[4]=(ld32(key+12)>>8)&0x00fffff;
  for(int i=0;i<4;i++)pad[i]=ld32(key+16+4*i);}
 void blocks(const uint8_t* m,size_t n,uint32_t hibit){
  const uint32_t r0=r[0],r1=r[1],r2=r[2],r3=r[3],r4=r[4],s1=r1*5,s2=r2*5,s3=r3*5,s4=r4*5;uint32_t h0=h[0],h1=h[1],h2=h[2],h3=h[3],h4=h[4];
  while(n>=16){h0+=(ld32(m+0))&0x3ffffff;h1+=(ld32(m+3)>>2)&0x3ffffff;h2+=(ld32(m+6)>>4)&0x3ffffff;h3+=(ld32(m+9)>>6)&0x3ffffff;h4+=(ld32(m+12)>>8)|hibit;
   uint64_t d0=uint64_t(h0)*r0+uint64_t(h1)*s4+uint64_t(h2)*s3+uint64_t(h3)*s2+uint64_t(h4)*s1;
   uint64_t d1=uint64_t(h0)*r1+uint64_t(h1)*r0+uint64_t(h2)*s4+uint64_t(h3)*s3+uint64_t(h4)*s2;
   uint64_t d2=uint64_t(h0)*r2+uint64_t(h1)*r1+uint64_t(h2)*r0+uint64_t(h3)*s4+uint64_t(h4)*s3;
   uint64_t d3=uint64_t(h0)*r3+uint64_t(h1)*r2+uint64_t(h2)*r1+uint64_t(h3)*r0+uint64_t(h4)*s4;
   uint64_t d4=uint64_t(h0)*r4+uint64_t(h1)*r3+uint64_t(h2)*r2+uint64_t(h3)*r1+uint64_t(h4)*r0;
   uint32_t c=uint32_t(d0>>26);h0=uint32_t(d0)&0x3ffffff;d1+=c;c=uint32_t(d1>>26);h1=uint32_t(d1)&0x3ffffff;d2+=c;c=uint32_t(d2>>26);h2=uint32_t(d2)&0x3ffffff;
   d3+=c;c=uint32_t(d3>>26);h3=uint32_t(d3)&0x3ffffff;d4+=c;c=uint32_t(d4>>26);h4=uint32_t(d4)&0x3ffffff;h0+=c*5;c=h0>>26;h0&=0x3ffffff;h1+=c;
   m+=16;n-=16;}
  h[0]=h0;h[1]=h1;h[2]=h2;h[3]=h3;h[4]=h4;}
 void update(const uint8_t* m,size_t n){
  if(left){size_t c=16-left;if(c>n)c=n;std::memcpy(buf+left,m,c);left+=c;m+=c;n-=c;if(left<16)return;blocks(buf,16,1u<<24);left=0;}
  if(n>=16){size_t w=n&~size_t(15);blocks(m,w,1u<<24);m+=w;n-=w;}
  if(n){std::memcpy(buf,m,n);left=n;}}
 void final(uint8_t mac[16]){
  if(left){buf[left++]=1;while(left<16)buf[left++]=0;blocks(buf,16,0);}
  uint32_t h0=h[0],h1=h[1],h2=h[2],h3=h[3],h4=h[4],c;
  c=h1>>26;h1&=0x3ffffff;h2+=c;c=h2>>26;h2&=0x3ffffff;h3+=c;c=h3>>26;h3&=0x3ffffff;h4+=c;c=h4>>26;h4&=0x3ffffff;h0+=c*5;c=h0>>26;h0&=0x3ffffff;h1+=c;
  uint32_t g0=h0+5;c=g0>>26;g0&=0x3ffffff;uint32_t g1=h1+c;c=g1>>26;g1&=0x3ffffff;uint32_t g2=h2+c;c=g2>>26;g2&=0x3ffffff;uint32_t g3=h3+c;c=g3>>26;g3&=0x3ffffff;
  uint32_t g4=h4+c-(1u<<26);uint32_t mask=(g4>>31)-1;g0&=mask;g1&=mask;g2&=mask;g3&=mask;g4&=mask;mask=~mask;
  h0=(h0&mask)|g0;h1=(h1&mask)|g1;h2=(h2&mask)|g2;h3=(h3&mask)|g3;h4=(h4&mask)|g4;
  h0=(h0|(h1<<26));h1=((h1>>6)|(h2<<20));h2=((h2>>12)|(h3<<14));h3=((h3>>18)|(h4<<8));
  uint64_t f=uint64_t(h0)+pad[0];h0=uint32_t(f);f=uint64_t(h1)+pad[1]+(f>>32);h1=uint32_t(f);f=uint64_t(h2)+pad[2]+(f>>32);h2=uint32_t(f);f=uint64_t(h3)+pad[3]+(f>>32);h3=uint32_t(f);
  st32(mac,h0);st32(mac+4,h1);st32(mac+8,h2);st32(mac+12,h3);
  wipe(r,sizeof r);wipe(h,sizeof h);wipe(pad,sizeof pad);wipe(buf,sizeof buf);}
};
constexpr size_t TagBytes=16;
inline void aeadMac(const uint8_t key[32],const uint8_t nonce[12],const uint8_t* aad,size_t an,const uint8_t* ct,size_t cn,uint8_t tag[16]){
 uint8_t blk[64];chachaBlock(key,0,nonce,blk);Poly1305 p(blk);wipe(blk,64);
 static const uint8_t zero[16]={0};p.update(aad,an);if(an%16)p.update(zero,16-an%16);p.update(ct,cn);if(cn%16)p.update(zero,16-cn%16);
 uint8_t l[16];st64(l,an);st64(l+8,cn);p.update(l,16);p.final(tag);
}
// out = ciphertext (n bytes) + tag (16)
inline void seal(const uint8_t key[32],const uint8_t nonce[12],const uint8_t* aad,size_t an,const uint8_t* pt,size_t n,uint8_t* out){
 chachaXor(key,1,nonce,pt,out,n);aeadMac(key,nonce,aad,an,out,n,out+n);
}
// in = ciphertext + tag (n = both); out gets n-16 bytes. false = forged / damaged (out untouched)
inline bool open(const uint8_t key[32],const uint8_t nonce[12],const uint8_t* aad,size_t an,const uint8_t* in,size_t n,uint8_t* out){
 if(n<TagBytes)return false;size_t cn=n-TagBytes;uint8_t tag[16];aeadMac(key,nonce,aad,an,in,cn,tag);
 if(!equal(tag,in+cn,16))return false;chachaXor(key,1,nonce,in,out,cn);return true;
}

} // namespace CscCrypto
