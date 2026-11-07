/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#include "precompiled.h"
#include "renderer/backend/stages/MobileLightingStage.h"
#include "renderer/backend/MobileRenderPlan.h"
#include "renderer/backend/MobileShadowCache.h"
#include "renderer/backend/stages/InteractionStage.h"
#include "renderer/backend/stages/AmbientOcclusionStage.h"
#include "renderer/backend/GLSLProgramManager.h"
#include "renderer/backend/GLSLUniforms.h"
#include "renderer/backend/MobileUniform.h"
#include "renderer/backend/MobileStereoView.h"
#include "renderer/backend/MobileMultiviewTargets.h"
#include "renderer/backend/FrameBufferManager.h"
#include "renderer/backend/FrameBuffer.h"
#include "renderer/vr/OpenXRBackend.h"
#include "renderer/QuestRenderDiagnostics.h"
#include <unordered_map>
#include <tuple>
#include <map>

namespace {
#include "glprogs/tdm_constants_shared.glsl"
}

idCVar vr_rendererDiagnostics( "vr_rendererDiagnostics", "0", CVAR_RENDERER | CVAR_BOOL,
	"Show live renderer settings and measured stereo rendering activity on the VR HUD; set in the launcher" );
QuestRenderCounters questRenderCounters;

idCVar r_questRenderer( "r_questRenderer", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_INIT,
	"Quest forward renderer: cached stereo shadows and up to eight lights per surface draw. Set at launch; 0 uses the legacy renderer." );
idCVar r_questShadowAtlasSize( "r_questShadowAtlasSize", "2048", CVAR_RENDERER | CVAR_INTEGER | CVAR_INIT,
	"Shared stereo depth atlas size in pixels", 512, 4096 );
idCVar r_questShadowFaceSize( "r_questShadowFaceSize", "256", CVAR_RENDERER | CVAR_INTEGER | CVAR_INIT,
	"Maximum shadow face resolution; smaller lights use smaller pages", 32, 512 );
idCVar r_questShowRenderStats( "r_questShowRenderStats", "0", CVAR_RENDERER | CVAR_BOOL,
	"Log shared shadow draws and two-eye lighting batch counts every 120 frames" );
idCVar r_questLightScissor( "r_questLightScissor", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE,
	"Clip mobile lighting, blend lights and volumetrics to bounds projected for the active eye" );
idCVar r_questShadowCache( "r_questShadowCache", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE,
	"Retain unchanged shadow faces across frames; update changed static and dynamic casters immediately" );
idCVar vr_fog( "vr_fog", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE,
	"Enable atmospheric fog lights, particles and sheets in VR. Off by default. Volumetric lighting is controlled separately." );

bool R_UseMobileLighting() {
#ifdef TDM_VR_OPENXR
	return r_questRenderer.GetBool() && vrBackend->IsActive();
#else
	return false;
#endif
}

bool R_QuestFogDisabled( const viewDef_t *view ) {
#ifdef TDM_VR_OPENXR
	return vrBackend->IsActive() && !vr_fog.GetBool() && view && !view->IsLightGem();
#else
	return false;
#endif
}

bool R_UseQuestLightScissors() { return R_UseMobileLighting() && r_questLightScissor.GetBool(); }

bool R_QuestSkipFogSurface( const viewDef_t *view, const srfTriangles_t *tri, const idMaterial *material ) {
	return R_QuestFogDisabled(view) && (tri->atmosphericFog || material->IsAtmosphericFog());
}

bool R_QuestSkipFogVolume( const viewDef_t *view, const idMaterial *material ) {
	return R_QuestFogDisabled(view) && (material->IsFogLight() || material->IsAtmosphericFog());
}

MobileRenderPlan::Rect RB_QuestViewViewport( const viewDef_t *view ) {
	const int w = frameBuffers->activeFbo->Width(), h = frameBuffers->activeFbo->Height();
	const int x = (tr.viewportOffset[0] + view->viewport.x1) * w / glConfig.vidWidth;
	const int y = (tr.viewportOffset[1] + view->viewport.y1) * h / glConfig.vidHeight;
	return { x, y, x + view->viewport.GetWidth() * w / glConfig.vidWidth,
		y + view->viewport.GetHeight() * h / glConfig.vidHeight };
}

MobileRenderPlan::Rect RB_QuestLightScissor( const viewDef_t *view, const viewLight_t *light,
	const MobileRenderPlan::Rect &viewport ) {
	if ( !R_UseQuestLightScissors() || !light->frustumTris ) return viewport;
	const idBounds &bounds = light->frustumTris->bounds;
	return MobileRenderPlan::ProjectScissor(bounds[0].ToFloatPtr(), bounds[1].ToFloatPtr(),
		view->worldSpace.modelViewMatrix, view->projectionMatrix, viewport);
}

