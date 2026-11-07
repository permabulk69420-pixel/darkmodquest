/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#include "precompiled.h"
#include "renderer/tr_local.h"
#include "renderer/backend/MobileMultiviewTargets.h"
#include "renderer/backend/FrameBuffer.h"
#include "renderer/backend/FrameBufferManager.h"
#ifdef TDM_VR_OPENXR
#include <EGL/egl.h>
#endif
#include <map>
#include <set>
#include "renderer/backend/stages/AmbientOcclusionStage.h"

idCVar r_questMultiview("r_questMultiview","1",CVAR_RENDERER|CVAR_BOOL|CVAR_INIT,
	"Submit Quest scene depth and lighting once for both eyes using GL_OVR_multiview2");

namespace {
void (*copyImage)(GLuint,GLenum,GLint,GLint,GLint,GLint,GLuint,GLenum,GLint,GLint,GLint,GLint,GLsizei,GLsizei,GLsizei)=nullptr;
}
void R_QuestCopyImage(GLuint source,GLenum sourceTarget,GLuint target,GLenum targetType,int width,int height,int layer){
	assert(copyImage);
	copyImage(source,sourceTarget,0,0,0,0,target,targetType,0,0,0,layer,width,height,1);
}

struct MobileMultiviewTargets::Impl {
	struct ImageState {
		GLuint texture=GLuint(idImage::TEXTURE_NOT_LOADED);
		int width=0,height=0,format=GL_RGBA8;
		void Read(const idImage *image){texture=image->texnum;width=image->uploadWidth;height=image->uploadHeight;format=image->internalFormat;}
		void Write(idImage *image)const{image->texnum=texture;image->uploadWidth=width;image->uploadHeight=height;image->internalFormat=format;}
	};
	struct EyeImage { ImageState eye[2];uint64_t revision=0; };
	std::map<idImage *,EyeImage> images;
	FrameBuffer::MultiviewAttachment attach=nullptr;
	FrameBuffer *layered=nullptr,*primary[2]={},*resolve[2]={},*gui[2]={};
	FrameBuffer *savedPrimary=nullptr,*savedResolve=nullptr,*savedGui=nullptr;
	GLuint color=0,depth=0,ssao=0;
	int width=0,height=0,colorFormat=0,depthFormat=0,aoSize=0,currentEye=0;
	bool active=false;
	void AddImage(idImage *image);
	void DestroyTargets();
	static void DeleteTexture(GLuint &texture){
		if(texture && texture!=GLuint(idImage::TEXTURE_NOT_LOADED)){
			qglDeleteTextures(1,&texture);
			for(auto &tmu:backEnd.glState.tmu){
				if(tmu.current2DMap==texture)tmu.current2DMap=-1;
				if(tmu.currentCubeMap==texture)tmu.currentCubeMap=-1;
			}
		}
		texture=0;
	}
	GLuint ArrayTexture(GLenum format,int w,int h){
		GLuint texture=0;qglGenTextures(1,&texture);qglBindTexture(GL_TEXTURE_2D_ARRAY,texture);
		qglTexStorage3D(GL_TEXTURE_2D_ARRAY,1,format,w,h,2);
		qglTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
		qglTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
		qglTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
		qglTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
		return texture;
	}
};

MobileMultiviewTargets::MobileMultiviewTargets():impl(new Impl){}
MobileMultiviewTargets::~MobileMultiviewTargets()=default;

