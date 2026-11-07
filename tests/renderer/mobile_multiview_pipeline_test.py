#!/usr/bin/env python3
"""Exercise the production lighting-stage entry and layered/eye handoff on GLES.

This deliberately retains the old single-eye logical target for a negative case:
the real stage's entry bind drops lighting from an eye (or rejects the draw). The corrected
path then runs depth, eye depth copies, the stage entry and per-eye material draws.
"""
import ctypes as C
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from mobile_gles_test import GLES, IDENTITY, PROJECTION, source
import mobile_gles_test as shader_fixtures
from mobile_multiview_targets_test import ENGINE, TARGETS, MOCKS, FBO, MANAGER
from shader_install_test import method

STAGE_FIXTURES=r'''
#define TRACE_GL_SCOPE(...)
struct viewDef_t{TestScissor scissor;bool IsLightGem()const{return false;}};
struct MobileStereoView{float projection[2][16]={},worldView[2][16]={};GLuint ssaoTexture=0;};
namespace MobileRenderPlan{
struct Rect{int x1=0,y1=0,x2=0,y2=0;void Add(const Rect &r){*this=r;}};
Rect ProjectScissor(const float *,const float *,const float *,const float *,const Rect &r){return r;}
}
struct Vec{float v[3]={};const float *ToFloatPtr()const{return v;}};
struct Tri{Vec bounds[2];};struct Light{Tri *frustumTris=nullptr;};
struct LightClip{Light *light;MobileRenderPlan::Rect rect;};struct Batch{};
struct ViewPlan{MobileRenderPlan::Rect viewport,currentScissor;std::vector<LightClip>lights;std::vector<Batch>batches;};
static idCVar vr_rendererDiagnostics("diagnostics","0");
bool R_UseQuestLightScissors(){return true;}
MobileRenderPlan::Rect RB_QuestViewViewport(const viewDef_t *){return {0,0,16,16};}
MobileRenderPlan::Rect RB_QuestLightScissor(const viewDef_t *,Light *,const MobileRenderPlan::Rect &r){return r;}
void FB_ApplyViewport(){glViewport(0,0,16,16);}void FB_ApplyScissor(){glScissor(0,0,16,16);}
const int CT_FRONT_SIDED=0;void GL_Cull(int){glDisable(GL_CULL_FACE);}
struct AO{void BindSSAOTexture(int){}}ao,*ambientOcclusion=&ao;
struct GLSLProgram{static void Deactivate(){glUseProgram(0);}};
class MobileLightingStage{
public:
 struct Impl{
  std::map<const viewDef_t *,ViewPlan>views;const MobileStereoView *stereo=nullptr;
  bool checkScissor=false;int eyePasses=0,multiviewPasses=0;uint64_t drawSerial=0;GLuint program;
  void DrawBatch(const viewDef_t *,ViewPlan &,const Batch &){
   glUseProgram(program);glEnable(GL_DEPTH_TEST);glDepthFunc(GL_EQUAL);glDepthMask(GL_FALSE);
   glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ONE);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
   glDrawArrays(GL_TRIANGLES,0,6);
  }
 };
 Impl *impl;void BindShadowTexture(int){};
 void Draw(const viewDef_t *,const MobileStereoView *);
};
LIGHTING_STAGE_DRAW

extern "C" int validate_pipeline(GLuint lightingProgram,GLuint depthProgram,GLuint materialProgram,int oldBinding,unsigned char *pixels){
 FrameBufferManager manager;frameBuffers=&manager;FrameBuffer primary,resolve,gui;
 manager.primaryFbo=&primary;manager.resolveFbo=&resolve;manager.guiFbo=&gui;manager.currentRenderFbo=&primary;
 idImageScratch current,depth,guiImage;Images images{&current,&depth,&guiImage};globalImages=&images;
 MobileMultiviewTargets targets;emptyCommand_t commands;
 if(!targets.BeginFrame(&commands))return -1;
 targets.SelectEye(1);manager.currentRenderFbo=manager.primaryFbo;manager.primaryFbo->Bind();
 targets.BindScene();glViewport(0,0,16,16);glDisable(GL_SCISSOR_TEST);glDisable(GL_STENCIL_TEST);glDisable(GL_BLEND);
 glEnable(GL_DEPTH_TEST);glDepthMask(GL_TRUE);glDepthFunc(GL_LESS);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
 glClearColor(0,0,0,1);glClearDepthf(1);glClearStencil(128);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
 glUseProgram(depthProgram);glDrawArrays(GL_TRIANGLES,0,6);
 // Match the scheduler's per-eye depth captures before it resumes shared lighting.
 for(int eye=0;eye<2;++eye){
  targets.SelectEye(eye);manager.currentRenderFbo=manager.primaryFbo;manager.primaryFbo->Bind();manager.resolveFbo->Bind();
  glBindFramebuffer(GL_READ_FRAMEBUFFER,manager.primaryFbo->fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER,manager.resolveFbo->fbo);
  glBlitFramebuffer(0,0,16,16,0,0,16,16,GL_DEPTH_BUFFER_BIT,GL_NEAREST);manager.primaryFbo->Bind();
 }
 if(oldBinding)targets.Layered()->Bind();else targets.BindScene();
 viewDef_t view;backEnd.viewDef=&view;MobileStereoView stereo;stereo.ssaoTexture=targets.SSAOTexture();
 Light light;MobileLightingStage::Impl impl;impl.program=lightingProgram;
 impl.views[&view].lights.push_back({&light,{}});impl.views[&view].batches.push_back({});
 MobileLightingStage stage;stage.impl=&impl;
 const GLenum before=glGetError();if(before!=GL_NO_ERROR)return int(before);
 stage.Draw(&view,&stereo);const GLenum result=glGetError();
 if(result==GL_NO_ERROR){
  for(int eye=0;eye<2;++eye){
   targets.SelectEye(eye);manager.currentRenderFbo=manager.primaryFbo;manager.primaryFbo->Bind();
   glDisable(GL_SCISSOR_TEST);glDisable(GL_STENCIL_TEST);glUseProgram(materialProgram);
   glEnable(GL_DEPTH_TEST);glDepthFunc(GL_EQUAL);glDepthMask(GL_FALSE);glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ONE);
   glDrawArrays(GL_TRIANGLES,0,6);
   glReadPixels(0,0,16,16,GL_RGBA,GL_UNSIGNED_BYTE,pixels+eye*16*16*4);
  }
 }
 targets.EndFrame();targets.Shutdown();current.PurgeImage();depth.PurgeImage();guiImage.PurgeImage();
 return int(result?result:glGetError());
}
'''

