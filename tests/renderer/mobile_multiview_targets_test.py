#!/usr/bin/env python3
"""Run the shipping stereo target/image lifetime code on a real GLES context.

Engine bookkeeping is stubbed; the target implementation, attachment helpers and
image allocation method are compiled from production sources and call real GL.
"""
import ctypes as C
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from mobile_gles_test import GLES
from shader_install_test import method

ROOT=Path(__file__).resolve().parents[2]
ENGINE=ROOT/'Q3E/src/main/jni/darkmod'
TARGETS=ENGINE/'renderer/backend/MobileMultiviewTargets.cpp'

MOCKS=r'''
#pragma once
#include <GLES3/gl32.h>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
GL_ALIASES
#define GL_R16 0x822A
#define GL_RG16 0x822C
#define GL_RGB16 0x8054
const int CVAR_RENDERER=1,CVAR_BOOL=2,CVAR_INIT=4,TT_2D=0;
struct idCVar{int value;template<class... A>idCVar(const char *,const char *v,A...):value(std::atoi(v)){}
 int GetInteger()const{return value;}bool GetBool()const{return value!=0;}void SetBool(bool v){value=v;}};
static idCVar r_multiSamples("msaa","0"),r_ssao("ssao","0");
static const char *va(const char *fmt,...){static char text[512];va_list args;va_start(args,fmt);vsnprintf(text,sizeof(text),fmt,args);va_end(args);return text;}
struct{struct{struct Tmu{GLuint current2DMap=~0u,currentCubeMap=~0u;}tmu[16];int currenttmu=0;}glState;}backEnd;
void GL_SelectTexture(int unit){glActiveTexture(GL_TEXTURE0+unit);backEnd.glState.currenttmu=unit;}
void GL_SetDebugLabel(GLenum,GLuint,const char *){}
class idImageScratch;
class idImage{
public:
 static const int TEXTURE_NOT_LOADED=-1;
 GLuint texnum=~0u;uint64_t contentRevision=1;int uploadWidth=0,uploadHeight=0,internalFormat=GL_RGBA8,type=TT_2D;
 const char *imgName="test";
 virtual ~idImage()=default;virtual idImageScratch *AsScratch(){return nullptr;}
 void Bind(){glBindTexture(GL_TEXTURE_2D,texnum);backEnd.glState.tmu[backEnd.glState.currenttmu].current2DMap=texnum;}
 void PurgeImage(){if(texnum!=~0u)glDeleteTextures(1,&texnum);texnum=~0u;uploadWidth=uploadHeight=0;++contentRevision;}
};
class idImageScratch:public idImage{
public:
 idImageScratch *AsScratch()override{return this;}
 void GenerateAttachment(int,int,GLenum,GLenum,GLenum=GL_CLAMP_TO_EDGE,int=0);
};
struct Images{idImageScratch *currentRenderImage,*currentDepthImage,*guiRenderImage;}*globalImages;
struct{int vidWidth=16,vidHeight=16;}glConfig;
struct{int depthDiscards=0;}questRenderCounters;
enum{RC_NOP,RC_COPY_RENDER};
struct emptyCommand_t{int commandId=RC_NOP;emptyCommand_t *next=nullptr;};
struct copyRenderCommand_t:emptyCommand_t{idImageScratch *image=nullptr;};
'''

FBO=r'''
#pragma once
#include "renderer/tr_local.h"
class FrameBuffer{
public:
 using Generator=std::function<void(FrameBuffer *)>;
 using MultiviewAttachment=void(*)(GLenum,GLenum,GLuint,GLint,GLint,GLsizei);
 GLuint fbo=0;uint64_t generation=0;int width=0,height=0;Generator generator;
 uint64_t Generation()const{return generation;}
 explicit FrameBuffer(Generator generator={}):generator(generator){}
 ~FrameBuffer(){Destroy();}
 void Destroy(){if(fbo)glDeleteFramebuffers(1,&fbo);fbo=0;}
 void Init(int w,int h,int=0){++generation;width=w;height=h;glGenFramebuffers(1,&fbo);Bind();}
 void Bind(){if(!fbo&&generator){generator(this);if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)throw std::runtime_error("incomplete real framebuffer");}glBindFramebuffer(GL_FRAMEBUFFER,fbo);}
 void BindDraw(){if(!fbo&&generator)Bind();glBindFramebuffer(GL_DRAW_FRAMEBUFFER,fbo);}
 void InvalidateDepthStencil();
 void AddArrayLayer(GLuint,GLuint,int);
 void AddMultiviewTextures(GLuint,GLuint,MultiviewAttachment);
 void AddColorRenderTexture(int attachment,idImage *image){Bind();glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0+attachment,GL_TEXTURE_2D,image->texnum,0);}
 void AddDepthRenderTexture(idImage *image){Bind();glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,image->texnum,0);}
};
'''

