#!/usr/bin/env python3
"""Compile the real GLES declarations and core loader, without an ARB extension."""
from pathlib import Path
import re
import subprocess
import tempfile
from shader_install_test import method

ENGINE=Path(__file__).resolve().parents[2]/'Q3E/src/main/jni/darkmod'
loader=method(ENGINE/'renderer/backend/qgl/glad.c','static void load_GL_VERSION_3_0(')
assignments=re.findall(r'\b(glad_gl\w+)\s*=\s*\((PFN\w+)\)',loader)
definitions='\n'.join(f'{kind} {name}=nullptr;' for name,kind in assignments)
source=r'''
#include "renderer/backend/qgl/qgl.h"
#include <cassert>
#include <cstring>
#include <cstdio>
int GLAD_GL_VERSION_3_0=0;
DEFINITIONS
int loads=0,discards=0;
void discard(GLenum target,GLsizei count,const GLenum *attachments){
 assert(target==GL_DRAW_FRAMEBUFFER&&count==2&&attachments[0]==GL_DEPTH_ATTACHMENT&&attachments[1]==GL_STENCIL_ATTACHMENT);++discards;
}
void unused(){}
void *lookup(const char *name){++loads;return !strcmp(name,"glInvalidateFramebuffer")?(void *)discard:(void *)unused;}
#define load(name) _load(name)
CORE_LOADER
int main(){
 load_GL_VERSION_3_0(lookup);assert(loads==0&&!qglInvalidateFramebuffer);
 GLAD_GL_VERSION_3_0=1;load_GL_VERSION_3_0(lookup);
 assert(qglInvalidateFramebuffer&&qglInvalidateSubFramebuffer);
 const GLenum attachments[]={GL_DEPTH_ATTACHMENT,GL_STENCIL_ATTACHMENT};
 qglInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER,2,attachments);assert(discards==1);
 std::puts("PASS: production GLES headers and core loader expose/load framebuffer invalidation without desktop ARB extension detection");
}
'''.replace('DEFINITIONS',definitions).replace('CORE_LOADER',loader)
with tempfile.TemporaryDirectory(prefix='quest-gles-loader-') as directory:
    cpp=Path(directory)/'loader.cpp';cpp.write_text(source);binary=Path(directory)/'loader'
    subprocess.run(['g++','-std=c++14','-O2','-D_GLES','-D__ANDROID__','-I',str(ENGINE),str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
