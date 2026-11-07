#!/usr/bin/env python3
"""Headless GLES shader/lighting regression tests (EGL, no game data or headset).

Requires a GLES 3.2 driver and EGL. Use EGL_PLATFORM=surfaceless on Mesa.
This renders tiny test targets; it does not implement a desktop VR/preview path.
"""
import ctypes as C
import ctypes.util
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from fog_policy_test import compiled_surface_policy

ROOT = Path(__file__).resolve().parents[2]
SHADERS = Path(os.environ.get('TDM_SHADER_SOURCE_DIR', ROOT / 'Q3E/src/main/jni/darkmod/glslprogs'))

def source(path):
    seen = set()
    def expand(name):
        if name in seen:
            return ''
        seen.add(name)
        text = (SHADERS / name).read_text()
        text = re.sub(r'^\s*#pragma tdm_include "([^"]+)".*$', lambda m: expand(m[1]), text, flags=re.M)
        return re.sub(r'^\s*#pragma tdm_define .*$', '', text, flags=re.M)
    return expand(path)

class GLES:
    def __init__(self):
        self.egl = C.CDLL(ctypes.util.find_library('EGL') or 'libEGL.so.1')
        def egl(name, result, args):
            f = getattr(self.egl, name); f.restype = result; f.argtypes = args
            return f
        ptr, I = C.c_void_p, C.c_int
        self.proc = egl('eglGetProcAddress', ptr, [C.c_char_p])
        get_display = C.CFUNCTYPE(ptr, C.c_uint, ptr, C.POINTER(I))(self.proc(b'eglGetPlatformDisplayEXT'))
        self.display = get_display(0x31DD, None, None)  # EGL_PLATFORM_SURFACELESS_MESA
        major, minor = I(), I()
        assert egl('eglInitialize', I, [ptr, C.POINTER(I), C.POINTER(I)])(self.display, C.byref(major), C.byref(minor))
        assert egl('eglBindAPI', I, [C.c_uint])(0x30A0)
        attrs = (I*13)(0x3033,1,0x3040,0x40,0x3024,8,0x3023,8,0x3022,8,0x3021,8,0x3038)
        config, count = ptr(), I()
        assert egl('eglChooseConfig',I,[ptr,C.POINTER(I),C.POINTER(ptr),I,C.POINTER(I)])(self.display,attrs,C.byref(config),1,C.byref(count)) and count.value
        context_attrs = (I*3)(0x3098,3,0x3038)
        self.context = egl('eglCreateContext',ptr,[ptr,ptr,ptr,C.POINTER(I)])(self.display,config,None,context_attrs)
        assert self.context
        assert egl('eglMakeCurrent',I,[ptr,ptr,ptr,ptr])(self.display,None,None,self.context)
        self.terminate = egl('eglTerminate',I,[ptr])
        self.destroy = egl('eglDestroyContext',I,[ptr,ptr])
        self.make_current = egl('eglMakeCurrent',I,[ptr,ptr,ptr,ptr])
        self.cache = {}
    def fn(self, name, ret, *args):
        if name not in self.cache:
            address = self.proc(name.encode())
            assert address, name
            self.cache[name] = C.CFUNCTYPE(ret,*args)(address)
        return self.cache[name]
    def call(self, name, *values):
        args = tuple(C.c_float if isinstance(v,float) else C.c_void_p if isinstance(v,(C.Array,C.c_void_p)) or v is None else C.c_uint for v in values)
        return self.fn(name,None,*args)(*values)
    def gen(self, name):
        value = C.c_uint()
        self.fn(name,None,C.c_int,C.POINTER(C.c_uint))(1,C.byref(value))
        return value.value
    def error(self):
        return self.fn('glGetError',C.c_uint)()
    def program(self, vs, fs):
        program = self.fn('glCreateProgram',C.c_uint)()
        for kind, text in ((0x8B31,vs),(0x8B30,fs)):
            shader = self.fn('glCreateShader',C.c_uint,C.c_uint)(kind)
            ptr = C.c_char_p(text.encode())
            self.fn('glShaderSource',None,C.c_uint,C.c_int,C.POINTER(C.c_char_p),C.c_void_p)(shader,1,C.byref(ptr),None)
            self.call('glCompileShader',shader)
            ok = C.c_int()
            self.fn('glGetShaderiv',None,C.c_uint,C.c_uint,C.POINTER(C.c_int))(shader,0x8B81,C.byref(ok))
            if not ok.value:
                log = C.create_string_buffer(65536)
                self.fn('glGetShaderInfoLog',None,C.c_uint,C.c_int,C.c_void_p,C.c_void_p)(shader,len(log),None,log)
                raise AssertionError(log.value.decode())
            self.call('glAttachShader',program,shader)
            self.call('glDeleteShader',shader)
        for index,name in ((0,'attr_Position'),(8,'attr_TexCoord'),(2,'attr_Normal'),(3,'attr_Color'),(9,'attr_Tangent'),(10,'attr_Bitangent')):
            self.fn('glBindAttribLocation',None,C.c_uint,C.c_uint,C.c_char_p)(program,index,name.encode())
        self.call('glLinkProgram',program)
        ok=C.c_int()
        self.fn('glGetProgramiv',None,C.c_uint,C.c_uint,C.POINTER(C.c_int))(program,0x8B82,C.byref(ok))
        if not ok.value:
            log=C.create_string_buffer(65536)
            self.fn('glGetProgramInfoLog',None,C.c_uint,C.c_int,C.c_void_p,C.c_void_p)(program,len(log),None,log)
            raise AssertionError(log.value.decode())
        return program
    def uniform(self, program, name, value, kind='f'):
        loc=self.fn('glGetUniformLocation',C.c_int,C.c_uint,C.c_char_p)(program,name.encode())
        if loc < 0: return
        if not isinstance(value,(list,tuple)): value=[value]
        if kind=='m':
            data=(C.c_float*len(value))(*value)
            self.fn('glUniformMatrix4fv',None,C.c_int,C.c_int,C.c_ubyte,C.c_void_p)(loc,len(value)//16,0,data)
        elif kind=='i':
            self.fn('glUniform1i',None,C.c_int,C.c_int)(loc,value[0])
        else:
            self.fn('glUniform%df'%len(value),None,C.c_int,*([C.c_float]*len(value)))(loc,*value)
    def texture(self, unit, rgba=(255,255,255,255), depth=False, cube=False, size=1):
        texture=self.gen('glGenTextures')
        self.call('glActiveTexture',0x84C0+unit)
        target=0x8513 if cube else 0x0DE1
        self.call('glBindTexture',target,texture)
        for enum in (0x2800,0x2801): self.call('glTexParameteri',target,enum,0x2600)
        for enum in (0x2802,0x2803): self.call('glTexParameteri',target,enum,0x812F)
        data=None if depth else (C.c_ubyte*4)(*rgba)
        for face in range(6 if cube else 1):
            self.fn('glTexImage2D',None,C.c_uint,C.c_int,C.c_int,C.c_int,C.c_int,C.c_int,C.c_uint,C.c_uint,C.c_void_p)(
                0x8515+face if cube else target,0,0x81A6 if depth else 0x8058,size,size,0,0x1902 if depth else 0x1908,0x1405 if depth else 0x1401,data)
        return texture
    def target(self, width=16, height=16, depth_texture=None, depth_buffer=False):
        fbo=self.gen('glGenFramebuffers'); self.call('glBindFramebuffer',0x8D40,fbo)
        tex=depth_texture or self.texture(0,size=1)
        if not depth_texture:
            self.fn('glTexImage2D',None,C.c_uint,C.c_int,C.c_int,C.c_int,C.c_int,C.c_int,C.c_uint,C.c_uint,C.c_void_p)(0x0DE1,0,0x8058,width,height,0,0x1908,0x1401,None)
        self.call('glFramebufferTexture2D',0x8D40,0x8D00 if depth_texture else 0x8CE0,0x0DE1,tex,0)
        if depth_buffer:
            rb=self.gen('glGenRenderbuffers');self.call('glBindRenderbuffer',0x8D41,rb)
            self.call('glRenderbufferStorage',0x8D41,0x88F0,width,height)
            self.call('glFramebufferRenderbuffer',0x8D40,0x821A,0x8D41,rb)
        if depth_texture:
            none=(C.c_uint*1)(0); self.call('glDrawBuffers',1,none); self.call('glReadBuffer',0)
        assert self.fn('glCheckFramebufferStatus',C.c_uint,C.c_uint)(0x8D40)==0x8CD5
        return fbo
    def quad(self,z=-4.0,span=10.0):
        vao=self.gen('glGenVertexArrays'); self.call('glBindVertexArray',vao)
        buffer=self.gen('glGenBuffers'); self.call('glBindBuffer',0x8892,buffer)
        points=[(-span,-span),(span,-span),(span,span),(-span,-span),(span,span),(-span,span)]
        data=(C.c_float*30)(*[v for x,y in points for v in (x,y,z,0.5,0.5)])
        self.fn('glBufferData',None,C.c_uint,C.c_ssize_t,C.c_void_p,C.c_uint)(0x8892,C.sizeof(data),data,0x88E4)
        for loc,size,offset in ((0,3,0),(8,2,12)):
            self.call('glEnableVertexAttribArray',loc)
            self.fn('glVertexAttribPointer',None,C.c_uint,C.c_int,C.c_uint,C.c_ubyte,C.c_int,C.c_void_p)(loc,size,0x1406,0,20,C.c_void_p(offset))
        for index,value in ((2,[0.,0.,1.,0.]),(9,[1.,0.,0.,0.]),(10,[0.,1.,0.,0.]),(3,[1.,1.,1.,1.])):
            self.fn('glVertexAttrib4f',None,C.c_uint,C.c_float,C.c_float,C.c_float,C.c_float)(index,*value)
    def pixel(self):
        data=(C.c_ubyte*4)()
        self.fn('glReadPixels',None,C.c_int,C.c_int,C.c_int,C.c_int,C.c_uint,C.c_uint,C.c_void_p)(8,8,1,1,0x1908,0x1401,data)
        assert self.error()==0
        return list(data)
    def pixels(self,width,height):
        data=(C.c_ubyte*(width*height*4))()
        self.fn('glReadPixels',None,C.c_int,C.c_int,C.c_int,C.c_int,C.c_uint,C.c_uint,C.c_void_p)(0,0,width,height,0x1908,0x1401,data)
        assert self.error()==0
        return bytes(data)
    def close(self):
        self.make_current(self.display,None,None,None)
        self.destroy(self.display,self.context); self.terminate(self.display)

IDENTITY=[1.,0.,0.,0.,0.,1.,0.,0.,0.,0.,1.,0.,0.,0.,0.,1.]
PROJECTION=[1.,0.,0.,0.,0.,1.,0.,0.,0.,0.,-1.,-1.,0.,0.,-2.,0.]

class MobileShaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gl=GLES()
        cls.forward=cls.gl.program(source('stages/mobile/forward.vert.glsl'),source('stages/mobile/forward.frag.glsl'))
        cls.shadow=cls.gl.program(source('stages/mobile/shadow.vert.glsl'),source('stages/mobile/shadow.frag.glsl'))
    @classmethod
    def tearDownClass(cls): cls.gl.close()
    def setup_forward(self, ambient=True, light_count=1, eye=-0.25):
        g=self.gl; p=self.forward
        g.target(); g.call('glViewport',0,0,16,16); g.call('glUseProgram',p)
        g.call('glDisable',0x0B71); g.call('glDisable',0x0BE2); g.call('glDisable',0x0B90)
        g.call('glDisable',0x0B44); g.call('glDisable',0x0C11)
        for i in range(16): g.texture(i,cube=i in (5,7,8,10,12,13))
        g.texture(0,(128,128,255,255)); g.texture(1,(128,128,128,255));g.texture(2,(100,100,100,255))
        for name,unit in dict(normalTexture=0,diffuseTexture=1,specularTexture=2,parallaxTexture=3,projection0=4,cube0=5,falloff0=6,
                              ambientDiffuse0=7,ambientSpecular0=8,projection1=9,cube1=10,falloff1=11,ambientDiffuse1=12,ambientSpecular1=13,
                              shadowAtlas=14,ssaoTexture=15).items(): g.uniform(p,'u_'+name,unit,'i')
        for name in ('modelMatrix','modelViewMatrix'): g.uniform(p,'u_'+name,IDENTITY,'m')
        g.uniform(p,'u_projectionMatrix',PROJECTION,'m');g.uniform(p,'u_viewOrigin',[eye,0.,0.])
        g.uniform(p,'u_ambientParams',[0.,1.]);g.uniform(p,'u_clipPlane',[0.,0.,0.,0.])
        g.uniform(p,'u_lightCount',light_count,'i');g.uniform(p,'u_flags',0x24,'i');g.uniform(p,'u_vertexColorMode',0,'i')
        g.uniform(p,'u_shadowFilter',1,'i')
        for name in ('bumpMatrix','diffuseMatrix','specularMatrix'):
            g.uniform(p,'u_'+name+'[0]',[1.,0.,0.,0.]);g.uniform(p,'u_'+name+'[1]',[0.,1.,0.,0.])
        for i in range(8):
            g.uniform(p,f'u_lightTextureSet[{i}]',i%2,'i')
            g.uniform(p,f'u_lightFlags[{i}]',0x100000 if ambient else 0,'i')
            g.uniform(p,f'u_lightOrigin[{i}]',[2.,0.,0.]);g.uniform(p,f'u_shadowParams[{i}]',[0.,0.,0.,0.])
            g.uniform(p,f'u_diffuseColor[{i}]',[0.25,0.25,0.25,1.]);g.uniform(p,f'u_specularColor[{i}]',[0.15,0.15,0.15,1.])
            # Each column is a projection/falloff plane, as in drawInteraction_t.
            g.uniform(p,f'u_lightProjection[{i}]',[0.,0.,0.,0.5, 0.,0.,0.,0.5, 0.,0.,0.,1., 0.,0.,0.,0.5],'m')
            g.uniform(p,f'u_lightTextureMatrix[{2*i}]',[1.,0.,0.,0.]);g.uniform(p,f'u_lightTextureMatrix[{2*i+1}]',[0.,1.,0.,0.])
        g.quad();g.call('glDrawArrays',0x0004,0,6)
        return g.pixel()
    def test_batched_lights_add_without_repeating_material_draw(self):
        one=self.setup_forward(light_count=1); two=self.setup_forward(light_count=2)
        self.assertGreater(one[0],10)
        for i in range(3): self.assertAlmostEqual(two[i],2*one[i],delta=2)

    def test_eight_light_batch_matches_eight_draws_with_two_texture_sets(self):
        g=self.gl;p=self.forward
        for eye in (-0.03125,0.03125):
            self.setup_forward(light_count=8,eye=eye)
            g.texture(4,(230,170,90,255));g.texture(9,(60,150,240,255))
            def light(slot,i):
                g.uniform(p,f'u_lightTextureSet[{slot}]',i%2,'i')
                g.uniform(p,f'u_lightFlags[{slot}]',0x100000 if i%3==0 else 0,'i')
                g.uniform(p,f'u_lightOrigin[{slot}]',[i*0.25-0.8,0.,0.])
                g.uniform(p,f'u_diffuseColor[{slot}]',[0.025+i*0.003,0.035,0.02,1.])
                g.uniform(p,f'u_specularColor[{slot}]',[0.02,0.025,0.03,1.])
                g.uniform(p,f'u_shadowParams[{slot}]',[0.,0.,0.,0.])
            for i in range(8):light(i,i)
            g.call('glDisable',0x0BE2);g.call('glClearColor',0.,0.,0.,0.);g.call('glClear',0x00004000)
            g.call('glDrawArrays',4,0,6);batch=g.pixels(16,16)
            g.call('glClear',0x00004000);g.call('glEnable',0x0BE2);g.call('glBlendFunc',1,1)
            g.uniform(p,'u_lightCount',1,'i')
            for i in range(8):light(0,i);g.call('glDrawArrays',4,0,6)
            reference=g.pixels(16,16)
            self.assertGreater(max(batch[0::4]),5)
            self.assertLessEqual(max(abs(a-b) for a,b in zip(batch,reference)),4)
            g.call('glDisable',0x0BE2)

    def test_cached_global_and_local_shadow_depth_matches_fresh_frames(self):
        # Compile the shipping face cache policy and drive the actual shadow
        # shaders through cold/warm frames, moving/removing casters, alpha
        # changes, layout/storage invalidation, and global/local atlas rows.
        with tempfile.TemporaryDirectory(prefix='quest-shadow-gles-') as directory:
            cpp=Path(directory)/'cache.cpp';so=Path(directory)/'cache.so'
            cpp.write_text('''
#include "renderer/backend/MobileShadowCache.h"
using namespace MobileShadowCache;
FaceState state[2];
Signature key(unsigned long long v){Signature s;s.Add(v);return s;}
extern "C" int evaluate(int i,unsigned long long st,unsigned long long dy){
 auto s=key(st),f=s;f.Add(dy);
 bool dirty=state[i].StaticDirty(s);return (dirty?1:0)|((dirty||state[i].FinalDirty(f))?2:0);
}
extern "C" void commit(int i,unsigned long long st,unsigned long long dy){
 state[i].staticContent=key(st);state[i].finalContent=state[i].staticContent;state[i].finalContent.Add(dy);
 state[i].haveStatic=state[i].haveFinal=true;
}
extern "C" void invalidate(){state[0]={};state[1]={};}
''')
            subprocess.run(['g++','-std=c++14','-O2','-shared','-fPIC','-I'+str(ROOT/'Q3E/src/main/jni/darkmod'),str(cpp),'-o',str(so)],check=True)
            policy=C.CDLL(str(so))
            for name in ('evaluate','commit'):
                getattr(policy,name).argtypes=[C.c_int,C.c_ulonglong,C.c_ulonglong]
            policy.evaluate.restype=C.c_int
            g=self.gl
            textures=[g.texture(i+1,depth=True,size=64) for i in range(3)]
            targets=[g.target(depth_texture=t) for t in textures]
            for target in targets:
                g.call('glBindFramebuffer',0x8D40,target);g.call('glDisable',0x0C11)
                g.call('glDepthMask',1);g.call('glClearDepthf',1.);g.call('glClear',0x00000100)
            vs='#version 320 es\nprecision highp float;in vec4 attr_Position;out vec2 tc;void main(){gl_Position=vec4(attr_Position.xy,0,1);tc=attr_Position.xy*0.5+0.5;}'
            fs='#version 320 es\nprecision highp float;uniform highp sampler2D depth;in vec2 tc;out vec4 color;void main(){color=vec4(vec3(texture(depth,tc).r),1);}'
            sample=g.program(vs,fs)
            def pixels(texture):
                g.target(width=64,height=64);g.call('glViewport',0,0,64,64)
                for flag in (0x0B71,0x0BE2,0x0C11,0x0B44):g.call('glDisable',flag)
                g.call('glUseProgram',sample);g.call('glActiveTexture',0x84C0+14);g.call('glBindTexture',0x0DE1,texture)
                g.uniform(sample,'depth',14,'i');g.quad(z=0.,span=1.);g.call('glDrawArrays',4,0,6)
                return g.pixels(64,64)
            def prepare(target,face,clear=False):
                g.call('glBindFramebuffer',0x8D40,target);g.call('glEnable',0x0C11)
                g.call('glViewport',0,face*32,16,16);g.call('glScissor',0,face*32,16,16)
                g.call('glEnable',0x0B71);g.call('glDepthFunc',0x0201);g.call('glDepthMask',1)
                if clear:g.call('glClear',0x00000100)
                g.call('glUseProgram',self.shadow);g.uniform(self.shadow,'u_parallel',1,'i')
                g.uniform(self.shadow,'u_shadowProjection',IDENTITY,'m');g.uniform(self.shadow,'u_textureMatrix',IDENTITY,'m')
            def draw(x,z,span,alpha=255):
                matrix=IDENTITY.copy();matrix[12]=x
                g.uniform(self.shadow,'u_modelMatrix',matrix,'m');g.uniform(self.shadow,'u_texture',0,'i')
                g.uniform(self.shadow,'u_alphaTest',0.5 if alpha<255 else -1.)
                g.texture(0,(255,255,255,alpha));g.quad(z=z,span=span);g.call('glDrawArrays',4,0,6)
            frames=[(-0.4,255,0,False),(-0.4,255,0,False),(0.,255,0,False),(0.4,255,0,False),
                    (None,255,0,False),(None,255,0,False),(None,0,0,False),(0.1,0,0,False),
                    (0.1,255,1,False),(0.1,255,1,False),(0.1,255,1,True)]
            cached_draws=reference_draws=0
            for actor,alpha,revision,invalidate in frames:
                if invalidate:policy.invalidate()
                for face in range(2):
                    st=1+revision*1000+(alpha if face==0 else 0)
                    dy=0 if actor is None else 100+int((actor+1)*100)
                    action=policy.evaluate(face,st,dy)
                    if action&1:
                        prepare(targets[0],face,True);draw(0.,0.65,0.9);cached_draws+=1
                        if face==0:draw(-0.4,0.2,0.3,alpha);cached_draws+=1
                    if action&2:
                        g.call('glBindFramebuffer',0x8CA8,targets[0]);g.call('glBindFramebuffer',0x8CA9,targets[1]);g.call('glDisable',0x0C11)
                        y=face*32;g.call('glBlitFramebuffer',0,y,16,y+16,0,y,16,y+16,0x00000100,0x2600)
                        prepare(targets[1],face)
                        if actor is not None:draw(actor,-0.2,0.2);cached_draws+=1
                    policy.commit(face,st,dy)
                    prepare(targets[2],face,True);draw(0.,0.65,0.9);reference_draws+=1
                    if face==0:draw(-0.4,0.2,0.3,alpha);reference_draws+=1
                    if actor is not None:draw(actor,-0.2,0.2);reference_draws+=1
                self.assertEqual(pixels(textures[1]),pixels(textures[2]),(actor,alpha,revision,invalidate))
            self.assertLess(cached_draws,reference_draws//2)
            print(f'Cached-shadow GLES sequence: {reference_draws} reference draws -> {cached_draws} draws, identical pixels')

    def test_fog_switch_removes_soft_particle_pixels_but_preserves_smoke(self):
        g = self.gl
        p = g.program(source('stages/surface_passes/soft_particle.vert.glsl'),
                      source('stages/surface_passes/soft_particle.frag.glsl'))
        g.target();g.call('glViewport',0,0,16,16);g.call('glUseProgram',p)
        for feature in (0x0B71,0x0B90,0x0B44,0x0C11):g.call('glDisable',feature)
        g.call('glEnable',0x0BE2);g.call('glBlendFunc',0x0302,0x0303)
        g.texture(0,(180,180,180,180));g.texture(1,(254,254,254,255))
        g.uniform(p,'u_particleColorTexture',0,'i');g.uniform(p,'u_depthTexture',1,'i')
        g.uniform(p,'u_projectionMatrix',PROJECTION,'m')
        g.uniform(p,'u_modelViewMatrix',IDENTITY,'m')
        g.uniform(p,'u_colorMul',[1.,1.,1.,1.]);g.uniform(p,'u_colorAdd',[0.,0.,0.,0.])
        g.uniform(p,'u_invParticleRadius',0.5);g.uniform(p,'u_invSceneFadeCoeff',0.5)
        g.uniform(p,'u_invDepthTextureSize',[1./16,1./16]);g.uniform(p,'u_fadeMask',[1.,1.,1.,0.])
        g.quad()
        def draw(skip):
            g.call('glClearColor',0.05,0.10,0.15,1.);g.call('glClear',0x00004000)
            if not skip:g.call('glDrawArrays',4,0,6)
            return g.pixels(16,16)
        background=draw(True)
        with compiled_surface_policy() as skip:
            material=b'textures/sfx/shotgunpuff'
            for eye in (-0.03125,0.03125):
                matrix=IDENTITY.copy();matrix[12]=eye
                g.uniform(p,'u_modelViewMatrix',matrix,'m')
                visible=draw(skip(True,True,False,b'tdm_dustfog3',material))
                hidden=draw(skip(True,False,False,b'tdm_dustfog3',material))
                smoke=draw(skip(True,False,False,b'tdm_smoke',material))
                self.assertNotEqual(visible,background)
                self.assertEqual(hidden,background)
                self.assertEqual(smoke,visible)
                self.assertEqual(draw(skip(True,False,True,b'tdm_dustfog3',material)),visible)
        g.call('glDisable',0x0BE2)
    def test_eye_origin_changes_specular_shading(self):
        left=self.setup_forward(ambient=False,eye=-1.5)
        right=self.setup_forward(ambient=False,eye=1.5)
        self.assertGreater(abs(left[0]-right[0]),3,(left,right))
    def test_independent_projection_textures_and_light_volume_clipping(self):
        self.setup_forward(light_count=2)
        g=self.gl; p=self.forward
        g.texture(4,(255,0,0,255));g.texture(9,(0,0,255,255))
        g.call('glDrawArrays',4,0,6); pixel=g.pixel()
        self.assertGreater(pixel[0],10);self.assertGreater(pixel[2],10);self.assertEqual(pixel[1],0)
        # The canonical full mesh is drawn once: an out-of-volume light must
        # contribute zero even when the mesh itself covers the whole viewport.
        for i in range(2):
            g.uniform(p,f'u_lightProjection[{i}]',[0.,0.,0.,2.,0.,0.,0.,0.5,0.,0.,0.,1.,0.,0.,0.,0.5],'m')
        g.call('glDrawArrays',4,0,6);self.assertEqual(g.pixel()[:3],[0,0,0])
    def test_mirror_clip_plane_is_applied_to_forward_lighting(self):
        self.setup_forward()
        g=self.gl
        g.call('glClearColor',0.,0.,0.,0.);g.call('glClear',0x00004000)
        g.uniform(self.forward,'u_clipPlane',[0.,0.,0.,-1.]);g.call('glDrawArrays',4,0,6)
        self.assertEqual(g.pixel(),[0,0,0,0])
    def test_forward_matches_real_depth_prepass(self):
        g=self.gl
        depth=g.program(source('stages/depth/depth.vert.glsl'),source('stages/depth/depth.frag.glsl'))
        # Nonintegral positions expose depth rounding that an identity plane
        # at z=-4 hides. Exercise both asymmetric eye projections and distant
        # geometry, with the real separate shader programs and GL_EQUAL.
        for eye in (-0.03125,0.03125):
            for z in (-4.123,-24.3,-4000.7):
                with self.subTest(eye=eye,z=z):
                    self.setup_forward(eye=eye)
                    g.target(depth_buffer=True);g.texture(0,(128,128,255,255))
                    g.call('glEnable',0x0B71);g.call('glDepthMask',1);g.call('glDepthFunc',0x0201)
                    g.call('glClearDepthf',1.);g.call('glClearColor',0.,0.,0.,0.);g.call('glClear',0x4100)
                    projection=PROJECTION.copy();projection[8]=eye
                    model_view=IDENTITY.copy();model_view[12]=-eye;model_view[14]=-0.4375
                    g.call('glUseProgram',depth)
                    for name in ('modelMatrix','textureMatrix'):g.uniform(depth,'u_'+name,IDENTITY,'m')
                    g.uniform(depth,'u_modelViewMatrix',model_view,'m')
                    g.uniform(depth,'u_projectionMatrix',projection,'m')
                    g.uniform(depth,'u_clipPlane',[0.,0.,0.,0.]);g.uniform(depth,'u_alphaTest',-1.)
                    g.uniform(depth,'u_color',[0.,0.,0.,1.]);g.quad(z=z,span=-2*z);g.call('glDrawArrays',4,0,6)
                    self.assertEqual(g.pixel()[:3],[0,0,0])
                    g.call('glUseProgram',self.forward)
                    g.uniform(self.forward,'u_modelViewMatrix',model_view,'m')
                    g.uniform(self.forward,'u_projectionMatrix',projection,'m')
                    g.call('glDepthFunc',0x0202);g.call('glDepthMask',0);g.call('glDrawArrays',4,0,6)
                    self.assertGreater(g.pixel()[0],10)
    def test_eye_light_scissors_preserve_rendered_pixels(self):
        # Call the production C++ projection helper, then compare complete
        # rendered images against unscissored lighting. No copied projection
        # implementation or hard-coded screen rectangles are used here.
        with tempfile.TemporaryDirectory(prefix='tdm-scissor-') as directory:
            wrapper=Path(directory)/'scissor.cpp'
            wrapper.write_text('''
            #include "Q3E/src/main/jni/darkmod/renderer/backend/MobileRenderPlan.h"
            extern "C" void project(const float *lo,const float *hi,const float *view,
              const float *projection,const int *viewport,int *out) {
                const auto r=MobileRenderPlan::ProjectScissor(lo,hi,view,projection,
                    {viewport[0],viewport[1],viewport[2],viewport[3]});
                out[0]=r.x1;out[1]=r.y1;out[2]=r.x2;out[3]=r.y2;
            }
            ''')
            library=Path(directory)/'scissor.so'
            subprocess.run(['g++','-std=c++14','-O2','-shared','-fPIC','-I'+str(ROOT),str(wrapper),'-o',str(library)],check=True,capture_output=True)
            lib=C.CDLL(str(library));project=lib.project
            project.argtypes=[C.POINTER(C.c_float)]*4+[C.POINTER(C.c_int)]*2
            project.restype=None
            f=lambda value:(C.c_float*len(value))(*value)
            cases=[
                (-4.,[([-1.95,-.6,-4.5],[-1.25,.6,-3.5]),([1.25,-.6,-4.5],[1.95,.6,-3.5])]),
                (-1.5,[([-1.05,-.6,-2.],[ -.35,.6,.5]),([.35,-.6,-2.],[1.05,.6,.5])]),
                (-2.,[([-3.,-3.,-3.],[3.,3.,3.]),([.8,-.6,-2.5],[1.2,.6,-1.5])]),
                (-4.,[([-4.1,-.6,-4.2],[-3.7,.6,-3.8]),([3.7,-.6,-4.2],[4.1,.6,-3.8])]),
            ]
            for case,(z,boxes) in enumerate(cases):
                for eye in (-.35,.35):
                    with self.subTest(case=case,eye=eye):
                        self.setup_forward(light_count=2)
                        g=self.gl;p=self.forward
                        g.target(64,64);g.texture(0,(128,128,255,255));g.call('glViewport',0,0,64,64)
                        g.call('glEnable',0x0BE2);g.call('glBlendFunc',1,1)
                        view=IDENTITY.copy();view[12]=-eye
                        if case==2:view[0]=-1.
                        projection=PROJECTION.copy();projection[8]=eye*.25
                        g.uniform(p,'u_modelViewMatrix',view,'m');g.uniform(p,'u_projectionMatrix',projection,'m')
                        g.texture(4,(255,0,0,255));g.texture(9,(0,0,255,255))
                        matrices=[];rects=[]
                        for i,(lo,hi) in enumerate(boxes):
                            matrix=[1./(hi[0]-lo[0]),0.,0.,-lo[0]/(hi[0]-lo[0]),
                                    0.,1./(hi[1]-lo[1]),0.,-lo[1]/(hi[1]-lo[1]),
                                    0.,0.,0.,1.,0.,0.,1./(hi[2]-lo[2]),-lo[2]/(hi[2]-lo[2])]
                            matrices.append(matrix);g.uniform(p,f'u_lightProjection[{i}]',matrix,'m')
                            out=(C.c_int*4)();project(f(lo),f(hi),f(view),f(projection),(C.c_int*4)(0,0,64,64),out)
                            rects.append(list(out))
                        def clear():
                            g.call('glDisable',0x0C11);g.call('glColorMask',1,1,1,1)
                            g.call('glClearColor',0.,0.,0.,1.);g.call('glClear',0x4000)
                            g.call('glColorMask',1,1,1,0)
                        clear();g.quad(z=z);g.call('glDrawArrays',4,0,6);reference=g.pixels(64,64)
                        clear();g.call('glEnable',0x0C11)
                        visible=[r for r in rects if r[2]>r[0] and r[3]>r[1]]
                        if visible:
                            union=[min(r[0] for r in visible),min(r[1] for r in visible),max(r[2] for r in visible),max(r[3] for r in visible)]
                            area=lambda r:(r[2]-r[0])*(r[3]-r[1])
                            if len(visible)==2 and area(union)<=sum(map(area,visible)):
                                g.call('glScissor',union[0],union[1],union[2]-union[0],union[3]-union[1]);g.call('glDrawArrays',4,0,6)
                            else:
                                for i,r in enumerate(rects):
                                    if r[2]<=r[0] or r[3]<=r[1]:continue
                                    g.uniform(p,'u_lightCount',1,'i');g.uniform(p,'u_projection0',4 if i==0 else 9,'i')
                                    g.uniform(p,'u_lightProjection[0]',matrices[i],'m')
                                    g.call('glScissor',r[0],r[1],r[2]-r[0],r[3]-r[1]);g.call('glDrawArrays',4,0,6)
                            if case==0:self.assertLess(sum(map(area,visible)),64*64//4)
                        self.assertEqual(g.pixels(64,64),reference)
                        g.call('glColorMask',1,1,1,1);g.call('glDisable',0x0C11)
    def test_volumetric_shader_links_with_mobile_shadows(self):
        self.gl.program(source('volumetric.vs'),source('volumetric_raymarching.fs'))
    def test_shadow_depth_and_local_caster_mask(self):
        g=self.gl
        depth=g.texture(14,depth=True,size=64); target=g.target(depth_texture=depth)
        g.call('glViewport',0,0,64,64);g.call('glDisable',0x0C11)
        g.call('glClearDepthf',1.);g.call('glDepthMask',1);g.call('glClear',0x00000100)
        g.call('glEnable',0x0B71);g.call('glDepthFunc',0x0201)
        g.call('glUseProgram',self.shadow)
        for name in ('modelMatrix','textureMatrix'): g.uniform(self.shadow,'u_'+name,IDENTITY,'m')
        g.uniform(self.shadow,'u_lightOrigin',[0.,0.,0.]);g.uniform(self.shadow,'u_parallel',0,'i')
        g.uniform(self.shadow,'u_face',4,'i');g.uniform(self.shadow,'u_alphaTest',-1.)
        # +Z global face = column 1, row 1 for face 4, face size=16.
        g.call('glViewport',16,16,16,16);g.quad(z=4.);g.call('glDrawArrays',0x0004,0,6)
        vs='#version 320 es\nprecision highp float; in vec4 attr_Position; void main(){ gl_Position=vec4(attr_Position.xy,0,1); }'
        fs='#version 320 es\nprecision highp float; precision highp int;\n'+source('tdm_mobile_shadows.glsl')+'''
        uniform highp sampler2D atlas; uniform vec3 world, receiverNormal; uniform vec4 params; uniform int filtered, useReceiverNormal;
        out vec4 color;
        void main(){float s=mobileShadow(atlas,vec4(0,0,0.25,1.0/64.0),params,mat4(1),world,vec3(0),useReceiverNormal!=0?receiverNormal:normalize(-world),filtered!=0); color=vec4(s,s,s,1);}
        '''
        p=g.program(vs,fs);g.target();g.call('glUseProgram',p);g.call('glViewport',0,0,16,16);g.call('glDisable',0x0B71)
        g.call('glActiveTexture',0x84C0+14);g.call('glBindTexture',0x0DE1,depth);g.uniform(p,'atlas',14,'i')
        g.uniform(p,'world',[0.,0.,8.]);g.uniform(p,'params',[1.,0.,0.,1.]);g.quad();g.call('glDrawArrays',4,0,6)
        self.assertLess(g.pixel()[0],5)  # global caster blocks the receiver
        g.uniform(p,'filtered',1,'i');g.call('glDrawArrays',4,0,6);self.assertLess(g.pixel()[0],5)
        g.uniform(p,'world',[0.,0.,2.]);g.call('glDrawArrays',4,0,6);self.assertGreater(g.pixel()[0],250)
        g.uniform(p,'world',[0.,0.,-8.]);g.call('glDrawArrays',4,0,6);self.assertGreater(g.pixel()[0],250) # another face is clear
        # Render only a local caster; local receivers must remain unshadowed.
        g.call('glBindFramebuffer',0x8D40,target);g.call('glClear',0x00000100);g.call('glEnable',0x0B71)
        g.call('glUseProgram',self.shadow);g.call('glViewport',16,48,16,16);g.quad(z=4.);g.call('glDrawArrays',4,0,6)
        g.target();g.call('glViewport',0,0,16,16);g.call('glDisable',0x0B71);g.call('glUseProgram',p)
        g.uniform(p,'world',[0.,0.,8.]);g.uniform(p,'params',[1.,0.,0.,1.]);g.quad();g.call('glDrawArrays',4,0,6)
        self.assertGreater(g.pixel()[0],250)
        g.uniform(p,'params',[1.,1.,0.,1.]);g.call('glDrawArrays',4,0,6);self.assertLess(g.pixel()[0],5)
        # A cutout caster with zero alpha must not fill the depth atlas.
        g.call('glBindFramebuffer',0x8D40,target);g.call('glClear',0x00000100);g.call('glEnable',0x0B71)
        g.call('glUseProgram',self.shadow);g.uniform(self.shadow,'u_alphaTest',0.5);g.uniform(self.shadow,'u_texture',0,'i')
        g.texture(0,(255,255,255,0));g.call('glViewport',16,16,16,16);g.quad(z=4.);g.call('glDrawArrays',4,0,6)
        g.target();g.call('glViewport',0,0,16,16);g.call('glDisable',0x0B71);g.call('glUseProgram',p)
        g.uniform(p,'params',[1.,1.,0.,1.]);g.quad();g.call('glDrawArrays',4,0,6);self.assertGreater(g.pixel()[0],250)
        # Render and sample every cube direction with the actual depth shader.
        for face in range(6):
            axis=face//2; normal=[0.,0.,0.];normal[axis]=-1. if face%2 else 1.
            tangent=[0.,1.,0.] if axis==0 else [1.,0.,0.]
            bitangent=[0.,1.,0.] if axis==2 else [0.,0.,1.]
            model=tangent+[0.]+bitangent+[0.]+normal+[0.]+[4.*v for v in normal]+[1.]
            g.call('glBindFramebuffer',0x8D40,target);g.call('glClear',0x00000100);g.call('glEnable',0x0B71)
            g.call('glUseProgram',self.shadow);g.uniform(self.shadow,'u_alphaTest',-1.)
            g.uniform(self.shadow,'u_modelMatrix',model,'m');g.uniform(self.shadow,'u_face',face,'i')
            g.call('glViewport',(face%3)*16,(face//3)*16,16,16);g.quad(z=0.);g.call('glDrawArrays',4,0,6)
            g.target();g.call('glViewport',0,0,16,16);g.call('glDisable',0x0B71);g.call('glUseProgram',p)
            g.uniform(p,'params',[1.,0.,0.,1.]);g.uniform(p,'world',[8.*v for v in normal]);g.quad();g.call('glDrawArrays',4,0,6)
            self.assertLess(g.pixel()[0],5,face)
        # Directional lights use a fitted orthographic projection, not the
        # engine's far-away point-light approximation.
        g.call('glBindFramebuffer',0x8D40,target);g.call('glClear',0x00000100);g.call('glEnable',0x0B71)
        g.call('glUseProgram',self.shadow);g.uniform(self.shadow,'u_parallel',1,'i')
        for name in ('modelMatrix','shadowProjection'):g.uniform(self.shadow,'u_'+name,IDENTITY,'m')
        g.call('glViewport',0,0,16,16);g.quad(z=0.);g.call('glDrawArrays',4,0,6)
        g.target();g.call('glViewport',0,0,16,16);g.call('glDisable',0x0B71);g.call('glUseProgram',p)
        g.uniform(p,'params',[1.,0.,1.,1.]);g.uniform(p,'world',[0.,0.,0.8]);g.quad();g.call('glDrawArrays',4,0,6)
        self.assertLess(g.pixel()[0],5)
        g.uniform(p,'world',[0.,0.,-0.8]);g.call('glDrawArrays',4,0,6);self.assertGreater(g.pixel()[0],250)
        # A sloped directional-light receiver samples another point on its
        # own plane at a shadow texel centre. This must remain fully lit for
        # both hard shadows and PCF, while a separate receiver stays occluded.
        g.call('glBindFramebuffer',0x8D40,target);g.call('glDepthMask',1)
        g.call('glClear',0x00000100);g.call('glEnable',0x0B71);g.call('glUseProgram',self.shadow)
        model=IDENTITY.copy();model[2]=0.8
        g.uniform(self.shadow,'u_modelMatrix',model,'m');g.call('glViewport',0,0,16,16)
        g.quad(z=0.,span=1.);g.call('glDrawArrays',4,0,6)
        g.target();g.call('glViewport',0,0,16,16);g.call('glDisable',0x0B71);g.call('glUseProgram',p)
        g.uniform(p,'useReceiverNormal',1,'i');g.uniform(p,'receiverNormal',[-0.8,0.,1.])
        g.quad()
        for filtered in (0,1):
            g.uniform(p,'filtered',filtered,'i')
            for x in (0.07,0.19,-0.43):
                g.uniform(p,'world',[x,0.,0.8*x]);g.call('glDrawArrays',4,0,6)
                self.assertGreater(g.pixel()[0],250,(filtered,x))
            g.uniform(p,'world',[0.07,0.,0.8]);g.call('glDrawArrays',4,0,6)
            self.assertLess(g.pixel()[0],5)

if __name__=='__main__':
    os.environ.setdefault('EGL_PLATFORM','surfaceless')
    unittest.main(verbosity=2)
