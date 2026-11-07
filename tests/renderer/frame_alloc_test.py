#!/usr/bin/env python3
"""Exercise and benchmark the production frame allocator against its old body."""
from pathlib import Path
import argparse
import subprocess
import tempfile
from shader_install_test import method

ROOT = Path(__file__).resolve().parents[2]
ENGINE = ROOT / 'Q3E/src/main/jni/darkmod'

HARNESS = r'''
#include "renderer/FrameAllocCache.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>
#include <algorithm>
using byte=unsigned char;
const int FRAME_ALLOC_ALIGNMENT=FrameAllocCache::Alignment;
const int MAX_FRAME_MEMORY=64*1024*1024;
struct Frame {std::atomic<int>frameMemoryAllocated{128};byte *frameMemory;int memoryHighwater=0;} arena;
Frame *frameData=&arena;
uint64_t frameAllocationEpoch=0;
struct {bool value=true;bool GetBool()const{return value;}}r_frameAllocChunks;
struct idLib {template<class... T> static void Error(const char *,T...){throw std::runtime_error("frame arena exhausted");}};
RESET
ALLOCATOR
ORIGINAL

void correctness(){
 // Changes to generation and storage invalidate cached spans. Rewinds must not
 // reuse the old cursor, and zero-length requests retain the alignment contract.
 for(bool optimized:{false,true}){
  r_frameAllocChunks.value=optimized;R_ResetFrameAlloc(128);
  byte *first=(byte*)R_FrameAlloc(17);assert((uintptr_t(first)&127)==0);
  memset(first,0x6d,128);byte *second=(byte*)R_FrameAlloc(256);
  assert(second>=first+128);memset(second,0x32,256);assert(first[0]==0x6d);
  R_ResetFrameAlloc(128);assert(R_FrameAlloc(17)==first);
  assert((uintptr_t(R_FrameAlloc(0))&127)==0);
  byte *large=(byte*)R_FrameAlloc(FrameAllocCache::BlockSize*2+13);
  assert((uintptr_t(large)&127)==0);memset(large,0x17,FrameAllocCache::BlockSize*2+13);
  byte *small=(byte*)R_FrameAlloc(512);memset(small,0x42,512);assert(large[0]==0x17);
  bool rejected=false;try{R_FrameAlloc(-1);}catch(const std::runtime_error &){rejected=true;}assert(rejected);
 }
 r_frameAllocChunks.value=true;
 // Check a backing allocation that is only 16-byte aligned, as Mem_Alloc16 may
 // provide. Even its final partial block and zero-byte request stay 128-aligned.
 FrameAllocCache cache;std::atomic<int>used{112};byte *base=arena.frameMemory+16;
 for(int i=0;i<20;++i){byte *p=cache.Allocate(used,base,4096,1,128);assert(p&&(uintptr_t(p)&127)==0);}
 while(cache.Allocate(used,base,4096,1,128)){}
 assert((uintptr_t(cache.Allocate(used,base,4096,1,0))&127)==0);
 // Failed large requests cannot poison the shared allocation frontier.
 used.store(112);FrameAllocCache nearEnd;
 assert(!nearEnd.Allocate(used,base,4096,2,8192));assert(used==112);
 assert(nearEnd.Allocate(used,base,4096,2,128));
 FrameAllocCache moved;std::atomic<int>top{128};
 assert(moved.Allocate(top,base,4096,3,128)==base+128);top.store(128);
 assert(moved.Allocate(top,base+4096,4096,3,128)==base+4096+128);
 // Verify reservation amortization without putting instrumentation in production.
 R_ResetFrameAlloc(128);int previous=128,reservations=0;
 for(int i=0;i<1000;++i){R_FrameAlloc(128);int now=arena.frameMemoryAllocated.load();if(now!=previous)++reservations;previous=now;}
 assert(reservations==8);
 // Many workers allocate differing sizes. Validate every byte after all jobs
 // join, including across repeated recycling of the same frame arena.
 for(int generation=0;generation<6;++generation){
  R_ResetFrameAlloc(128);
  struct Item{byte *p;int size;byte marker;};
  std::vector<Item>results[8];std::vector<std::thread>workers;
  for(int t=0;t<8;++t)workers.emplace_back([&,t]{
   for(int i=0;i<2000;++i){int bytes=1+(i*71+t*37)%1200;byte marker=byte(1+(i+t)%254);
    byte *p=(byte*)R_FrameAlloc(bytes);memset(p,marker,bytes);results[t].push_back({p,bytes,marker});}
  });
  for(auto &worker:workers)worker.join();
  std::vector<Item>all;for(auto &thread:results)for(const auto &item:thread){
   assert((uintptr_t(item.p)&127)==0);for(int i=0;i<item.size;++i)assert(item.p[i]==item.marker);all.push_back(item);}
  std::sort(all.begin(),all.end(),[](const Item&a,const Item&b){return a.p<b.p;});
  for(size_t i=1;i<all.size();++i)assert(all[i-1].p+all[i-1].size<=all[i].p);
 }
 // Keep the same worker TLS alive while alternating the frontend/back-end
 // arenas. Current allocations must not overwrite the previous frame's data.
 Frame other;other.frameMemory=(byte*)std::malloc(MAX_FRAME_MEMORY);assert(other.frameMemory);
 std::atomic<int>phase{0},finished{0};std::vector<std::thread>persistent;
 struct Sample{byte *p;byte marker;};std::vector<Sample>samples[8],previousFrame;
 for(int t=0;t<8;++t)persistent.emplace_back([&,t]{
  for(int generation=1;generation<=6;++generation){
   while(phase.load()!=generation)std::this_thread::yield();
   for(int i=0;i<2000;++i){byte *p=(byte*)R_FrameAlloc(256);byte value=byte(1+(i+t+generation)%254);
    memset(p,value,256);samples[t].push_back({p,value});}
   finished.fetch_add(1);
  }
 });
 for(int generation=1;generation<=6;++generation){
  frameData=(generation&1)?&arena:&other;
  const auto address=uintptr_t(frameData->frameMemory);
  R_ResetFrameAlloc(128-int(address&127));
  for(auto &thread:samples)thread.clear();finished.store(0);phase.store(generation);
  while(finished.load()!=8)std::this_thread::yield();
  for(const auto &item:previousFrame)for(int i=0;i<256;++i)assert(item.p[i]==item.marker);
  previousFrame.clear();
  for(auto &thread:samples)for(const auto &item:thread){
   assert((uintptr_t(item.p)&127)==0);for(int i=0;i<256;++i)assert(item.p[i]==item.marker);previousFrame.push_back(item);}
 }
 for(auto &thread:persistent)thread.join();frameData=&arena;std::free(other.frameMemory);
 std::puts("PASS: production frame allocations preserve alignment, data, large/zero requests, resets, debug rewinds, backing-store changes and eight-worker isolation; 1000 small requests use eight arena reservations");
}

using Allocate=void *(*)(int);
double measure(Allocate allocate,int threads){
 const int sizes[]={128,256,384,512,128,256,128,640};
 auto before=std::chrono::steady_clock::now();
 for(int frame=0;frame<25;++frame){
  R_ResetFrameAlloc(128);std::atomic<int>ready{0};std::atomic<bool>go{false};std::vector<std::thread>workers;
  for(int t=0;t<threads;++t)workers.emplace_back([&,t]{
   ready.fetch_add(1);while(!go.load(std::memory_order_acquire))std::this_thread::yield();
   for(int i=0;i<30000;++i){byte *p=(byte*)allocate(sizes[(i+t)&7]);p[0]=byte(i);p[127]=byte(t);}
  });
  while(ready.load()!=threads)std::this_thread::yield();go.store(true,std::memory_order_release);
  for(auto &worker:workers)worker.join();
 }
 return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();
}
int main(int argc,char **){
 void *memory=nullptr;assert(posix_memalign(&memory,128,MAX_FRAME_MEMORY)==0);arena.frameMemory=(byte*)memory;
 memset(memory,0,MAX_FRAME_MEMORY);correctness();
 if(argc>1){for(int threads:{1,4}){
  std::vector<double>oldTimes,newTimes;
  for(int order=0;order<6;++order){
   for(bool optimized:(order&1)?std::vector<bool>{true,false}:std::vector<bool>{false,true}){
    r_frameAllocChunks.value=true;double elapsed=measure(optimized?R_FrameAlloc:R_FrameAllocOriginal,threads);
    (optimized?newTimes:oldTimes).push_back(elapsed);
   }
  }
  std::sort(oldTimes.begin(),oldTimes.end());std::sort(newTimes.begin(),newTimes.end());
  std::printf("BENCH threads=%d original median=%.3fms range=%.3f..%.3f chunked median=%.3fms range=%.3f..%.3f\n",
   threads,(oldTimes[2]+oldTimes[3])*.5,oldTimes.front(),oldTimes.back(),(newTimes[2]+newTimes[3])*.5,newTimes.front(),newTimes.back());
 }}
 free(memory);
}
'''

