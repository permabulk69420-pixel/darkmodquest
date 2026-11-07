#!/usr/bin/env python3
"""Exercise shipping mobile submission code with recorded GL calls and mesh fixtures."""
from pathlib import Path
import subprocess
import tempfile
from shader_install_test import method

ROOT = Path(__file__).resolve().parents[2]
ENGINE = ROOT / 'Q3E/src/main/jni/darkmod'

UNIFORMS = r'''
#pragma once
#include <cassert>
#include <cstring>
#include <vector>
using GLfloat=float;using GLint=int;const int GL_FALSE=0;
struct GLSLProgram{};
struct GLSLUniformBase {
 int paramLocation;
 GLSLUniformBase(GLSLProgram *,const char *name):paramLocation(std::strcmp(name,"missing")?7:-1){}
 bool IsPresent()const{return paramLocation>=0;}
};
int uploads=0,components=0,elements=0;std::vector<float> uploaded;
template<class T>void record(int n,int count,const T *values){
 ++uploads;components=n;elements=count;uploaded.assign(values,values+n*count);
}
#define RECORDER(N) \
 void qglUniform##N##iv(int,int count,const int *p){record(N,count,p);} \
 void qglUniform##N##fv(int,int count,const float *p){record(N,count,p);}
RECORDER(1) RECORDER(2) RECORDER(3) RECORDER(4)
void qglUniformMatrix4fv(int,int count,int transpose,const float *p){assert(!transpose);record(16,count,p);}
'''

