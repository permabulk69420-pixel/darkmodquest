#!/usr/bin/env python3
"""Run production SpaceWarp rendering/lifetimes with GLES and an OpenXR fixture.

The shader, render/history implementation, swapchain implementation, matrix
multiply and layered attachment helper are production code. Only the engine
shell and compositor are fixtures. This checks data and protocol correctness;
it does not emulate Meta frame synthesis or measure Quest performance.
"""
import ctypes as C
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
import zipfile
from mobile_gles_test import GLES, source
from shader_install_test import method

ROOT=Path(__file__).resolve().parents[2]
ENGINE=ROOT/'Q3E/src/main/jni/darkmod'

SHELL=r'''
#pragma once
#include <GLES3/gl32.h>
#include <EGL/egl.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <xmmintrin.h>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
GL_ALIASES
#include <openxr/openxr.h>
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr_platform.h>
static const float XR_GameUnitsToMetres=.02309f,XR_MetresToGameUnits=1.0f/.02309f;
void XR_Log(const char *,...){}
bool XR_Check(XrResult r,const char *,XrInstance){return XR_SUCCEEDED(r);}
using uint32=uint32_t;
struct idVec3{float v[3]={};const float *ToFloatPtr()const{return v;}};
struct idDrawVert{idVec3 xyz;float st[2];float padding[15];};
void identity(float *m){std::fill(m,m+16,0.f);m[0]=m[5]=m[10]=m[15]=1;}
struct Matrix{float m[16];Matrix(){identity(m);}const float *ToFloatPtr()const{return m;}void ToGL(float *p)const{memcpy(p,m,sizeof(m));}};
struct idStr:std::string{using std::string::string;operator const char *()const{return c_str();}static idStr Fmt(const char *fmt,const char *n,int i){char text[256];snprintf(text,sizeof(text),fmt,n,i);return idStr(text);}};
const int CVAR_RENDERER=1,CVAR_BOOL=2,CVAR_INIT=4,CVAR_ARCHIVE=8;
struct idCVar{float value;template<class... T>idCVar(const char *,const char *v,T...):value(std::stof(v)){}int GetInteger()const{return int(value);}bool GetBool()const{return value!=0;}float GetFloat()const{return value;}void SetBool(bool b){value=b;}};
idCVar r_offsetFactor("a","0"),r_offsetUnits("b","0");
template<class T>struct idList:std::vector<T>{void Append(const T &v){this->push_back(v);}void SetNum(int n){this->resize(n);}int Num()const{return this->size();}T *Ptr(){return this->data();}void Clear(){this->clear();}};
class idImage{public:static const int TEXTURE_NOT_LOADED=-1;GLuint texnum=~0u;int uploadWidth=0,uploadHeight=0;std::string imgName;
 virtual ~idImage()=default;virtual void PurgeImage(){if(texnum!=~0u)glDeleteTextures(1,&texnum);texnum=~0u;}
 void BindToUnit(int unit){glActiveTexture(GL_TEXTURE0+unit);glBindTexture(GL_TEXTURE_2D,texnum);}};
#define VERTCACHE_FRAMENUM_BITS 15
CACHE_HANDLE
const vertCacheHandle_t NO_CACHE={0,0,0,false};const uint32_t VERTCACHE_FRAME_MASK=(1<<15)-1;
using byte=unsigned char;
struct idCVarBool{bool value=false;operator bool()const{return value;}};
idCVarBool r_usePersistentMapping;bool GLAD_GL_ARB_buffer_storage=false;
#ifndef GL_MAP_PERSISTENT_BIT
#define GL_MAP_PERSISTENT_BIT 0x0040
#define GL_MAP_COHERENT_BIT 0x0080
#endif
unsigned ALIGN(unsigned n,unsigned a){return (n+a-1)/a*a;}
void *Mem_Alloc16(unsigned n){void *p=nullptr;assert(posix_memalign(&p,16,n)==0);return p;}void Mem_Free16(void *p){free(p);}
void glBufferStorage(GLenum,GLsizeiptr,const void *,GLbitfield){assert(false);}
#include "renderer/backend/GpuBuffer.cpp"
struct Cache{
 GLuint vertices=0,indices=0;uint64_t generation=1;bool isolated=false;GpuBuffer streaming;
 uint64_t StaticRevision()const{return generation;}
 GLuint VertexBuffer(vertCacheHandle_t h)const{return isolated&&!h.isStatic?streaming.GetAPIObjectForOffset(h.offset):vertices;}
 GLuint IndexBuffer(vertCacheHandle_t)const{return indices;}
 GLuint VertexOffset(vertCacheHandle_t h)const{return isolated&&!h.isStatic?streaming.PhysicalOffset(h.offset):h.offset;}
 GLuint IndexOffset(vertCacheHandle_t h)const{return h.offset;}
}vertexCache;
const int MC_OPAQUE=0,MC_PERFORATED=1,MC_TRANSLUCENT=2,SS_PORTAL_SKY=9,MF_POLYGONOFFSET=1,CT_FRONT_SIDED=1,DSF_ATMOSPHERIC_FOG=16;
struct shaderStage_t{bool hasAlphaTest=false,enabled=true;int alphaTestRegister=0;float alpha=1;struct{idImage *image=nullptr;}texture;};
struct idMaterial{int coverage=MC_OPAQUE,sort=0;std::vector<shaderStage_t>stages{{}};bool IsDrawn()const{return true;}int Coverage()const{return coverage;}int GetSort()const{return sort;}int GetNumStages()const{return stages.size();}const shaderStage_t *GetStage(int n)const{return &stages[n];}bool IsAtmosphericFog()const{return false;}int GetCullType()const{return 0;}bool TestMaterialFlag(int)const{return false;}float GetPolygonOffset()const{return 0;}};
struct viewEntity_t{uint64_t mobileCacheId=1;float modelMatrix[16];viewEntity_t(){identity(modelMatrix);}};
struct drawSurf_t{const viewEntity_t *space;const idMaterial *material;int numIndexes=6,numVerts=4,dsFlags=0;vertCacheHandle_t ambientCache,indexCache;uint64_t motionGeometryId=1,motionTopologyId=1;float registers[1]={.5f};const float *shaderRegisters=registers;
 bool IsStageEnabled(const shaderStage_t *s)const{return s->enabled;}std::array<float,4>GetStageColor(const shaderStage_t *s)const{return {1,1,1,s->alpha};}Matrix GetTextureMatrix(const shaderStage_t *)const{return {};}};
struct viewDef_s{int numDrawSurfs=0;drawSurf_t **drawSurfs=nullptr;};using viewDef_t=viewDef_s;
bool R_QuestFogDisabled(const viewDef_t *){return false;}
const int GLS_DEPTHFUNC_LESS=1,GLS_DEFAULT=0;const GLenum GL_INDEX_TYPE=GL_UNSIGNED_INT;
void GL_State(int){glColorMask(1,1,1,1);glDisable(GL_BLEND);glDepthMask(1);glDepthFunc(GL_LESS);}
void GL_Cull(int){glDisable(GL_CULL_FACE);}
void GL_SelectTexture(int n){glActiveTexture(GL_TEXTURE0+n);}
static const char *va(const char *fmt,...){static char text[256];va_list args;va_start(args,fmt);vsnprintf(text,sizeof(text),fmt,args);va_end(args);return text;}
MULTIPLY_MATRIX
'''

