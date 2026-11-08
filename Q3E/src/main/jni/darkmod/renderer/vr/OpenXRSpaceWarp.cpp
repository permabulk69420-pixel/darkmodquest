/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#include "precompiled.h"
#ifdef TDM_VR_OPENXR
#include "renderer/tr_local.h"
#include "renderer/vr/OpenXRSpaceWarp.h"
#include "renderer/vr/OpenXRSwapchain.h"
#include "renderer/vr/SpaceWarpMath.h"
#include "renderer/backend/FrameBuffer.h"
#include "renderer/backend/FrameBufferManager.h"
#include "renderer/backend/GLSLProgramManager.h"
#include "renderer/backend/MobileUniform.h"
#include "renderer/backend/MobileStereoView.h"
#include "renderer/backend/stages/MobileLightingStage.h"
#include <unordered_map>
#include <map>
#include <limits>

idCVar vr_spaceWarp("vr_spaceWarp","1",CVAR_RENDERER|CVAR_BOOL|CVAR_INIT,
	"Meta Application SpaceWarp: submit scene motion vectors and depth for half-rate frame synthesis; set in launcher");

namespace {
struct MotionUniforms : GLSLUniformGroup {
	UNIFORM_GROUP_DEF(MotionUniforms)
	DEFINE_MOBILE_UNIFORM(mat4,currentMvp)
	DEFINE_MOBILE_UNIFORM(mat4,previousMvp)
	DEFINE_MOBILE_UNIFORM(mat4,textureMatrix)
	DEFINE_MOBILE_UNIFORM(sampler,texture)
	DEFINE_MOBILE_UNIFORM(float,alphaTest)
	DEFINE_MOBILE_UNIFORM(float,alpha)
};
struct MeshKey {
	uint64_t entity, geometry, topology;
	const idMaterial *material;
	bool operator==(const MeshKey &b) const {
		return entity==b.entity && geometry==b.geometry && topology==b.topology && material==b.material;
	}
};
struct MeshHash {
	size_t operator()(const MeshKey &k) const {
		return size_t(k.entity ^ (k.geometry*1099511628211ull) ^ (k.topology*16777619ull) ^ uintptr_t(k.material));
	}
};
struct MeshHistory {
	float model[16];
	vertCacheHandle_t vertices = NO_CACHE;
	uint64_t frame = 0;
	int numVerts = 0, numIndexes = 0;
};
bool Eligible(const drawSurf_t *surf, const viewDef_t *view) {
	if (!surf || !surf->space || !surf->material || !surf->numIndexes || !surf->numVerts ||
		!surf->ambientCache.IsValid() || !surf->indexCache.IsValid()) return false;
	const idMaterial *material=surf->material;
	if (!material->IsDrawn() || material->Coverage()==MC_TRANSLUCENT || material->GetSort()==SS_PORTAL_SKY) return false;
	if (R_QuestFogDisabled(view) && ((surf->dsFlags&DSF_ATMOSPHERIC_FOG) || material->IsAtmosphericFog())) return false;
	for (int i=0;i<material->GetNumStages();++i)
		if (surf->IsStageEnabled(material->GetStage(i))) return true;
	return false;
}
}

struct OpenXRSpaceWarp::Impl {
	bool initialized=false, ready=false, havePrevious=false, skip=false, chained=false, lastAccepted=false;
	int width=0,height=0,draws=0,animated=0,fallbacks=0;
	const char *reason="not initialized";
	XrResult lastResult=XR_SUCCESS;
	unsigned acceptedFrames=0;
	XrGlSwapchain motion,depth;
	FrameBuffer::MultiviewAttachment attach=nullptr;
	std::map<std::pair<GLuint,GLuint>,FrameBuffer *> targets;
	GLSLProgram *program=nullptr;
	GLuint vao=0;
	bool packedDepthStencil=false;
	std::unordered_map<MeshKey,MeshHistory,MeshHash> meshes;
	uint64_t serial=0,generation=0;
	XrTime previousTime=0,currentTime=0;
	XrPosef previousPose=SpaceWarpMath::Identity(),currentPose=SpaceWarpMath::Identity(),delta=SpaceWarpMath::Identity();
	float previousWorldView[2][16]={},previousProjection[2][16]={};
	MobileStereoView current;
	float nearMetres=0.1f;
	void DrawSurface(const drawSurf_t *surf, MotionUniforms *u);
};