SOURCE = r'''
#include "renderer/backend/MobileUniform.h"
#include <cstdio>
#include <cstdlib>
void uniformTests(){
 GLSLProgram program;
 MobileUniform_int flag(&program,"flag");flag.Set(true);assert(uploads==1&&uploaded[0]==1);
 for(int i=0;i<1000;++i)flag.Set(true);
 assert(uploads==1);flag.Set(false);assert(uploads==2&&uploaded[0]==0);
 enum Mode{on=1};flag.Set(on);assert(uploads==3&&uploaded[0]==1);
 MobileUniform_sampler sampler(&program,"sampler");sampler.Set(4);sampler.Set(4);assert(uploads==4);
 MobileUniform_ivec3 steps(&program,"steps");steps.Set(8,4,2);steps.Set(8,4,2);
 assert(uploads==5&&components==3&&uploaded[2]==2);
 MobileUniform_vec4 colors(&program,"colors");float values[8]={1,2,3,4,5,6,7,8};
 colors.SetArray(2,values);assert(uploads==6&&elements==2);
 colors.SetArray(2,values);assert(uploads==6);
 values[7]=9;colors.SetArray(2,values);assert(uploads==7&&uploaded[7]==9);
 colors.SetArray(1,values);assert(uploads==8&&elements==1);
 colors.SetArray(2,values);assert(uploads==9&&elements==2);
 struct Vec3{float values[3];const float *ToFloatPtr()const{return values;}};
 MobileUniform_vec3 origin(&program,"origin");Vec3 eye{{.1f,.2f,.3f}};
 origin.Set(eye);origin.Set(eye);assert(uploads==10&&components==3&&uploaded[2]==.3f);
 MobileUniform_mat4 matrix(&program,"matrix");float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
 matrix.Set(identity);matrix.Set(identity);assert(uploads==11&&components==16&&uploaded[15]==1);
 MobileUniform_float missing(&program,"missing");missing.Set(2);assert(uploads==11);
 // A reloaded program's new group must upload even if its values match the old group.
 MobileUniform_mat4 reloaded(&program,"matrix");reloaded.Set(identity);assert(uploads==12);
 std::puts("PASS: cached uniform uploads preserve changed values/counts and reset on program reload; 1000 identical updates issue one GL call");
}

enum{TT_2D,TT_CUBIC,IT_ASSET};
struct Cvar{bool enabled=false;bool GetBool()const{return enabled;}}r_showPrimitives,com_developer;
struct View{bool gem=false;bool IsLightGem()const{return gem;}};
struct tmu_t{unsigned current2DMap=0,currentCubeMap=0;};
struct{struct{tmu_t tmu[16];int currenttmu=0;}glState;
 struct{int textureLoadTime=0,textureLoads=0;}pc;View *viewDef=nullptr;int frameCount=17;}backEnd;
int selections=0,bindings=0;
void GL_SelectTexture(int unit){if(backEnd.glState.currenttmu!=unit){++selections;backEnd.glState.currenttmu=unit;}}
void qglBindTexture(int,unsigned){++bindings;}
const int GL_TEXTURE_2D=1,GL_TEXTURE_CUBE_MAP=2;
int Sys_Milliseconds(){return 0;}
struct idImage{
 static const unsigned TEXTURE_NOT_LOADED=~0u;
 unsigned texnum=TEXTURE_NOT_LOADED;int type=TT_2D,bindCount=0,frameUsed=0;
 int GetType()const{return IT_ASSET;}idImage *AsAsset(){return this;}
 void ActuallyLoadImage(){texnum=99;}
 void Bind();bool IsBound(int unit)const;void BindToUnit(int unit);
};
struct{idImage *whiteImage;}images,*globalImages=&images;
BIND_FUNCTIONS
void textureTests(){
 idImage white;white.texnum=1;images.whiteImage=&white;
 idImage diffuse,cube;diffuse.texnum=2;cube.texnum=3;cube.type=TT_CUBIC;
 diffuse.BindToUnit(1);cube.BindToUnit(2);assert(selections==2&&bindings==2);
 for(int i=0;i<1000;++i){diffuse.BindToUnit(1);cube.BindToUnit(2);}
 assert(selections==2&&bindings==2&&diffuse.bindCount==0);
 View view;backEnd.viewDef=&view;r_showPrimitives.enabled=true;
 diffuse.BindToUnit(1);assert(diffuse.bindCount==1&&diffuse.frameUsed==17);
 view.gem=true;diffuse.BindToUnit(1);assert(diffuse.bindCount==1);
 com_developer.enabled=true;diffuse.BindToUnit(1);assert(diffuse.bindCount==2);
 // Cached units use their own target; a 2D binding cannot satisfy a cubemap.
 idImage other;other.texnum=2;other.type=TT_CUBIC;other.BindToUnit(1);
 assert(bindings==3&&selections==3&&backEnd.glState.tmu[1].currentCubeMap==2);
 // A lost texture cache entry must cause a real bind, and unloaded assets must load.
 backEnd.glState.tmu[1].current2DMap=0;diffuse.BindToUnit(1);assert(bindings==4);
 idImage deferred;deferred.BindToUnit(4);assert(deferred.texnum==99&&backEnd.pc.textureLoads==1&&bindings==5);
 std::puts("PASS: shipping texture binding skips 2000 redundant unit selections and preserves targets, statistics and deferred loading");
}

struct Handle{int id=0,frame=1;bool IsValid()const{return id!=0;}
 bool operator==(const Handle &rhs)const{return id==rhs.id&&frame==rhs.frame;}};
struct srfTriangles_t{
 const srfTriangles_t *ambientSurface=nullptr;void *verts=nullptr;
 int numVerts=4,numIndexes=6,bounds=0;unsigned shadowIndexRevision=0;
 Handle ambientCache,indexCache,shadowCache;
};
struct viewEntity_s{};struct idScreenRect{};
struct idMaterial{bool fog=false,blend=false;bool IsFogLight()const{return fog;}bool IsBlendLight()const{return blend;}};
struct drawSurf_s{
 const drawSurf_s *mobileCanonical=nullptr;const srfTriangles_t *frontendGeo=nullptr;
 int geometryBounds=0,numVerts=0,numIndexes=0;unsigned shadowIndexRevision=0;
 Handle ambientCache,indexCache,shadowCache;
 const viewEntity_s *space=nullptr;const idMaterial *material=nullptr;
 const float *shaderRegisters=nullptr;drawSurf_s *nextOnLight=nullptr;int dsFlags=0;
 COPY_GEO
};
using drawSurf_t=drawSurf_s;
bool mobileActive=true;bool R_UseMobileLighting(){return mobileActive;}
struct{bool CacheIsCurrent(const Handle &h)const{return h.frame==1;}}vertexCache;
struct LightDef{const idMaterial *lightShader;};
std::vector<void *> allocations;
void *R_FrameAlloc(size_t size){void *p=std::calloc(1,size);allocations.push_back(p);return p;}
float shaderRegisters[1]={1};
drawSurf_t *R_PrepareLightSurf(const srfTriangles_t *tri,const viewEntity_s *space,const idMaterial *material,
 const idScreenRect &,bool inside,bool block){
 auto *surf=static_cast<drawSurf_t *>(R_FrameAlloc(sizeof(drawSurf_t)));surf->CopyGeo(tri);
 surf->space=space;surf->material=material;surf->shaderRegisters=shaderRegisters;
 surf->dsFlags=int(inside)|int(block)*2;return surf;
}
struct idInteraction{
 enum linkLocation_t{INTERACTION_TRANSLUCENT,INTERACTION_LOCAL,INTERACTION_GLOBAL,SHADOW_LOCAL,SHADOW_GLOBAL,MAX_LOCATIONS};
 LightDef *lightDef;drawSurf_t *surfsToLink[MAX_LOCATIONS]={};
 void PrepareLightSurf(linkLocation_t,const srfTriangles_t *,const viewEntity_s *,const idMaterial *,const idScreenRect &,bool,bool);
};
PREPARE_SURFACE
void meshTests(){
 idMaterial material,lightMaterial;LightDef light{&lightMaterial};idInteraction interaction;interaction.lightDef=&light;
 viewEntity_s space;idScreenRect scissor;int vertices;
 srfTriangles_t source;source.verts=&vertices;source.ambientCache.id=1;source.indexCache.id=10;source.numIndexes=12;
 srfTriangles_t clipped=source;clipped.ambientSurface=&source;clipped.indexCache.id=20;clipped.numIndexes=6;
 auto prepare=[&](idInteraction::linkLocation_t link=idInteraction::INTERACTION_GLOBAL){
  interaction.PrepareLightSurf(link,&clipped,&space,&material,scissor,true,true);return interaction.surfsToLink[link];
 };
 auto *first=prepare();assert(first->numIndexes==6&&first->indexCache.id==20&&first->mobileCanonical);
 assert(first->mobileCanonical->numIndexes==12&&first->mobileCanonical->indexCache.id==10);
 assert(first->mobileCanonical->space==&space&&first->mobileCanonical->material==&material);
 assert(first->mobileCanonical->shaderRegisters==shaderRegisters&&first->mobileCanonical->dsFlags==3);
 // Two original surfaces may use the exact same vertices but different triangles.
 srfTriangles_t other=source;other.indexCache.id=11;other.numIndexes=18;
 clipped.ambientSurface=&other;auto *second=prepare();
 assert(second->mobileCanonical->indexCache.id==11&&second->mobileCanonical->numIndexes==18);
 assert(first->mobileCanonical->indexCache.id==10&&second->indexCache.id==20);
 other.indexCache.frame=0;assert(!prepare()->mobileCanonical);other.indexCache.frame=1;
 other.indexCache.id=0;assert(!prepare()->mobileCanonical);other.indexCache.id=11;
 other.verts=nullptr;assert(!prepare()->mobileCanonical);other.verts=&vertices;
 other.ambientCache.id=2;assert(!prepare()->mobileCanonical);other.ambientCache.id=1;
 assert(!prepare(idInteraction::SHADOW_GLOBAL)->mobileCanonical);
 lightMaterial.fog=true;assert(!prepare()->mobileCanonical);lightMaterial.fog=false;
 lightMaterial.blend=true;assert(!prepare()->mobileCanonical);lightMaterial.blend=false;
 mobileActive=false;assert(!prepare()->mobileCanonical);mobileActive=true;
 for(void *p:allocations)std::free(p);
 std::puts("PASS: shipping interaction preparation preserves clipped geometry and exact source mapping, including shared vertices and unsafe-cache fallbacks");
}
int main(){uniformTests();textureTests();meshTests();}
'''

source = SOURCE.replace('BIND_FUNCTIONS', '\n'.join(
    method(ENGINE / 'renderer/resources/Image_load.cpp', signature) for signature in (
        'void idImage::Bind()', 'bool idImage::IsBound(', 'void idImage::BindToUnit(')))
source = source.replace('COPY_GEO', method(ENGINE / 'renderer/tr_local.h', 'void CopyGeo('))
source = source.replace('PREPARE_SURFACE', method(
    ENGINE / 'renderer/frontend/Interaction.cpp', 'void idInteraction::PrepareLightSurf('))
with tempfile.TemporaryDirectory(prefix='quest-submission-test-') as directory:
    base = Path(directory)
    stub = base / 'renderer/backend/GLSLUniforms.h'
    stub.parent.mkdir(parents=True)
    stub.write_text(UNIFORMS)
    cpp = base / 'submission.cpp'
    cpp.write_text(source)
    binary = base / 'submission'
    subprocess.run(['g++', '-std=c++14', '-O2', '-I', str(base), '-I', str(ENGINE),
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
