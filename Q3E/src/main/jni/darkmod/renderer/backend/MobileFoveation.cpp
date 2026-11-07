/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#include "precompiled.h"
#include "renderer/tr_local.h"
#include "renderer/backend/MobileFoveation.h"
#include "renderer/backend/MobileVrsProbe.h"
#include "renderer/backend/FrameBuffer.h"
#include "renderer/backend/FrameBufferManager.h"
#ifdef TDM_VR_OPENXR
#include <EGL/egl.h>
#endif
#include <cstring>
#include <cstdio>

idCVar vr_foveation("vr_foveation","0",CVAR_RENDERER|CVAR_INTEGER,
	"Fixed foveation using hardware shading rates for peripheral lighting. Eye resolution and depth/stencil stay unchanged. "
	"0 = off, 1 = low, 2 = medium, 3 = high. Set in the launcher",0,3);

namespace {
// EXT and QCOM use the same rate enumerants. Keep these local so older NDK
// headers can compile the extension path without exporting vendor GL state.
const unsigned RATE_1X1=0x96A6, RATE_1X2=0x96A7, RATE_2X2=0x96A9, RATE_4X2=0x96AC;
const unsigned ATTACHMENT=0x96D1, KEEP=0x96D2, REPLACE=0x96D3;
bool HasExtension(const char *list,const char *name) {
	if(!list)return false;
	const size_t length=strlen(name);
	for(const char *p=list;(p=strstr(p,name));p+=length)
		if((p==list||p[-1]==' ')&&(p[length]==' '||!p[length]))return true;
	return false;
}
}

void MobileFoveation::Init() {
	if(initialized)return;
	initialized=true;
#ifdef TDM_VR_OPENXR
	const char *extensions=(const char *)qglGetString(GL_EXTENSIONS);
	if(HasExtension(extensions,"GL_EXT_fragment_shading_rate") &&
		HasExtension(extensions,"GL_EXT_fragment_shading_rate_attachment")) {
		rate=(RateFunction)eglGetProcAddress("glShadingRateEXT");
		combine=(CombinerFunction)eglGetProcAddress("glShadingRateCombinerOpsEXT");
		attachment=(AttachmentFunction)eglGetProcAddress("glFramebufferShadingRateEXT");
		query=(QueryFunction)eglGetProcAddress("glGetFragmentShadingRatesEXT");
		GLint minW=0,minH=0,maxW=0,maxH=0;
		qglGetIntegerv(0x96D7,&minW);qglGetIntegerv(0x96D9,&minH);
		qglGetIntegerv(0x96D8,&maxW);qglGetIntegerv(0x96DA,&maxH);
		const int minimum=std::max(minW,minH), maximum=std::min(maxW,maxH);
		if(rate&&combine&&attachment&&query&&minimum>0&&minimum<=maximum)
			tileSize=std::max(minimum,std::min(16,maximum));
		else rate=nullptr;
	}
	if(!rate&&HasExtension(extensions,"GL_QCOM_shading_rate")) {
		rate=(RateFunction)eglGetProcAddress("glShadingRateQCOM");qcom=rate!=nullptr;
	}
	currentRate=RATE_1X1;
	if(rate){const auto probe=R_QuestProbeVrs(rate);gpuWidth=probe.width;gpuHeight=probe.height;}
	common->Printf("Quest foveation: %s; full-resolution depth/stencil, no foveated colour textures\n",
		!rate?"hardware shading rate unavailable":qcom?"QCOM conservative draw rates":"EXT spatial shading-rate map");
#endif
}

void MobileFoveation::BeginFrame() {
	EndLighting();reducedDraws=fullDraws=mappedPasses=attachmentUpdates=0;
}

void MobileFoveation::SetRate(unsigned value) {
	if(currentRate!=value){rate(value);currentRate=value;}
}

