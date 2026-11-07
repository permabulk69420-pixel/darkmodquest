#!/usr/bin/env python3
"""Exercise the shipping OpenXR stereo command scheduler with renderer fixtures."""
from pathlib import Path
import subprocess
import tempfile
from shader_install_test import method

ROOT=Path(__file__).resolve().parents[2]
SOURCE=r'''
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
struct FrameBuffer{int eye=-1;void Bind();};
FrameBuffer *boundFramebuffer=nullptr;
void FrameBuffer::Bind(){boundFramebuffer=this;}
struct Image{int values[2]={};};
struct viewDef_t{
 int id=1;bool world=true,eligible=true,outputColorIsBackground=false;
 float projectionMatrix[16]={};struct{float modelViewMatrix[16]={};}worldSpace;
 struct{struct Vec{float x=0;}vieworg;}renderView;
 Image *capture=nullptr;
};
enum{RC_NOP,RC_DRAW_VIEW,RC_DRAW_LIGHTGEM,RC_SET_BUFFER,RC_BLOOM,RC_COPY_RENDER,RC_TONEMAP};
struct emptyCommand_t{int commandId;const emptyCommand_t *next=nullptr;};
struct drawSurfsCommand_t:emptyCommand_t{viewDef_t *viewDef=nullptr;};
struct drawLightgemCommand_t:emptyCommand_t{viewDef_t *viewDef=nullptr;unsigned char *dataBuffer=nullptr;};
struct copyRenderCommand_t:emptyCommand_t{Image *image=nullptr;};
struct bloomCommand_t:emptyCommand_t{};
struct tonemapCommand_t:emptyCommand_t{bool forceOutputToBlack=false;};
struct frameData_t{const emptyCommand_t *cmdHead;};
const emptyCommand_t *NextCommand(const emptyCommand_t *cmd){return cmd->next;}
struct{viewDef_t *viewDef=nullptr;}backEnd;
struct Cvar{bool v=false;bool GetBool()const{return v;}void SetBool(bool value){v=value;}};
Cvar r_tonemap{true},r_useScissor{true},r_useDepthBoundsTest{true},r_skipRender,r_skipRenderContext,vr_rendererDiagnostics;
struct{double lightingCpuMs=0;}questRenderCounters;
struct QuestCpuScope{QuestCpuScope(double &,bool){}};
int currentEye=0,depthDraws=0,lightingDraws=0,materials=0,lightgems=0,copies=0,blooms=0,tonemaps=0,fullPoseUpdates=0;
std::vector<bool> backgrounds;
void RB_SetDefaultGLState(){}void qglDisable(int){}void GL_ViewportRelative(int,int,int,int){}
void GL_ScissorRelative(int,int,int,int){}void qglClearColor(float,float,float,float){}void qglClear(int){}
void GL_CheckErrors(){}void FB_DebugShowContents(){}
const int GL_COLOR_BUFFER_BIT=1;
struct Manager{
 FrameBuffer *defaultFbo=nullptr,*primaryFbo=nullptr,*currentRenderFbo=nullptr;
 bool tonemapNotYet=false;
 void EnterPrimary(){currentRenderFbo=primaryFbo;}
 void LeavePrimary(bool=true){currentRenderFbo=defaultFbo;}
}manager,*frameBuffers=&manager;
struct MobileStereoView{
 float projection[2][16],worldView[2][16];decltype(viewDef_t::renderView.vieworg)origin[2];unsigned ssaoTexture;
};
struct Targets{
 FrameBuffer layered,layers[2];bool ended=false;
 Targets(){layers[0].eye=0;layers[1].eye=1;}
 void SelectEye(int eye){currentEye=eye;frameBuffers->primaryFbo=&layers[eye];}
 FrameBuffer *Layered(){return &layered;}unsigned SSAOTexture(){return 1;}
 void BindScene(){frameBuffers->currentRenderFbo=&layered;layered.Bind();}
 void EndFrame(){ended=true;}
};
struct AO{bool ShouldEnableForCurrentView(){return false;}void ComputeSSAOFromDepth(){}void CopySSAOToLayer(unsigned,int){}}ao,*ambientOcclusion=&ao;
struct MobileLighting{
 bool HasView(const viewDef_t *view){return view->eligible;}
 bool CanDrawMultiview(const viewDef_t *view,const Targets &){return view->eligible;}
 void Draw(const viewDef_t *,const MobileStereoView *stereo){
  // The real lighting stage rebinds currentRenderFbo on entry. Its logical
  // target must therefore be the same two-view FBO selected by the scheduler.
  frameBuffers->currentRenderFbo->Bind();
  assert(boundFramebuffer==frameBuffers->currentRenderFbo&&boundFramebuffer->eye==-1);
  assert(stereo->projection[0][0]==10&&stereo->projection[1][0]==11);
  assert(stereo->origin[0].x==0&&stereo->origin[1].x==1);++lightingDraws;
 }
};
struct Renderer{
 ::MobileLighting lighting;::MobileLighting &MobileLighting(){return lighting;}
 void DrawMultiviewDepth(const viewDef_t *,bool background,const MobileStereoView &){
  assert(boundFramebuffer==frameBuffers->currentRenderFbo&&boundFramebuffer->eye==-1);
  ++depthDraws;backgrounds.push_back(background);
 }
 void CaptureDepth(const viewDef_t *){}
 void DrawView(const viewDef_t *view,bool,bool shared){
  if(!shared){++depthDraws;++lightingDraws;}
  if(view->capture)assert(view->capture->values[currentEye]==10+currentEye);
  ++materials;
 }
 void DrawLightgem(const viewDef_t *,unsigned char *){++lightgems;}
 void Tonemap(){++tonemaps;}
}renderer,*renderBackend=&renderer;
void RB_CopyRender(const emptyCommand_t *cmd){++copies;((const copyRenderCommand_t *)cmd)->image->values[currentEye]=10+currentEye;}
bool RB_Bloom(bloomCommand_t *){++blooms;return true;}
struct Common{void Error(const char *,int){assert(false);}}instance,*common=&instance;
struct Swapchain{FrameBuffer fb;FrameBuffer *CurrentFrameBuffer(){return &fb;}};
struct OpenXRBackend{struct Impl{
 Targets multiview;Swapchain eyeSwapchains[2];bool rawSrgbWrites=false;
 bool IsWorldView(const viewDef_t *view){return view->world;}
 void UpdateViewForEye(viewDef_t *view,int eye,bool entities=true){
  if(entities)++fullPoseUpdates;view->projectionMatrix[0]=10+eye;
  view->worldSpace.modelViewMatrix[0]=20+eye;view->renderView.vieworg.x=eye;
 }
 void ExecuteMultiview(const frameData_t *);
};};
EXECUTE_STEREO
int main(){
 OpenXRBackend::Impl backend;Image capture;viewDef_t sky,main,ui,gem;
 sky.outputColorIsBackground=true;main.id=2;main.capture=&capture;ui.world=false;
 drawSurfsCommand_t skyCommand;skyCommand.commandId=RC_DRAW_VIEW;skyCommand.viewDef=&sky;
 copyRenderCommand_t captureCommand;captureCommand.commandId=RC_COPY_RENDER;captureCommand.image=&capture;
 drawSurfsCommand_t mainCommand;mainCommand.commandId=RC_DRAW_VIEW;mainCommand.viewDef=&main;
 drawLightgemCommand_t gemCommand;gemCommand.commandId=RC_DRAW_LIGHTGEM;gemCommand.viewDef=&gem;
 bloomCommand_t bloom;bloom.commandId=RC_BLOOM;tonemapCommand_t tone;tone.commandId=RC_TONEMAP;
 drawSurfsCommand_t uiCommand;uiCommand.commandId=RC_DRAW_VIEW;uiCommand.viewDef=&ui;
 skyCommand.next=&captureCommand;captureCommand.next=&mainCommand;mainCommand.next=&gemCommand;
 gemCommand.next=&bloom;bloom.next=&tone;tone.next=&uiCommand;
 frameData_t frame{&skyCommand};backend.ExecuteMultiview(&frame);
 assert(depthDraws==2&&lightingDraws==2&&materials==4);
 assert(lightgems==1&&copies==2&&blooms==2&&tonemaps==2&&fullPoseUpdates==4);
 assert(backgrounds.size()==2&&!backgrounds[0]&&backgrounds[1]&&backend.multiview.ended);
 assert(!r_useScissor.GetBool()&&!r_useDepthBoundsTest.GetBool());
 // An ineligible view retains its original per-eye rendering.
 main.capture=nullptr;main.eligible=false;mainCommand.next=nullptr;frame.cmdHead=&mainCommand;
 depthDraws=lightingDraws=materials=fullPoseUpdates=0;backend.ExecuteMultiview(&frame);
 assert(depthDraws==2&&lightingDraws==2&&materials==2&&fullPoseUpdates==2);
 std::puts("PASS: shipping stereo scheduler submits shared scene depth/lighting once, preserves both eye captures/backgrounds, keeps effects per eye and lightgem once, and retains unsupported-view rendering");
}
'''
source=SOURCE.replace('EXECUTE_STEREO',method(
    ROOT/'Q3E/src/main/jni/darkmod/renderer/vr/OpenXRBackend.cpp',
    'void OpenXRBackend::Impl::ExecuteMultiview('))
with tempfile.TemporaryDirectory(prefix='quest-stereo-commands-') as directory:
    cpp=Path(directory)/'commands.cpp';cpp.write_text(source)
    binary=Path(directory)/'commands'
    subprocess.run(['g++','-std=c++14','-O2',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