namespace {
const int AMBIENT_LIGHT = 0x100000;
const int MAX_BATCH_LIGHTS = 8;
int LightingVariant(int count) { return count<=1 ? 0 : count<=2 ? 1 : count<=4 ? 2 : 3; }

struct LightingUniforms : GLSLUniformGroup {
	UNIFORM_GROUP_DEF( LightingUniforms )
	DEFINE_MOBILE_UNIFORM( mat4, modelMatrix )
	DEFINE_MOBILE_UNIFORM( mat4, modelViewMatrix )
	DEFINE_MOBILE_UNIFORM( mat4, projectionMatrix )
	DEFINE_MOBILE_UNIFORM( vec3, viewOrigin )
	DEFINE_MOBILE_UNIFORM( vec4, clipPlane )
	DEFINE_MOBILE_UNIFORM( vec2, renderResolution )
	DEFINE_MOBILE_UNIFORM( vec2, ambientParams )
	DEFINE_MOBILE_UNIFORM( int, lightCount )
	DEFINE_MOBILE_UNIFORM( int, shadowFilter )
	DEFINE_MOBILE_UNIFORM( int, flags )
	DEFINE_MOBILE_UNIFORM( int, vertexColorMode )
	DEFINE_MOBILE_UNIFORM( vec4, bumpMatrix )
	DEFINE_MOBILE_UNIFORM( vec4, diffuseMatrix )
	DEFINE_MOBILE_UNIFORM( vec4, specularMatrix )
	DEFINE_MOBILE_UNIFORM( vec4, parallaxMatrix )
	DEFINE_MOBILE_UNIFORM( vec2, parallaxHeightScale )
	DEFINE_MOBILE_UNIFORM( ivec3, parallaxIterations )
	DEFINE_MOBILE_UNIFORM( float, parallaxGrazingAngle )
	DEFINE_MOBILE_UNIFORM( float, parallaxShadowSoftness )
	DEFINE_MOBILE_UNIFORM( int, lightFlags )
	DEFINE_MOBILE_UNIFORM( int, lightTextureSet )
	DEFINE_MOBILE_UNIFORM( vec3, lightOrigin )
	DEFINE_MOBILE_UNIFORM( vec4, diffuseColor )
	DEFINE_MOBILE_UNIFORM( vec4, specularColor )
	DEFINE_MOBILE_UNIFORM( mat4, lightProjection )
	DEFINE_MOBILE_UNIFORM( vec4, lightTextureMatrix )
	DEFINE_MOBILE_UNIFORM( vec4, shadowPage )
	DEFINE_MOBILE_UNIFORM( vec4, shadowParams )
	DEFINE_MOBILE_UNIFORM( mat4, shadowProjection )
	DEFINE_MOBILE_UNIFORM( sampler, normalTexture )
	DEFINE_MOBILE_UNIFORM( sampler, diffuseTexture )
	DEFINE_MOBILE_UNIFORM( sampler, specularTexture )
	DEFINE_MOBILE_UNIFORM( sampler, parallaxTexture )
	DEFINE_MOBILE_UNIFORM( sampler, projection0 )
	DEFINE_MOBILE_UNIFORM( sampler, projection1 )
	DEFINE_MOBILE_UNIFORM( sampler, cube0 )
	DEFINE_MOBILE_UNIFORM( sampler, cube1 )
	DEFINE_MOBILE_UNIFORM( sampler, falloff0 )
	DEFINE_MOBILE_UNIFORM( sampler, falloff1 )
	DEFINE_MOBILE_UNIFORM( sampler, ambientDiffuse0 )
	DEFINE_MOBILE_UNIFORM( sampler, ambientDiffuse1 )
	DEFINE_MOBILE_UNIFORM( sampler, ambientSpecular0 )
	DEFINE_MOBILE_UNIFORM( sampler, ambientSpecular1 )
	DEFINE_MOBILE_UNIFORM( sampler, shadowAtlas )
	DEFINE_MOBILE_UNIFORM( sampler, ssaoTexture )
};
struct ShadowUniforms : GLSLUniformGroup {
	UNIFORM_GROUP_DEF( ShadowUniforms )
	DEFINE_UNIFORM( mat4, modelMatrix )
	DEFINE_UNIFORM( mat4, textureMatrix )
	DEFINE_UNIFORM( mat4, shadowProjection )
	DEFINE_UNIFORM( vec3, lightOrigin )
	DEFINE_UNIFORM( int, face )
	DEFINE_UNIFORM( int, parallel )
	DEFINE_UNIFORM( float, alphaTest )
	DEFINE_UNIFORM( sampler, texture )
};
struct Packet {
	drawInteraction_t inter;
	const viewLight_t *light;
	bool local, translucent;
	const drawSurf_t *geometry;
	int lightIndex;
};
struct Batch { const Packet *lights[MAX_BATCH_LIGHTS] = {}; int count = 0; };
struct LightClip { const viewLight_t *light; MobileRenderPlan::Rect rect; };
struct ViewPlan {
	std::vector<Packet> packets;
	std::vector<Batch> batches;
	std::vector<LightClip> lights;
	MobileRenderPlan::Rect viewport, currentScissor;
};
struct Shadow {
	const viewDef_t *view;
	const viewLight_t *light;
	MobileRenderPlan::Page page;
	uint64_t cacheKey = 0;
	float maxCoverage = 0.0f;
	struct Face {
		std::vector<const drawSurf_t *> casters;
		MobileShadowCache::Signature content;
	};
	std::array<Face, 12> faces;
};

bool HasNegativeColor( const Packet &p ) {
	for ( int i = 0; i < 3; ++i )
		if ( p.inter.diffuseColor[i] < 0 || p.inter.specularColor[i] < 0 ) return true;
	return false;
}

// Compare only explicitly initialized state: cache handles and structs contain
// padding, and different lights have distinct drawSurf_t wrappers for one mesh.
auto GeometryKey( const Packet &p ) {
	const drawSurf_t &s = *p.geometry;
	return std::make_tuple( p.translucent, uintptr_t(s.space), uintptr_t(s.material),
		s.ambientCache.isStatic != 0, s.ambientCache.frameNumber + 0, s.ambientCache.offset,
		s.indexCache.isStatic != 0, s.indexCache.frameNumber + 0, s.indexCache.offset,
		s.numIndexes, s.numVerts, p.inter.surf->dsFlags );
}
bool Compatible( const Packet &a, const Packet &b ) {
	if ( GeometryKey(a) != GeometryKey(b) ) return false;
	// LDR targets clamp after every draw. Subtractive material/light stages
	// therefore cannot be combined without changing the result.
	if ( HasNegativeColor(a) || HasNegativeColor(b) ) return false;
	const auto &x = a.inter; const auto &y = b.inter;
	return x.bumpImage == y.bumpImage && x.diffuseImage == y.diffuseImage &&
		x.specularImage == y.specularImage && x.parallaxImage == y.parallaxImage && x.vertexColor == y.vertexColor &&
		memcmp( x.bumpMatrix, y.bumpMatrix, sizeof(x.bumpMatrix) ) == 0 &&
		memcmp( x.diffuseMatrix, y.diffuseMatrix, sizeof(x.diffuseMatrix) ) == 0 &&
		memcmp( x.specularMatrix, y.specularMatrix, sizeof(x.specularMatrix) ) == 0 &&
		memcmp( x.parallaxMatrix, y.parallaxMatrix, sizeof(x.parallaxMatrix) ) == 0 &&
		( !x.parallaxImage || memcmp( &x.parallax, &y.parallax, sizeof(x.parallax) ) == 0 );
}
// Eight lights can share the existing sixteen GLES texture units: reserve two
// texture tuples, rather than wasting five new units on every light. Projection
// matrices, colors, shadows and origins remain independent for every light.
auto LightTextureKey( const Packet &p ) {
	return std::make_tuple(p.light->lightShader->IsCubicLight(), uintptr_t(p.inter.lightImage),
		uintptr_t(p.inter.lightFalloffImage), uintptr_t(p.light->lightShader->LightAmbientDiffuse()),
		uintptr_t(p.light->lightShader->LightAmbientSpecular()));
}
bool CanAppend( const Batch &batch, const Packet &p ) {
	if ( !batch.count ) return true;
	if ( batch.count == MAX_BATCH_LIGHTS || !Compatible(*batch.lights[0], p) ) return false;
	const auto first = LightTextureKey(*batch.lights[0]);
	const auto wanted = LightTextureKey(p);
	if ( first == wanted ) return true;
	for ( int i = 1; i < batch.count; ++i )
		if ( LightTextureKey(*batch.lights[i]) != first ) return LightTextureKey(*batch.lights[i]) == wanted;
	return true;
}
void Bind( int unit, idImage *image ) { image->BindToUnit(unit); }
}

struct MobileLightingStage::Impl {
	GLSLProgram *lighting[8] = {}, *depth = nullptr;
	uint64_t drawSerial = 0, configuredSerial[8] = {};
	const MobileStereoView *stereo = nullptr;
	int multiviewPasses = 0;
	idImageScratch *atlas = nullptr;
	FrameBuffer *fbo = nullptr;
	MobileShadowCache::AtlasLayout layout;
	struct CachedLight {
		MobileRenderPlan::Page page;
		int columns = 0, rows = 0;
		std::array<MobileShadowCache::FaceState, 12> faces;
	};
	std::map<uint64_t, CachedLight> shadowCache;
	uint64_t cacheFrame = 0, staticRevision = 0, fboRevision = 0;
	int cachedAtlasSize = 0, cachedFaceSize = 0;
	int reusedFaces = 0, refreshedFaces = 0, reusedViews = 0;
	int atlasSize = 0;
	std::unordered_map<const viewDef_t *, ViewPlan> views;
	std::unordered_map<const viewLight_t *, size_t> shadowIndex;
	std::vector<Shadow> shadows;
	int packets = 0, batches = 0, shadowDraws = 0, eyeDraws = 0, frames = 0;
	int culledBatches = 0;
	int splitBatches = 0;
	long long scissorPixels = 0, fullPixels = 0;
	int shadowBuilds = 0, eyePasses = 0, scissorChecks = 0, scissorMismatches = 0;
	bool checkScissor = false;