# Preserve the pre-change shipping implementation as an independent baseline.
# CI uses a shallow checkout, so the comparison must not depend on git history.
ORIGINAL = r'''
void *R_FrameAllocOriginal(int bytes) {
 bytes=(bytes+FRAME_ALLOC_ALIGNMENT-1)&~(FRAME_ALLOC_ALIGNMENT-1);
 int end=frameData->frameMemoryAllocated+=bytes;
 if(end>MAX_FRAME_MEMORY)idLib::Error("frame arena exhausted");
 return frameData->frameMemory+end-bytes;
}
'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--emulated-tls', action='store_true')
    parser.add_argument('--skip-sanitizers', action='store_true')
    parser.add_argument('--benchmark', action='store_true', help='Run alternating timing comparisons as well as correctness checks')
    args=parser.parse_args()
    text = HARNESS.replace('RESET', method(ENGINE/'renderer/tr_main.cpp', 'void R_ResetFrameAlloc('))
    text = text.replace('ALLOCATOR', method(ENGINE/'renderer/tr_main.cpp', 'void *R_FrameAlloc(')).replace('ORIGINAL', ORIGINAL)
    with tempfile.TemporaryDirectory(prefix='quest-frame-alloc-') as directory:
        source = Path(directory)/'frame.cpp';source.write_text(text)
        binary = Path(directory)/'frame-test'
        extra=['-femulated-tls'] if args.emulated_tls else []
        subprocess.run([args.cxx,'-std=c++14','-O2',*extra,'-pthread','-I',str(ENGINE),str(source),'-o',str(binary)],check=True)
        subprocess.run([str(binary),*(['benchmark'] if args.benchmark else [])],check=True)
        # A separately instrumented run checks all allocation/lifecycle cases;
        # sanitizer overhead is excluded from the timing comparisons above.
        if not args.skip_sanitizers:
            subprocess.run(['g++','-std=c++14','-O1','-g','-pthread','-fsanitize=address,undefined',
                            '-fno-omit-frame-pointer','-I',str(ENGINE),str(source),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)


if __name__ == '__main__':
    main()
