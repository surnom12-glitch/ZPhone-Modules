// fix77: src/csc_crypto.h against OpenSSL (Linux only):
//   g++ -std=c++17 -O2 -Wall -fsanitize=address,undefined cryptotest.cpp -o cryptotest -lcrypto && ./cryptotest
#include <cstdio>
#include <vector>
#include <random>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include "../../src/csc_crypto.h"
using namespace CscCrypto;
static int fails=0,oks=0;
#define CHECK(c,...) do{if(!(c)){++fails;printf("FAIL %s:%d: ",__FILE__,__LINE__);printf(__VA_ARGS__);printf("\n");}else ++oks;}while(0)
static std::mt19937_64 rng(77);
static std::vector<uint8_t> rnd(size_t n){std::vector<uint8_t> v(n);for(auto& b:v)b=uint8_t(rng());return v;}
static std::vector<uint8_t> hex(const char* s){std::vector<uint8_t> v;for(;s[0]&&s[1];s+=2){unsigned x;sscanf(s,"%2x",&x);v.push_back(uint8_t(x));}return v;}
int main(){
 // SHA-256 / HMAC vs OpenSSL, many lengths
 for(int n=0;n<300;n++){auto m=rnd(size_t(n)*3);uint8_t a[32],b[32];sha256(m.data(),m.size(),a);SHA256(m.data(),m.size(),b);CHECK(!memcmp(a,b,32),"sha256 len %d",n*3);
  auto k=rnd(size_t(n%130));k.reserve(1);unsigned bl=32;static const uint8_t ek[1]={0};hmac256(k.data(),k.size(),m.data(),m.size(),a);HMAC(EVP_sha256(),k.empty()?ek:k.data(),int(k.size()),m.data(),m.size(),b,&bl);CHECK(!memcmp(a,b,32),"hmac len %d key %zu",n*3,k.size());}
 // incremental SHA
 {auto m=rnd(1000);Sha256 s;for(size_t i=0;i<m.size();i+=37)s.update(m.data()+i,std::min<size_t>(37,m.size()-i));uint8_t a[32],b[32];s.final(a);SHA256(m.data(),m.size(),b);CHECK(!memcmp(a,b,32),"sha256 incremental");}
 // HKDF: RFC 5869 test case 1 + OpenSSL
 {auto ikm=hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"),salt=hex("000102030405060708090a0b0c"),info=hex("f0f1f2f3f4f5f6f7f8f9");
  auto want=hex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");uint8_t out[42];
  hkdf(salt.data(),salt.size(),ikm.data(),ikm.size(),info.data(),info.size(),out,42);CHECK(!memcmp(out,want.data(),42),"hkdf rfc5869 #1");}
 for(int t=0;t<50;t++){auto ikm=rnd(32),salt=rnd(size_t(t%40)),info=rnd(size_t(t*2));size_t L=size_t(1+t*5);std::vector<uint8_t> a(L),b(L);
  hkdf(salt.data(),salt.size(),ikm.data(),ikm.size(),info.data(),info.size(),a.data(),L);
  EVP_PKEY_CTX* c=EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF,nullptr);EVP_PKEY_derive_init(c);EVP_PKEY_CTX_set_hkdf_md(c,EVP_sha256());
  EVP_PKEY_CTX_set1_hkdf_salt(c,salt.data(),int(salt.size()));EVP_PKEY_CTX_set1_hkdf_key(c,ikm.data(),int(ikm.size()));EVP_PKEY_CTX_add1_hkdf_info(c,info.data(),int(info.size()));
  size_t bl=L;EVP_PKEY_derive(c,b.data(),&bl);EVP_PKEY_CTX_free(c);CHECK(a==b,"hkdf vs openssl L=%zu",L);}
 // X25519: RFC 7748 vector + OpenSSL, both ways
 {auto k=hex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),u=hex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"),
   want=hex("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");uint8_t q[32];x25519(q,k.data(),u.data());CHECK(!memcmp(q,want.data(),32),"x25519 rfc7748");}
 for(int t=0;t<200;t++){auto a=rnd(32),b=rnd(32);uint8_t A[32],B[32],s1[32],s2[32];x25519Base(A,a.data());x25519Base(B,b.data());
  CHECK(x25519(s1,a.data(),B)&&x25519(s2,b.data(),A)&&!memcmp(s1,s2,32),"x25519 agree %d",t);
  EVP_PKEY* pa=EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519,nullptr,a.data(),32);uint8_t oA[32];size_t l=32;EVP_PKEY_get_raw_public_key(pa,oA,&l);CHECK(!memcmp(oA,A,32),"x25519 public vs openssl");
  EVP_PKEY* pb=EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519,nullptr,B,32);EVP_PKEY_CTX* c=EVP_PKEY_CTX_new(pa,nullptr);EVP_PKEY_derive_init(c);EVP_PKEY_derive_set_peer(c,pb);
  uint8_t o[32];l=32;EVP_PKEY_derive(c,o,&l);CHECK(!memcmp(o,s1,32),"x25519 shared vs openssl");EVP_PKEY_CTX_free(c);EVP_PKEY_free(pa);EVP_PKEY_free(pb);}
 {uint8_t zero[32]={0},q[32];auto a=rnd(32);CHECK(!x25519(q,a.data(),zero),"x25519 low order point refused");}
 // ChaCha20-Poly1305 vs OpenSSL, all lengths 0..1200 + tamper
 for(int n=0;n<=1200;n+=(n<80?1:13)){auto key=rnd(32),nonce=rnd(12),aad=rnd(size_t(n%33)),pt=rnd(size_t(n));std::vector<uint8_t> me(pt.size()+16),os(pt.size()+16);
  seal(key.data(),nonce.data(),aad.data(),aad.size(),pt.data(),pt.size(),me.data());
  EVP_CIPHER_CTX* c=EVP_CIPHER_CTX_new();EVP_EncryptInit_ex(c,EVP_chacha20_poly1305(),nullptr,nullptr,nullptr);EVP_CIPHER_CTX_ctrl(c,EVP_CTRL_AEAD_SET_IVLEN,12,nullptr);
  EVP_EncryptInit_ex(c,nullptr,nullptr,key.data(),nonce.data());int l=0;if(!aad.empty())EVP_EncryptUpdate(c,nullptr,&l,aad.data(),int(aad.size()));
  if(!pt.empty())EVP_EncryptUpdate(c,os.data(),&l,pt.data(),int(pt.size()));EVP_EncryptFinal_ex(c,os.data()+pt.size(),&l);EVP_CIPHER_CTX_ctrl(c,EVP_CTRL_AEAD_GET_TAG,16,os.data()+pt.size());EVP_CIPHER_CTX_free(c);
  CHECK(me==os,"aead seal vs openssl len %d",n);
  std::vector<uint8_t> back(pt.size()+1);CHECK(open(key.data(),nonce.data(),aad.data(),aad.size(),me.data(),me.size(),back.data())&&std::equal(pt.begin(),pt.end(),back.begin()),"aead open len %d",n);
  auto bad=me;bad[size_t(rng()%bad.size())]^=uint8_t(1u<<(rng()%8));CHECK(!open(key.data(),nonce.data(),aad.data(),aad.size(),bad.data(),bad.size(),back.data()),"aead tamper refused len %d",n);
  if(!aad.empty()){auto a2=aad;a2[0]^=1;CHECK(!open(key.data(),nonce.data(),a2.data(),a2.size(),me.data(),me.size(),back.data()),"aead aad tamper refused");}
  auto n2=nonce;n2[11]^=0x80;CHECK(!open(key.data(),n2.data(),aad.data(),aad.size(),me.data(),me.size(),back.data()),"aead other nonce refused");}
 // RFC 8439 2.8.2 AEAD vector
 {auto key=hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"),nonce=hex("070000004041424344454647"),aad=hex("50515253c0c1c2c3c4c5c6c7");
  const char* txt="Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";size_t n=strlen(txt);
  std::vector<uint8_t> out(n+16);seal(key.data(),nonce.data(),aad.data(),aad.size(),reinterpret_cast<const uint8_t*>(txt),n,out.data());
  auto tag=hex("1ae10b594f09e26a7e902ecbd0600691");CHECK(!memcmp(out.data()+n,tag.data(),16),"aead rfc8439 tag");
  auto ct=hex("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116");
  CHECK(!memcmp(out.data(),ct.data(),n),"aead rfc8439 ciphertext");}
 uint8_t r1[32],r2[32];CHECK(random(r1,32)&&random(r2,32)&&memcmp(r1,r2,32),"csprng");
 printf("%d checks, %d failed\n%s\n",oks+fails,fails,fails?"FAILED":"ALL OK");return fails?1:0;
}