class MultiviewPipelineTests(unittest.TestCase):
    def test_real_lighting_stage_preserves_layered_target_and_eye_materials(self):
        gl=GLES()
        try:
            with tempfile.TemporaryDirectory(prefix='quest-multiview-pipeline-') as directory:
                base=Path(directory)
                ndk=Path(os.environ.get('ANDROID_NDK_HOME','/tmp/renderer-tools/android-ndk-r23c'))
                headers=ndk/'toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include'
                if not headers.is_dir():headers=Path('/usr/include')
                for name in ('GLES3/gl32.h','GLES3/gl3platform.h','KHR/khrplatform.h'):
                    target=base/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(headers/name,target)
                attachments='\n'.join(method(ENGINE/'renderer/backend/FrameBuffer.cpp',signature) for signature in (
                    'void FrameBuffer::AddArrayLayer(', 'void FrameBuffer::AddMultiviewTextures('))
                generate=method(ENGINE/'renderer/resources/Image_load.cpp','void idImageScratch::GenerateAttachment(')
                draw=method(ENGINE/'renderer/backend/stages/MobileLightingStage.cpp','void MobileLightingStage::Draw(')
                names=set(re.findall(r'\bqgl(\w+)',TARGETS.read_text()+attachments+generate+draw))
                aliases='\n'.join(f'#define qgl{name} gl{name}' for name in sorted(names))
                mocks=MOCKS.replace('GL_ALIASES',aliases).replace('struct{struct{struct Tmu',
                    'struct TestScissor{int x1=0,y1=0,x2=15,y2=15;};\nstruct viewDef_t;\n'
                    'struct{const viewDef_t *viewDef=nullptr;TestScissor currentScissor;struct{struct Tmu')
                # Match FrameBuffer::Generate's protection of material texture units.
                fb=FBO.replace('generator(this);','int unit=backEnd.glState.currenttmu;GL_SelectTexture(15);generator(this);GL_SelectTexture(unit);')
                files={'precompiled.h':'','renderer/tr_local.h':mocks,'renderer/backend/FrameBuffer.h':fb,
                    'renderer/backend/FrameBufferManager.h':MANAGER,'renderer/backend/stages/AmbientOcclusionStage.h':'',
                    'EGL/egl.h':'extern "C" void *eglGetProcAddress(const char *);\n'}
                for name,text in files.items():
                    target=base/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_text(text)
                cpp=base/'pipeline.cpp';cpp.write_text('#include "renderer/backend/MobileMultiviewTargets.cpp"\n'+generate+'\n'+attachments+'\n'+STAGE_FIXTURES.replace('LIGHTING_STAGE_DRAW',draw))
                library=base/'pipeline.so';search=[flag for path in os.environ.get('LD_LIBRARY_PATH','').split(':') if path for flag in ('-L',path)]
                subprocess.run(['g++','-std=c++14','-O2','-shared','-fPIC','-DTDM_VR_OPENXR','-D_GLES',
                    '-I',str(base),'-I',str(ENGINE),str(cpp),*search,'-l:libEGL.so.1','-l:libGLESv2.so.2','-o',str(library)],check=True)
                native=C.CDLL(str(library));native.validate_pipeline.argtypes=[C.c_uint,C.c_uint,C.c_uint,C.c_int,C.c_void_p]
                native.validate_pipeline.restype=C.c_int
                def mv(path):return source(path).replace('#version 320 es','#version 320 es\n#define QUEST_MULTIVIEW 1',1)
                general=gl.program(source('stages/mobile/forward.vert.glsl'),source('stages/mobile/forward.frag.glsl'))
                forward=gl.program(mv('stages/mobile/forward.vert.glsl'),mv('stages/mobile/forward_1.frag.glsl'))
                depth=gl.program(mv('stages/depth/depth.vert.glsl'),source('stages/depth/depth.frag.glsl'))
                material=gl.program(source('stages/depth/depth.vert.glsl'),source('stages/depth/depth.frag.glsl'))
                fixture=shader_fixtures.MobileShaderTests();fixture.gl=gl;fixture.forward=general
                references=[]
                for eye in (-.03125,.03125):
                    fixture.setup_forward(ambient=False,eye=eye);references.append(gl.pixels(16,16))
                for old_binding in (1,0):
                    for program,stereo,color in ((depth,True,[0.,0.,0.,1.]),(material,False,[.04,.08,.12,0.])):
                        gl.call('glUseProgram',program)
                        for eye in (range(2) if stereo else (0,)):
                            suffix=f'[{eye}]' if stereo else ''
                            gl.uniform(program,'u_modelViewMatrix'+suffix,IDENTITY,'m');gl.uniform(program,'u_projectionMatrix'+suffix,PROJECTION,'m')
                        gl.uniform(program,'u_modelMatrix',IDENTITY,'m');gl.uniform(program,'u_textureMatrix',IDENTITY,'m')
                        gl.uniform(program,'u_clipPlane',[0.,0.,0.,0.]);gl.uniform(program,'u_color',color);gl.uniform(program,'u_alphaTest',-1.)
                    fixture.forward=forward;fixture.setup_forward(ambient=False,draw=False)
                    for eye in range(2):
                        gl.uniform(forward,f'u_modelViewMatrix[{eye}]',IDENTITY,'m');gl.uniform(forward,f'u_projectionMatrix[{eye}]',PROJECTION,'m')
                        gl.uniform(forward,f'u_viewOrigin[{eye}]',[-.03125 if eye==0 else .03125,0.,0.])
                    data=(C.c_ubyte*(2*16*16*4))()
                    error=native.validate_pipeline(forward,depth,material,old_binding,data)
                    self.assertIn(error,(0,0x0502) if old_binding else (0,))
                    differences=[]
                    if error==0:
                        for eye in range(2):
                            actual=bytes(data)[eye*1024:(eye+1)*1024]
                            expected=bytes(min(255,value+([10,20,31,0][i%4])) for i,value in enumerate(references[eye]))
                            differences.append(max(abs(a-b) for a,b in zip(actual,expected)))
                        if old_binding:self.assertGreater(max(differences),10,differences)
                        else:self.assertLessEqual(max(differences),1,differences)
                    print('Old binding' if old_binding else 'Corrected binding', 'GL error:',error,'eye pixel differences:',differences)
                print('PASS: real lighting-stage entry reproduces missing eye lighting; corrected scene target preserves lighting and later material colour in both eyes')
        finally:gl.close()

if __name__=='__main__':unittest.main(verbosity=2)
