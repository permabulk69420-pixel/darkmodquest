#!/usr/bin/env python3
"""Run production shadow batching, staging reads and draw submission on GLES."""
import ctypes as C
import os
from pathlib import Path
import re
import subprocess
import tempfile
from mobile_gles_test import GLES, source
from shader_install_test import method

ROOT=Path(__file__).resolve().parents[2]
ENGINE=ROOT/'Q3E/src/main/jni/darkmod'
SHELL=r'''
#pragma once
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
using byte=unsigned char;using uint32=uint32_t;using glIndex_t=int;
#define VERTCACHE_FRAMENUM_BITS 15
const unsigned VERTCACHE_FRAME_MASK=(1u<<VERTCACHE_FRAMENUM_BITS)-1;
const int CVAR_ARCHIVE=1,CVAR_RENDERER=2;
struct idCVar{int value=0;template<class... T>idCVar(const char *,const char *,T...){}int GetInteger()const{return value;}};
struct Bool{bool value=false;operator bool()const{return value;}bool GetBool()const{return value;}};
using idCVarBool=Bool;
#define GL_MAP_PERSISTENT_BIT GL_MAP_PERSISTENT_BIT_EXT
#define GL_MAP_COHERENT_BIT GL_MAP_COHERENT_BIT_EXT
Bool r_usePersistentMapping,r_showPrimitives{true};bool GLAD_GL_ARB_buffer_storage=false;
unsigned ALIGN(unsigned v,unsigned n){return (v+n-1)/n*n;}
void *Mem_Alloc16(unsigned n){void *p=nullptr;assert(posix_memalign(&p,16,n)==0);return p;}void Mem_Free16(void *p){free(p);}
void qglBufferStorage(GLenum,GLsizeiptr,const void *,GLbitfield){assert(false);}
ALIASES
static unsigned actualDraws=0;
void RecordedDrawElements(GLenum mode,GLsizei n,GLenum type,const void *p){++actualDraws;glDrawElements(mode,n,type,p);}
#undef qglDrawElements
#define qglDrawElements RecordedDrawElements
#include "renderer/backend/GpuBuffer.cpp"
#include "renderer/backend/MobileShadowBatch.h"
CACHE_HANDLE
struct idDrawVert{float xyz[3],st[2],normal[3],tangent[6];byte color[4];};static_assert(sizeof(idDrawVert)==60,"");
enum attribBind_t{ATTRIB_REGULAR,ATTRIB_SHADOW};enum{VF_REGULAR,VF_SHADOW};struct shadowCache_t{float v[4];};
struct VertexState{void BindVertexBufferAndSetVertexFormat(GLuint vbo,int format){
 glBindBuffer(GL_ARRAY_BUFFER,vbo);glEnableVertexAttribArray(0);glVertexAttribPointer(0,3,GL_FLOAT,0,format==VF_REGULAR?60:16,nullptr);
 glEnableVertexAttribArray(8);glVertexAttribPointer(8,2,GL_FLOAT,0,60,reinterpret_cast<void *>(12));
}}vaState;
struct Geo{GpuBuffer vertexBuffer,indexBuffer;};
struct idVertexCache{
 std::vector<glIndex_t>submittedStaticIndexes;
 Geo dynamicData;GLuint staticVertexBuffer=0,staticIndexBuffer=0,staticShadowBuffer=0,currentIndexBuffer=0;
 int vertexUseCount=0,indexUseCount=0,basePointer=0,currentFrame=2;
 GLuint VertexOffset(vertCacheHandle_t h)const{return h.isStatic?h.offset:dynamicData.vertexBuffer.PhysicalOffset(h.offset);}
 GLuint IndexOffset(vertCacheHandle_t h)const{return h.isStatic?h.offset:dynamicData.indexBuffer.PhysicalOffset(h.offset);}
 void VertexPosition(vertCacheHandle_t,attribBind_t attrib=ATTRIB_REGULAR);
 void *IndexPosition(vertCacheHandle_t);void BindExternalIndexBuffer(GLuint);
 const glIndex_t *ReadSubmittedIndexes(vertCacheHandle_t,unsigned)const;
 void SnapshotStaticIndexes(const glIndex_t *,unsigned,bool);
}vertexCache;
VERTEX_POSITION
INDEX_POSITION
BIND_INDEX
READ_INDEX
SNAPSHOT_INDEX
struct Entity{float modelMatrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};};
struct drawSurf_t{Entity *space;vertCacheHandle_t ambientCache,indexCache;unsigned count=6;bool cutout=false;};
struct View{bool IsLightGem()const{return false;}void *viewEntitys=this;};
struct Backend{View *viewDef=nullptr;struct{int c_drawCalls[8]={},c_drawIndexes[8]={},c_drawVerts[8]={};}pc;}backEnd;
const int DCK_SHADOW=3;
struct ShadowUniforms{
 struct Matrix{GLint location;void Set(const float *v){glUniformMatrix4fv(location,1,0,v);}}modelMatrix;
 struct Float{GLint location;void Set(float v){glUniform1f(location,v);}}alphaTest;
 explicit ShadowUniforms(GLuint p):modelMatrix{glGetUniformLocation(p,"u_modelMatrix")},alphaTest{glGetUniformLocation(p,"u_alphaTest")}{}
};
struct MobileLightingStage{struct Impl{MobileShadowBatch::Cache shadowBatches;int shadowDraws=0;
 void DrawShadowBatch(const MobileShadowBatch::Draw &,ShadowUniforms *);
};};
DRAW_BATCH
void require(bool v,const char *message){if(!v)throw std::runtime_error(message);}
static float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
static void uniform(GLuint p,const char *name,int value){glUniform1i(glGetUniformLocation(p,name),value);}
struct Fixture{
 static constexpr int PerFace=96,MeshCount=6*PerFace,Prefix=66000;
 std::vector<idDrawVert>vertices;std::vector<drawSurf_t>surfs;
 std::array<std::vector<MobileShadowBatch::Source>,6>sources;
 Entity entity,other;GLuint vao=0,program,depth[2]={},fbo[2]={},sample=0,sampleFbo=0,sampleTexture=0,alpha=0;
 byte *indexStagingBase=nullptr;
 View view;MobileLightingStage::Impl renderer;ShadowUniforms u;uint64_t frame=0;
 Fixture(GLuint p,GLuint sampler):program(p),sample(sampler),u(p){
  backEnd.viewDef=&view;glGenVertexArrays(1,&vao);glBindVertexArray(vao);
  vertices.resize(Prefix+4*MeshCount);surfs.resize(MeshCount);
  for(int mesh=0;mesh<MeshCount;++mesh){
   const int face=mesh/PerFace,j=mesh%PerFace;float a=(j%12-5.5f)*.15f,b=(j/12-3.5f)*.15f,d=3.f+.013f*j;
   for(int corner=0;corner<4;++corner){
    float x=a+((corner==1||corner==2)? .075f:-.075f),y=b+((corner>=2)? .075f:-.075f);
    float xyz[3];
    if(face==0){xyz[0]=d;xyz[1]=y;xyz[2]=x;}else if(face==1){xyz[0]=-d;xyz[1]=y;xyz[2]=x;}
    else if(face==2){xyz[0]=x;xyz[1]=d;xyz[2]=y;}else if(face==3){xyz[0]=x;xyz[1]=-d;xyz[2]=y;}
    else if(face==4){xyz[0]=x;xyz[1]=y;xyz[2]=d;}else{xyz[0]=x;xyz[1]=y;xyz[2]=-d;}
    auto &v=vertices[Prefix+mesh*4+corner];memcpy(v.xyz,xyz,sizeof(xyz));v.st[0]=(corner==1||corner==2);v.st[1]=corner>=2;
   }
  }
  glGenBuffers(1,&vertexCache.staticVertexBuffer);glBindBuffer(GL_ARRAY_BUFFER,vertexCache.staticVertexBuffer);
  glBufferData(GL_ARRAY_BUFFER,vertices.size()*sizeof(idDrawVert),vertices.data(),GL_STATIC_DRAW);
  vertexCache.dynamicData.indexBuffer.InitWriteFrameAhead(GL_ELEMENT_ARRAY_BUFFER,MeshCount*32,16,false,true);
  auto *staging=vertexCache.dynamicData.indexBuffer.CurrentWriteLocation();indexStagingBase=staging-MeshCount*32;memset(staging,0,MeshCount*32);
  const uint32_t indexes[]={0,1,2,0,2,3};
  for(int i=0;i<MeshCount;++i){
   memcpy(staging+i*32,indexes,sizeof(indexes));auto &s=surfs[i];s.space=&entity;s.cutout=i%PerFace==PerFace-1;
   s.ambientCache=vertCacheHandle_t::Create(4*60,(Prefix+4*i)*60,0,true);
   s.indexCache=vertCacheHandle_t::Create(24,uintptr_t(vertexCache.dynamicData.indexBuffer.BufferOffset(staging+i*32)),1,false);
  }
  vertexCache.SnapshotStaticIndexes(reinterpret_cast<const glIndex_t *>(staging),MeshCount*32,true);
  auto staticHandle=vertCacheHandle_t::Create(24,0,0,true);
  const auto *owned=vertexCache.ReadSubmittedIndexes(staticHandle,6);require(owned&&owned[1]==1,"owned static index snapshot unavailable");
  require(!vertexCache.ReadSubmittedIndexes(vertCacheHandle_t::Create(24,MeshCount*32-4,0,true),6),"static snapshot range overflow");
  vertexCache.SnapshotStaticIndexes(nullptr,0,false);require(!vertexCache.ReadSubmittedIndexes(staticHandle,6),"off retained static index snapshot");
  vertexCache.dynamicData.indexBuffer.Commit(MeshCount*32);vertexCache.dynamicData.indexBuffer.SwitchFrame();
  auto stale=surfs[0].indexCache;stale.frameNumber=17;require(!vertexCache.ReadSubmittedIndexes(stale,6),"accepted stale index frame number");
  require(!vertexCache.dynamicData.indexBuffer.ReadDrawingRange(0,24),"read outside submitted ring region");
  require(!vertexCache.dynamicData.indexBuffer.ReadDrawingRange(MeshCount*64-4,24),"read crossed submitted ring boundary");
  for(int i=0;i<MeshCount;++i){
   auto &s=surfs[i];if(s.cutout)continue;
   MobileShadowBatch::Source src;src.surface=&s;src.indexes=reinterpret_cast<const uint32_t *>(vertexCache.ReadSubmittedIndexes(s.indexCache,6));
   require(src.indexes,"submitted CPU index copy unavailable");src.count=6;src.vertices=4;src.baseVertex=Prefix+4*i;
   src.vertexBuffer=vertexCache.staticVertexBuffer;src.modelMatrix=entity.modelMatrix;src.slot=i%PerFace;src.content.Add(i);
   sources[i/PerFace].push_back(src);
  }
  glGenTextures(2,depth);glGenFramebuffers(2,fbo);
  for(int i=0;i<2;++i){glBindTexture(GL_TEXTURE_2D,depth[i]);glTexStorage2D(GL_TEXTURE_2D,1,GL_DEPTH_COMPONENT24,64,64);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
   glBindFramebuffer(GL_FRAMEBUFFER,fbo[i]);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,depth[i],0);GLenum none=GL_NONE;glDrawBuffers(1,&none);glReadBuffer(GL_NONE);require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"depth target");}
  glGenTextures(1,&sampleTexture);glBindTexture(GL_TEXTURE_2D,sampleTexture);glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA32UI,64,64);
  glGenFramebuffers(1,&sampleFbo);glBindFramebuffer(GL_FRAMEBUFFER,sampleFbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,sampleTexture,0);require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"integer depth reader");
  glGenTextures(1,&alpha);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,alpha);
  const byte checker[]={255,255,255,0,255,255,255,255,255,255,255,255,255,255,255,0};
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,checker);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  require(glGetError()==GL_NO_ERROR,"fixture GL setup");
 }
 ~Fixture(){renderer.shadowBatches.Clear();vertexCache.dynamicData.indexBuffer.Destroy();glDeleteBuffers(1,&vertexCache.staticVertexBuffer);glDeleteTextures(2,depth);glDeleteTextures(1,&sampleTexture);glDeleteTextures(1,&alpha);glDeleteFramebuffers(2,fbo);glDeleteFramebuffers(1,&sampleFbo);glDeleteVertexArrays(1,&vao);vertexCache.currentIndexBuffer=0;}
 void splitModels(bool split){
  other.modelMatrix[0]=-.9f;other.modelMatrix[5]=1.1f;other.modelMatrix[12]=.213f;
  for(int i=0;i<MeshCount;++i)surfs[i].space=split&&i%2?&other:&entity;
  for(auto &list:sources)for(auto &source:list)source.modelMatrix=static_cast<const drawSurf_t *>(source.surface)->space->modelMatrix;
 }
 void rotateIndexes(bool clipped){
  auto &buffer=vertexCache.dynamicData.indexBuffer;auto *staging=buffer.CurrentWriteLocation();
  unsigned offset=unsigned(uintptr_t(buffer.BufferOffset(staging)));
  const uint32_t indexes[]={0,1,2,0,2,3},half[]={0,1,2,0,1,2};
  for(int i=0;i<MeshCount;++i){memcpy(staging+i*32,clipped?half:indexes,24);
   surfs[i].indexCache=vertCacheHandle_t::Create(24,offset+i*32,vertexCache.currentFrame,false);}
  buffer.Commit(MeshCount*32);buffer.SwitchFrame();++vertexCache.currentFrame;
  for(auto &list:sources)for(auto &source:list){auto *s=static_cast<const drawSurf_t *>(source.surface);
   source.indexes=reinterpret_cast<const uint32_t *>(vertexCache.ReadSubmittedIndexes(s->indexCache,6));require(source.indexes,"new submitted ring unavailable");}
 }
 void legacy(const drawSurf_t &s){
  u.modelMatrix.Set(s.space->modelMatrix);u.alphaTest.Set(s.cutout?.5f:-1);vertexCache.VertexPosition(s.ambientCache);
  const void *offset=vertexCache.IndexPosition(s.indexCache);++actualDraws;glDrawElementsBaseVertex(GL_TRIANGLES,6,GL_UNSIGNED_INT,offset,vertexCache.basePointer);
 }
 void draw(int target,int face,bool optimize,bool parallel=false){
  std::vector<MobileShadowBatch::Draw> batches;
  if(optimize)batches=renderer.shadowBatches.Prepare(7,face,sources[face]);
  glBindFramebuffer(GL_FRAMEBUFFER,fbo[target]);glViewport(0,0,64,64);glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glDisable(GL_STENCIL_TEST);glDisable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST);glDepthMask(1);glDepthFunc(GL_LESS);glColorMask(0,0,0,0);glClearDepthf(1);glClear(GL_DEPTH_BUFFER_BIT);
  glUseProgram(program);uniform(program,"u_face",face);uniform(program,"u_parallel",parallel);uniform(program,"u_texture",0);
  glUniform3f(glGetUniformLocation(program,"u_lightOrigin"),.001f,-.002f,.003f);
  glUniformMatrix4fv(glGetUniformLocation(program,"u_textureMatrix"),1,0,identity);
  float projection[16]={.2f,0,0,0,0,.2f,0,0,0,0,.1f,0,0,0,0,1};
  glUniformMatrix4fv(glGetUniformLocation(program,"u_shadowProjection"),1,0,projection);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,alpha);
  std::array<bool,PerFace>batched={};
  for(const auto &batch:batches){renderer.DrawShadowBatch(batch,&u);for(unsigned slot:batch.slots)batched[slot]=true;}
  for(int i=0;i<PerFace;++i)if(!batched[i])legacy(surfs[face*PerFace+i]);
 }
 std::vector<uint32_t>pixels(int target){
  glBindFramebuffer(GL_FRAMEBUFFER,sampleFbo);glViewport(0,0,64,64);glDisable(GL_DEPTH_TEST);glColorMask(1,1,1,1);glUseProgram(sample);
  glActiveTexture(GL_TEXTURE14);glBindTexture(GL_TEXTURE_2D,depth[target]);uniform(sample,"u_depth",14);glDrawArrays(GL_TRIANGLES,0,3);
  std::vector<uint32_t>data(64*64*4);glReadPixels(0,0,64,64,GL_RGBA_INTEGER,GL_UNSIGNED_INT,data.data());require(glGetError()==GL_NO_ERROR,"depth readback");return data;
 }
 void next(bool on=true,uint64_t revision=1){renderer.shadowBatches.BeginFrame(on,revision,++frame);}
};
extern "C" const char *validate(GLuint program,GLuint sample){
 static std::string error;
 try{Fixture f(program,sample);unsigned baseline=0,merged=0;
  for(int frame=0;frame<11;++frame){
   if(frame==2)f.rotateIndexes(false);
   if(frame==9)f.splitModels(true);
   if(frame==3)f.entity.modelMatrix[12]=.137f; // transform change, immediate fallback then rebake
   if(frame==5){f.rotateIndexes(true);for(auto &list:f.sources)for(auto &s:list)s.content.Add(99);}
   f.next(frame!=7,frame>=6?2:1);
   for(int face=0;face<6;++face){
    actualDraws=0;f.draw(0,face,false,frame==8);baseline+=actualDraws;
    actualDraws=0;f.draw(1,face,true,frame==8);merged+=actualDraws;
    const auto reference=f.pixels(0),optimized=f.pixels(1);
    require(reference==optimized,"merged shadows differ from original draw depths");
    require(frame==8 || std::any_of(reference.begin(),reference.end(),[](uint32_t v){return v!=0x3f800000u;}),"empty shadow reference");
   }
   if(frame==1){require(f.renderer.shadowBatches.sourceDraws==570,"expected 570 opaque source draws");require(f.renderer.shadowBatches.batchDraws==6,"expected one indexed batch per cube face");}
   if(frame==2)require(f.renderer.shadowBatches.uploadedBytes==0&&f.renderer.shadowBatches.hits==6,"warm batches rebuilt index buffers");
   if(frame==10)require(f.renderer.shadowBatches.batchDraws==12,"different model matrices were combined");
   if(frame==7)require(f.renderer.shadowBatches.residentBytes==0,"off switch retained GPU cache");
  }
  std::printf("PASS: production submitted-ring reads and actual shadow batch submission preserve every sampled depth bit across six cube faces, cutout fallbacks, >16-bit vertex bases, transforms, geometry changes, storage revision, disable/re-enable and directional projection; %u reference submissions -> %u\n",baseline,merged);
  f.splitModels(false);f.next();for(int face=0;face<6;++face)f.draw(1,face,true);f.next();for(int face=0;face<6;++face)f.draw(1,face,true);glFinish();
  // Includes production Prepare() and draw submission. Software-driver timing
  // characterizes this workload only, not Qualcomm GPU or game-frame FPS.
  std::vector<double>reference,optimized;
  for(int block=0;block<8;++block){bool fast=block%4==1||block%4==2;glFinish();auto start=std::chrono::steady_clock::now();
   for(int frame=0;frame<40;++frame){f.next();for(int face=0;face<6;++face)f.draw(fast?1:0,face,fast);}
   glFinish();double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/40;
   (fast?optimized:reference).push_back(ms);
  }
  std::sort(reference.begin(),reference.end());std::sort(optimized.begin(),optimized.end());
  std::printf("Shadow workload ABBA median (Mesa, prepare+draw+completion): baseline %.3f ms, cached merging %.3f ms; warm draw submissions 576 -> 12, no depth copies\n",(reference[1]+reference[2])*.5,(optimized[1]+optimized[2])*.5);
  require(glGetError()==GL_NO_ERROR,"GL error after benchmark");error.clear();
 }catch(const std::exception &e){error=e.what();}return error.c_str();
}
'''