	void Collect( const viewDef_t *view );
	void PlanShadows();
	bool DescribeCaster( const drawSurf_t *surf, const viewLight_t *light, MobileShadowCache::Signature &content );
	void DrawShadows();
	void DrawCaster( const drawSurf_t *surf, ShadowUniforms *u );
	void DrawBatch( const viewDef_t *view, ViewPlan &plan, const Batch &batch );
	LightingUniforms *UseLighting(const viewDef_t *view,int count);
	bool ShadowInfo( const viewLight_t *light, bool local, idVec4 &page, idVec4 &params, float projection[16] ) const;
};

MobileLightingStage::MobileLightingStage() : impl( new Impl ) {}
MobileLightingStage::~MobileLightingStage() = default;

void MobileLightingStage::Init() {
#ifdef TDM_VR_OPENXR
	for(int i=0;i<8;++i) {
		const int count=1<<(i%4);
		const idStr fragment=count==8 ? "stages/mobile/forward.frag.glsl" : va("stages/mobile/forward_%d.frag.glsl",count);
		impl->lighting[i]=programManager->LoadFromGenerator(va("quest_%sforward_%d",i>=4?"multiview_":"",count),[fragment,i](GLSLProgram *program){
			idHashMapDict defines;if(i>=4)defines["QUEST_MULTIVIEW"]="1";
			program->LoadFromFiles("stages/mobile/forward.vert.glsl",fragment,defines);
		});
	}
	impl->depth = programManager->LoadFromFiles( "quest_shadow_depth", "stages/mobile/shadow.vert.glsl", "stages/mobile/shadow.frag.glsl" );
#endif
}
void MobileLightingStage::Shutdown() {
	EndFrame();
	if ( impl->fbo ) impl->fbo->Destroy();
	if ( impl->atlas ) impl->atlas->PurgeImage();
	impl->shadowCache.clear();impl->layout.Clear();
	impl->fbo = nullptr;
	impl->atlas = nullptr;
}
void MobileLightingStage::EndFrame() {
	if ( !impl->views.empty() && impl->eyeDraws > 0 && ++impl->frames % 120 == 0 && r_questShowRenderStats.GetBool() ) {
		common->Printf("Quest renderer: %d views prepared once; %d light packets -> %d batches; %d shared shadow draws; %d lighting draws across replays; atlas %d\n",
			int(impl->views.size()), impl->packets, impl->batches, impl->shadowDraws, impl->eyeDraws, impl->atlasSize);
		common->Printf("Quest lighting scissors: %.1f%% of full draw area; %d offscreen batches skipped; %d separated-light batches split\n",
			100.0 * double(impl->scissorPixels) / std::max(1.0, double(impl->fullPixels)), impl->culledBatches, impl->splitBatches);
	}
	impl->views.clear();
	impl->shadowIndex.clear();
	impl->shadows.clear();
}
bool MobileLightingStage::HasView( const viewDef_t *view ) const {
	return R_UseMobileLighting() && impl->views.count(view) != 0;
}
bool MobileLightingStage::CanDrawMultiview(const viewDef_t *view,const MobileMultiviewTargets &targets) const {
	auto it=impl->views.find(view);if(it==impl->views.end())return false;
	// A captured render texture in an interaction cannot be broadcast to both
	// eyes. Preserve ordering/LDR clamping by retaining that view's normal path.
	// Ambient/refraction material stages always keep their separate eye passes.
	for(const auto &packet:it->second.packets){
		const auto &d=packet.inter;
		idImage *textures[]={d.bumpImage,d.diffuseImage,d.specularImage,d.parallaxImage,d.lightImage,d.lightFalloffImage,
			packet.light->lightShader->LightAmbientDiffuse(),packet.light->lightShader->LightAmbientSpecular()};
		for(auto *image:textures)if(targets.IsPerEyeImage(image))return false;
	}
	return true;
}
bool MobileLightingStage::HasFrame() const { return !impl->views.empty(); }

void MobileLightingStage::GetDiagnosticsText( char *buffer, int size ) const {
	const Impl &i = *impl;
	const bool active = R_UseMobileLighting() && !i.views.empty();
	if ( !active ) {
		idStr::Copynz(buffer, R_UseMobileLighting() ? "Pipeline: IDLE (no 3D scene)\n" :
			"Pipeline: legacy (Quest forward OFF)\nScissors/shared shadows: mobile path inactive\n", size);
		return;
	}
	snprintf(buffer, size,
		"Pipeline: Quest forward | %d multiview + %d eye passes\n"
		"Forward shader: %s | shadow shader: %s\n"
		"Scissors: %s\n"
		"Lighting rectangle area: %.1f%% | culled %d | split %d\n"
		"Scissor GL samples: %d | mismatches: %d\n"
		"Shadow builds: %d/frame | shared draws: %d | atlas %d\n"
		"Shadow cache: %d reused | %d redrawn | no depth copies\n"
		"Light packets: %d -> %d batches | stereo draws: %d\n",
		i.multiviewPasses, i.eyePasses, i.lighting[0] && i.lighting[0]->IsLinked() ? "LINKED" : "NOT LINKED",
		i.depth && i.depth->IsLinked() ? "LINKED" : "NOT LINKED",
		QuestScissorStatus(R_UseQuestLightScissors(), i.fullPixels, i.scissorChecks, i.scissorMismatches),
		100.0 * double(i.scissorPixels) / std::max(1.0, double(i.fullPixels)), i.culledBatches, i.splitBatches,
		i.scissorChecks, i.scissorMismatches, i.shadowBuilds, i.shadowDraws, i.atlasSize, i.reusedFaces, i.refreshedFaces,
		i.packets, i.batches, i.eyeDraws);
}