OpenXRSpaceWarp::OpenXRSpaceWarp():impl(new Impl) {}
OpenXRSpaceWarp::~OpenXRSpaceWarp()=default;

bool OpenXRSpaceWarp::Initialize(XrSession session,int w,int h,const int64_t *formats,unsigned count) {
	Shutdown();
	auto &s=*impl;
	const char *extensions=(const char *)qglGetString(GL_EXTENSIONS);
	if (!extensions || !strstr(extensions,"GL_OVR_multiview2")) {s.reason="multiview unavailable";return false;}
	s.attach=(FrameBuffer::MultiviewAttachment)eglGetProcAddress("glFramebufferTextureMultiviewOVR");
	if (!s.attach || w<=0 || h<=0) {s.reason="invalid runtime motion size";return false;}
	auto supports=[&](int64_t format){return std::find(formats,formats+count,format)!=formats+count;};
	const int64_t depthFormat=supports(GL_DEPTH_COMPONENT24)?GL_DEPTH_COMPONENT24:
		supports(GL_DEPTH24_STENCIL8)?GL_DEPTH24_STENCIL8:0;
	if (!supports(GL_RGBA16F) || !depthFormat) {s.reason="required swapchain formats unavailable";return false;}
	s.width=w;s.height=h;s.packedDepthStencil=depthFormat==GL_DEPTH24_STENCIL8;
	if (!s.motion.Create(session,"xr SpaceWarp motion",GL_RGBA16F,w,h,false,2) ||
		!s.depth.Create(session,"xr SpaceWarp depth",depthFormat,w,h,true,2)) {
		s.motion.Destroy();s.depth.Destroy();s.reason="swapchain creation failed";return false;
	}
	s.program=programManager->LoadFromFiles("quest_spacewarp_motion","stages/mobile/spacewarp.vert.glsl","stages/mobile/spacewarp.frag.glsl");
	if (!s.program || !s.program->EnsureLinked()) {
		s.motion.Destroy();s.depth.Destroy();s.reason="motion shader failed";return false;
	}
	qglGenVertexArrays(1,&s.vao);
	s.initialized=true;s.reason="waiting for a world frame";
	XR_Log("Application SpaceWarp ready: %dx%d motion/depth, RGBA16F, two-view motion pass",w,h);
	return true;
}

void OpenXRSpaceWarp::ResetHistory() {
	auto &s=*impl;s.meshes.clear();s.havePrevious=false;s.previousTime=0;s.serial=0;s.ready=false;s.lastAccepted=false;
}

void OpenXRSpaceWarp::Shutdown() {
	auto &s=*impl;
	for (auto &entry:s.targets) {
		entry.second->Destroy();entry.second->SetGenerator([](FrameBuffer *){});
	}
	s.targets.clear();
	s.motion.Destroy();s.depth.Destroy();
	if(s.vao)qglDeleteVertexArrays(1,&s.vao);
	s.vao=0;s.program=nullptr;s.attach=nullptr;s.initialized=false;s.reason="not initialized";
	s.lastResult=XR_SUCCESS;s.acceptedFrames=0;
	ResetHistory();
}

void OpenXRSpaceWarp::BeginFrame() {
	auto &s=*impl;s.ready=s.chained=false;s.draws=s.animated=s.fallbacks=0;
	if(s.initialized)s.reason="no eligible primary world view";
	if(!vr_spaceWarp.GetBool())ResetHistory();
}

