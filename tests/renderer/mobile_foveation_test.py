#!/usr/bin/env python3
"""Exercise shipping VRS policy/controller with real GLES targets and API spies.

Mesa does not expose the Quest vendor rate control. Spies check the extension
contract and rate selection; real GLES checks allocation, state and image data.
This does not emulate or establish Qualcomm's actual shading performance.
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
from mobile_multiview_targets_test import ENGINE, MOCKS

SOURCE=ENGINE/'renderer/backend/MobileFoveation.cpp'
HARNESS=r'''
#include "renderer/tr_local.h"
#include "renderer/backend/MobileFoveation.h"
struct Console{void Printf(const char *,...){}void Warning(const char *,...) {}}console,*common=&console;
static int mode=0,rateCalls=0,querySamples=0;
static bool failMap=false,coarse4=true;
static unsigned requestedRate=0x96A6,attachedTexture=0,combiner0=0x96D2,combiner1=0x96D2;
static int tileWidth=0,tileHeight=0;
static std::vector<unsigned> rates;
const GLubyte *testString(GLenum name){
 if(name!=GL_EXTENSIONS)return glGetString(name);
 return (const GLubyte *)(mode==1?"GL_QCOM_shading_rate":mode==2?"GL_EXT_fragment_shading_rate GL_EXT_fragment_shading_rate_attachment":
  mode==3?"GL_QCOM_shading_rate_bogus":mode==4?"GL_QCOM_shading_rate":"");
}
void testInteger(GLenum name,GLint *value){
 if(name==0x96D7||name==0x96D9){*value=8;return;}
 if(name==0x96D8||name==0x96DA){*value=32;return;}
 glGetIntegerv(name,value);
}
GLenum testStatus(GLenum target){return failMap&&attachedTexture?GL_FRAMEBUFFER_UNSUPPORTED:glCheckFramebufferStatus(target);}
void testRate(unsigned rate){requestedRate=rate;rates.push_back(rate);++rateCalls;}
void testCombine(unsigned a,unsigned b){combiner0=a;combiner1=b;}
void testAttach(unsigned target,unsigned attachment,unsigned texture,int layer,int layers,int tw,int th){
 assert(target==GL_DRAW_FRAMEBUFFER&&attachment==0x96D1&&layer==0&&layers==1);
 attachedTexture=texture;tileWidth=tw;tileHeight=th;
}
void testQuery(int samples,int maximum,int *count,unsigned *out){
 querySamples=samples;unsigned values[]={0x96A6,0x96A7,0x96A9,0x96AC};*count=coarse4?4:3;
 assert(maximum>=*count);std::copy(values,values+*count,out);
}
extern "C" void *eglGetProcAddress(const char *name){
 if(mode==4)return nullptr;
 if(!strcmp(name,"glShadingRateQCOM")||!strcmp(name,"glShadingRateEXT"))return (void *)testRate;
 if(!strcmp(name,"glShadingRateCombinerOpsEXT"))return (void *)testCombine;
 if(!strcmp(name,"glFramebufferShadingRateEXT"))return (void *)testAttach;
 if(!strcmp(name,"glGetFragmentShadingRatesEXT"))return (void *)testQuery;
 return nullptr;
}
#include "renderer/backend/MobileFoveation.cpp"
void require(bool okay,const char *message){if(!okay)throw std::runtime_error(message);}
extern "C" const char *validate_foveation(){
 static std::string error;
 try{
  using namespace MobileRenderPlan;
  Rect area{0,0,1024,1024},centre{500,500,524,524},edge{0,0,64,64};
  const float optical[2][2]={{-.15f,.12f},{.15f,-.12f}};
  require(FoveationDensity(area,area,optical,3)==0,"large ground/wall crossed centre at coarse rate");
  for(int level=1;level<=3;++level){
   require(FoveationDensity(centre,area,optical,level)==0,"fovea lost full-rate shading");
   require(FoveationDensity(edge,area,optical,level)>0,"peripheral draw not reduced");
   // Every pixel inside either full-quality disc must be assigned full rate.
   const double radius=level==1?.75:level==2?.60:.50;
   for(int y=0;y<1024;y+=7)for(int x=0;x<1024;x+=7)for(int eye=0;eye<2;++eye){
    const double dx=(x+.5)/512.-1.-optical[eye][0],dy=(y+.5)/512.-1.-optical[eye][1];
    if(dx*dx+dy*dy<=radius*radius)require(FoveationDensity({x,y,x+1,y+1},area,optical,level)==0,"stereo centre coverage violated");
   }
  }
  require(FoveationDensity(edge,area,optical,0)==0,"off selected coarse shading");
  require(FoveationDensity({32,48,96,112},{32,48,1056,1072},optical,2)==FoveationDensity(edge,area,optical,2),"viewport offset changed density");
  float projection[16]={},right[16]={};projection[8]=.15f;projection[9]=-.12f;right[8]=-.15f;right[9]=.12f;
  GLuint fb=0,color=0,depth=0;glGenFramebuffers(1,&fb);glBindFramebuffer(GL_FRAMEBUFFER,fb);
  glGenTextures(1,&color);glBindTexture(GL_TEXTURE_2D,color);glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,1024,1024);
  glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,color,0);
  glGenTextures(1,&depth);glBindTexture(GL_TEXTURE_2D,depth);glTexStorage2D(GL_TEXTURE_2D,1,GL_DEPTH24_STENCIL8,1024,1024);
  glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,depth,0);
  require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"real test framebuffer incomplete");
  GL_SelectTexture(3);glBindTexture(GL_TEXTURE_2D,color);glEnable(GL_SCISSOR_TEST);glScissor(3,5,43,57);
  glViewport(7,9,123,141);glDepthFunc(GL_EQUAL);glStencilFunc(GL_EQUAL,128,255);glPixelStorei(GL_UNPACK_ALIGNMENT,8);
  GLuint unpack=0;glGenBuffers(1,&unpack);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,unpack);glBufferData(GL_PIXEL_UNPACK_BUFFER,128,nullptr,GL_STATIC_DRAW);
  glPixelStorei(GL_UNPACK_ROW_LENGTH,37);glPixelStorei(GL_UNPACK_SKIP_PIXELS,2);glPixelStorei(GL_UNPACK_SKIP_ROWS,3);
  for(int selected:{0,1,2,3,4}){
   mode=selected;MobileFoveation foveation;vr_foveation.value=2;rates.clear();rateCalls=0;
   foveation.BeginFrame();foveation.BeginLighting(area,projection,right,0,true);
   if(mode==1){
    require(foveation.UsesDrawRates(),"QCOM fallback unavailable");
    foveation.Draw(edge);require(requestedRate==0x96A9,"peripheral draw not 2x2");
    foveation.Draw(edge);require(rateCalls==1,"redundant rate call");
    foveation.Draw(centre);require(requestedRate==0x96A6,"centre not restored to 1x1");
    foveation.Draw(edge);
   }else if(mode==2){
    require(!foveation.UsesDrawRates()&&attachedTexture,"spatial map not attached");
    require(querySamples==1&&tileWidth==16&&tileHeight==16,"invalid sample count/tile size");
    require(combiner0==0x96D2&&combiner1==0x96D3,"map combiner not selected");
    GLuint read=0;glGenFramebuffers(1,&read);glBindFramebuffer(GL_READ_FRAMEBUFFER,read);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,attachedTexture,0);
    require(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"real R8UI map allocation failed");
    std::vector<unsigned char> pixels(64*64);glReadPixels(0,0,64,64,GL_RED_INTEGER,GL_UNSIGNED_BYTE,pixels.data());
    require(pixels[32*64+32]==0&&pixels[0]==5,"map encoding or centre/edge density wrong");
    glBindFramebuffer(GL_READ_FRAMEBUFFER,fb);glDeleteFramebuffers(1,&read);
   }else {foveation.Draw(edge);require(rateCalls==0&&!foveation.UsesDrawRates(),"unsupported extension was called");}
   foveation.EndLighting();require(requestedRate==0x96A6&&!attachedTexture,"rate/map leaked outside lighting");
   if(mode==2)require(combiner1==0x96D2,"attachment combiner leaked");
   GLint value;glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&value);require(value==GLint(fb),"VRS changed draw target");
   glGetIntegerv(GL_DEPTH_FUNC,&value);require(value==GL_EQUAL,"VRS changed depth comparison");
   GLint box[4];glGetIntegerv(GL_SCISSOR_BOX,box);require(box[0]==3&&box[1]==5&&box[2]==43&&box[3]==57,"VRS changed scissor");
   glGetIntegerv(GL_VIEWPORT,box);require(box[0]==7&&box[1]==9&&box[2]==123&&box[3]==141,"VRS changed viewport");
   glGetIntegerv(GL_ACTIVE_TEXTURE,&value);require(value==GL_TEXTURE3,"VRS leaked active texture unit");
   glGetIntegerv(GL_UNPACK_ALIGNMENT,&value);require(value==8,"VRS leaked pixel alignment");
   glGetIntegerv(GL_UNPACK_ROW_LENGTH,&value);require(value==37,"VRS leaked unpack row length");
   glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&value);require(value==2,"VRS leaked unpack skip pixels");
   glGetIntegerv(GL_UNPACK_SKIP_ROWS,&value);require(value==3,"VRS leaked unpack skip rows");
   glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&value);require(value==GLint(unpack),"VRS leaked unpack buffer");
   for(bool main:{false,true}){
    const int before=rateCalls;vr_foveation.value=main?0:2;
    foveation.BeginLighting(area,projection,right,0,main);foveation.Draw(edge);foveation.EndLighting();
    require(rateCalls==before,"off/sky/mirror/lightgem touched shading rate");
   }
   foveation.Shutdown();
  }
  mode=2;failMap=true;vr_foveation.value=3;MobileFoveation foveation;
  foveation.BeginLighting(area,projection,right,0,true);require(!attachedTexture&&!foveation.UsesDrawRates(),"rejected map was retained");
  foveation.Draw(edge);foveation.EndLighting();failMap=false;foveation.Shutdown();
  // Context restart must query extensions again, not retain stale function/state.
  mode=1;foveation.BeginLighting(area,projection,right,0,true);foveation.Draw(edge);
  require(requestedRate==0x96AC,"restart/high outer-periphery rate failed");foveation.Shutdown();
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);glDeleteBuffers(1,&unpack);
  glDeleteFramebuffers(1,&fb);glDeleteTextures(1,&color);glDeleteTextures(1,&depth);
  require(glGetError()==GL_NO_ERROR,"GLES error in production VRS controller");
  return nullptr;
 }catch(const std::exception &e){error=e.what();return error.c_str();}
}
'''

class FoveationTests(unittest.TestCase):
    def test_production_policy_controller_and_gles_state(self):
        gl=GLES()
        try:
            with tempfile.TemporaryDirectory(prefix='quest-foveation-') as directory:
                base=Path(directory)
                headers=Path(os.environ.get('ANDROID_NDK_HOME','/tmp/renderer-tools/android-ndk-r23c'))/'toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include'
                if not headers.is_dir():headers=Path('/usr/include')
                for name in ('GLES3/gl32.h','GLES3/gl3platform.h','KHR/khrplatform.h'):
                    dest=base/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(headers/name,dest)
                names=set(re.findall(r'\bqgl(\w+)',SOURCE.read_text()))-{'GetString','GetIntegerv','CheckFramebufferStatus'}
                aliases='\n'.join(f'#define qgl{name} gl{name}' for name in sorted(names))
                aliases+='\n#define qglGetString testString\n#define qglGetIntegerv testInteger\n#define qglCheckFramebufferStatus testStatus\n'
                for name,text in {'precompiled.h':'','renderer/tr_local.h':MOCKS.replace('GL_ALIASES',aliases).replace('CVAR_RENDERER=1,','CVAR_INTEGER=8,CVAR_RENDERER=1,'),
                                  'EGL/egl.h':'extern "C" void *eglGetProcAddress(const char *);\n'}.items():
                    dest=base/name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_text(text)
                cpp=base/'foveation.cpp';cpp.write_text(HARNESS)
                library=base/'foveation.so'
                search=[flag for path in os.environ.get('LD_LIBRARY_PATH','').split(':') if path for flag in ('-L',path)]
                subprocess.run(['g++','-std=c++14','-O2','-shared','-fPIC','-DTDM_VR_OPENXR','-I',str(base),'-I',str(ENGINE),str(cpp),
                                *search,'-l:libGLESv2.so.2','-o',str(library)],check=True)
                native=C.CDLL(str(library));native.validate_foveation.restype=C.c_char_p
                result=native.validate_foveation()
                self.assertIsNone(result,result.decode() if result else '')
                print('PASS: production VRS policy/controller, stereo fovea, map data, rate reset, unsupported/rejected paths and GLES state')
        finally:gl.close()

if __name__=='__main__':unittest.main(verbosity=2)
