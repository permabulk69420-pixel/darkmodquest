#!/usr/bin/env python3
"""Pixel checks for production direct stereo tonemap and mixed-precision BRDF."""
import ctypes as C
import random
import re
import unittest
from mobile_gles_test import GLES, source
import mobile_gles_test as fixtures


class BandwidthShaderTests(unittest.TestCase):
    def test_vrs_compatible_ssao_coordinates_match_pixel_coordinates(self):
        g=GLES()
        try:
            vertex=source('stages/mobile/forward.vert.glsl')
            fragment=source('stages/mobile/forward_1.frag.glsl')
            code=re.sub(r'/\*.*?\*/|//[^\n]*','',fragment,flags=re.S)
            self.assertNotRegex(code,r'\bgl_FragCoord\b','lighting still contains the EXT full-rate condition')
            expression='(questScreen.xy / questScreen.z * 0.5 + 0.5) * u_ssaoViewport.zw + u_ssaoViewport.xy'
            self.assertIn(expression,fragment)
            reference=fragment.replace(expression,'gl_FragCoord.xy / u_renderResolution')
            shaders=[g.program(vertex,text) for text in (fragment,reference)]
            fixture=fixtures.MobileShaderTests();fixture.gl=g;images=[]
            for program in shaders:
                fixture.forward=program;fixture.setup_forward(ambient=True,draw=False)
                g.call('glViewport',2,3,11,9);g.call('glClearColor',0.,0.,0.,1.);g.call('glClear',0x4000)
                g.uniform(program,'u_renderResolution',[16.,16.]);g.uniform(program,'u_ssaoViewport',[2/16,3/16,11/16,9/16])
                g.uniform(program,'u_lightFlags[0]',0x100000|0x800,'i')
                # Perspective-varying W and an offset viewport exercise the
                # reconstruction rather than a constant-W/fullscreen shortcut.
                points=[(-8.,-8.,-3.),(8.,-8.,-7.),(8.,8.,-7.),(-8.,-8.,-3.),(8.,8.,-7.),(-8.,8.,-3.)]
                vertices=(C.c_float*30)(*[v for point in points for v in (*point,.5,.5)])
                g.fn('glBufferSubData',None,C.c_uint,C.c_ssize_t,C.c_ssize_t,C.c_void_p)(0x8892,0,C.sizeof(vertices),vertices)
                g.texture(15)
                pixels=(C.c_ubyte*1024)(*[v for y in range(16) for x in range(16) for v in ((x*9+y*7)%256,0,0,255)])
                g.call('glTexImage2D',0x0DE1,0,0x8058,16,16,0,0x1908,0x1401,pixels)
                g.call('glDrawArrays',0x0004,0,6);images.append(g.pixels(16,16))
            self.assertLessEqual(max(abs(a-b) for a,b in zip(*images)),1)
            print('PASS: lighting has no static gl_FragCoord reference; SSAO matches pixel coordinates with perspective and viewport offset')
        finally:g.close()

    def test_direct_array_tonemap_matches_scene_copy_for_both_eyes(self):
        g=GLES()
        try:
            vertex=source('fullscreen_tri.vert.glsl')
            ordinary=g.program(vertex,source('stages/tonemap/tonemap.frag.glsl'))
            direct=g.program(vertex,source('stages/tonemap/tonemap.frag.glsl',{'QUEST_TONEMAP_ARRAY':'1'}))
            location=g.fn('glGetUniformLocation',C.c_int,C.c_uint,C.c_char_p)(direct,b'u_eyeLayer')
            self.assertGreaterEqual(location,0,'shipping preprocessor did not produce the array shader')
            image=[]
            for eye in range(2):
                for y in range(16):
                    for x in range(16):
                        image.extend(((x*13+eye*43)%256,(y*17+eye*71)%256,(x*11+y*7+eye*107)%256,255))
            data=(C.c_ubyte*len(image))(*image)
            scene=g.gen('glGenTextures');g.call('glActiveTexture',0x84C0);g.call('glBindTexture',0x8C1A,scene)
            g.call('glTexStorage3D',0x8C1A,1,0x8058,16,16,2)
            g.call('glTexSubImage3D',0x8C1A,0,0,0,0,16,16,2,0x1908,0x1401,data)
            for enum in (0x2800,0x2801):g.call('glTexParameteri',0x8C1A,enum,0x2600)
            for enum in (0x2802,0x2803):g.call('glTexParameteri',0x8C1A,enum,0x812F)
            source_fbo=g.gen('glGenFramebuffers')
            gui=g.target();gui_texture=g.fn('glGetFramebufferAttachmentParameteriv',None,C.c_uint,C.c_uint,C.c_uint,C.c_void_p)
            texture_name=C.c_int();gui_texture(0x8D40,0x8CE0,0x8CD1,C.byref(texture_name))
            output=g.target();g.texture(1,(89,151,213,255));g.call('glBindVertexArray',g.gen('glGenVertexArrays'))
            for cap in (0x0B71,0x0B90,0x0BE2,0x0C11):g.call('glDisable',cap)
            g.call('glViewport',0,0,16,16)

            def configure(program,eye,sharpen,compress,dither):
                g.call('glUseProgram',program)
                for name,value in {'texture':0,'noiseImage':1,'eyeLayer':eye,'sharpen':int(sharpen),'compressEnable':int(compress)}.items():
                    g.uniform(program,'u_'+name,value,'i')
                for name,value in {'exposure':1.13,'overbrightDesaturation':.15,'compressSwitchPoint':1.,
                                   'compressSwitchMultiplier':.7,'compressInitialSlope':1.2,'compressTailMultiplier':.3,
                                   'compressTailShift':1.,'compressTailPower':2.,'gamma':1.2,'brightness':.95,
                                   'desaturation':.1,'sharpness':.5,'ditherInput':.001 if dither else 0.,
                                   'ditherOutput':.002 if dither else 0.}.items():g.uniform(program,'u_'+name,value)
            for eye in range(2):
                # Reproduce the old primary -> GUI full-frame colour blit.
                g.call('glBindFramebuffer',0x8D40,source_fbo)
                g.call('glFramebufferTextureLayer',0x8D40,0x8CE0,scene,0,eye)
                g.call('glBindFramebuffer',0x8CA9,gui)
                g.call('glBlitFramebuffer',0,0,16,16,0,0,16,16,0x4000,0x2601)
                for sharpen,compress,dither in ((False,False,False),(True,False,False),(False,True,True),(True,True,True)):
                    results=[]
                    for program in (ordinary,direct):
                        g.call('glBindFramebuffer',0x8D40,output);g.call('glActiveTexture',0x84C0)
                        g.call('glBindTexture',0x0DE1,texture_name.value);g.call('glBindTexture',0x8C1A,scene)
                        configure(program,eye,sharpen,compress,dither)
                        g.call('glDrawArrays',0x0004,0,3);results.append(g.pixels(16,16))
                    self.assertEqual(results[0],results[1],(eye,sharpen,compress,dither))
            print('PASS: direct scene-array tonemapping equals the GUI-copy path for both eyes, sharpening, compression and dithering')
        finally:g.close()

    def test_mixed_precision_brdf_matches_highp_and_rounds_bounded_weights(self):
        g=GLES()
        try:
            vertex=source('stages/mobile/forward.vert.glsl')
            mixed_source=source('stages/mobile/forward_1.frag.glsl')
            mixed=g.program(vertex,mixed_source)
            full=g.program(vertex,source('stages/mobile/forward_1.frag.glsl',{'QUEST_FULL_PRECISION_LIGHTING':'1'}))
            for capacity in (2,4,8):
                path='stages/mobile/forward.frag.glsl' if capacity==8 else f'stages/mobile/forward_{capacity}.frag.glsl'
                g.program(vertex,source(path,{'QUEST_FULL_PRECISION_LIGHTING':'1'}))
            legacy=g.program(vertex,mixed_source.replace('questAdvancedInteraction(geom,','computeAdvancedInteraction(geom,'))
            # Make assignment rounding explicit as an additional numerical
            # stress check, even on drivers that promote mediump to FP32.
            rounding='''
highp float questRound16(highp float x){return unpackHalf2x16(packHalf2x16(vec2(x,0.0))).x;}
highp vec3 questRound16(highp vec3 x){return vec3(unpackHalf2x16(packHalf2x16(x.xy)),unpackHalf2x16(packHalf2x16(vec2(x.z,0.0))).x);}
'''
            rounded_source=mixed_source.replace('highp vec3 questAdvancedInteraction(',rounding+'\nhighp vec3 questAdvancedInteraction(')
            rounded_source=re.sub(r'(QUEST_UNIT\s+(?:float|vec3)\s+\w+\s*=)([^;]+);',r'\1questRound16(\2);',rounded_source)
            rounded=g.program(vertex,rounded_source)
            fixture=fixtures.MobileShaderTests();fixture.gl=g
            randomizer=random.Random(72);worst=0
            cases=[(0.,0.,.0,1.),(6.,-.5,.8,1.),(-8.,.1,-.8,1.)]
            cases.extend((randomizer.uniform(-12,12),randomizer.uniform(-8,8),randomizer.uniform(-2,2),1.) for _ in range(36))
            cases.extend((x,y,eye,scale) for x,y,eye,scale in ((0.,0.,0.,8.),(3.,-.5,.8,64.),(-4.,1.,-.4,256.)))
            for x,y,eye,brightness in cases:
                normal=(randomizer.randrange(32,224),randomizer.randrange(32,224),randomizer.randrange(128,256),255)
                images=[]
                for program in (legacy,full,mixed,rounded):
                    fixture.forward=program;fixture.setup_forward(ambient=False,eye=eye,draw=False)
                    g.texture(0,normal)
                    g.uniform(program,'u_lightOrigin[0]',[x,y,-.5])
                    diffuse=[v*brightness for v in (.17,.09,.035)]
                    specular=[v*brightness for v in (.35,.27,.15)]
                    scale=max(1.,*map(abs,diffuse+specular)) if program in (mixed,rounded) else 1.
                    g.uniform(program,'u_diffuseColor[0]',[v/scale for v in diffuse]+[1.])
                    g.uniform(program,'u_specularColor[0]',[v/scale for v in specular]+[1.])
                    g.uniform(program,'u_lightColorScale[0]',scale)
                    if brightness>1.:g.texture(4,(8,8,8,255))
                    # Also exercise the bounded normal/direction computations
                    # at grazing incidence and the bump-light toggling fix.
                    g.uniform(program,'u_flags',0x24|0x020000,'i')
                    g.call('glDrawArrays',0x0004,0,6);images.append(g.pixels(16,16))
                self.assertLessEqual(max(abs(a-b) for a,b in zip(images[0],images[1])),1)
                for image in images[2:]:
                    error=max(abs(a-b) for a,b in zip(images[1],image));worst=max(worst,error)
                    self.assertLessEqual(error,3,(x,y,eye,error))
            print(f'PASS: mixed BRDF and explicit FP16 assignment rounding stay within {worst}/255 of highp across {len(cases)} lighting/view cases')
        finally:g.close()


if __name__=='__main__':unittest.main(verbosity=2)
