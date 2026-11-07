#!/usr/bin/env python3
"""Check production VRS probe GL state/readback on GLES, without Quest hardware.

The draw-state test supplies a constant encoded result because Mesa lacks the
vendor density interface. The real embedded shader is separately compiled by
the production entry; an unsupported shader must report unverified, never 2x2.
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

SOURCE=ENGINE/'renderer/backend/MobileVrsProbe.cpp'
HARNESS=r'''
#include "renderer/backend/MobileVrsProbe.cpp"
#include <string>
#include <vector>
static std::vector<unsigned> requested;
void recordRate(unsigned rate){requested.push_back(rate);}
void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
extern "C" const char *validate_probe(GLuint fixtureProgram){
 static std::string error;
 try{
  GLuint fb[2],rb[2],vao=0,packBuffer=0;glGenFramebuffers(2,fb);glGenRenderbuffers(2,rb);
  for(int i=0;i<2;++i){glBindFramebuffer(GL_FRAMEBUFFER,fb[i]);glBindRenderbuffer(GL_RENDERBUFFER,rb[i]);
   glRenderbufferStorage(GL_RENDERBUFFER,GL_RGBA8,16,16);glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,rb[i]);}
  glBindFramebuffer(GL_READ_FRAMEBUFFER,fb[0]);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,fb[1]);glBindRenderbuffer(GL_RENDERBUFFER,rb[0]);
  glGenVertexArrays(1,&vao);glBindVertexArray(vao);glUseProgram(0);
  glGenBuffers(1,&packBuffer);glBindBuffer(GL_PIXEL_PACK_BUFFER,packBuffer);glBufferData(GL_PIXEL_PACK_BUFFER,1024,nullptr,GL_STATIC_DRAW);
  glPixelStorei(GL_PACK_ALIGNMENT,8);glPixelStorei(GL_PACK_ROW_LENGTH,7);glPixelStorei(GL_PACK_SKIP_PIXELS,2);glPixelStorei(GL_PACK_SKIP_ROWS,3);
  glViewport(7,9,123,141);glScissor(3,5,43,57);glColorMask(false,true,false,true);
  const GLenum caps[]={GL_DEPTH_TEST,GL_STENCIL_TEST,GL_SCISSOR_TEST,GL_BLEND,GL_CULL_FACE,GL_DITHER,GL_RASTERIZER_DISCARD};
  for(int i=0;i<7;++i){if(i%2)glDisable(caps[i]);else glEnable(caps[i]);}
  auto check=[&](){
   GLint value,box[4];GLboolean mask[4];
   glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&value);require(value==GLint(fb[0]),"probe leaked read framebuffer");
   glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&value);require(value==GLint(fb[1]),"probe leaked draw framebuffer");
   glGetIntegerv(GL_RENDERBUFFER_BINDING,&value);require(value==GLint(rb[0]),"probe leaked renderbuffer");
   glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&value);require(value==GLint(vao),"probe leaked VAO");
   glGetIntegerv(GL_CURRENT_PROGRAM,&value);require(value==0,"probe leaked GL program");
   glGetIntegerv(GL_VIEWPORT,box);require(box[0]==7&&box[1]==9&&box[2]==123&&box[3]==141,"probe leaked viewport");
   glGetIntegerv(GL_SCISSOR_BOX,box);require(box[0]==3&&box[1]==5&&box[2]==43&&box[3]==57,"probe leaked scissor");
   glGetBooleanv(GL_COLOR_WRITEMASK,mask);require(!mask[0]&&mask[1]&&!mask[2]&&mask[3],"probe leaked colour mask");
   glGetIntegerv(GL_PACK_ALIGNMENT,&value);require(value==8,"probe leaked pack alignment");
   glGetIntegerv(GL_PACK_ROW_LENGTH,&value);require(value==7,"probe leaked pack row length");
   glGetIntegerv(GL_PACK_SKIP_PIXELS,&value);require(value==2,"probe leaked pack skip pixels");
   glGetIntegerv(GL_PACK_SKIP_ROWS,&value);require(value==3,"probe leaked pack skip rows");
   glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&value);require(value==GLint(packBuffer),"probe leaked pixel pack buffer");
   for(int i=0;i<7;++i)require(bool(glIsEnabled(caps[i]))==(i%2==0),"probe leaked GL enable state");
  };
  const auto result=DrawProbe(fixtureProgram,recordRate);
  require(result.width==2&&result.height==2,"probe failed to decode GPU readback");
  require(requested==std::vector<unsigned>({0x96A9,0x96A6}),"probe did not request coarse then restore full rate");check();
  const auto native=R_QuestProbeVrs(recordRate);check();
  // A driver with this GLSL interface but no actual vendor rate command can
  // report 1x1. A driver without the interface must report unverified (zero).
  require((native.width==0&&native.height==0)||(native.width==1&&native.height==1),"probe invented hardware coarse shading support");
  glBindBuffer(GL_PIXEL_PACK_BUFFER,0);glDeleteBuffers(1,&packBuffer);glBindVertexArray(0);glDeleteVertexArrays(1,&vao);
  glBindFramebuffer(GL_FRAMEBUFFER,0);glBindRenderbuffer(GL_RENDERBUFFER,0);glDeleteRenderbuffers(2,rb);glDeleteFramebuffers(2,fb);
  require(glGetError()==GL_NO_ERROR,"GL error in production VRS probe");return nullptr;
 }catch(const std::exception &e){error=e.what();return error.c_str();}
}
'''

class ProbeTests(unittest.TestCase):
    def test_production_probe_state_and_unsupported_shader(self):
        g=GLES()
        try:
            vertex=re.search(r'probeVertex=R"GLSL\((.*?)\)GLSL"',SOURCE.read_text(),re.S).group(1)
            program=g.program(vertex,'#version 320 es\nprecision highp float;out vec4 color;void main(){color=vec4(.5,.5,0.,1.);}')
            with tempfile.TemporaryDirectory(prefix='quest-vrs-probe-') as directory:
                base=Path(directory)
                headers=Path(os.environ.get('ANDROID_NDK_HOME','/tmp/renderer-tools/android-ndk-r23c'))/'toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include'
                if not headers.is_dir():headers=Path('/usr/include')
                for name in ('GLES3/gl32.h','GLES3/gl3platform.h','KHR/khrplatform.h'):
                    dest=base/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(headers/name,dest)
                names=set(re.findall(r'\bqgl(\w+)',SOURCE.read_text()))
                aliases='\n'.join(f'#define qgl{name} gl{name}' for name in sorted(names))
                for name,text in {'precompiled.h':'','renderer/tr_local.h':MOCKS.replace('GL_ALIASES',aliases)}.items():
                    dest=base/name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_text(text)
                cpp=base/'probe.cpp';cpp.write_text(HARNESS);library=base/'probe.so'
                search=[flag for path in os.environ.get('LD_LIBRARY_PATH','').split(':') if path for flag in ('-L',path)]
                subprocess.run(['g++','-std=c++14','-O2','-shared','-fPIC','-DTDM_VR_OPENXR','-I',str(base),'-I',str(ENGINE),str(cpp),
                                *search,'-l:libGLESv2.so.2','-o',str(library)],check=True)
                native=C.CDLL(str(library));native.validate_probe.argtypes=[C.c_uint];native.validate_probe.restype=C.c_char_p
                result=native.validate_probe(program);self.assertIsNone(result,result.decode() if result else '')
                print('PASS: production VRS probe decodes GPU readback, restores GL/FBO/program/VAO/PBO state and reports unsupported vendor shaders as unverified')
        finally:g.close()

if __name__=='__main__':unittest.main(verbosity=2)