void OpenXRSpaceWarp::Impl::DrawSurface(const drawSurf_t *surf,MotionUniforms *u) {
	const MeshKey key={surf->space->mobileCacheId,surf->motionGeometryId,surf->motionTopologyId,surf->material};
	auto &old=meshes[key];
	const bool matching=havePrevious && old.frame+1==serial && old.numVerts==surf->numVerts && old.numIndexes==surf->numIndexes;
	vertCacheHandle_t previousVertices=surf->ambientCache;
	const bool oldVerticesValid=matching && old.vertices.IsValid() && old.vertices.isStatic==surf->ambientCache.isStatic &&
		(old.vertices.isStatic || ((old.vertices.frameNumber+1)&VERTCACHE_FRAME_MASK)==surf->ambientCache.frameNumber);
	if(oldVerticesValid)previousVertices=old.vertices;
	if(!surf->ambientCache.isStatic) {if(oldVerticesValid)++animated;else ++fallbacks;}
	float currentMvp[2][16],previousMvp[2][16],modelView[16];
	for(int eye=0;eye<2;++eye) {
		myGlMultMatrix(surf->space->modelMatrix,current.worldView[eye],modelView);
		myGlMultMatrix(modelView,current.projection[eye],currentMvp[eye]);
		const float *model=matching?old.model:surf->space->modelMatrix;
		myGlMultMatrix(model,havePrevious?previousWorldView[eye]:current.worldView[eye],modelView);
		myGlMultMatrix(modelView,havePrevious?previousProjection[eye]:current.projection[eye],previousMvp[eye]);
	}
	u->currentMvp.SetArray(2,currentMvp[0]);u->previousMvp.SetArray(2,previousMvp[0]);
	const GLuint currentBuffer=vertexCache.VertexBuffer(surf->ambientCache), previousBuffer=vertexCache.VertexBuffer(previousVertices);
	qglBindBuffer(GL_ARRAY_BUFFER,currentBuffer);
	qglVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(idDrawVert),(void *)(uintptr_t(vertexCache.VertexOffset(surf->ambientCache))+offsetof(idDrawVert,xyz)));
	qglVertexAttribPointer(8,2,GL_FLOAT,GL_FALSE,sizeof(idDrawVert),(void *)(uintptr_t(vertexCache.VertexOffset(surf->ambientCache))+offsetof(idDrawVert,st)));
	qglBindBuffer(GL_ARRAY_BUFFER,previousBuffer);
	qglVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(idDrawVert),(void *)(uintptr_t(vertexCache.VertexOffset(previousVertices))+offsetof(idDrawVert,xyz)));
	qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER,vertexCache.IndexBuffer(surf->indexCache));
	GL_Cull(surf->material->GetCullType());
	const bool offset=surf->material->TestMaterialFlag(MF_POLYGONOFFSET);
	if(offset) {qglEnable(GL_POLYGON_OFFSET_FILL);qglPolygonOffset(r_offsetFactor.GetFloat(),r_offsetUnits.GetFloat()*surf->material->GetPolygonOffset());}
	auto issue=[&](const shaderStage_t *stage) {
		if(stage) {
			stage->texture.image->BindToUnit(0);u->textureMatrix.Set(surf->GetTextureMatrix(stage));
			u->alphaTest.Set(surf->shaderRegisters[stage->alphaTestRegister]);u->alpha.Set(surf->GetStageColor(stage)[3]);
		}else {u->alphaTest.Set(-1.0f);u->alpha.Set(1.0f);}
		qglDrawElements(GL_TRIANGLES,surf->numIndexes,GL_INDEX_TYPE,(void *)uintptr_t(vertexCache.IndexOffset(surf->indexCache)));++draws;
	};
	bool attempted=false;
	if(surf->material->Coverage()==MC_PERFORATED) {
		for(int i=0;i<surf->material->GetNumStages();++i) {
			const auto *stage=surf->material->GetStage(i);
			if(!stage->hasAlphaTest || !surf->IsStageEnabled(stage))continue;
			attempted=true;if(surf->GetStageColor(stage)[3]>0)issue(stage);
		}
	}
	if(!attempted)issue(nullptr);
	if(offset)qglDisable(GL_POLYGON_OFFSET_FILL);
	memcpy(old.model,surf->space->modelMatrix,sizeof(old.model));old.vertices=surf->ambientCache;
	old.frame=serial;old.numVerts=surf->numVerts;old.numIndexes=surf->numIndexes;
}