void MobileMultiviewTargets::Impl::AddImage(idImage *image){
	auto found=images.find(image);
	if(found!=images.end()&&found->second.revision==image->contentRevision)return;
	if(found!=images.end()){
		DeleteTexture(found->second.eye[1].texture);images.erase(found);
		for(int eye=0;eye<2;++eye){if(resolve[eye])resolve[eye]->Destroy();if(gui[eye])gui[eye]->Destroy();}
	}
	auto &entry=images[image];entry.eye[0].Read(image);entry.revision=image->contentRevision;
	// A capture may be sampled before its first update (feedback materials).
	// Give the second eye its own initial contents rather than a white fallback.
	if(image->texnum!=GLuint(idImage::TEXTURE_NOT_LOADED)&&image->uploadWidth>0&&image->uploadHeight>0){
		ImageState &right=entry.eye[1];right=entry.eye[0];
		GL_SelectTexture(15);image->Bind();GLint swizzle[4],minFilter,magFilter,wrapS,wrapT;
		for(int c=0;c<4;++c)qglGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_R+c,&swizzle[c]);
		qglGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,&minFilter);
		qglGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,&magFilter);
		qglGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,&wrapS);
		qglGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,&wrapT);
		// Captures/resolves redefine storage with glTexImage2D. Their clones must
		// stay mutable too; immutable glTexStorage2D would reject the next resize.
		image->texnum=GLuint(idImage::TEXTURE_NOT_LOADED);
		assert(image->AsScratch());image->AsScratch()->GenerateAttachment(right.width,right.height,right.format,minFilter,wrapS);
		right.Read(image);entry.eye[0].Write(image);
		qglTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,magFilter);
		qglTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,wrapT);
		for(int c=0;c<4;++c)qglTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_R+c,swizzle[c]);
		R_QuestCopyImage(image->texnum,GL_TEXTURE_2D,right.texture,GL_TEXTURE_2D,right.width,right.height,0);
	}
}

void MobileMultiviewTargets::Impl::DestroyTargets(){
	if(layered)layered->Destroy();
	for(int eye=0;eye<2;++eye){
		if(primary[eye])primary[eye]->Destroy();
		if(resolve[eye])resolve[eye]->Destroy();
		if(gui[eye])gui[eye]->Destroy();
	}
	DeleteTexture(color);DeleteTexture(depth);DeleteTexture(ssao);
	width=height=aoSize=0;
}

bool MobileMultiviewTargets::BeginFrame(const emptyCommand_t *commands){
#ifdef TDM_VR_OPENXR
	if(!r_questMultiview.GetBool()||r_multiSamples.GetInteger()>1)return false;
	auto &s=*impl;
	if(!s.attach){
		const char *extensions=(const char *)qglGetString(GL_EXTENSIONS);
		if(!extensions||!strstr(extensions,"GL_OVR_multiview2"))return false;
		s.attach=(FrameBuffer::MultiviewAttachment)eglGetProcAddress("glFramebufferTextureMultiviewOVR");
		copyImage=(decltype(copyImage))eglGetProcAddress("glCopyImageSubData");
		GLint maxViews=0;qglGetIntegerv(0x9631,&maxViews);
		if(!s.attach||!copyImage||maxViews<2){s.attach=nullptr;return false;}
	}
	s.savedPrimary=frameBuffers->primaryFbo;s.savedResolve=frameBuffers->resolveFbo;s.savedGui=frameBuffers->guiFbo;
	// Regenerate the ordinary FBOs before any later non-multiview frame uses
	// image names whose storage was replaced by a stereo resolve or capture.
	s.savedPrimary->Destroy();s.savedResolve->Destroy();s.savedGui->Destroy();
	const bool resized=s.width!=frameBuffers->renderWidth||s.height!=frameBuffers->renderHeight||
		s.colorFormat!=int(frameBuffers->colorFormat)||s.depthFormat!=int(frameBuffers->depthStencilFormat);
	if(resized){
		s.DestroyTargets();s.width=frameBuffers->renderWidth;s.height=frameBuffers->renderHeight;
		s.colorFormat=frameBuffers->colorFormat;s.depthFormat=frameBuffers->depthStencilFormat;
		GL_SelectTexture(15);s.color=s.ArrayTexture(s.colorFormat,s.width,s.height);s.depth=s.ArrayTexture(s.depthFormat,s.width,s.height);
	}
	const int aoSize=r_ssao.GetBool()?s.width:1;
	if(!s.ssao||s.aoSize!=aoSize){
		Impl::DeleteTexture(s.ssao);s.aoSize=aoSize;GL_SelectTexture(15);
		s.ssao=s.ArrayTexture(GL_RGBA8,aoSize,r_ssao.GetBool()?s.height:1);
	}
	std::set<idImage *> wanted={globalImages->currentRenderImage,globalImages->currentDepthImage,globalImages->guiRenderImage};
	for(auto *cmd=commands;cmd;cmd=(const emptyCommand_t *)cmd->next){
		if(cmd->commandId==RC_COPY_RENDER){auto *image=((const copyRenderCommand_t *)cmd)->image;if(image)wanted.insert(image);}
	}
	for(auto it=s.images.begin();it!=s.images.end();){
		if(!wanted.count(it->first)){Impl::DeleteTexture(it->second.eye[1].texture);it=s.images.erase(it);}else ++it;
	}
	for(auto *image:wanted)s.AddImage(image);
	if(!s.layered)s.layered=frameBuffers->CreateFromGenerator("quest multiview primary",[&s](FrameBuffer *fb){
		fb->Init(s.width,s.height,0);fb->AddMultiviewTextures(s.color,s.depth,s.attach);
	});
	for(int eye=0;eye<2;++eye){
		if(!s.primary[eye])s.primary[eye]=frameBuffers->CreateFromGenerator(va("quest primary eye %d",eye),[&s,eye](FrameBuffer *fb){
			fb->Init(s.width,s.height,0);fb->AddArrayLayer(s.color,s.depth,eye);
		});
		if(!s.resolve[eye])s.resolve[eye]=frameBuffers->CreateFromGenerator(va("quest resolve eye %d",eye),[&s](FrameBuffer *fb){
			fb->Init(s.width,s.height,0);
			globalImages->currentRenderImage->GenerateAttachment(s.width,s.height,s.colorFormat,GL_LINEAR);
			fb->AddColorRenderTexture(0,globalImages->currentRenderImage);
			globalImages->currentDepthImage->GenerateAttachment(s.width,s.height,s.depthFormat,GL_NEAREST);
			globalImages->currentDepthImage->Bind();GLint swizzle[]={GL_RED,GL_RED,GL_RED,GL_RED};
			for(int c=0;c<4;++c)qglTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_R+c,swizzle[c]);
			fb->AddDepthRenderTexture(globalImages->currentDepthImage);
		});
		if(!s.gui[eye])s.gui[eye]=frameBuffers->CreateFromGenerator(va("quest gui eye %d",eye),[&s](FrameBuffer *fb){
			fb->Init(glConfig.vidWidth,glConfig.vidHeight,0);
			globalImages->guiRenderImage->GenerateAttachment(glConfig.vidWidth,glConfig.vidHeight,s.colorFormat,GL_NEAREST);
			fb->AddColorRenderTexture(0,globalImages->guiRenderImage);
		});
	}
	s.active=true;s.currentEye=0;SelectEye(0);s.layered->Bind();
	return true;