bool MobileFoveation::PrepareMap(int samples,FrameBuffer *target) {
	if(qcom||mapFailed||tileSize<=0)return false;
	const bool changed=!mapTexture||cachedLevel!=level||cachedSamples!=samples||
		memcmp(centres,cachedCentres,sizeof(centres))||
		memcmp(&viewport,&cachedViewport,sizeof(viewport));
	if(changed) {
		unsigned supported[16]={};int count=0;
		query(std::max(1,samples),16,&count,supported);
		auto supports=[&](unsigned value){return std::find(supported,supported+std::min(count,16),value)!=supported+std::min(count,16);};
		if(!supports(RATE_1X1)||!supports(RATE_1X2)||!supports(RATE_2X2))return false;
		// Coordinates of the shading-rate image are framebuffer absolute, not
		// viewport-relative. Include any viewport offset and round up edge tiles.
		const int w=(viewport.x2+tileSize-1)/tileSize,h=(viewport.y2+tileSize-1)/tileSize;
		std::vector<unsigned char> pixels(w*h);
		for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
			const MobileRenderPlan::Rect tile={x*tileSize,y*tileSize,(x+1)*tileSize,(y+1)*tileSize};
			int density=MobileRenderPlan::FoveationDensity(tile,viewport,centres,level);
			if(density==3&&!supports(RATE_4X2))density=2;
			pixels[y*w+x]=density==3?9:density==2?5:density==1?1:0;
		}
		const int unit=backEnd.glState.currenttmu;GL_SelectTexture(15);
		GLint bound=0,unpack[5]={};qglGetIntegerv(GL_TEXTURE_BINDING_2D,&bound);
		const unsigned parameters[]={GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,GL_UNPACK_SKIP_PIXELS,GL_UNPACK_SKIP_ROWS,GL_PIXEL_UNPACK_BUFFER_BINDING};
		for(int i=0;i<5;++i)qglGetIntegerv(parameters[i],&unpack[i]);
		if(mapTexture&&(w!=mapWidth||h!=mapHeight)){
			qglDeleteTextures(1,&mapTexture);mapTexture=0;preparedTargets.clear();
		}
		if(!mapTexture){qglGenTextures(1,&mapTexture);qglBindTexture(GL_TEXTURE_2D,mapTexture);
			qglTexStorage2D(GL_TEXTURE_2D,1,GL_R8UI,w,h);
			qglTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);qglTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
			qglTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);qglTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
		}else qglBindTexture(GL_TEXTURE_2D,mapTexture);
		qglBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
		for(int i=0;i<4;++i)qglPixelStorei(parameters[i],i==0?1:0);
		qglTexSubImage2D(GL_TEXTURE_2D,0,0,0,w,h,GL_RED_INTEGER,GL_UNSIGNED_BYTE,pixels.data());
		for(int i=0;i<4;++i)qglPixelStorei(parameters[i],unpack[i]);
		qglBindBuffer(GL_PIXEL_UNPACK_BUFFER,unpack[4]);qglBindTexture(GL_TEXTURE_2D,bound);GL_SelectTexture(unit);
		mapWidth=w;mapHeight=h;cachedLevel=level;cachedSamples=samples;
		cachedViewport=viewport;memcpy(cachedCentres,centres,sizeof(centres));
	}
	auto &state=preparedTargets[target];
	if(state.generation!=target->Generation()||state.texture!=mapTexture){
		attachment(GL_DRAW_FRAMEBUFFER,ATTACHMENT,mapTexture,0,1,tileSize,tileSize);
		++attachmentUpdates;
		if(qglCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) {
			attachment(GL_DRAW_FRAMEBUFFER,ATTACHMENT,0,0,1,tileSize,tileSize);mapFailed=true;
			common->Warning("Quest shading-rate map rejected; retaining full-rate lighting");return false;
		}
		state.generation=target->Generation();state.texture=mapTexture;
	}
	return true;
}

void MobileFoveation::PrepareTarget(const MobileRenderPlan::Rect &area,const float *left,
	const float *right,int samples,bool mainView,FrameBuffer *target) {
	EndLighting();level=std::max(0,std::min(3,vr_foveation.GetInteger()));
	prepared=false;preparedTarget=target;
	if(!level||!mainView||area.Empty())return;
	Init();if(!rate)return;
	viewport=area;
	for(int eye=0;eye<2;++eye){const float *p=eye&&right?right:left;
		centres[eye][0]=-p[8];centres[eye][1]=-p[9];}
	if(!qcom){combine(KEEP,KEEP);target->Bind();prepared=PrepareMap(samples,target);}
	else prepared=true;
}

void MobileFoveation::BeginLighting(const MobileRenderPlan::Rect &area,const float *left,
	const float *right,int samples,bool mainView) {
	EndLighting();
	if(!mainView||vr_foveation.GetInteger()<=0||!prepared||preparedTarget!=frameBuffers->currentRenderFbo)return;
	drawing=true;mapped=!qcom;
	if(mapped){combine(KEEP,REPLACE);++mappedPasses;}
	// EXT without a usable spatial map cannot provide conservative draw-rate
	// fallback unless the matching shader extension is active. Keep it full rate.
	if(!qcom&&!mapped)drawing=false;
}

void MobileFoveation::Draw(const MobileRenderPlan::Rect &bounds) {
	if(!drawing)return;
	if(mapped)return;
	const int density=MobileRenderPlan::FoveationDensity(bounds,viewport,centres,level);
	SetRate(density==3?RATE_4X2:density==2?RATE_2X2:density==1?RATE_1X2:RATE_1X1);
	if(density)++reducedDraws;else ++fullDraws;
}

void MobileFoveation::EndLighting() {
	if(!drawing)return;
	if(mapped)combine(KEEP,KEEP);
	SetRate(RATE_1X1);drawing=mapped=false;
}

void MobileFoveation::Shutdown() {
	EndLighting();if(mapTexture)qglDeleteTextures(1,&mapTexture);
	*this=MobileFoveation();
}

void MobileFoveation::GetDiagnostics(char *buffer,int size) const {
	const char *state=vr_foveation.GetInteger()<=0?"OFF":!rate?"UNAVAILABLE":mappedPasses?"SPATIAL VRS":
		reducedDraws?"DRAW VRS":"IDLE: full-rate draws";
	const int total=reducedDraws+fullDraws;
	char gpu[64];
	if(gpuWidth&&gpuHeight)snprintf(gpu,sizeof(gpu),"2x2 request -> %dx%d",gpuWidth,gpuHeight);
	else snprintf(gpu,sizeof(gpu),"unverified");
	if(mappedPasses)snprintf(buffer,size,"Foveation: %s (level %d) | spatial passes %d | target updates %d\n"
		"VRS GPU probe: %s\n",state,vr_foveation.GetInteger(),mappedPasses,attachmentUpdates,gpu);
	else snprintf(buffer,size,"Foveation: %s (level %d) | coarse draws %d/%d (%.1f%%)\n"
		"VRS GPU probe: %s\n",state,vr_foveation.GetInteger(),reducedDraws,total,total?100.0*reducedDraws/total:0.0,gpu);
}