void MobileLightingStage::BeginFrame( const emptyCommand_t *commands ) {
	QuestCpuScope timing(questRenderCounters.prepareCpuMs,vr_rendererDiagnostics.GetBool());
	EndFrame();
	impl->packets = impl->batches = impl->shadowDraws = impl->eyeDraws = 0;
	impl->culledBatches = impl->splitBatches = 0;
	impl->scissorPixels = impl->fullPixels = 0;
	impl->shadowBuilds = impl->eyePasses = impl->scissorChecks = impl->scissorMismatches = 0;
	impl->multiviewPasses=0;
	impl->reusedFaces = impl->refreshedFaces = impl->reusedViews = 0;
	++impl->cacheFrame;
	if ( !R_UseMobileLighting() || r_skipRender.GetBool() ) return;
	// A missing/failed program must not leave the depth shader active while
	// replaying lighting draws, which paints black geometry without lighting.
	bool linked=impl->depth->EnsureLinked();
	for(int i=0;i<4;++i) linked=impl->lighting[i]->EnsureLinked() && linked;
	if ( !linked ) {
		common->Error("Quest lighting shaders failed to load or compile. See the shader filename and compiler error in the console log.");
		return;
	}
	TRACE_GL_SCOPE( "QuestPrepareStereo" );
	// The previous frame ends with a 2D UI pass, which disables depth testing.
	// Reset before shared depth, not just before the subsequent eye replays.
	RB_SetDefaultGLState();
	impl->packets = impl->batches = impl->shadowDraws = impl->eyeDraws = 0;
	impl->culledBatches = 0;
	impl->splitBatches = 0;
	impl->scissorPixels = impl->fullPixels = 0;
	const float savedScale = backEnd.lightScale;
	backEnd.lightScale = r_lightScale.GetFloat();
	for ( const emptyCommand_t *cmd = commands; cmd; cmd = (const emptyCommand_t *)cmd->next ) {
		const viewDef_t *view = nullptr;
		if ( cmd->commandId == RC_DRAW_VIEW ) view = ((const drawSurfsCommand_t *)cmd)->viewDef;
		if ( cmd->commandId == RC_DRAW_LIGHTGEM ) view = ((const drawLightgemCommand_t *)cmd)->viewDef;
		if ( view && view->viewEntitys && !impl->views.count(view) ) impl->Collect( view );
	}
	backEnd.lightScale = savedScale;
	GLint maxSize = 0;
	qglGetIntegerv( GL_MAX_TEXTURE_SIZE, &maxSize );
	impl->atlasSize = std::min( maxSize, r_questShadowAtlasSize.GetInteger() );
	if ( impl->staticRevision != vertexCache.StaticRevision() || impl->cachedAtlasSize != impl->atlasSize ||
		impl->cachedFaceSize != r_questShadowFaceSize.GetInteger() ) {
		impl->shadowCache.clear();impl->layout.Clear();
		if ( impl->fbo ) impl->fbo->Destroy();
		impl->staticRevision = vertexCache.StaticRevision();
		impl->cachedAtlasSize = impl->atlasSize;impl->cachedFaceSize = r_questShadowFaceSize.GetInteger();
	}
	impl->PlanShadows();
	std::vector<int> requested;
	std::vector<std::pair<int,int>> shapes;
	for ( const Shadow &s : impl->shadows ) {
		const float coverage = s.maxCoverage;
		int size = r_questShadowFaceSize.GetInteger();
		if ( coverage < 0.25f ) size /= 2;
		if ( coverage < 0.0625f ) size /= 2;
		if ( s.view->IsLightGem() ) size /= 4;
		requested.push_back( std::max(16, size) );
		shapes.emplace_back(s.light->mobileParallel ? 1 : 3, s.light->localShadows ? 4 : 2);
	}
	std::vector<MobileRenderPlan::Page> pages;
	if ( !MobileRenderPlan::Pack( impl->atlasSize, requested, pages, shapes ) )
		common->Error( "Quest shadow atlas cannot fit %d lights", int(requested.size()) );
	std::vector<MobileShadowCache::Request> requests;
	for ( size_t i = 0; i < pages.size(); ++i )
		requests.push_back({impl->shadows[i].cacheKey, pages[i].size, shapes[i].first, shapes[i].second});
	bool repacked = false;
	if ( !impl->layout.Prepare(impl->atlasSize, requests, pages, repacked) ) common->Error("Quest persistent shadow layout failed");
	if ( repacked ) impl->shadowCache.clear();
	for ( auto it = impl->shadowCache.begin(); it != impl->shadowCache.end(); )
		if ( !impl->layout.slots.count(it->first) ) it = impl->shadowCache.erase(it); else ++it;
	for ( size_t i = 0; i < pages.size(); ++i ) impl->shadows[i].page = pages[i];
	impl->DrawShadows();
	impl->shadowBuilds = impl->refreshedFaces ? 1 : 0;
}

void MobileLightingStage::Impl::Collect( const viewDef_t *view ) {
	std::vector<Packet> opaque, translucent;
	ViewPlan &plan = views[view];
	for ( const viewLight_t *light = view->viewLights; light; light = light->next ) {
		if ( light->shadows == LS_MAPS && !light->noShadows && (light->globalShadows || light->localShadows) ) {
			shadowIndex[light] = shadows.size();
			shadows.push_back( {view, light, {}} );
		}
		if ( light->lightShader->IsFogLight() || light->lightShader->IsBlendLight() ) continue;
		if ( light->lightShader->IsAmbientLight() ) {
			if ( r_skipAmbient.GetInteger() & 2 ) continue;
		} else if ( r_skipInteractions.GetBool() && (r_skipInteractions.GetInteger() == 1 || !light->noShadows) ) continue;
		const int lightIndex = int(plan.lights.size());
		plan.lights.push_back({light, {}});
		for ( int list = 0; list < 3; ++list ) {
			if ( list == 2 && r_skipTranslucent.GetBool() ) continue;
			const drawSurf_t *first = list == 0 ? light->localInteractions : list == 1 ? light->globalInteractions : light->translucentInteractions;
			for ( const drawSurf_t *surf = first; surf; surf = surf->nextOnLight ) {
				if ( !surf->ambientCache.IsValid() || !surf->indexCache.IsValid() || !surf->numIndexes ) continue;
				for ( int stage = 0; stage < light->lightShader->GetNumStages(); ++stage ) {
					const shaderStage_t *ls = light->lightShader->GetStage(stage);
					if ( !light->IsStageEnabled(ls) ) continue;
					InteractionStage::GenerateInteractions( light, ls, surf, [&]( const drawInteraction_t &value ) {
						const drawSurf_t *geometry = surf->mobileCanonical ? surf->mobileCanonical : surf;
						Packet p = { value, light, list == 0, list == 2, geometry, lightIndex };
						extern idCVar r_materialNewParse;
						if ( !p.inter.bumpImage && !r_skipBump.GetBool() && !r_materialNewParse.GetBool() ) return;
						if ( !p.inter.diffuseImage && !p.inter.specularImage ) return;
						// Initialize absent matrices too, so packet keys never depend on stack data.
						const idVec4 identity[2] = { idVec4(1,0,0,0), idVec4(0,1,0,0) };
						if ( !p.inter.bumpImage ) { p.inter.bumpImage = globalImages->flatNormalMap; memcpy(p.inter.bumpMatrix, identity, sizeof(identity)); }
						if ( !p.inter.diffuseImage || r_skipDiffuse.GetBool() ) { p.inter.diffuseImage = globalImages->blackImage; memcpy(p.inter.diffuseMatrix, identity, sizeof(identity)); }
						if ( !p.inter.specularImage || r_skipSpecular.GetBool() ) { p.inter.specularImage = globalImages->blackImage; memcpy(p.inter.specularMatrix, identity, sizeof(identity)); }
						if ( !p.inter.parallaxImage ) memcpy(p.inter.parallaxMatrix, identity, sizeof(identity));
						(list == 2 ? translucent : opaque).push_back(p);
					} );
				}
			}
		}
	}
	// Reorder only opaque interactions. Translucent material/light order remains
	// the original additive order (no depth writes); never cross geometry keys.
	const bool reorderOpaque = std::none_of(opaque.begin(), opaque.end(), HasNegativeColor);
	if ( reorderOpaque )
		std::stable_sort( opaque.begin(), opaque.end(), [](const Packet &a, const Packet &b) { return GeometryKey(a) < GeometryKey(b); } );
	plan.packets.reserve(opaque.size()+translucent.size());
	plan.packets.insert(plan.packets.end(),opaque.begin(),opaque.end());
	plan.packets.insert(plan.packets.end(),translucent.begin(),translucent.end());
	size_t groupStart = 0;
	for ( const Packet &p : plan.packets ) {
			size_t target = plan.batches.size();
			if ( !p.translucent && reorderOpaque ) {
				if ( groupStart < plan.batches.size() && GeometryKey(*plan.batches[groupStart].lights[0]) != GeometryKey(p) ) groupStart = plan.batches.size();
				// Group matching positive material stages even when interleaved
				// with another stage on the same mesh. Negative stages keep order.
				for ( size_t i = groupStart; i < plan.batches.size(); ++i )
					if ( CanAppend(plan.batches[i],p) ) { target = i;break; }
			} else if ( !plan.batches.empty() && CanAppend(plan.batches.back(),p) ) target = plan.batches.size()-1;
			if ( target == plan.batches.size() ) plan.batches.push_back({});
			Batch &b = plan.batches[target];
			b.lights[b.count++] = &p;
			++packets;
	}
	batches += int(plan.batches.size());
	if(reorderOpaque) {
		auto end=std::find_if(plan.batches.begin(),plan.batches.end(),[](const Batch &b){return b.lights[0]->translucent;});
		std::stable_sort(plan.batches.begin(),end,[](const Batch &a,const Batch &b){return LightingVariant(a.count)<LightingVariant(b.count);});
	}
}

