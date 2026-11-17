// Explicit Windows Source ICallQueue / IRefCounted bridge, no foreign C++ RTTI.
// SDK CFunctor variants invoke slot 2 (without virtual destructor) or slot 3.
// The engine owns one reference. Never call a foreign destructor: Release owns it.
#pragma once
#include <atomic>
#include <functional>
#include <utility>
namespace EngineQueueBridge {
inline std::atomic<int> pending{0};
struct Command {void** vtable;unsigned userID=0;std::atomic<int> refs{1};std::atomic<bool> invoked{false};std::function<void()> action;};
inline int addRef(Command* c){return ++c->refs;}
inline int release(Command* c){int n=--c->refs;if(!n){delete c;--pending;}return n;}
inline void invoke(Command* c) noexcept {if(!c->invoked.exchange(true)){try{c->action();}catch(...){/* module actions report their own errors */}}}
inline void* table[]={reinterpret_cast<void*>(addRef),reinterpret_cast<void*>(release),reinterpret_cast<void*>(invoke),reinterpret_cast<void*>(invoke),reinterpret_cast<void*>(invoke)};
inline void submit(void* queue,std::function<void()> action){
 auto c=new Command{table,0,1,false,std::move(action)};++pending;
 if(queue){using Fn=void(*)(void*,void*);reinterpret_cast<Fn>((*reinterpret_cast<void***>(queue))[0])(queue,c);}
 else{invoke(c);release(c);}
}
}
