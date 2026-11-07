/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#include "precompiled.h"
#include "renderer/tr_local.h"
#include "renderer/backend/MobileVrsProbe.h"

#ifdef TDM_VR_OPENXR
namespace {
const char *probeVertex=R"GLSL(#version 320 es
void main(){vec2 p=vec2(gl_VertexID==1?3.0:-1.0,gl_VertexID==2?3.0:-1.0);gl_Position=vec4(p,0.0,1.0);}
)GLSL";
const char *probeFragment=R"GLSL(#version 320 es
#if defined(GL_EXT_fragment_invocation_density)
#extension GL_EXT_fragment_invocation_density : require
#elif defined(GL_EXT_fragment_shading_rate)
#extension GL_EXT_fragment_shading_rate : require
#else
#error No shader invocation-density interface
#endif
precision highp float;
precision highp int;
layout(early_fragment_tests) in;
out vec4 color;
void main(){
#if defined(GL_EXT_fragment_invocation_density)
    vec2 size=vec2(gl_FragSizeEXT);
#else
    int bits=int(gl_ShadingRateEXT);
    vec2 size=vec2(float(1<<((bits>>2)&3)),float(1<<(bits&3)));
#endif
    color=vec4(size*0.25,0.0,1.0);
}
)GLSL";

GLuint CompileProbe(GLenum type,const char *source) {
	GLuint shader=qglCreateShader(type);qglShaderSource(shader,1,&source,nullptr);qglCompileShader(shader);
	GLint okay=0;qglGetShaderiv(shader,GL_COMPILE_STATUS,&okay);
	if(!okay){qglDeleteShader(shader);return 0;}return shader;
}

MobileVrsProbeResult DrawProbe(GLuint program,void (*setRate)(unsigned)) {
	// Bind temporary GL objects directly, then restore the exact physical
	// bindings. Engine FBO/program/VAO/texture caches remain unchanged.
	GLint read=0,draw=0,renderbuffer=0,vao=0,previousProgram=0,viewport[4],scissor[4],pack[5];
	qglGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);qglGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw);
	qglGetIntegerv(GL_RENDERBUFFER_BINDING,&renderbuffer);qglGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);
	qglGetIntegerv(GL_CURRENT_PROGRAM,&previousProgram);qglGetIntegerv(GL_VIEWPORT,viewport);qglGetIntegerv(GL_SCISSOR_BOX,scissor);
	const GLenum packNames[]={GL_PACK_ALIGNMENT,GL_PACK_ROW_LENGTH,GL_PACK_SKIP_PIXELS,GL_PACK_SKIP_ROWS,GL_PIXEL_PACK_BUFFER_BINDING};
	for(int i=0;i<5;++i)qglGetIntegerv(packNames[i],&pack[i]);
	const GLenum caps[]={GL_DEPTH_TEST,GL_STENCIL_TEST,GL_SCISSOR_TEST,GL_BLEND,GL_CULL_FACE,GL_DITHER,GL_RASTERIZER_DISCARD};
	GLboolean enabled[7],mask[4];for(int i=0;i<7;++i)enabled[i]=qglIsEnabled(caps[i]);
	qglGetBooleanv(GL_COLOR_WRITEMASK,mask);
	GLuint fb=0,rb=0,probeVao=0;qglGenFramebuffers(1,&fb);qglBindFramebuffer(GL_FRAMEBUFFER,fb);
	qglGenRenderbuffers(1,&rb);qglBindRenderbuffer(GL_RENDERBUFFER,rb);qglRenderbufferStorage(GL_RENDERBUFFER,GL_RGBA8,16,16);
	qglFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,rb);
	MobileVrsProbeResult result;
	if(qglCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE){
		qglGenVertexArrays(1,&probeVao);qglBindVertexArray(probeVao);qglUseProgram(program);
		for(auto cap:caps)qglDisable(cap);qglColorMask(true,true,true,true);qglViewport(0,0,16,16);
		qglBindBuffer(GL_PIXEL_PACK_BUFFER,0);for(int i=0;i<4;++i)qglPixelStorei(packNames[i],i==0?1:0);
		setRate(0x96A9); // 2x2 is the common supported coarse rate.
		qglDrawArrays(GL_TRIANGLES,0,3);
		unsigned char pixel[4]={};qglReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
		setRate(0x96A6); // restore the renderer's full-rate baseline
		if(pixel[3]==255){result.width=(pixel[0]*4+127)/255;result.height=(pixel[1]*4+127)/255;}
	}
	qglBindFramebuffer(GL_READ_FRAMEBUFFER,read);qglBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw);
	qglBindRenderbuffer(GL_RENDERBUFFER,renderbuffer);qglBindVertexArray(vao);qglUseProgram(previousProgram);
	qglViewport(viewport[0],viewport[1],viewport[2],viewport[3]);qglScissor(scissor[0],scissor[1],scissor[2],scissor[3]);
	for(int i=0;i<7;++i){if(enabled[i])qglEnable(caps[i]);else qglDisable(caps[i]);}
	qglColorMask(mask[0],mask[1],mask[2],mask[3]);
	for(int i=0;i<4;++i)qglPixelStorei(packNames[i],pack[i]);qglBindBuffer(GL_PIXEL_PACK_BUFFER,pack[4]);
	if(probeVao)qglDeleteVertexArrays(1,&probeVao);qglDeleteRenderbuffers(1,&rb);qglDeleteFramebuffers(1,&fb);
	return result;
}
}
#endif

MobileVrsProbeResult R_QuestProbeVrs(void (*setRate)(unsigned)) {
#ifdef TDM_VR_OPENXR
	GLuint vertex=CompileProbe(GL_VERTEX_SHADER,probeVertex);if(!vertex)return {};
	GLuint fragment=CompileProbe(GL_FRAGMENT_SHADER,probeFragment);if(!fragment){qglDeleteShader(vertex);return {};}
	GLuint program=qglCreateProgram();qglAttachShader(program,vertex);qglAttachShader(program,fragment);qglLinkProgram(program);
	GLint linked=0;qglGetProgramiv(program,GL_LINK_STATUS,&linked);
	MobileVrsProbeResult result;if(linked)result=DrawProbe(program,setRate);
	qglDeleteProgram(program);qglDeleteShader(vertex);qglDeleteShader(fragment);return result;
#else
	return {};
#endif
}