bool MobileLightingStage::Impl::DescribeCaster( const drawSurf_t *surf, const viewLight_t *light, MobileShadowCache::Signature &content ) {
	if ( !surf->numIndexes || !surf->ambientCache.IsValid() || !surf->indexCache.IsValid() ) return false;
	if ( auto stage = surf->material->GetParallaxStage() ) if ( stage->parallax->offsetExternalShadows ) return false;
	MobileShadowCache::Signature key;
	key.Add(surf->space->mobileCacheId);
	const uintptr_t materialId = reinterpret_cast<uintptr_t>(surf->material);
	// Identify this instance and geometry; its output state below determines
	// whether the retained face remains valid after movement or animation.
	key.Add(materialId);key.Add(surf->ambientCache.offset);key.Add(light->mobileCacheId);key.Add(surf->numVerts);
	content = key;
	content.Add(surf->numIndexes);
	content.Bytes(surf->space->modelMatrix, sizeof(surf->space->modelMatrix));
	auto addHandle = [&](const vertCacheHandle_t &h) {
		content.Add(h.size);content.Add(h.offset);
		content.Add(uint32_t(h.frameNumber));content.Add(uint32_t(h.isStatic));
	};
	addHandle(surf->ambientCache);
	const bool stableIndices = MobileShadowCache::IndexSignature(content,surf->shadowIndexRevision,surf->indexCache.isStatic,
		surf->indexCache.offset,surf->indexCache.size,surf->indexCache.frameNumber);
	bool canCache = surf->ambientCache.isStatic && stableIndices;
	const auto coverage = surf->material->Coverage();content.Add(coverage);
	if ( coverage == MC_PERFORATED ) {
		extern idCVar r_shadowMapAlphaTested;
		const bool alpha = r_shadowMapAlphaTested.GetBool();content.Add(alpha);
		if ( alpha ) {
			bool attempted = false, visible = false;
			for ( int i = 0; i < surf->material->GetNumStages(); ++i ) {
				const shaderStage_t *stage = surf->material->GetStage(i);
				if ( !stage->hasAlphaTest || !surf->IsStageEnabled(stage) ) continue;
				attempted = true;
				if ( surf->GetStageColor(stage)[3] <= 0 ) continue;
				visible = true;content.Add(i);
				idImage *image = stage->texture.image;
				Bind(0,image); // resolve deferred uploads before capturing the image revision
				const uintptr_t imageId = reinterpret_cast<uintptr_t>(image);
				content.Add(imageId);content.Add(image->texnum);content.Add(image->contentRevision);
				const float alphaTest = surf->shaderRegisters[stage->alphaTestRegister];content.Add(alphaTest);
				float matrix[16];surf->GetTextureMatrix(stage).ToGL(matrix);content.Bytes(matrix,sizeof(matrix));
				if ( image->GetType() != IT_ASSET || stage->texture.cinematic ) canCache = false;
			}
			if ( attempted && !visible ) return false;
		}
	}
	if ( !canCache ) content.Add(cacheFrame); // dynamic buffers/images must never reuse old depth
	return true;
}

void MobileLightingStage::Impl::PlanShadows() {
	using MobileShadowCache::Signature;
	std::map<std::tuple<uint64_t,bool,uint64_t,uint64_t>,size_t> equivalent;
	std::map<std::pair<uint64_t,bool>,unsigned> ordinals;
	std::vector<Shadow> unique;
	shadowIndex.clear();
	for ( Shadow &s : shadows ) {
		s.maxCoverage = float(s.light->scissorRect.GetArea()) / std::max(1, s.view->viewport.GetArea());
		Signature base;
		base.Add(s.light->mobileCacheId);base.Add(staticRevision);base.Add(depth->Generation());
		base.Add(s.light->mobileParallel);base.Add(bool(s.light->localShadows));
		base.Bytes(s.light->globalLightOrigin.ToFloatPtr(),3*sizeof(float));
		if ( s.light->mobileParallel ) base.Bytes(s.light->mobileShadowProjection,sizeof(s.light->mobileShadowProjection));
		std::array<MobileShadowCache::CasterSet,12> casters;
		for ( int local = 0; local < 2; ++local ) {
			const drawSurf_t *first = local ? s.light->localShadows : s.light->globalShadows;
			for ( const drawSurf_t *surf = first; surf; surf = surf->nextOnLight ) {
				Signature content;
				if ( !DescribeCaster(surf,s.light,content) ) continue;
				unsigned mask = 63;
				if ( !s.light->mobileParallel && !surf->geometryBounds.IsCleared() ) {
					idVec3 corners[8];surf->geometryBounds.ToPoints(corners);
					idBounds bounds;bounds.Clear();
					for ( const auto &corner : corners ) {
						idVec3 world;R_LocalPointToGlobal(surf->space->modelMatrix,corner,world);
						bounds.AddPoint(world-s.light->globalLightOrigin);
					}
					mask = MobileRenderPlan::CubeFaceMask(bounds[0].ToFloatPtr(),bounds[1].ToFloatPtr());
				}
				for ( int face = 0; face < (s.light->mobileParallel ? 1 : 6); ++face ) {
					if ( !(mask & (1u<<face)) ) continue;
					const int index = local*6+face;
					s.faces[index].casters.push_back(surf);casters[index].Add(content);
				}
			}
		}
		Signature complete = base;
		for ( int i = 0; i < 12; ++i ) {
			s.faces[i].content = casters[i].Finish(base);
			complete.Add(s.faces[i].content.a);complete.Add(s.faces[i].content.b);
		}
		const bool lightgem = s.view->IsLightGem();
		const auto equivalenceKey = std::make_tuple(s.light->mobileCacheId,lightgem,complete.a,complete.b);
		auto found = equivalent.find(equivalenceKey);
		if ( found != equivalent.end() ) {
			shadowIndex[s.light] = found->second;
			unique[found->second].maxCoverage = std::max(unique[found->second].maxCoverage,s.maxCoverage);
			++reusedViews;continue;
		}
		Signature owner;owner.Add(s.light->mobileCacheId);owner.Add(lightgem);
		owner.Add(ordinals[{s.light->mobileCacheId,lightgem}]++);
		s.cacheKey = owner.a;
		shadowIndex[s.light] = unique.size();equivalent.emplace(equivalenceKey,unique.size());
		unique.push_back(std::move(s));
	}
	shadows.swap(unique);
}

