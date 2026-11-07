#!/usr/bin/env python3
"""Exercise production multiview program validation and its reload cache."""
from pathlib import Path
import subprocess
import tempfile
from shader_install_test import method

ROOT=Path(__file__).resolve().parents[2]
ENGINE=ROOT/'Q3E/src/main/jni/darkmod'
SOURCE=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <memory>
#include <vector>
#include <cstdarg>
using idHashMapDict=std::map<std::string,std::string>;using idStr=std::string;
const char *va(const char *format,...){static char buffer[200];va_list args;va_start(args,format);vsnprintf(buffer,sizeof(buffer),format,args);va_end(args);return buffer;}
struct GLSLProgram{
 bool linked=true,model=true,projection=true,origin=true,layer=true;uint64_t generation=1;int queries=0;
 bool EnsureLinked(){return linked;}uint64_t Generation()const{return generation;}
 int GetUniformLocation(const char *name){++queries;
  if(!std::strcmp(name,"u_modelViewMatrix[1]"))return model?1:-1;
  if(!std::strcmp(name,"u_projectionMatrix[1]"))return projection?2:-1;
  if(!std::strcmp(name,"u_viewOrigin[1]"))return origin?3:-1;
  if(!std::strcmp(name,"u_eyeLayer"))return layer?4:-1;assert(false);return -1;
 }
 const std::string &GetName()const{static std::string name="test";return name;}
 bool stereoDefine=false,fullDefine=false;
 template<class A,class B>void LoadFromFiles(const A &,const B &,const idHashMapDict &defines){
  stereoDefine=defines.count("QUEST_MULTIVIEW");fullDefine=defines.count("QUEST_FULL_PRECISION_LIGHTING");}
};
struct Programs{
 std::vector<std::unique_ptr<GLSLProgram>>owned;
 template<class Generator>GLSLProgram *LoadFromGenerator(const char *,Generator generator){owned.emplace_back(new GLSLProgram);generator(owned.back().get());return owned.back().get();}
 GLSLProgram *LoadFromFiles(const char *,const char *,const char *){owned.emplace_back(new GLSLProgram);return owned.back().get();}
}programsInstance,*programManager=&programsInstance;
struct Common{int warnings=0;template<class...Args>void Warning(const char *,Args...){++warnings;}}instance,*common=&instance;
struct DepthStage{
 GLSLProgram *multiviewShader=nullptr;uint64_t multiviewCheckedGeneration=0;bool multiviewReady=false;
 bool PrepareMultiview();
};
struct MobileLightingStage{
 struct Impl{GLSLProgram *lighting[12]={},*depth=nullptr;uint64_t multiviewCheckedGeneration[4]={};bool multiviewReady[4]={};};
 Impl *impl;bool PrepareMultiview();void Init();
};
using GLuint=unsigned;struct FrameBuffer{};struct idImage{};
struct{bool enabled=true;bool GetBool()const{return enabled;}}r_tonemap;
struct TonemapStage{
 GLSLProgram *arrayShader=nullptr;uint64_t arrayCheckedGeneration=0;bool arrayReady=false;
 int draws=0;unsigned texture=0;int eye=-1;
 void Apply(FrameBuffer *,idImage *image,GLuint tex,int layer,GLSLProgram *){assert(!image);++draws;texture=tex;eye=layer;}
 bool ApplyTonemapArray(FrameBuffer *,GLuint,int);
};
PRODUCTION
int main(){
 GLSLProgram depth;depth.model=false;DepthStage stage;stage.multiviewShader=&depth;
 assert(!stage.PrepareMultiview()&&depth.queries==1&&common->warnings==1);
 for(int frame=0;frame<100;++frame)assert(!stage.PrepareMultiview());
 assert(depth.queries==1&&common->warnings==1);
 depth.model=true;++depth.generation;assert(stage.PrepareMultiview()&&depth.queries==3);
 for(int frame=0;frame<100;++frame)assert(stage.PrepareMultiview());assert(depth.queries==3);
 depth.linked=false;assert(!stage.PrepareMultiview());depth.linked=true;
 GLSLProgram programs[4];MobileLightingStage::Impl impl;MobileLightingStage lighting{&impl};
 assert(!lighting.PrepareMultiview());
 for(int variant=0;variant<4;++variant)impl.lighting[variant+4]=&programs[variant];
 programs[0].projection=false;assert(!lighting.PrepareMultiview()&&programs[0].queries==2);
 programs[0].projection=true;++programs[0].generation;assert(lighting.PrepareMultiview());
 int queries=0;for(const auto &p:programs)queries+=p.queries;
 for(int frame=0;frame<100;++frame)assert(lighting.PrepareMultiview());
 int after=0;for(const auto &p:programs)after+=p.queries;assert(after==queries);
 programs[2].origin=false;++programs[2].generation;assert(!lighting.PrepareMultiview());
 programs[2].origin=true;++programs[2].generation;assert(lighting.PrepareMultiview());
 GLSLProgram array;array.layer=false;TonemapStage tone;tone.arrayShader=&array;FrameBuffer output;
 assert(!tone.ApplyTonemapArray(&output,71,0)&&tone.draws==0&&array.queries==1);
 array.layer=true;++array.generation;assert(tone.ApplyTonemapArray(&output,71,1));
 assert(tone.draws==1&&tone.texture==71&&tone.eye==1&&array.queries==2);
 for(int i=0;i<100;++i)assert(tone.ApplyTonemapArray(&output,71,i%2));assert(array.queries==2);
 array.linked=false;assert(!tone.ApplyTonemapArray(&output,71,0));array.linked=true;
 array.layer=false;++array.generation;assert(!tone.ApplyTonemapArray(&output,71,0));
 r_tonemap.enabled=false;assert(!tone.ApplyTonemapArray(&output,71,0));
 MobileLightingStage::Impl registered;MobileLightingStage registration{&registered};registration.Init();
 for(int i=0;i<12;++i){assert(registered.lighting[i]);assert(registered.lighting[i]->stereoDefine==(i>=4&&i<8));
  assert(registered.lighting[i]->fullDefine==(i>=8));}
 std::puts("PASS: production multiview/array-tonemap validation rejects stale/missing/failed programs, refreshes on reload and skips repeated GL queries");
}
'''
production='\n'.join(method(ENGINE/path,signature) for path,signature in (
    ('renderer/backend/stages/DepthStage.cpp','bool DepthStage::PrepareMultiview('),
    ('renderer/backend/stages/MobileLightingStage.cpp','bool MobileLightingStage::PrepareMultiview('),
    ('renderer/backend/stages/MobileLightingStage.cpp','void MobileLightingStage::Init('),
    ('renderer/backend/stages/TonemapStage.cpp','bool TonemapStage::ApplyTonemapArray(')))
with tempfile.TemporaryDirectory(prefix='quest-program-check-') as directory:
    cpp=Path(directory)/'programs.cpp';cpp.write_text(SOURCE.replace('PRODUCTION',production))
    binary=Path(directory)/'programs'
    subprocess.run(['g++','-std=c++14','-O2','-DTDM_VR_OPENXR',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