bool OpenXRSpaceWarp::Render(const viewDef_t *view,const MobileStereoView &stereo,const XrPosef &appPose,XrTime time,float nearMetres) {
	auto &s=*impl;
	if(!vr_spaceWarp.GetBool() || !s.initialized)return false;
	if(!s.program->EnsureLinked()) {s.reason="motion shader failed";ResetHistory();return false;}
	const uint64_t generation=vertexCache.StaticRevision();
	if(s.generation!=generation)ResetHistory();
	s.generation=generation;s.current=stereo;s.currentTime=time;s.currentPose=appPose;s.nearMetres=nearMetres;
	s.delta=s.havePrevious?SpaceWarpMath::Delta(s.previousPose,appPose):SpaceWarpMath::Identity();
	s.skip=!s.havePrevious || SpaceWarpMath::CameraCut(s.delta,double(time-s.previousTime)*1e-9);
	if(s.skip) {s.meshes.clear();s.havePrevious=false;}
	++s.serial;
	// The scene projection deliberately nudges far depth for GL_EQUAL rendering.
	// This independent pass uses a true infinite far plane, so the submitted
	// nearZ/farZ describe the actual depth values, in metres, exactly.
	for(int eye=0;eye<2;++eye) {s.current.projection[eye][10]=-1.0f;s.current.projection[eye][14]=-2.0f*nearMetres*XR_MetresToGameUnits;}
	if(!s.motion.Acquire() || !s.depth.Acquire()) {
		s.motion.Release();s.depth.Release();s.reason="swapchain acquire failed";ResetHistory();return false;
	}
	FrameBuffer *savedRead=frameBuffers->activeFbo,*savedDraw=frameBuffers->activeDrawFbo;
	GLint savedVao=0,savedBuffer=0;qglGetIntegerv(GL_VERTEX_ARRAY_BINDING,&savedVao);qglGetIntegerv(GL_ARRAY_BUFFER_BINDING,&savedBuffer);
	struct Restore {
		Impl &state;FrameBuffer *read,*draw;GLint vao,buffer;
		~Restore() {
			GLSLProgram::Deactivate();qglBindVertexArray(vao);qglBindBuffer(GL_ARRAY_BUFFER,buffer);
			qglEnable(GL_STENCIL_TEST);GL_Cull(CT_FRONT_SIDED);GL_State(GLS_DEFAULT);
			read->Bind();draw->BindDraw();
			// Compositor inputs retain depth. Also release on engine exceptions.
			state.motion.Release();state.depth.Release();
		}
	} restore{s,savedRead,savedDraw,savedVao,savedBuffer};
	const auto key=std::make_pair(s.motion.CurrentTexture(),s.depth.CurrentTexture());
	auto &target=s.targets[key];
	if(!target) target=frameBuffers->CreateFromGenerator(va("xr SpaceWarp %u %u",key.first,key.second),[&s,key](FrameBuffer *fb){
		fb->Init(s.width,s.height,0);fb->AddMultiviewTextures(key.first,key.second,s.attach,s.packedDepthStencil);
	});
	target->Bind();
	const bool complete=qglCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
	GLenum error=GL_NO_ERROR;
	if(complete) {
		GL_State(GLS_DEPTHFUNC_LESS);qglEnable(GL_DEPTH_TEST);qglDisable(GL_STENCIL_TEST);qglEnable(GL_SCISSOR_TEST);
		qglViewport(0,0,s.width,s.height);qglScissor(0,0,s.width,s.height);
		qglDepthRangef(0,1);qglClearDepthf(1);qglClearColor(0,0,0,1);qglClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
		qglBindVertexArray(s.vao);qglEnableVertexAttribArray(0);qglEnableVertexAttribArray(1);qglEnableVertexAttribArray(8);
		s.program->Activate();auto *u=s.program->GetUniformGroup<MotionUniforms>();u->texture.Set(0);
		for(int i=0;i<view->numDrawSurfs;++i)if(Eligible(view->drawSurfs[i],view))s.DrawSurface(view->drawSurfs[i],u);
		error=qglGetError();
	}
	for(auto it=s.meshes.begin();it!=s.meshes.end();) {
		if(it->second.frame+2<s.serial)it=s.meshes.erase(it);else ++it;
	}
	s.ready=complete && error==GL_NO_ERROR;
	s.reason=!complete?"motion framebuffer incomplete":error!=GL_NO_ERROR?"motion pass GL error":s.skip?"warming up / camera cut":"motion and depth ready";
	if(error!=GL_NO_ERROR)XR_Log("SpaceWarp motion GL error: 0x%x",unsigned(error));
	if(!s.ready)ResetHistory();
	return s.ready;
}

