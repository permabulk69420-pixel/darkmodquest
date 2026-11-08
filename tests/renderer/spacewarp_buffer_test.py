#!/usr/bin/env python3
"""Check production GPU ring fences against delayed current/previous-frame reads."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
ENGINE=ROOT/'Q3E/src/main/jni/darkmod'

SHELL=r'''
#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <stddef.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
using byte=unsigned char;using GLenum=unsigned;using GLuint=unsigned;using GLsync=void *;
using GLbitfield=unsigned;using GLintptr=long;using GLsizeiptr=long;
enum{GL_INVALID_ENUM,GL_ARRAY_BUFFER,GL_DYNAMIC_DRAW,GL_MAP_WRITE_BIT=4,GL_MAP_PERSISTENT_BIT=8,GL_MAP_COHERENT_BIT=16,GL_MAP_INVALIDATE_RANGE_BIT=32,GL_MAP_UNSYNCHRONIZED_BIT=64,GL_SYNC_GPU_COMMANDS_COMPLETE=128,GL_SYNC_FLUSH_COMMANDS_BIT=256,GL_ALREADY_SIGNALED,GL_CONDITION_SATISFIED,GL_WAIT_FAILED,GL_TIMEOUT_EXPIRED};
const int CVAR_ARCHIVE=1,CVAR_RENDERER=2;
struct idCVar{int value=0;template<class... T>idCVar(const char *,const char *,T...){}int GetInteger()const{return value;}};
struct idCVarBool{bool value=false;operator bool()const{return value;}};
idCVarBool r_usePersistentMapping;bool GLAD_GL_ARB_buffer_storage=true;
unsigned ALIGN(unsigned x,unsigned n){return (x+n-1)/n*n;}
void *Mem_Alloc16(unsigned n){void *p=nullptr;assert(posix_memalign(&p,16,n)==0);return p;}
void Mem_Free16(void *p){std::free(p);}
static unsigned nextBuffer=1,boundBuffer=0;
static uint64_t submitted=0,completed=0;
static std::map<unsigned,std::vector<byte>>buffers;
static std::map<GLsync,uint64_t>fences;
static int waits=0;
void qglGenBuffers(int,GLuint *p){*p=nextBuffer++;buffers[*p]={};}
void qglBindBuffer(GLenum,GLuint b){boundBuffer=b;}
void qglBufferData(GLenum,unsigned n,const void *,GLenum){buffers[boundBuffer].resize(n);}
void qglBufferStorage(GLenum,unsigned n,const void *,GLbitfield){buffers[boundBuffer].resize(n);}
void qglBufferSubData(GLenum,unsigned start,unsigned n,const void *p){memcpy(buffers[boundBuffer].data()+start,p,n);}
void *qglMapBufferRange(GLenum,unsigned start,unsigned,GLbitfield){return buffers[boundBuffer].data()+start;}
void qglUnmapBuffer(GLenum){}
void qglBindBufferRange(GLenum,GLuint,GLuint,GLintptr,GLuint){}
void qglDeleteBuffers(int,GLuint *p){buffers.erase(*p);}
GLsync qglFenceSync(GLenum,unsigned){auto *sync=new int;fences[sync]=++submitted;return sync;}
void qglDeleteSync(GLsync s){assert(fences.erase(s)==1);delete static_cast<int *>(s);}
GLenum qglClientWaitSync(GLsync s,GLbitfield flags,uint64_t){
 const auto stamp=fences.at(s);if(completed>=stamp)return GL_ALREADY_SIGNALED;
 if(!flags)return GL_TIMEOUT_EXPIRED;completed=stamp;++waits;return GL_CONDITION_SATISFIED;
}
'''

HARNESS=r'''
#include "renderer/backend/GpuBuffer.cpp"
#include <cstdio>
bool exercise(bool retained,bool previousReads,bool persistent,int updateMode){
 r_usePersistentMapping.value=persistent;r_gpuBufferNonpersistentUpdateMode.value=updateMode;
 submitted=completed=0;waits=0;
 GpuBuffer buffer;buffer.InitWriteFrameAhead(GL_ARRAY_BUFFER,128,16,retained);
 const int count=buffer.FrameCount();assert(count==(retained?4:3));
 std::vector<uint64_t>lastRead(count,0);
 auto *base=buffer.CurrentWriteLocation()-128;
 bool safe=true;
 for(int frame=0;frame<60;++frame){
  const int write=(buffer.CurrentWriteLocation()-base)/128;
  assert(write>=0&&write<count);
  // The simulated GPU may still be processing any commands beyond 'completed'.
  // Writing a region before its LAST consumer completes is a real race.
  if(lastRead[write]>completed){safe=false;break;}
  memset(buffer.CurrentWriteLocation(),frame,128);buffer.Commit(128);
  const int drawing=frame%count;
  lastRead[drawing]=++submitted;
  if(previousReads)lastRead[(drawing+count-1)%count]=++submitted;
  buffer.SwitchFrame();
 }
 buffer.Destroy();assert(fences.empty()&&buffers.empty());
 // Restart without SpaceWarp must go back to the original three-region ring.
 buffer.InitWriteFrameAhead(GL_ARRAY_BUFFER,128,16);assert(buffer.FrameCount()==3);buffer.Destroy();
 assert(fences.empty()&&buffers.empty());return safe;
}
int main(){
 for(bool persistent:{false,true})for(int update:{0,1}){
  assert(exercise(false,false,persistent,update));
  // Negative control reproduces the old fence's early overwrite when a motion
  // pass extends the previous vertex region's lifetime.
  assert(!exercise(false,true,persistent,update));
  assert(exercise(true,true,persistent,update));
 }
 std::puts("PASS: production four-region vertex ring protects current and previous GPU consumers under delayed completion, persistent mapping, both upload paths and restart; old three-region history reproduces an overwrite race");
}
'''

def main():
    with tempfile.TemporaryDirectory(prefix='quest-spacewarp-buffers-') as directory:
        base=Path(directory);(base/'precompiled.h').write_text(SHELL)
        cpp=base/'buffers.cpp';cpp.write_text(HARNESS)
        binary=base/'buffers'
        subprocess.run(['g++','-std=c++14','-O2','-I',str(base),'-I',str(ENGINE),str(cpp),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)

if __name__=='__main__':main()
