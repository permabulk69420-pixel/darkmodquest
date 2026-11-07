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
struct GLSLProgram{
 bool linked=true,model=true,projection=true,origin=true;uint64_t generation=1;int queries=0;
 bool EnsureLinked(){return linked;}uint64_t Generation()const{return generation;}
 int GetUniformLocation(const char *name){++queries;
  if(!std::strcmp(name,"u_modelViewMatrix[1]"))return model?1:-1;
  if(!std::strcmp(name,"u_projectionMatrix[1]"))return projection?2:-1;
  if(!std::strcmp(name,"u_viewOrigin[1]"))return origin?3:-1;assert(false);return -1;
 }
 const std::string &GetName()const{static std::string name="test";return name;}
};
struct Common{int warnings=0;template<class...Args>void Warning(const char *,Args...){++warnings;}}instance,*common=&instance;
struct DepthStage{
 GLSLProgram *multiviewShader=nullptr;uint64_t multiviewCheckedGeneration=0;bool multiviewReady=false;
 bool PrepareMultiview();
};
struct MobileLightingStage{
 struct Impl{GLSLProgram *lighting[8]={};uint64_t multiviewCheckedGeneration[4]={};bool multiviewReady[4]={};};
 Impl *impl;bool PrepareMultiview();
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
 std::puts("PASS: production multiview validation rejects mono/missing/failed programs, accepts two-eye uniforms, refreshes on reload and skips repeated GL queries");
}
'''
production='\n'.join(method(ENGINE/path,signature) for path,signature in (
    ('renderer/backend/stages/DepthStage.cpp','bool DepthStage::PrepareMultiview('),
    ('renderer/backend/stages/MobileLightingStage.cpp','bool MobileLightingStage::PrepareMultiview(')))
with tempfile.TemporaryDirectory(prefix='quest-program-check-') as directory:
    cpp=Path(directory)/'programs.cpp';cpp.write_text(SOURCE.replace('PRODUCTION',production))
    binary=Path(directory)/'programs'
    subprocess.run(['g++','-std=c++14','-O2',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