MANAGER=r'''
#pragma once
#include "renderer/backend/FrameBuffer.h"
struct FrameBufferManager{
 int renderWidth=16,renderHeight=16;GLenum colorFormat=GL_RGBA8,depthStencilFormat=GL_DEPTH24_STENCIL8;
 FrameBuffer *primaryFbo=nullptr,*resolveFbo=nullptr,*guiFbo=nullptr,*currentRenderFbo=nullptr;
 std::map<std::string,std::unique_ptr<FrameBuffer>> owned;
 FrameBuffer *CreateFromGenerator(const char *name,FrameBuffer::Generator generator){
  auto &fb=owned[name];if(!fb)fb.reset(new FrameBuffer(generator));else {fb->Destroy();fb->generator=generator;}return fb.get();
 }
}*frameBuffers;
'''

HARNESS=r'''
#include "renderer/backend/MobileMultiviewTargets.cpp"
GENERATE_ATTACHMENT
ARRAY_ATTACHMENTS
void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
void clearColor(FrameBuffer *fb,float r,float g){fb->Bind();glDisable(GL_SCISSOR_TEST);glClearColor(r,g,0,1);glClear(GL_COLOR_BUFFER_BIT);}
std::vector<unsigned char> readColor(FrameBuffer *fb){fb->Bind();std::vector<unsigned char> p(16*16*4);glReadPixels(0,0,16,16,GL_RGBA,GL_UNSIGNED_BYTE,p.data());return p;}
extern "C" const char *validate_targets(){
 static std::string error;
 try{
  FrameBufferManager manager;frameBuffers=&manager;FrameBuffer primary,resolve,gui;
  manager.primaryFbo=&primary;manager.resolveFbo=&resolve;manager.guiFbo=&gui;
  idImageScratch current,depth,guiImage,capture;
  Images images{&current,&depth,&guiImage};globalImages=&images;
  capture.GenerateAttachment(16,16,GL_RGBA8,GL_LINEAR);
  GLuint read=0;glGenFramebuffers(1,&read);glBindFramebuffer(GL_FRAMEBUFFER,read);
  glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,capture.texnum,0);
  glClearColor(.25,.5,.75,1);glClear(GL_COLOR_BUFFER_BIT);
  copyRenderCommand_t command;command.commandId=RC_COPY_RENDER;command.image=&capture;
  MobileMultiviewTargets targets;
  const GLuint original=capture.texnum;
  require(targets.BeginFrame(&command),"multiview failed to activate on supported GLES");
  require(targets.ColorTexture()!=0&&targets.CanTonemapDirectly(),"matching scene cannot be sampled for direct tonemap");
  targets.Layered()->Bind();require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"layered target incomplete");
  GLuint eyeNames[2];
  for(int eye=0;eye<2;++eye){
   targets.SelectEye(eye);eyeNames[eye]=capture.texnum;
   // The clone contains the source image, with separate ownership.
   glBindFramebuffer(GL_FRAMEBUFFER,read);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,capture.texnum,0);
   unsigned char sample[4];glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,sample);
   require(sample[0]==64&&sample[1]==128&&sample[2]==191,"initial eye capture pixels differ");
   // Production GenerateAttachment must be able to redefine both eye images.
   capture.GenerateAttachment(8,8,GL_RGBA8,GL_NEAREST);
   require(glGetError()==GL_NO_ERROR,"eye capture clone has immutable or invalid storage");
   manager.resolveFbo->Bind();require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"eye resolve incomplete");
   clearColor(manager.primaryFbo,eye==0?1.f:0.f,eye==1?1.f:0.f);
  }
  require(eyeNames[0]==original&&eyeNames[0]!=eyeNames[1],"captures share a texture between eyes");
  for(int eye=0;eye<2;++eye){targets.SelectEye(eye);auto pixels=readColor(manager.primaryFbo);
   require(pixels[0]==(eye==0?255:0)&&pixels[1]==(eye==1?255:0),"primary layers overwrite each other");
   require(capture.texnum==eyeNames[eye]&&capture.uploadWidth==8,"eye image metadata not restored");
  }
  targets.EndFrame();require(capture.texnum==original,"left image identity not restored");
  require(manager.primaryFbo==&primary&&manager.resolveFbo==&resolve&&manager.guiFbo==&gui,"original FBO pointers not restored");
  r_multiSamples.value=4;require(!targets.BeginFrame(&command),"unsupported MSAA silently changed");r_multiSamples.value=0;
  require(targets.BeginFrame(&command),"second frame failed");targets.SelectEye(1);
  require(capture.texnum==eyeNames[1],"right capture was recreated every frame");targets.EndFrame();
  // Image reload/storage invalidation must retire the second eye's old storage.
  capture.PurgeImage();capture.GenerateAttachment(16,16,GL_RGBA8,GL_LINEAR);
  glBindFramebuffer(GL_FRAMEBUFFER,read);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,capture.texnum,0);
  glClearColor(0,0,1,1);glClear(GL_COLOR_BUFFER_BIT);
  require(targets.BeginFrame(&command),"reload frame failed");targets.SelectEye(1);
  require(capture.uploadWidth==16,"capture reload retained stale dimensions");
  glBindFramebuffer(GL_FRAMEBUFFER,read);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,capture.texnum,0);
  unsigned char refreshed[4];glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,refreshed);
  require(refreshed[0]==0&&refreshed[2]==255,"capture reload retained stale pixels");targets.EndFrame();
  emptyCommand_t empty;require(targets.BeginFrame(&empty),"capture retirement frame failed");targets.EndFrame();
  // Resize both array layers and every lazily generated per-eye attachment.
  manager.renderWidth=24;manager.renderHeight=20;
  require(targets.BeginFrame(&empty),"resized frame failed");targets.Layered()->Bind();
  require(!targets.CanTonemapDirectly(),"scaled scene bypassed the original resampling order");
  require(targets.Layered()->width==24&&targets.Layered()->height==20,"layered dimensions stale");
  for(int eye=0;eye<2;++eye){targets.SelectEye(eye);manager.primaryFbo->Bind();manager.resolveFbo->Bind();}
  targets.EndFrame();targets.Shutdown();
  // Renderer restart deletes the manager's registered FBOs before XR starts again.
  manager.owned.clear();require(targets.BeginFrame(&empty),"renderer restart failed");
  targets.Layered()->Bind();for(int eye=0;eye<2;++eye){targets.SelectEye(eye);manager.primaryFbo->Bind();}
  targets.EndFrame();targets.Shutdown();
  require(glGetError()==GL_NO_ERROR,"GL error in shipping stereo target lifecycle");
  glDeleteFramebuffers(1,&read);current.PurgeImage();depth.PurgeImage();guiImage.PurgeImage();capture.PurgeImage();
  return nullptr;
 }catch(const std::exception &e){error=e.what();return error.c_str();}
}
'''