PROGRAM=r'''
#pragma once
#include "renderer/tr_local.h"
class GLSLUniformGroup;
class GLSLProgram{public:GLuint id=0;bool valid=true;
 std::map<std::string,std::shared_ptr<GLSLUniformGroup>>groups;
 int GetUniformLocation(const char *n){return glGetUniformLocation(id,n);}bool EnsureLinked(){return valid;}
 void Activate(){glUseProgram(id);}static void Deactivate(){glUseProgram(0);}
 template<class T>T *GetUniformGroup(){auto &p=groups[typeid(T).name()];if(!p)p.reset(new T(this));return static_cast<T *>(p.get());}
};
struct ProgramManager{GLSLProgram program;GLSLProgram *LoadFromFiles(const char *,const char *,const char *){return &program;}}managerProgram,*programManager=&managerProgram;
'''

UNIFORMS=r'''
#pragma once
#include "renderer/backend/GLSLProgramManager.h"
class GLSLUniformGroup{public:explicit GLSLUniformGroup(GLSLProgram *p):program(p){}virtual ~GLSLUniformGroup()=default;protected:GLSLProgram *program;};
class GLSLUniformBase{public:bool IsPresent()const{return paramLocation>=0;}protected:GLSLUniformBase(GLSLProgram *p,const char *n):paramLocation(p->GetUniformLocation(n)){}int paramLocation;};
#define UNIFORM_GROUP_DEF(T) explicit T(GLSLProgram *p):GLSLUniformGroup(p){}
'''

FBO=r'''
#pragma once
#include "renderer/tr_local.h"
class FrameBuffer{public:
 using Generator=std::function<void(FrameBuffer *)>;using MultiviewAttachment=void(*)(GLenum,GLenum,GLuint,GLint,GLint,GLsizei);
 GLuint fbo=0;Generator generator;
 explicit FrameBuffer(Generator g={}):generator(g){}~FrameBuffer(){Destroy();}
 void Init(int,int,int=0){glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);}
 void Destroy(){if(fbo)glDeleteFramebuffers(1,&fbo);fbo=0;}void SetGenerator(Generator g){generator=g;}
 void Bind();void BindDraw();
 void AddColorRenderTexture(int n,idImage *image){Bind();glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0+n,GL_TEXTURE_2D,image->texnum,0);}
 void AddMultiviewTextures(GLuint,GLuint,MultiviewAttachment,bool packedDepthStencil=true);
};
'''