#else
	return false;
#endif
}

void MobileMultiviewTargets::SelectEye(int eye){
	auto &s=*impl;assert(s.active&&eye>=0&&eye<2);
	if(s.currentEye!=eye){
		for(auto &pair:s.images){pair.second.eye[s.currentEye].Read(pair.first);pair.second.eye[eye].Write(pair.first);}
		s.currentEye=eye;
	}
	frameBuffers->primaryFbo=s.primary[eye];frameBuffers->resolveFbo=s.resolve[eye];frameBuffers->guiFbo=s.gui[eye];
}

void MobileMultiviewTargets::BindScene(){
	assert(impl->active);
	// Stages rebind currentRenderFbo on entry. Selecting only the GL binding
	// would let them switch back to a one-eye layer before a two-view draw.
	frameBuffers->currentRenderFbo=impl->layered;
	impl->layered->Bind();
}

void MobileMultiviewTargets::EndFrame(){
	auto &s=*impl;if(!s.active)return;SelectEye(0);
	for(auto &pair:s.images)pair.second.revision=pair.first->contentRevision;
	frameBuffers->primaryFbo=s.savedPrimary;frameBuffers->resolveFbo=s.savedResolve;frameBuffers->guiFbo=s.savedGui;
	s.active=false;
}
void MobileMultiviewTargets::Shutdown(){
	EndFrame();auto &s=*impl;s.DestroyTargets();
	for(auto &pair:s.images)Impl::DeleteTexture(pair.second.eye[1].texture);
	s.images.clear();
	// FrameBufferManager deletes its registered objects during renderer shutdown.
	// A subsequent OpenXR initialization must register fresh targets, rather than
	// dereferencing pointers retained from the previous GL context.
	s.layered=nullptr;s.attach=nullptr;
	for(int eye=0;eye<2;++eye)s.primary[eye]=s.resolve[eye]=s.gui[eye]=nullptr;
}
FrameBuffer *MobileMultiviewTargets::Layered()const{return impl->layered;}
GLuint MobileMultiviewTargets::SSAOTexture()const{return impl->ssao;}
bool MobileMultiviewTargets::IsPerEyeImage(idImage *image)const{return impl->images.count(image)!=0;}