class StereoTargetTests(unittest.TestCase):
    def test_shipping_target_lifecycle_on_real_gles(self):
        gl=GLES()
        try:
            with tempfile.TemporaryDirectory(prefix='quest-multiview-targets-') as directory:
                base=Path(directory)
                # Khronos headers are portable; use the pinned NDK's copies when
                # host GLES development headers are not installed.
                ndk=Path(os.environ.get('ANDROID_NDK_HOME','/tmp/renderer-tools/android-ndk-r23c'))
                headers=ndk/'toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include'
                if not headers.is_dir():headers=Path('/usr/include')
                for name in ('GLES3/gl32.h','GLES3/gl3platform.h','KHR/khrplatform.h'):
                    target=base/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(headers/name,target)
                attachments='\n'.join(method(ENGINE/'renderer/backend/FrameBuffer.cpp',signature) for signature in (
                    'void FrameBuffer::AddArrayLayer(', 'void FrameBuffer::AddMultiviewTextures('))
                generate=method(ENGINE/'renderer/resources/Image_load.cpp','void idImageScratch::GenerateAttachment(')
                names=set(re.findall(r'\bqgl(\w+)',TARGETS.read_text()+attachments+generate))
                aliases='\n'.join(f'#define qgl{name} gl{name}' for name in sorted(names))
                files={'precompiled.h':'', 'renderer/tr_local.h':MOCKS.replace('GL_ALIASES',aliases),
                       'renderer/backend/FrameBuffer.h':FBO,'renderer/backend/FrameBufferManager.h':MANAGER,
                       'renderer/backend/stages/AmbientOcclusionStage.h':'',
                       'EGL/egl.h':'extern "C" void *eglGetProcAddress(const char *);\n'}
                for name,text in files.items():
                    target=base/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_text(text)
                cpp=base/'targets.cpp';cpp.write_text(HARNESS.replace('GENERATE_ATTACHMENT',generate).replace('ARRAY_ATTACHMENTS',attachments))
                library=base/'targets.so'
                search=[flag for path in os.environ.get('LD_LIBRARY_PATH','').split(':') if path for flag in ('-L',path)]
                subprocess.run(['g++','-std=c++14','-O2','-shared','-fPIC','-DTDM_VR_OPENXR','-D_GLES',
                    '-I',str(base),'-I',str(ENGINE),str(cpp),*search,'-l:libEGL.so.1','-l:libGLESv2.so.2','-o',str(library)],check=True)
                native=C.CDLL(str(library));native.validate_targets.restype=C.c_char_p
                result=native.validate_targets()
                self.assertIsNone(result,result.decode() if result else '')
                print('PASS: shipping multiview targets on real GLES preserve eye layers/captures, mutable storage, reloads, resizing, retirement, renderer restart and original FBO state')
        finally:gl.close()

if __name__=='__main__':unittest.main(verbosity=2)