MANAGER=r'''
#pragma once
#include "renderer/backend/FrameBuffer.h"
struct FrameBufferManager{FrameBuffer *activeFbo=nullptr,*activeDrawFbo=nullptr;std::vector<std::unique_ptr<FrameBuffer>>owned;
 FrameBuffer *CreateFromGenerator(const char *,FrameBuffer::Generator g){owned.emplace_back(new FrameBuffer(g));return owned.back().get();}
}managerBuffers,*frameBuffers=&managerBuffers;
void FrameBuffer::Bind(){if(!fbo&&generator)generator(this);glBindFramebuffer(GL_FRAMEBUFFER,fbo);frameBuffers->activeFbo=frameBuffers->activeDrawFbo=this;}
void FrameBuffer::BindDraw(){if(!fbo&&generator)generator(this);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,fbo);frameBuffers->activeDrawFbo=this;}
'''

XR=r'''
struct RuntimeImages{GLenum format;unsigned width,height,layers;bool depth=false,acquired=false,waited=false;unsigned index=0,next=0;GLuint images[3]={};};
static std::map<XrSwapchain,RuntimeImages>runtime;
static uintptr_t nextChain=1;static bool failDepthAcquire=false;static int waitTimeouts=1;
extern "C" XrResult xrCreateSwapchain(XrSession,const XrSwapchainCreateInfo *info,XrSwapchain *chain){
 if(info->arraySize!=2 || info->sampleCount!=1 || !(info->usageFlags&XR_SWAPCHAIN_USAGE_SAMPLED_BIT))return XR_ERROR_VALIDATION_FAILURE;
 *chain=reinterpret_cast<XrSwapchain>(nextChain++);auto &r=runtime[*chain];r.format=info->format;r.width=info->width;r.height=info->height;r.layers=info->arraySize;
 r.depth=(info->usageFlags&XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)!=0;
 if(r.depth){assert(!(info->usageFlags&XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT));r.next=1;}
 else assert(info->usageFlags&XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
 glGenTextures(3,r.images);for(GLuint texture:r.images){glBindTexture(GL_TEXTURE_2D_ARRAY,texture);glTexStorage3D(GL_TEXTURE_2D_ARRAY,1,info->format,info->width,info->height,2);glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MAG_FILTER,GL_NEAREST);}
 return XR_SUCCESS;
}
extern "C" XrResult xrDestroySwapchain(XrSwapchain chain){auto &r=runtime.at(chain);glDeleteTextures(3,r.images);runtime.erase(chain);return XR_SUCCESS;}
extern "C" XrResult xrEnumerateSwapchainImages(XrSwapchain chain,uint32_t capacity,uint32_t *count,XrSwapchainImageBaseHeader *images){
 *count=3;if(capacity){assert(capacity>=3);auto *out=reinterpret_cast<XrSwapchainImageOpenGLESKHR *>(images);for(int i=0;i<3;++i)out[i].image=runtime.at(chain).images[i];}return XR_SUCCESS;}
extern "C" XrResult xrAcquireSwapchainImage(XrSwapchain chain,const XrSwapchainImageAcquireInfo *,uint32_t *index){
 auto &r=runtime.at(chain);if(r.depth&&failDepthAcquire)return XR_ERROR_RUNTIME_FAILURE;assert(!r.acquired);r.acquired=true;r.waited=false;*index=r.index=r.next++%3;return XR_SUCCESS;}
extern "C" XrResult xrWaitSwapchainImage(XrSwapchain chain,const XrSwapchainImageWaitInfo *){auto &r=runtime.at(chain);assert(r.acquired);if(waitTimeouts>0){--waitTimeouts;return XR_TIMEOUT_EXPIRED;}r.waited=true;return XR_SUCCESS;}
extern "C" XrResult xrReleaseSwapchainImage(XrSwapchain chain,const XrSwapchainImageReleaseInfo *){auto &r=runtime.at(chain);assert(r.acquired&&r.waited);r.acquired=false;return XR_SUCCESS;}
'''