void MobileLightingStage::Impl::DrawCaster( const drawSurf_t *surf, ShadowUniforms *u ) {
	if ( !surf->numIndexes || !surf->ambientCache.IsValid() || !surf->indexCache.IsValid() ) return;
	if ( auto stage = surf->material->GetParallaxStage() )
		if ( stage->parallax->offsetExternalShadows ) return;
	vertexCache.VertexPosition( surf->ambientCache );
	u->modelMatrix.Set( surf->space->modelMatrix );
	bool attempted = false;
	if ( surf->material->Coverage() == MC_PERFORATED ) {
		extern idCVar r_shadowMapAlphaTested;
		if ( r_shadowMapAlphaTested.GetBool() ) {
			for ( int i = 0; i < surf->material->GetNumStages(); ++i ) {
				const shaderStage_t *stage = surf->material->GetStage(i);
				if ( !stage->hasAlphaTest || !surf->IsStageEnabled(stage) ) continue;
				attempted = true;
				if ( surf->GetStageColor(stage)[3] <= 0 ) continue;
				Bind( 0, stage->texture.image );
				float textureMatrix[16]; surf->GetTextureMatrix(stage).ToGL(textureMatrix);
				u->textureMatrix.Set(textureMatrix);
				u->alphaTest.Set( surf->shaderRegisters[stage->alphaTestRegister] );
				RB_DrawElementsWithCounters( surf, DCK_SHADOW ); ++shadowDraws;
			}
		}
	}
	if ( !attempted ) {
		u->alphaTest.Set(-1.0f);
		RB_DrawElementsWithCounters( surf, DCK_SHADOW ); ++shadowDraws;
	}
}

void MobileLightingStage::Impl::DrawShadows() {
    if(shadows.empty()) return;
    TRACE_GL_SCOPE("QuestRetainedShadowDepth");
    FrameBuffer *savedRead=frameBuffers->activeFbo,*savedDraw=frameBuffers->activeDrawFbo;
    const viewDef_t *savedView=backEnd.viewDef;
    if(!atlas) atlas=globalImages->ImageScratch("_questShadowAtlas");
    if(!fbo) fbo=frameBuffers->CreateFromGenerator("questSharedShadowDepth",[this](FrameBuffer *fb){
        fb->Init(atlasSize,atlasSize);
        atlas->GenerateAttachment(atlasSize,atlasSize,GL_DEPTH_COMPONENT24,GL_NEAREST);
        fb->AddDepthRenderTexture(atlas);
        GLenum none=GL_NONE;qglDrawBuffers(1,&none);qglReadBuffer(GL_NONE);
    });
    fbo->Bind();
    if(fboRevision!=fbo->Generation()){shadowCache.clear();fboRevision=fbo->Generation();}
    GL_State(GLS_DEPTHFUNC_LESS|GLS_REDMASK|GLS_GREENMASK|GLS_BLUEMASK|GLS_ALPHAMASK);
    qglDisable(GL_STENCIL_TEST);qglEnable(GL_SCISSOR_TEST);
#ifdef _GLES
    qglDepthRangef(0.0f,1.0f);qglClearDepthf(1.0f);
#else
    qglDepthRange(0.0,1.0);qglClearDepth(1.0);
#endif
    qglPolygonOffset(0,0);qglDisable(GL_POLYGON_OFFSET_FILL);
    depth->Activate();auto *u=depth->GetUniformGroup<ShadowUniforms>();
    u->texture.Set(0);Bind(0,globalImages->whiteImage);
    for(Shadow &s:shadows){
        auto &cached=shadowCache[s.cacheKey];
        const int columns=s.light->mobileParallel?1:3,rows=s.light->localShadows?4:2;
        if(cached.page.x!=s.page.x||cached.page.y!=s.page.y||cached.page.size!=s.page.size||cached.columns!=columns||cached.rows!=rows)cached={};
        cached.page=s.page;cached.columns=columns;cached.rows=rows;
        backEnd.viewDef=s.view;GL_Cull(CT_TWO_SIDED);
        u->lightOrigin.Set(s.light->globalLightOrigin);u->parallel.Set(s.light->mobileParallel);
        if(s.light->mobileParallel)u->shadowProjection.Set(s.light->mobileShadowProjection);
        for(int local=0;local<(s.light->localShadows?2:1);++local)
            for(int face=0;face<(s.light->mobileParallel?1:6);++face){
                const int i=local*6+face;auto &job=s.faces[i];auto &state=cached.faces[i];
                if(r_questShadowCache.GetBool()&&!state.FinalDirty(job.content)){++reusedFaces;continue;}
                // Keep the final depth in-place. One render target, no scratch
                // depth atlas and no depth blits/tile transfers between layers.
                const int x=s.page.x+(face%3)*s.page.size,y=s.page.y+(face/3+2*local)*s.page.size;
                qglViewport(x,y,s.page.size,s.page.size);qglScissor(x,y,s.page.size,s.page.size);
                qglClear(GL_DEPTH_BUFFER_BIT);u->face.Set(face);
                for(const drawSurf_t *surf:job.casters)DrawCaster(surf,u);
                state.finalContent=job.content;state.haveFinal=true;++refreshedFaces;
            }
    }
    GLSLProgram::Deactivate();backEnd.viewDef=savedView;
    savedRead->Bind();savedDraw->BindDraw();GL_State(GLS_DEFAULT);
    qglEnable(GL_STENCIL_TEST);qglEnable(GL_CULL_FACE);backEnd.glState.faceCulling=-1;
}

bool MobileLightingStage::Impl::ShadowInfo( const viewLight_t *light, bool local, idVec4 &page, idVec4 &params, float projection[16] ) const {
	page.Zero(); params.Zero();
	memset(projection, 0, 16*sizeof(float));
	auto it = shadowIndex.find(light);
	if ( it == shadowIndex.end() ) return false;
	const auto &p = shadows[it->second].page;
	const float inv = 1.0f / atlasSize;
	page.Set(p.x*inv, p.y*inv, p.size*inv, inv);
	params.Set(1.0f, !local && light->localShadows ? 1.0f : 0.0f, light->mobileParallel ? 1.0f : 0.0f, 1.0f);
	if ( light->mobileParallel ) memcpy(projection, light->mobileShadowProjection, 16*sizeof(float));
	return true;
}

