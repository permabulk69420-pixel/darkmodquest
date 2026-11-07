#!/usr/bin/env python3
"""Compile the shipping lighting batch rules with small renderer fixtures."""
from pathlib import Path
import subprocess
import tempfile
from shader_install_test import method

root=Path(__file__).resolve().parents[2]
stage=root/'Q3E/src/main/jni/darkmod/renderer/backend/stages/MobileLightingStage.cpp'
functions='\n'.join(method(stage,s) for s in ('bool HasNegativeColor(', 'auto GeometryKey(',
    'bool Compatible(', 'auto LightTextureKey(', 'bool CanAppend('))
source=r'''
#include <cassert>
#include <cstring>
#include <tuple>
#include <cstdint>
#include <cstdio>
const int MAX_BATCH_LIGHTS=8;
struct Image{};
struct Handle{bool isStatic=true;unsigned frameNumber=0,offset=0,size=96;};
struct drawSurf_t{void *space=nullptr,*material=nullptr;Handle ambientCache,indexCache;int numIndexes=6,numVerts=4,dsFlags=0;};
struct Interaction{
 Image *bumpImage=nullptr,*diffuseImage=nullptr,*specularImage=nullptr,*parallaxImage=nullptr,*lightImage=nullptr,*lightFalloffImage=nullptr;
 int vertexColor=0;float diffuseColor[4]={0.1,0.1,0.1,1},specularColor[4]={0.1,0.1,0.1,1};
 float bumpMatrix[2][4]={},diffuseMatrix[2][4]={},specularMatrix[2][4]={},parallaxMatrix[2][4]={};
 struct{float values[8]={};}parallax;const drawSurf_t *surf=nullptr;
};
struct Material{bool cubic=false;Image *ambientDiffuse=nullptr,*ambientSpecular=nullptr;
 bool IsCubicLight()const{return cubic;}Image *LightAmbientDiffuse()const{return ambientDiffuse;}Image *LightAmbientSpecular()const{return ambientSpecular;}};
struct Light{Material *lightShader;};
struct Packet{Interaction inter;const Light *light;bool local=false,translucent=false;const drawSurf_t *geometry;int lightIndex=0;};
struct Batch{const Packet *lights[MAX_BATCH_LIGHTS]={};int count=0;};
FUNCTIONS
int main(){
 Image images[4];Material materials[9];Light lights[9];Packet packets[9];drawSurf_t mesh;
 for(int i=0;i<9;++i){lights[i].lightShader=&materials[i];packets[i].light=&lights[i];packets[i].geometry=&mesh;
  packets[i].inter.surf=&mesh;packets[i].inter.lightImage=&images[i%2];packets[i].inter.lightFalloffImage=&images[2];}
 Batch batch;
 for(int i=0;i<8;++i){assert(CanAppend(batch,packets[i]));batch.lights[batch.count++]=&packets[i];}
 assert(batch.count==8&&!CanAppend(batch,packets[8]));
 batch.count=2;packets[8].inter.lightImage=&images[3];assert(!CanAppend(batch,packets[8]));
 packets[8].inter.lightImage=&images[0];assert(CanAppend(batch,packets[8]));
 packets[8].inter.diffuseColor[0]=-0.1f;assert(!CanAppend(batch,packets[8]));
 packets[8].inter.diffuseColor[0]=0.1f;packets[8].inter.bumpMatrix[0][0]=2;assert(!CanAppend(batch,packets[8]));
 packets[8].inter.bumpMatrix[0][0]=0;packets[8].translucent=true;assert(!CanAppend(batch,packets[8]));
 std::puts("PASS: shipping batch rules accept eight lights/two texture sets and reject overflow, third tuple, subtractive, material and ordering conflicts");
}
'''.replace('FUNCTIONS',functions)
with tempfile.TemporaryDirectory(prefix='quest-batch-test-') as directory:
    cpp=Path(directory)/'batch.cpp';binary=Path(directory)/'batch'
    cpp.write_text(source)
    subprocess.run(['g++','-std=c++14','-O2',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