CHECKS=r'''
#include "renderer/vr/OpenXRSpaceWarp.cpp"
#include "renderer/vr/OpenXRSwapchain.cpp"
ATTACHMENTS
void require(bool ok,const char *s){if(!ok)throw std::runtime_error(s);}
void closeTo(float a,float b,const char *s){if(std::fabs(a-b)>0.002f){char text[256];snprintf(text,sizeof(text),"%s: %.6f != %.6f",s,a,b);throw std::runtime_error(text);}}
struct WarpFixture{
 OpenXRSpaceWarp warp;FrameBuffer realDefault;idMaterial material;viewEntity_t entity;
 drawSurf_t surf;drawSurf_t *draws[1];viewDef_t view;MobileStereoView stereo;
 XrPosef pose=SpaceWarpMath::Identity();XrTime time=1000000000;GLuint reader=0,depthReader=0,depthColor=0,alpha=0,depthProgram=0;
 byte *streamingBase=nullptr;
 std::array<idDrawVert,12>vertices;uint32_t indexes[6]={0,1,2,0,2,3};
 explicit WarpFixture(GLuint program,GLuint depthProgram,bool packed=false):depthProgram(depthProgram){
  programManager->program.id=program;programManager->program.groups.clear();programManager->program.valid=true;
  realDefault.Init(16,16);realDefault.Bind();
  surf.space=&entity;surf.material=&material;surf.ambientCache={uint32_t(sizeof(idDrawVert)*4),0,1,false};surf.indexCache={sizeof(indexes),0,1,false};
  draws[0]=&surf;view.numDrawSurfs=1;view.drawSurfs=draws;
  for(int eye=0;eye<2;++eye){identity(stereo.worldView[eye]);identity(stereo.projection[eye]);stereo.projection[eye][10]=-1;stereo.projection[eye][11]=-1;stereo.projection[eye][14]=-2;stereo.projection[eye][15]=0;}
  for(int ring=0;ring<3;++ring)for(int n=0;n<4;++n){auto &v=vertices[ring*4+n];v.xyz.v[0]=(n==1||n==2)?10:-10;v.xyz.v[1]=n>=2?10:-10;v.xyz.v[2]=-4;v.st[0]=v.st[1]=.5f;}
  glGenBuffers(1,&vertexCache.vertices);glBindBuffer(GL_ARRAY_BUFFER,vertexCache.vertices);glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices.data(),GL_DYNAMIC_DRAW);
  glGenBuffers(1,&vertexCache.indices);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,vertexCache.indices);glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(indexes),indexes,GL_STATIC_DRAW);
  glGenFramebuffers(1,&reader);glGenFramebuffers(1,&depthReader);glGenTextures(1,&depthColor);
  glBindTexture(GL_TEXTURE_2D,depthColor);glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA16F,16,16);
  glBindFramebuffer(GL_FRAMEBUFFER,depthReader);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,depthColor,0);
  glGenTextures(1,&alpha);glBindTexture(GL_TEXTURE_2D,alpha);unsigned char white[4]={255,255,255,255};glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,white);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  glBindVertexArray(glIsVertexArray(1)?1:0);realDefault.Bind();
  const int64_t formats[]={GL_RGBA8,GL_RGBA16F,packed?GL_DEPTH24_STENCIL8:GL_DEPTH_COMPONENT24};vr_spaceWarp.SetBool(true);
  require(warp.Initialize(reinterpret_cast<XrSession>(1),16,16,formats,3),"production initialization failed");
 }
 ~WarpFixture(){vertexCache.streaming.Destroy();vertexCache.isolated=false;warp.Shutdown();managerBuffers.owned.clear();glDeleteBuffers(1,&vertexCache.vertices);glDeleteBuffers(1,&vertexCache.indices);glDeleteFramebuffers(1,&reader);glDeleteFramebuffers(1,&depthReader);glDeleteTextures(1,&depthColor);glDeleteTextures(1,&alpha);}
 void enableStreaming(){
  warp.ResetHistory();vertexCache.streaming.InitWriteFrameAhead(GL_ARRAY_BUFFER,4*sizeof(idDrawVert),16,true,true);
  vertexCache.isolated=true;streamingBase=vertexCache.streaming.CurrentWriteLocation()-4*sizeof(idDrawVert);
 }
 void streamedPosition(float x,float z){
  auto &buffer=vertexCache.streaming;auto *destination=buffer.CurrentWriteLocation();
  std::array<idDrawVert,4>data;
  for(int n=0;n<4;++n){data[n]=vertices[n];data[n].xyz.v[0]=((n==1||n==2)?10:-10)+x;data[n].xyz.v[2]=z;}
  memcpy(destination,data.data(),sizeof(data));surf.ambientCache.offset=destination-streamingBase;++surf.ambientCache.frameNumber;
  buffer.Commit(sizeof(data));buffer.SwitchFrame();
 }
 void position(int ring,float x,float z){for(int n=0;n<4;++n){vertices[ring*4+n].xyz.v[0]=((n==1||n==2)?10:-10)+x;vertices[ring*4+n].xyz.v[2]=z;}glBindBuffer(GL_ARRAY_BUFFER,vertexCache.vertices);glBufferSubData(GL_ARRAY_BUFFER,ring*4*sizeof(idDrawVert),4*sizeof(idDrawVert),&vertices[ring*4]);surf.ambientCache.offset=ring*4*sizeof(idDrawVert);++surf.ambientCache.frameNumber;}
 std::array<float,4>pixel(const XrSwapchainSubImage &sub,bool depth=false){
  const auto &r=runtime.at(sub.swapchain);require(!r.acquired,"runtime image submitted before release");GLuint texture=r.images[r.index];std::array<float,4>p;
  if(depth){
   glBindFramebuffer(GL_FRAMEBUFFER,depthReader);glViewport(0,0,16,16);glDisable(GL_DEPTH_TEST);glDisable(GL_STENCIL_TEST);glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);
   glUseProgram(depthProgram);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D_ARRAY,texture);glUniform1i(glGetUniformLocation(depthProgram,"image"),0);glUniform1i(glGetUniformLocation(depthProgram,"eye"),sub.imageArrayIndex);glDrawArrays(GL_TRIANGLES,0,3);
  }else {glBindFramebuffer(GL_FRAMEBUFFER,reader);glFramebufferTextureLayer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,texture,0,sub.imageArrayIndex);}
  glReadPixels(8,8,1,1,GL_RGBA,GL_FLOAT,p.data());return p;
 }
 void frame(float x,float z,float depth,bool skip,float rightX=std::numeric_limits<float>::quiet_NaN()){
  time+=27777778;warp.BeginFrame();realDefault.Bind();require(warp.Render(&view,stereo,pose,time,XR_GameUnitsToMetres),"production render failed");
  XrCompositionLayerProjectionView views[2]={{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  XrCompositionLayerSpaceWarpInfoFB info[2]={};require(warp.Chain(views,info),"motion/depth not chained");
  for(int eye=0;eye<2;++eye){require(views[eye].next==&info[eye],"layer chain wrong");require(info[eye].motionVectorSubImage.imageArrayIndex==uint32_t(eye),"eye index wrong");require(info[eye].depthSubImage.imageArrayIndex==uint32_t(eye),"depth eye index wrong");require(info[eye].nearZ==XR_GameUnitsToMetres&&std::isinf(info[eye].farZ),"depth units/range wrong");require(info[eye].layerFlags==(skip?XR_COMPOSITION_LAYER_SPACE_WARP_INFO_FRAME_SKIP_BIT_FB:0),"history reset flag wrong");
   auto motion=pixel(info[eye].motionVectorSubImage);closeTo(motion[0],eye==1&&!std::isnan(rightX)?rightX:x,"motion x");closeTo(motion[2],z,"motion z");closeTo(motion[3],depth==1?1:0,"motion/background alpha");closeTo(pixel(info[eye].depthSubImage,true)[0],depth,"compositor depth");}
  require(glGetError()==GL_NO_ERROR,"GL error in real SpaceWarp pass");warp.CompleteFrame(true);realDefault.Bind();
 }
};
idCVar vr_uiOverlayHeight("height","1.8"),vr_uiOverlayDistance("distance","2.5"),vr_uiOverlayVerticalOffset("offset","-.2");
static bool expectWarp=true,packetValid=false;static int submissions=0;
static XrResult endFrameResult=XR_SUCCESS;
extern "C" XrResult xrEndFrame(XrSession,const XrFrameEndInfo *end){
 ++submissions;packetValid=end->layerCount==2&&end->displayTime>0;
 if(!packetValid)return XR_ERROR_VALIDATION_FAILURE;
 const auto *projection=reinterpret_cast<const XrCompositionLayerProjection *>(end->layers[0]);
 const auto *hud=reinterpret_cast<const XrCompositionLayerQuad *>(end->layers[1]);
 packetValid=projection->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION&&projection->viewCount==2&&hud->type==XR_TYPE_COMPOSITION_LAYER_QUAD&&hud->next==nullptr;
 for(int eye=0;eye<2&&packetValid;++eye){
  const auto &view=projection->views[eye];
  if(!expectWarp){packetValid=view.next==nullptr;continue;}
  const auto *warp=reinterpret_cast<const XrCompositionLayerSpaceWarpInfoFB *>(view.next);
  packetValid=warp&&warp->type==XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB&&warp->next==nullptr;
  if(!packetValid)break;
  packetValid=warp->motionVectorSubImage.imageArrayIndex==uint32_t(eye)&&warp->depthSubImage.imageArrayIndex==uint32_t(eye)&&
   !runtime.at(warp->motionVectorSubImage.swapchain).acquired&&!runtime.at(warp->depthSubImage.swapchain).acquired;
 }
 return packetValid?endFrameResult:XR_ERROR_VALIDATION_FAILURE;
}
struct OpenXRBackend{struct Impl{
 explicit Impl(OpenXRSpaceWarp &warp):spaceWarp(warp){}
 OpenXRSpaceWarp &spaceWarp;bool frameBegun=true,haveRenderViews=true,menuAnchored=false,recenterPending=false;
 XrTime predictedDisplayTime=2000000000,recenterTime=0;
 XrSession session=reinterpret_cast<XrSession>(1);XrInstance instance=reinterpret_cast<XrInstance>(1);
 XrSpace localSpace=reinterpret_cast<XrSpace>(2),viewSpace=reinterpret_cast<XrSpace>(3);
 XrView renderViews[2]={{XR_TYPE_VIEW},{XR_TYPE_VIEW}};XrPosef menuPose=SpaceWarpMath::Identity();
 struct Chain{XrSwapchainSubImage CurrentSubImage()const{return {};}}eyeSwapchains[2],uiSwapchain;
 void EndFrame(bool submitLayers,bool hasWorldView);
};};
const int UI_WIDTH=1920,UI_HEIGHT=1440;
END_FRAME
extern "C" const char *validate_spacewarp(GLuint program,GLuint depthProgram){
 static std::string error;
 try{{
  WarpFixture f(program,depthProgram);f.frame(0,0,.75,true);
  // Rigid motion: world/model transforms, independently of mesh vertices.
  f.entity.modelMatrix[12]=1;f.position(1,0,-4);f.frame(.25,0,.75,false);
  // Animated deformation: actual previous/current ring-buffer vertices.
  f.position(2,1,-4);f.frame(.25,0,.75,false);
  // Depth changes must be represented in the RGB vector, not just XY.
  f.position(0,1,-5);f.frame(-.015625,.1,.8,false);
  // Camera/head translation supplies motion for otherwise static geometry.
  for(int eye=0;eye<2;++eye)f.stereo.worldView[eye][12]=.5;
  f.position(1,1,-5);f.frame(.1,0,.8,false);
  // A topology change cannot reuse previous animated positions.
  ++f.surf.motionTopologyId;f.position(2,2,-5);f.frame(0,0,.8,false);
  // Reallocated/reloaded GPU buffers invalidate both mesh and camera history.
  ++vertexCache.generation;f.position(0,3,-5);f.frame(0,0,.8,true);
  // Alpha-tested holes leave marked background and far depth, in both eyes.
  f.material.coverage=MC_PERFORATED;idImage alpha;alpha.texnum=f.alpha;f.material.stages[0].texture.image=&alpha;f.material.stages[0].hasAlphaTest=true;
  glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,f.alpha);unsigned char black[4]={0,0,0,0};glTexSubImage2D(GL_TEXTURE_2D,0,0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,black);
  f.position(1,3,-5);f.frame(0,0,1,false);
  unsigned char white[4]={255,255,255,255};glBindTexture(GL_TEXTURE_2D,f.alpha);glTexSubImage2D(GL_TEXTURE_2D,0,0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,white);
  f.position(2,3,-5);f.frame(0,0,.8,false);
  f.material.coverage=MC_TRANSLUCENT;f.position(0,3,-5);f.frame(0,0,1,false);f.material.coverage=MC_OPAQUE;
  f.warp.ResetHistory();f.position(1,3,-5);f.frame(0,0,.8,true);
  // Opposing eye translations detect accidental single-eye matrix reuse.
  f.stereo.worldView[0][12]+=.5;f.stereo.worldView[1][12]-=.5;
  f.position(2,3,-5);f.frame(.1,0,.8,false,-.1);
  // Teleports/camera cuts request no extrapolation for that frame.
  f.pose.position.x=20;f.position(0,3,-5);f.frame(0,0,.8,true);
  // Failed acquisition releases the other chain and never chains partial data.
  failDepthAcquire=true;f.warp.BeginFrame();require(!f.warp.Render(&f.view,f.stereo,f.pose,f.time+27777778,XR_GameUnitsToMetres),"partial acquisition accepted");
  for(auto &entry:runtime)require(!entry.second.acquired,"partial acquire leaked motion image");
  XrCompositionLayerProjectionView views[2]={};XrCompositionLayerSpaceWarpInfoFB info[2]={};require(!f.warp.Chain(views,info),"partial data chained");failDepthAcquire=false;
  f.position(0,3,-5);f.frame(0,0,.8,true);
  // A rejected submission must not become next frame's motion history.
  f.warp.CompleteFrame(true,XR_ERROR_VALIDATION_FAILURE);
  char diagnostic[512];f.warp.GetDiagnostics(diagnostic,sizeof(diagnostic),true);require(strstr(diagnostic,"REJECTED by runtime"),"rejection not reported");
  f.position(1,3,-5);f.frame(0,0,.8,true);
  // Exercise the ACTUAL backend xrEndFrame builder, including the separate
  // unwarped HUD layer and the lifetime of both next-chain structures.
  f.warp.BeginFrame();f.time+=27777778;require(f.warp.Render(&f.view,f.stereo,f.pose,f.time,XR_GameUnitsToMetres),"packet render failed");
  OpenXRBackend::Impl backend(f.warp);backend.EndFrame(true,true);require(packetValid,"xrEndFrame warp/eye/HUD packet invalid");
  const int calls=submissions;backend.EndFrame(true,true);require(submissions==calls,"frame ended twice");
  expectWarp=false;backend.frameBegun=true;backend.EndFrame(true,false);require(packetValid,"menu unexpectedly warped");expectWarp=true;
  // Off produces neither a motion draw nor an extension chain.
  vr_spaceWarp.SetBool(false);f.warp.BeginFrame();require(!f.warp.Render(&f.view,f.stereo,f.pose,f.time,XR_GameUnitsToMetres),"disabled SpaceWarp rendered");require(!f.warp.Chain(views,info),"disabled SpaceWarp chained");vr_spaceWarp.SetBool(true);
  // Delta pose includes rotated locomotion in metres, with identity for tracking alone.
  XrPosef a=SpaceWarpMath::Identity(),b=a;a.orientation={0,std::sqrt(.5f),0,std::sqrt(.5f)};b.orientation=a.orientation;b.position={1,0,0};
  auto delta=SpaceWarpMath::Delta(a,b);closeTo(delta.position.x,0,"delta x");closeTo(delta.position.z,1,"delta z");closeTo(delta.orientation.w,1,"delta rotation");
  const float pos[]={-40,20,30},quat[]={0,0,-std::sqrt(.5f),std::sqrt(.5f)};
  auto gamePose=SpaceWarpMath::FromGameSpace(pos,quat,.02309f);
  closeTo(gamePose.position.x,-20*.02309f,"game/XR x");closeTo(gamePose.position.y,30*.02309f,"game/XR y");closeTo(gamePose.position.z,40*.02309f,"game/XR z");
  const auto forward=SpaceWarpMath::Rotate(gamePose.orientation,{0,0,-1});closeTo(forward.x,-1,"game/XR yaw");
  programManager->program.valid=false;f.warp.BeginFrame();require(!f.warp.Render(&f.view,f.stereo,f.pose,f.time,XR_GameUnitsToMetres),"failed shader rendered");require(!f.warp.Chain(views,info),"failed shader chained");programManager->program.valid=true;
  }
  {WarpFixture streamed(program,depthProgram);streamed.enableStreaming();
   streamed.streamedPosition(0,-5);streamed.frame(0,0,.8,true);
   for(int frame=1;frame<=12;++frame){streamed.streamedPosition(frame%2?1:0,-5);streamed.frame(frame%2?.2f:-.2f,0,.8,false);}
   require(vertexCache.streaming.HasIsolatedFrames(),"isolated vertex path was not active");
   std::puts("PASS: production SpaceWarp reads isolated current/previous animated vertex objects with local offsets across twelve frame-store wraps");
  }
  {WarpFixture packed(program,depthProgram,true);packed.frame(0,0,.75,true);}
  {OpenXRSpaceWarp unavailable;const int64_t missing[]={GL_RGBA8,GL_DEPTH_COMPONENT24};
   require(!unavailable.Initialize(reinterpret_cast<XrSession>(1),16,16,missing,2),"missing motion format accepted");
   char text[512];unavailable.GetDiagnostics(text,sizeof(text),false);require(strstr(text,"UNSUPPORTED by runtime"),"missing extension not reported");unavailable.Shutdown();}
  error.clear();
 }catch(const std::exception &e){error=e.what();}
 return error.c_str();
}
'''