bool MobileLightingStage::GetShadow( const viewDef_t *view, const viewLight_t *light, idVec4 &page, idVec4 &params, idMat4 &projection ) const {
	if ( !HasView(view) ) return false;
	float matrix[16];
	if ( !impl->ShadowInfo(light, false, page, params, matrix) ) return false;
	projection = idMat4::FromGL(matrix);
	return true;
}
void MobileLightingStage::BindShadowTexture( int unit ) {
	Bind(unit, impl->atlas ? (idImage *)impl->atlas : globalImages->whiteImage);
}

void MobileLightingStage::Impl::DrawBatch( const viewDef_t *view, ViewPlan &plan, const Batch &batch ) {
	const Packet *visible[MAX_BATCH_LIGHTS];
	int count = 0;
	MobileRenderPlan::Rect rect;
	for ( int i = 0; i < batch.count; ++i ) {
		const Packet &p = *batch.lights[i];
		const auto &r = plan.lights[p.lightIndex].rect;
		if ( r.Empty() ) continue;
		visible[count++] = &p;
		rect.Add(r);
	}
	if ( count == 0 ) { fullPixels += plan.viewport.Area(); ++culledBatches; return; }
	long long individualArea = 0;
	for ( int i = 0; i < count; ++i ) individualArea += plan.lights[visible[i]->lightIndex].rect.Area();
	if ( count > 1 && rect.Area() > individualArea ) {
		// A large empty gap makes a fused draw more expensive than two tight
		// light draws. Retain batching for overlapping lights, not distant ones.
		++splitBatches;
		Batch single;
		single.count = 1;
		for ( int i = 0; i < count; ++i ) {
			single.lights[0] = visible[i];
			DrawBatch(view, plan, single);
		}
		return;
	}
	fullPixels += plan.viewport.Area();
	scissorPixels += rect.Area();
	if ( rect.x1 != plan.currentScissor.x1 || rect.y1 != plan.currentScissor.y1 ||
		rect.x2 != plan.currentScissor.x2 || rect.y2 != plan.currentScissor.y2 ) {
		GL_ScissorAbsolute(rect.x1, rect.y1, rect.x2-rect.x1, rect.y2-rect.y1);
		plan.currentScissor = rect;
	}
	const auto &first = *visible[0];
	LightingUniforms *u=UseLighting(view,count);
	if ( checkScissor && vr_rendererDiagnostics.GetBool() && !view->IsLightGem() ) {
		// One state sample per eye/view, never a query on every draw.
		GLint box[4];
		qglGetIntegerv(GL_SCISSOR_BOX, box);
		++scissorChecks;
		if ( !qglIsEnabled(GL_SCISSOR_TEST) || box[0] != rect.x1 || box[1] != rect.y1 ||
			box[2] != rect.x2-rect.x1 || box[3] != rect.y2-rect.y1 ) ++scissorMismatches;
		checkScissor = false;
	}
	const drawInteraction_t &inter = first.inter;
	const drawSurf_t *surf = count>1 ? first.geometry : inter.surf;
	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK |
		(r_fboColorBits.GetInteger() == 64 ? GLS_ALPHAMASK : 0) |
		(first.translucent ? GLS_DEPTHFUNC_LESS : GLS_DEPTHFUNC_EQUAL) );
	ApplyDepthTweaks tweaks(surf);
	GL_Cull(surf->material->GetCullType());
	vertexCache.VertexPosition(surf->ambientCache);
	u->modelMatrix.Set(surf->space->modelMatrix);
	if(stereo){
		float matrices[2][16];stereo->ModelViews(surf->space,matrices);
		u->modelViewMatrix.SetArray(2,matrices[0]);
	} else u->modelViewMatrix.Set(surf->space->modelViewMatrix);
	u->vertexColorMode.Set(inter.vertexColor);
	u->bumpMatrix.SetArray(2, inter.bumpMatrix[0].ToFloatPtr());
	u->diffuseMatrix.SetArray(2, inter.diffuseMatrix[0].ToFloatPtr());
	u->specularMatrix.SetArray(2, inter.specularMatrix[0].ToFloatPtr());
	u->parallaxMatrix.SetArray(2, inter.parallaxMatrix[0].ToFloatPtr());
	int flags = SFL_SURFACE_HAS_NORMAL_TEXTURE | SFL_SURFACE_HAS_TEXTURE_MATRIX;
	if ( inter.bumpImage->internalFormat == GL_COMPRESSED_RG_RGTC2 ) flags |= SFL_SURFACE_NORMAL_TEXTURE_RGTC;
	if ( inter.parallaxImage ) flags |= SFL_SURFACE_HAS_PARALLAX_TEXTURE;
	if ( inter.parallax.offsetExternalShadows ) flags |= SFL_INTERACTION_PARALLAX_OFFSET_EXTERNAL_SHADOWS;
	if ( r_useBumpmapLightTogglingFix.GetBool() && !surf->material->ShouldCreateBackSides() ) flags |= SFL_INTERACTION_BUMPMAP_LIGHT_TOGGLING_FIX;
	u->flags.Set(flags);
	u->parallaxHeightScale.Set(inter.parallax.heightMin, inter.parallax.heightMax);
	u->parallaxGrazingAngle.Set(inter.parallax.grazingAngle);
	u->parallaxShadowSoftness.Set(inter.parallax.shadowSoftness);
	u->parallaxIterations.Set(inter.parallax.linearSteps, inter.parallax.refineSteps,
		(inter.surf->dsFlags & DSF_BLOCK_SELF_SHADOWS) ? 0 : inter.parallax.shadowSteps);
	Bind(0, inter.bumpImage); Bind(1, inter.diffuseImage); Bind(2, inter.specularImage);
	Bind(3, inter.parallaxImage ? inter.parallaxImage : globalImages->blackImage);
	idVec3 origins[MAX_BATCH_LIGHTS];
	idVec4 diffuse[MAX_BATCH_LIGHTS], specular[MAX_BATCH_LIGHTS], pages[MAX_BATCH_LIGHTS], params[MAX_BATCH_LIGHTS], tex[2*MAX_BATCH_LIGHTS];
	float projections[16*MAX_BATCH_LIGHTS] = {}, shadowProjections[16*MAX_BATCH_LIGHTS] = {};
	int lightFlags[MAX_BATCH_LIGHTS] = {}, textureSets[MAX_BATCH_LIGHTS] = {};
	const Packet *texturePackets[2] = {};
	int textureCount = 0;
	for ( int i = 0; i < count; ++i ) {
		const Packet &p = *visible[i]; const auto &d = p.inter; const auto *l = p.light;
		origins[i] = l->globalLightOrigin; diffuse[i] = d.diffuseColor; specular[i] = d.specularColor;
		memcpy(projections + i*16, d.lightProjection, 16*sizeof(float));
		tex[2*i] = d.lightTextureMatrix[0]; tex[2*i+1] = d.lightTextureMatrix[1];
		lightFlags[i] = l->lightShader->IsCubicLight() ? SFL_LIGHT_CUBIC : 0;
		if ( l->lightShader->IsAmbientLight() ) lightFlags[i] |= AMBIENT_LIGHT;
		idImage *ambientDiffuse = l->lightShader->LightAmbientDiffuse();
		idImage *ambientSpecular = l->lightShader->LightAmbientSpecular();
		if ( ambientDiffuse ) lightFlags[i] |= SFL_LIGHT_AMBIENT_HAS_DIFFUSE_CUBEMAP;
		if ( ambientSpecular ) lightFlags[i] |= SFL_LIGHT_AMBIENT_HAS_SPECULAR_CUBEMAP;
		if ( ambientOcclusion->ShouldEnableForCurrentView() ) lightFlags[i] |= SFL_LIGHT_AMBIENT_HAS_SSAO;
		ShadowInfo(l, p.local, pages[i], params[i], shadowProjections+i*16);
		extern idCVar r_shadowMapOnTranslucent;
		if ( p.translucent && !r_shadowMapOnTranslucent.GetBool() ) params[i].w = 0;
		int textureSet = 0;
		while ( textureSet < textureCount && LightTextureKey(*texturePackets[textureSet]) != LightTextureKey(p) ) ++textureSet;
		textureSets[i] = textureSet;
		if ( textureSet < textureCount ) continue;
		assert(textureSet < 2);
		texturePackets[textureCount++] = &p;
		const int base = 4 + 5*textureSet;
		// Samplers of different types always have distinct units, even if the
		// inactive branch won't sample one of them (GLES program validation).
		Bind(base, l->lightShader->IsCubicLight() ? globalImages->whiteImage : d.lightImage);
		Bind(base+1, l->lightShader->IsCubicLight() ? d.lightImage : globalImages->whiteCubeMapImage);
		Bind(base+2, d.lightFalloffImage);
		Bind(base+3, ambientDiffuse ? ambientDiffuse : globalImages->whiteCubeMapImage);
		Bind(base+4, ambientSpecular ? ambientSpecular : globalImages->blackCubeMapImage);
	}
	if ( textureCount == 1 && count>1 ) {
		// GLES considers every active sampler, even in a uniform-disabled branch.
		// Never inherit incomplete textures or a render target from an earlier
		// pass in the unused second light slot.
		Bind(9, globalImages->whiteImage);
		Bind(10, globalImages->whiteCubeMapImage);
		Bind(11, globalImages->whiteImage);
		Bind(12, globalImages->whiteCubeMapImage);
		Bind(13, globalImages->blackCubeMapImage);
	}
	u->lightCount.Set(count);
	u->lightTextureSet.SetArray(count, textureSets);
	u->lightFlags.SetArray(count, lightFlags);
	u->lightOrigin.SetArray(count, origins[0].ToFloatPtr());
	u->diffuseColor.SetArray(count, diffuse[0].ToFloatPtr());
	u->specularColor.SetArray(count, specular[0].ToFloatPtr());
	u->lightProjection.SetArray(count, projections);
	u->lightTextureMatrix.SetArray(2*count, tex[0].ToFloatPtr());
	u->shadowPage.SetArray(count, pages[0].ToFloatPtr());
	u->shadowParams.SetArray(count, params[0].ToFloatPtr());
	u->shadowProjection.SetArray(count, shadowProjections);
	RB_DrawElementsWithCounters(surf, DCK_INTERACTION); ++eyeDraws;
}