SAMPLE_VS='''#version 320 es
precision highp float;
void main(){vec2 p=vec2((gl_VertexID==1)?3.0:-1.0,(gl_VertexID==2)?3.0:-1.0);gl_Position=vec4(p,0,1);}'''
SAMPLE_FS='''#version 320 es
precision highp float; precision highp int;
uniform highp sampler2D u_depth;
layout(location=0) out uvec4 color;
void main(){color=uvec4(floatBitsToUint(texelFetch(u_depth,ivec2(gl_FragCoord.xy),0).r));}'''

def main():
    gl=GLES()
    try:
        shadow=gl.program(source('stages/mobile/shadow.vert.glsl'),source('stages/mobile/shadow.frag.glsl'))
        sampler=gl.program(SAMPLE_VS,SAMPLE_FS)
        with tempfile.TemporaryDirectory(prefix='quest-shadow-batch-') as directory:
            base=Path(directory)
            handle=(ENGINE/'renderer/resources/Model.h').read_text().split('struct vertCacheHandle_t {',1)[1].split('\n};',1)[0]
            substitutions={'CACHE_HANDLE':'struct vertCacheHandle_t {'+handle+'\n};',
                'VERTEX_POSITION':method(ENGINE/'renderer/VertexCache.cpp','void idVertexCache::VertexPosition('),
                'INDEX_POSITION':method(ENGINE/'renderer/VertexCache.cpp','void *idVertexCache::IndexPosition('),
                'BIND_INDEX':method(ENGINE/'renderer/VertexCache.cpp','void idVertexCache::BindExternalIndexBuffer('),
                'READ_INDEX':method(ENGINE/'renderer/VertexCache.cpp','const glIndex_t *idVertexCache::ReadSubmittedIndexes('),
                'SNAPSHOT_INDEX':method(ENGINE/'renderer/VertexCache.cpp','void idVertexCache::SnapshotStaticIndexes('),
                'DRAW_BATCH':method(ENGINE/'renderer/backend/stages/MobileLightingStage.cpp','void MobileLightingStage::Impl::DrawShadowBatch(')}
            text=SHELL
            for key,value in substitutions.items():text=text.replace(key,value)
            production=''.join((ENGINE/p).read_text() for p in ('renderer/backend/GpuBuffer.cpp','renderer/backend/MobileShadowBatch.h'))
            names=set(re.findall(r'\bqgl(\w+)',production+text)) - {'BufferStorage','DrawElements'}
            aliases='\n'.join(f'#define qgl{name} gl{name}' for name in sorted(names))
            text=text.replace('ALIASES',aliases)
            (base/'precompiled.h').write_text('#pragma once\n')
            (base/'batch.cpp').write_text(text)
            library=base/'batch.so';search=[flag for p in os.environ.get('LD_LIBRARY_PATH','').split(':') if p for flag in ('-L',p)]
            headers=Path('/tmp/renderer-tools/root/usr/include')
            include=['-I',str(headers)] if (headers/'GLES3/gl32.h').exists() else []
            subprocess.run(['g++','-std=c++14','-O2','-shared','-fPIC','-I',str(base),'-I',str(ENGINE),*include,str(base/'batch.cpp'),*search,'-l:libGLESv2.so.2','-o',str(library)],check=True)
            native=C.CDLL(str(library));native.validate.argtypes=[C.c_uint,C.c_uint];native.validate.restype=C.c_char_p
            error=native.validate(shadow,sampler)
            if error:raise AssertionError(error.decode())
    finally:gl.close()
if __name__=='__main__':main()
