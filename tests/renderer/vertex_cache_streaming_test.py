#!/usr/bin/env python3
"""Render real allocator-produced UI/scene handles across streaming frame stores.

Compiles production ActuallyAlloc, GpuBuffer, vertex/index binding and indexed
submission methods. GUI-like quads use that path; the reference uses independent
direct GL uploads.
"""
import ctypes as C
import os
from pathlib import Path
import re
import subprocess
import tempfile
from mobile_gles_test import GLES
from mobile_shadow_batch_test import SHELL, ENGINE
from shader_install_test import method

VS='''#version 320 es
precision highp float;
in vec3 attr_Position;
void main(){gl_Position=vec4(attr_Position,1.0);}'''
FS='''#version 320 es
precision highp float;
uniform vec4 u_color;out vec4 color;
void main(){color=u_color;}'''
HARNESS=r'''
void renderCheck(GLuint program,bool isolated,bool retained,int uploadMode){
 r_gpuBufferNonpersistentUpdateMode.value=uploadMode;
 constexpr unsigned VBytes=20*240,IBytes=20*32;
 auto &g=vertexCache.dynamicData;
 vertexCache.currentFrame=1;vertexCache.currentIndexBuffer=0;
 g.vertexBuffer.InitWriteFrameAhead(GL_ARRAY_BUFFER,VBytes,240,retained,isolated);
 g.indexBuffer.InitWriteFrameAhead(GL_ELEMENT_ARRAY_BUFFER,IBytes,16,false,isolated);
 vertexCache.currentVertexCacheSize=VBytes;vertexCache.currentIndexCacheSize=IBytes;
 GLuint vao[2],fbo[2],tex[2],referenceVbo,referenceIbo,staticIbo;
 glGenVertexArrays(2,vao);glGenFramebuffers(2,fbo);glGenTextures(2,tex);
 for(int i=0;i<2;++i){glBindTexture(GL_TEXTURE_2D,tex[i]);glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,96,96);glBindFramebuffer(GL_FRAMEBUFFER,fbo[i]);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex[i],0);require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"target creation");}
 alignas(16) idDrawVert world[4]={},moving[4]={},glyphs[16*4]={};
 auto quad=[](idDrawVert *v,float x,float y,float w,float h){
  const float xy[4][2]={{0,0},{1,0},{1,1},{0,1}};
  for(int i=0;i<4;++i){v[i].xyz[0]=x+w*xy[i][0];v[i].xyz[1]=y+h*xy[i][1];v[i].xyz[2]=0;}
 };
 quad(world,.2f,-.8f,.6f,1.6f);
 alignas(16) glIndex_t six[]={0,1,2,0,2,3};
 alignas(16) glIndex_t text[16*6];
 for(int q=0;q<16;++q)for(int k=0;k<6;++k)text[q*6+k]=4*q+six[k];
 glGenBuffers(1,&vertexCache.staticVertexBuffer);glBindBuffer(GL_ARRAY_BUFFER,vertexCache.staticVertexBuffer);glBufferData(GL_ARRAY_BUFFER,sizeof(world),world,GL_STATIC_DRAW);
 glGenBuffers(1,&staticIbo);vertexCache.staticIndexBuffer=staticIbo;
 glBindVertexArray(vao[0]);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,staticIbo);glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(six),six,GL_STATIC_DRAW);
 glGenBuffers(1,&referenceVbo);glGenBuffers(1,&referenceIbo);
 auto color=[&](float r,float green,float b){glUniform4f(glGetUniformLocation(program,"u_color"),r,green,b,1);};
 auto prepare=[&](int target){glBindVertexArray(vao[target]);glBindFramebuffer(GL_FRAMEBUFFER,fbo[target]);glViewport(0,0,96,96);glDisable(GL_DEPTH_TEST);glDisable(GL_STENCIL_TEST);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);glDisable(GL_DITHER);glColorMask(1,1,1,1);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glUseProgram(program);};
 auto reference=[&](const idDrawVert *v,unsigned count,const glIndex_t *indices,unsigned n){
  glBindBuffer(GL_ARRAY_BUFFER,referenceVbo);glBufferData(GL_ARRAY_BUFFER,count*sizeof(idDrawVert),v,GL_STREAM_DRAW);
  glEnableVertexAttribArray(0);glVertexAttribPointer(0,3,GL_FLOAT,0,sizeof(idDrawVert),nullptr);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,referenceIbo);glBufferData(GL_ELEMENT_ARRAY_BUFFER,n*sizeof(glIndex_t),indices,GL_STREAM_DRAW);
  glDrawElements(GL_TRIANGLES,n,GL_UNSIGNED_INT,nullptr);
 };
 for(int frame=0;frame<12;++frame){
  ClearGeoBufferSet(g);
  quad(moving,-.85f+float(frame%3)*.12f,.35f,.34f,.35f);
  for(int q=0;q<16;++q)quad(glyphs+4*q,-.9f+(q%4)*.22f,-.85f+(q/4)*.22f,.12f+float(frame%2)*.02f,.12f);
  const auto vh=vertexCache.ActuallyAlloc(g,moving,sizeof(moving),CACHE_VERTEX);
  const auto ih=vertexCache.ActuallyAlloc(g,six,sizeof(six),CACHE_INDEX);
  const auto tv=vertexCache.ActuallyAlloc(g,glyphs,sizeof(glyphs),CACHE_VERTEX);
  const auto ti=vertexCache.ActuallyAlloc(g,text,sizeof(text),CACHE_INDEX);
  // Upload/advance via the production end-of-frame helper. New frontend
  // writes use separate stores; the backend consumes the just-issued handles.
  SwitchFrameGeoBufferSet(g);++vertexCache.currentFrame;
  glBindVertexArray(vao[0]);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);vertexCache.currentIndexBuffer=0;
  auto engine=[&](vertCacheHandle_t v,vertCacheHandle_t i,unsigned count){
   vertexCache.VertexPosition(v);
   drawSurf_t surf={};surf.ambientCache=v;surf.indexCache=i;surf.numIndexes=count;
   RB_DrawElementsWithCounters(&surf,DCK_SURFACE_PASS);
  };
  prepare(0);color(.1f,.2f,.3f);
  engine(vertCacheHandle_t::Create(sizeof(world),0,0,true),vertCacheHandle_t::Create(sizeof(six),0,0,true),6);
  color(frame%2?1.f:.2f,.3f,frame%2?.2f:1.f);engine(vh,ih,6);
  color(.2f,frame%2?.6f:1.f,.2f);engine(tv,ti,16*6);
  std::vector<byte>actual(96*96*4),expected(actual.size());glReadPixels(0,0,96,96,GL_RGBA,GL_UNSIGNED_BYTE,actual.data());
  prepare(1);color(.1f,.2f,.3f);reference(world,4,six,6);
  color(frame%2?1.f:.2f,.3f,frame%2?.2f:1.f);reference(moving,4,six,6);
  color(.2f,frame%2?.6f:1.f,.2f);reference(glyphs,16*4,text,16*6);
  glReadPixels(0,0,96,96,GL_RGBA,GL_UNSIGNED_BYTE,expected.data());
  size_t bad=0;
  for(size_t p=0;p<actual.size();p+=4){bool mismatch=false;for(int c=0;c<4;++c)mismatch|=std::abs(int(actual[p+c])-int(expected[p+c]))>1;if(mismatch)++bad;}
  if(bad){
   char message[256];snprintf(message,sizeof(message),"allocator-produced geometry mismatch: isolated=%d history=%d upload=%d frame=%d; %zu corrupt pixels; vertex logical offset=%u",isolated,retained,uploadMode,frame,bad,vh.offset);throw std::runtime_error(message);}
  require(glGetError()==GL_NO_ERROR,"GL error in mixed scene/UI draws");
  const unsigned slot=(frame+1)%g.vertexBuffer.FrameCount();
  require(vh.offset==slot*VBytes&&tv.offset==slot*VBytes+240,"producer discarded logical frame address");
 }
 g.vertexBuffer.Destroy();g.indexBuffer.Destroy();glDeleteBuffers(1,&vertexCache.staticVertexBuffer);glDeleteBuffers(1,&staticIbo);glDeleteBuffers(1,&referenceVbo);glDeleteBuffers(1,&referenceIbo);glDeleteFramebuffers(2,fbo);glDeleteTextures(2,tex);glDeleteVertexArrays(2,vao);vertexCache.currentIndexBuffer=0;
}
extern "C" const char *validate(GLuint program){static std::string error;
 try{for(bool isolated:{false,true})for(bool retained:{false,true})for(int mode:{0,1})renderCheck(program,isolated,retained,mode);error.clear();}
 catch(const std::exception &e){error=e.what();}return error.c_str();
}
'''