LightingUniforms *MobileLightingStage::Impl::UseLighting(const viewDef_t *view,int count) {
    const int variant=LightingVariant(count)+(stereo?4:0);
	if(!lighting[variant]->EnsureLinked())common->Error("Quest lighting shader failed to compile or link");
	lighting[variant]->Activate();
    auto *u=lighting[variant]->GetUniformGroup<LightingUniforms>();
    if(configuredSerial[variant]==drawSerial) return u;
    configuredSerial[variant]=drawSerial;
	u->normalTexture.Set(0); u->diffuseTexture.Set(1); u->specularTexture.Set(2); u->parallaxTexture.Set(3);
	u->projection0.Set(4); u->cube0.Set(5); u->falloff0.Set(6); u->ambientDiffuse0.Set(7); u->ambientSpecular0.Set(8);
	u->projection1.Set(9); u->cube1.Set(10); u->falloff1.Set(11); u->ambientDiffuse1.Set(12); u->ambientSpecular1.Set(13);
	u->shadowAtlas.Set(14); u->ssaoTexture.Set(15);
	if(stereo){
		u->projectionMatrix.SetArray(2,stereo->projection[0]);
		u->viewOrigin.SetArray(2,stereo->origin[0].ToFloatPtr());
	} else {
		u->projectionMatrix.Set(view->projectionMatrix);
		u->viewOrigin.Set(view->renderView.vieworg);
	}
	u->clipPlane.Set(view->numClipPlanes ? view->clipPlane[0] : idPlane(0,0,0,0));
	u->renderResolution.Set(frameBuffers->activeFbo->Width(), frameBuffers->activeFbo->Height());
	u->ambientParams.Set(r_ambientMinLevel.GetFloat(), view->IsLightGem() ? 1.0f : r_ambientGamma.GetFloat());
	// Lightgem is a gameplay measurement and historically uses hard shadows.
	u->shadowFilter.Set(!view->IsLightGem() && r_softShadowsQuality.GetBool());
    return u;
}

void MobileLightingStage::Draw( const viewDef_t *view, const MobileStereoView *stereo ) {
	auto it = impl->views.find(view);
	if ( it == impl->views.end() ) return;
	if ( !view->IsLightGem() ) ++impl->eyePasses;
	impl->checkScissor = R_UseQuestLightScissors();
	TRACE_GL_SCOPE("QuestForwardLighting");
	frameBuffers->currentRenderFbo->Bind();
	FB_ApplyViewport();
	qglEnable(GL_SCISSOR_TEST);
	impl->stereo=stereo;
    ++impl->drawSerial;
    BindShadowTexture(14);
	if(stereo){GL_SelectTexture(15);qglBindTexture(GL_TEXTURE_2D_ARRAY,stereo->ssaoTexture);}
	else ambientOcclusion->BindSSAOTexture(15);
	qglDisable(GL_STENCIL_TEST);
	backEnd.currentScissor = view->scissor;
	FB_ApplyScissor();
	ViewPlan &plan = it->second;
	plan.viewport = RB_QuestViewViewport(view);
	plan.currentScissor = {}; // force the first draw to set actual pixel bounds
	for ( auto &light : plan.lights ) {
		if(stereo && R_UseQuestLightScissors() && light.light->frustumTris){
			light.rect={};const auto &bounds=light.light->frustumTris->bounds;
			for(int eye=0;eye<2;++eye)light.rect.Add(MobileRenderPlan::ProjectScissor(bounds[0].ToFloatPtr(),bounds[1].ToFloatPtr(),
				stereo->worldView[eye],stereo->projection[eye],plan.viewport));
		} else light.rect=RB_QuestLightScissor(view,light.light,plan.viewport);
	}
	for ( const Batch &batch : plan.batches ) impl->DrawBatch(view, plan, batch);
	FB_ApplyScissor();
	qglEnable(GL_STENCIL_TEST);
	qglStencilFunc(GL_ALWAYS, 128, 255);
	GL_Cull(CT_FRONT_SIDED);
	GL_SelectTexture(0);
	GLSLProgram::Deactivate();
	impl->stereo=nullptr;
	if(stereo){++impl->multiviewPasses;--impl->eyePasses;}
}