class SpaceWarpTests(unittest.TestCase):
    def test_shipping_motion_pipeline_and_swapchain_protocol(self):
        gl=GLES()
        try:
            motion=gl.program(source('stages/mobile/spacewarp.vert.glsl'),source('stages/mobile/spacewarp.frag.glsl'))
            depth=gl.program('''#version 320 es
precision highp float;out vec2 uv;void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);uv=p;gl_Position=vec4(p*2.0-1.0,0,1);}''','''#version 320 es
precision highp float;precision highp int;in vec2 uv;uniform highp sampler2DArray image;uniform int eye;out vec4 result;
void main(){result=vec4(texture(image,vec3(uv,float(eye))).r,0,0,1);}''')
            with tempfile.TemporaryDirectory(prefix='quest-spacewarp-') as directory:
                base=Path(directory)
                sdk=Path(os.environ.get('ANDROID_SDK_ROOT') or os.environ.get('ANDROID_HOME') or '/tmp/renderer-tools/android-sdk')
                ndk=Path(os.environ.get('ANDROID_NDK_HOME') or os.environ.get('ANDROID_NDK_ROOT') or sdk/'ndk/23.2.8568313')
                if not ndk.is_dir():ndk=Path('/tmp/renderer-tools/android-ndk-r23c')
                headers=ndk/'toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include'
                if not headers.is_dir():headers=Path('/usr/include')
                for name in ('GLES3/gl32.h','GLES3/gl3platform.h','KHR/khrplatform.h'):
                    target=base/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(headers/name,target)
                xr=Path(os.environ.get('TDM_OPENXR_HEADERS','/tmp/renderer-tools/openxr/prefab/modules/headers/include'))
                if not (xr/'openxr/openxr.h').exists():
                    cache=Path(os.environ.get('GRADLE_USER_HOME',Path.home()/'.gradle'))/'caches/modules-2/files-2.1/org.khronos.openxr/openxr_loader_for_android/1.0.34'
                    aars=sorted(cache.glob('*/*.aar'))
                    self.assertTrue(aars,'set TDM_OPENXR_HEADERS or build the APK to populate its OpenXR dependency')
                    xr=base/'openxr-headers'
                    with zipfile.ZipFile(aars[-1]) as archive:
                        for name in archive.namelist():
                            prefix='prefab/modules/headers/include/'
                            if name.startswith(prefix) and not name.endswith('/'):
                                target=xr/name[len(prefix):];target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(archive.read(name))
                self.assertTrue((xr/'openxr/openxr.h').exists(),'set TDM_OPENXR_HEADERS to the loader headers')
                text=''.join((ENGINE/p).read_text() for p in ('renderer/vr/OpenXRSpaceWarp.cpp','renderer/vr/OpenXRSwapchain.cpp','renderer/backend/MobileUniform.h','renderer/backend/GpuBuffer.cpp'))
                attachment=method(ENGINE/'renderer/backend/FrameBuffer.cpp','void FrameBuffer::AddMultiviewTextures(')
                names=set(re.findall(r'\bqgl(\w+)',text+attachment))
                aliases='\n'.join(f'#define qgl{name} gl{name}' for name in names)
                handle=(ENGINE/'renderer/resources/Model.h').read_text().split('struct vertCacheHandle_t {',1)[1].split('\n};',1)[0]
                shell=SHELL.replace('GL_ALIASES',aliases).replace('CACHE_HANDLE','struct vertCacheHandle_t {'+handle+'\n};')
                shell=shell.replace('MULTIPLY_MATRIX',method(ENGINE/'renderer/tr_main.cpp','void myGlMultMatrix('))
                files={'precompiled.h':'#include "renderer/tr_local.h"\n','renderer/tr_local.h':shell,
                    'renderer/backend/GLSLProgramManager.h':PROGRAM,'renderer/backend/GLSLUniforms.h':UNIFORMS,
                    'renderer/backend/FrameBuffer.h':FBO,'renderer/backend/FrameBufferManager.h':MANAGER,
                    'renderer/backend/stages/MobileLightingStage.h':'',
                    'EGL/egl.h':'#pragma once\ntypedef unsigned int EGLenum;typedef void *EGLDisplay;typedef void *EGLConfig;typedef void *EGLContext;extern "C" void *eglGetProcAddress(const char *);\n',
                    'renderer/vr/xr_common.h':'#pragma once\n#include "renderer/tr_local.h"\n'}
                for name,text in files.items():
                    path=base/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text)
                end_frame=method(ENGINE/'renderer/vr/OpenXRBackend.cpp','void OpenXRBackend::Impl::EndFrame(')
                cpp=base/'spacewarp.cpp';cpp.write_text('#include "precompiled.h"\n'+XR+CHECKS.replace('ATTACHMENTS',attachment).replace('END_FRAME',end_frame))
                library=base/'spacewarp.so';search=[flag for p in os.environ.get('LD_LIBRARY_PATH','').split(':') if p for flag in ('-L',p)]
                subprocess.run(['g++','-std=c++14','-O2','-shared','-fPIC','-DTDM_VR_OPENXR','-D_GLES','-I',str(base),'-I',str(xr),'-I',str(ENGINE),str(cpp),*search,'-l:libEGL.so.1','-l:libGLESv2.so.2','-o',str(library)],check=True)
                native=C.CDLL(str(library));native.validate_spacewarp.argtypes=[C.c_uint,C.c_uint];native.validate_spacewarp.restype=C.c_char_p
                self.assertEqual(native.validate_spacewarp(motion,depth),b'')
            print('PASS: production SpaceWarp draws both eyes with signed XYZ motion and metre-correct depth; animated/rigid/camera motion, alpha holes, swapchain index mismatch, cuts, resets, failures and off switch checked on GLES')
        finally:gl.close()

if __name__=='__main__':unittest.main(verbosity=2)