def main():
    gl=GLES()
    try:
        program=gl.program(VS,FS)
        with tempfile.TemporaryDirectory(prefix='quest-allocator-render-') as directory:
            base=Path(directory)
            text=SHELL[:SHELL.index('struct Fixture{')]
            text=text.replace('struct Geo{GpuBuffer vertexBuffer,indexBuffer;};',r'''
const vertCacheHandle_t NO_CACHE={0,0,0,false};
constexpr int VERTEX_CACHE_ALIGN=240,INDEX_CACHE_ALIGN=16;
template<class T>T Min(T a,T b){return std::min(a,b);}
enum cacheType_t{CACHE_VERTEX,CACHE_INDEX};
struct Counter{int value=0;int Add(int n){value+=n;return value;}int GetValue()const{return value;}void SetValue(int n){value=n;}};
struct Geo{GpuBuffer vertexBuffer,indexBuffer;byte *mappedVertexBase=nullptr,*mappedIndexBase=nullptr;Counter vertexMemUsed,indexMemUsed;int allocations=0;};using geoBufferSet_t=Geo;
struct Copier{void MemcpyNT(void *dst,const void *src,int n){memcpy(dst,src,n);}}copier,*SIMDProcessor=&copier;
#define assert_16_byte_aligned(p) assert((uintptr_t(p)&15)==0)
''')
            text=text.replace(' Geo dynamicData;GLuint staticVertexBuffer',' int currentIndexCacheSize=0,currentVertexCacheSize=0,indexAllocCount=0,vertexAllocCount=0;\n vertCacheHandle_t ActuallyAlloc(geoBufferSet_t &,const void *,int,cacheType_t);\n int GetBaseVertex(){return basePointer;}\n void UnbindIndex(){glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);currentIndexBuffer=0;}\n Geo dynamicData;GLuint staticVertexBuffer')
            text=text.replace('unsigned count=6;bool cutout=false;', 'unsigned count=6;bool cutout=false;int numIndexes=0;struct{glIndex_t *indexes=nullptr;}*frontendGeo=nullptr;int dsFlags=0;struct Material{bool IsAtmosphericFog()const{return false;}}*material=nullptr;')
            text=text.replace('const int DCK_SHADOW=3;', 'using drawCallKind_t=int;const int DCK_SHADOW=3,DCK_SURFACE_PASS=1;\nstruct{bool GetBool()const{return false;}}vr_rendererDiagnostics;struct{int GetInteger()const{return 1;}}r_glCoreProfile;\nstruct{int fogSurfaceDraws=0,softParticleDraws=0;}questRenderCounters;\nconst int DSF_ATMOSPHERIC_FOG=1,DSF_SOFT_PARTICLE=2;const GLenum GL_INDEX_TYPE=GL_UNSIGNED_INT;\nvoid RB_PerfCounters(const drawSurf_t *,int,drawCallKind_t){}')
            handle=(ENGINE/'renderer/resources/Model.h').read_text().split('struct vertCacheHandle_t {',1)[1].split('\n};',1)[0]
            replacements={'CACHE_HANDLE':'struct vertCacheHandle_t {'+handle+'\n};'}
            for key,signature in [('VERTEX_POSITION','void idVertexCache::VertexPosition('),('INDEX_POSITION','void *idVertexCache::IndexPosition('),('BIND_INDEX','void idVertexCache::BindExternalIndexBuffer('),('READ_INDEX','const glIndex_t *idVertexCache::ReadSubmittedIndexes('),('SNAPSHOT_INDEX','void idVertexCache::SnapshotStaticIndexes(')]:replacements[key]=method(ENGINE/'renderer/VertexCache.cpp',signature)
            replacements['DRAW_BATCH']=method(ENGINE/'renderer/backend/stages/MobileLightingStage.cpp','void MobileLightingStage::Impl::DrawShadowBatch(')
            for key,value in replacements.items():text=text.replace(key,value)
            text+=method(ENGINE/'renderer/VertexCache.cpp','static void ClearGeoBufferSet(')+'\n'+method(ENGINE/'renderer/VertexCache.cpp','static void SwitchFrameGeoBufferSet(')+'\n'+method(ENGINE/'renderer/VertexCache.cpp','vertCacheHandle_t idVertexCache::ActuallyAlloc(')+'\n'+method(ENGINE/'renderer/backend/tr_render.cpp','void RB_DrawElementsWithCounters(')+'\n'+HARNESS
            production=(ENGINE/'renderer/backend/GpuBuffer.cpp').read_text()
            names=set(re.findall(r'\bqgl(\w+)',production+text))-{'BufferStorage','DrawElements'}
            text=text.replace('ALIASES','\n'.join(f'#define qgl{name} gl{name}' for name in sorted(names)))
            (base/'precompiled.h').write_text('#pragma once\n');cpp=base/'render.cpp';cpp.write_text(text)
            includes=Path('/tmp/renderer-tools/root/usr/include');extra=['-I',str(includes)] if includes.exists() else []
            search=[flag for p in os.environ.get('LD_LIBRARY_PATH','').split(':') if p for flag in ('-L',p)]
            library=base/'render.so'
            subprocess.run(['g++','-std=c++14','-O2','-shared','-fPIC','-I',str(base),'-I',str(ENGINE),*extra,str(cpp),*search,'-l:libGLESv2.so.2','-o',str(library)],check=True)
            native=C.CDLL(str(library));native.validate.argtypes=[C.c_uint];native.validate.restype=C.c_char_p
            error=native.validate(program)
            if error:raise AssertionError(error.decode())
            print('PASS: production allocation -> frame upload -> vertex/index binding -> draw renders moving geometry and 16 GUI glyph quads with matching expected scene/UI pixels (one RGBA8 rounding unit) against independent GL uploads over 12 frames; shared/isolated, 3/4 vertex stores and both upload methods')
    finally:gl.close()
if __name__=='__main__':main()