bool OpenXRSpaceWarp::Chain(XrCompositionLayerProjectionView (&views)[2],XrCompositionLayerSpaceWarpInfoFB (&info)[2]) const {
	auto &s=*impl;
	if(!vr_spaceWarp.GetBool() || !s.ready)return false;
	for(int eye=0;eye<2;++eye) {
		info[eye]={XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB};
		info[eye].layerFlags=s.skip?XR_COMPOSITION_LAYER_SPACE_WARP_INFO_FRAME_SKIP_BIT_FB:0;
		info[eye].motionVectorSubImage=s.motion.CurrentSubImage(eye);
		info[eye].depthSubImage=s.depth.CurrentSubImage(eye);
		info[eye].appSpaceDeltaPose=s.delta;
		info[eye].minDepth=0;info[eye].maxDepth=1;info[eye].nearZ=s.nearMetres;info[eye].farZ=std::numeric_limits<float>::infinity();
		info[eye].next=views[eye].next;views[eye].next=&info[eye];
	}
	s.chained=true;
	return true;
}

void OpenXRSpaceWarp::CompleteFrame(bool submitted,XrResult result) {
	auto &s=*impl;
	if(!submitted || !s.ready || !s.chained) {ResetHistory();return;}
	s.lastResult=result;
	if(XR_FAILED(result)) {s.reason="motion/depth submission rejected";ResetHistory();return;}
	s.previousTime=s.currentTime;s.previousPose=s.currentPose;
	memcpy(s.previousWorldView,s.current.worldView,sizeof(s.previousWorldView));
	memcpy(s.previousProjection,s.current.projection,sizeof(s.previousProjection));
	s.havePrevious=true;
	s.lastAccepted=true;
	++s.acceptedFrames;
}

void OpenXRSpaceWarp::GetDiagnostics(char *buffer,int size,bool available) const {
	const auto &s=*impl;
	const char *state=!vr_spaceWarp.GetBool()?"OFF":!available?"UNSUPPORTED by runtime":!s.initialized?"UNAVAILABLE":XR_FAILED(s.lastResult)?"REJECTED by runtime":s.ready?(s.lastAccepted?"ACTIVE (runtime accepted)":"PREPARING"):"WAITING";
	snprintf(buffer,size,"Application SpaceWarp: %s | accepted %u | result %d\nMotion/depth: %dx%d x2 | draws %d | animated %d | new meshes %d | %s\n",
		state,s.acceptedFrames,int(s.lastResult),s.width,s.height,s.draws,s.animated,s.fallbacks,
		!vr_spaceWarp.GetBool()?"launcher disabled":!available?"XR_FB_space_warp missing":s.reason);
}
#endif
