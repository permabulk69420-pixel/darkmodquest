/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#include "precompiled.h"
#include "renderer/backend/stages/MobileLightingStage.h"
#include "renderer/backend/MobileRenderPlan.h"
#include "renderer/backend/stages/InteractionStage.h"
#include "renderer/backend/stages/AmbientOcclusionStage.h"
#include "renderer/backend/GLSLProgramManager.h"
#include "renderer/backend/GLSLUniforms.h"
#include "renderer/backend/FrameBufferManager.h"
#include "renderer/backend/FrameBuffer.h"
#include "renderer/vr/OpenXRBackend.h"
#include <unordered_map>
#include <tuple>
#include <map>

namespace {
#include "glprogs/tdm_constants_shared.glsl"
}

idCVar r_questRenderer( "r_questRenderer", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_INIT,
	"Quest forward renderer: shared stereo shadows and two-light surface batches. Set at launch; 0 uses the legacy renderer." );
idCVar r_questShadowAtlasSize( "r_questShadowAtlasSize", "2048", CVAR_RENDERER | CVAR_INTEGER | CVAR_INIT,
	"Shared stereo depth atlas size in pixels", 512, 4096 );
idCVar r_questShadowFaceSize( "r_questShadowFaceSize", "256", CVAR_RENDERER | CVAR_INTEGER | CVAR_INIT,
	"Maximum shadow face resolution; smaller lights use smaller pages", 32, 512 );
idCVar r_questShowRenderStats( "r_questShowRenderStats", "0", CVAR_RENDERER | CVAR_BOOL,
	"Log shared shadow draws and two-eye lighting batch counts every 120 frames" );
idCVar r_questLightScissor( "r_questLightScissor", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE,
	"Clip mobile lighting, blend lights and volumetrics to bounds projected for the active eye" );
idCVar vr_fog( "vr_fog", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE,
	"Enable fog lights, fog particles, fog sheets and volumetric light effects in VR. Off by default; lightgem rendering is unchanged." );

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

struct LightingUniforms : GLSLUniformGroup {
	UNIFORM_GROUP_DEF( LightingUniforms )
	DEFINE_UNIFORM( mat4, modelMatrix )
	DEFINE_UNIFORM( mat4, modelViewMatrix )
	DEFINE_UNIFORM( mat4, projectionMatrix )
	DEFINE_UNIFORM( vec3, viewOrigin )
	DEFINE_UNIFORM( vec4, clipPlane )
	DEFINE_UNIFORM( vec2, renderResolution )
	DEFINE_UNIFORM( vec2, ambientParams )
	DEFINE_UNIFORM( int, lightCount )
	DEFINE_UNIFORM( int, shadowFilter )
	DEFINE_UNIFORM( int, flags )
	DEFINE_UNIFORM( int, vertexColorMode )
	DEFINE_UNIFORM( vec4, bumpMatrix )
	DEFINE_UNIFORM( vec4, diffuseMatrix )
	DEFINE_UNIFORM( vec4, specularMatrix )
	DEFINE_UNIFORM( vec4, parallaxMatrix )
	DEFINE_UNIFORM( vec2, parallaxHeightScale )
	DEFINE_UNIFORM( ivec3, parallaxIterations )
	DEFINE_UNIFORM( float, parallaxGrazingAngle )
	DEFINE_UNIFORM( float, parallaxShadowSoftness )
	DEFINE_UNIFORM( int, lightFlags )
	DEFINE_UNIFORM( vec3, lightOrigin )
	DEFINE_UNIFORM( vec4, diffuseColor )
	DEFINE_UNIFORM( vec4, specularColor )
	DEFINE_UNIFORM( mat4, lightProjection )
	DEFINE_UNIFORM( vec4, lightTextureMatrix )
	DEFINE_UNIFORM( vec4, shadowPage )
	DEFINE_UNIFORM( vec4, shadowParams )
	DEFINE_UNIFORM( mat4, shadowProjection )
	DEFINE_UNIFORM( sampler, normalTexture )
	DEFINE_UNIFORM( sampler, diffuseTexture )
	DEFINE_UNIFORM( sampler, specularTexture )
	DEFINE_UNIFORM( sampler, parallaxTexture )
	DEFINE_UNIFORM( sampler, projection0 )
	DEFINE_UNIFORM( sampler, projection1 )
	DEFINE_UNIFORM( sampler, cube0 )
	DEFINE_UNIFORM( sampler, cube1 )
	DEFINE_UNIFORM( sampler, falloff0 )
	DEFINE_UNIFORM( sampler, falloff1 )
	DEFINE_UNIFORM( sampler, ambientDiffuse0 )
	DEFINE_UNIFORM( sampler, ambientDiffuse1 )
	DEFINE_UNIFORM( sampler, ambientSpecular0 )
	DEFINE_UNIFORM( sampler, ambientSpecular1 )
	DEFINE_UNIFORM( sampler, shadowAtlas )
	DEFINE_UNIFORM( sampler, ssaoTexture )
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
struct Batch { Packet lights[2]; int count = 0; };
struct LightClip { const viewLight_t *light; MobileRenderPlan::Rect rect; };
struct ViewPlan {
	std::vector<Batch> batches;
	std::vector<LightClip> lights;
	MobileRenderPlan::Rect viewport, currentScissor;
};
struct Shadow {
	const viewDef_t *view;
	const viewLight_t *light;
	MobileRenderPlan::Page page;
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
void Bind( int unit, idImage *image ) { GL_SelectTexture( unit ); image->Bind(); }
}

struct MobileLightingStage::Impl {
	GLSLProgram *lighting = nullptr, *depth = nullptr;
	idImageScratch *atlas = nullptr;
	FrameBuffer *fbo = nullptr;
	int atlasSize = 0;
	std::unordered_map<const viewDef_t *, ViewPlan> views;
	std::unordered_map<const viewLight_t *, size_t> shadowIndex;
	std::vector<Shadow> shadows;
	int packets = 0, batches = 0, shadowDraws = 0, eyeDraws = 0, frames = 0;
	int culledBatches = 0;
	int splitBatches = 0;
	long long scissorPixels = 0, fullPixels = 0;

	void Collect( const viewDef_t *view );
	void DrawShadows();
	void DrawCaster( const drawSurf_t *surf, ShadowUniforms *u );
	void DrawBatch( const viewDef_t *view, ViewPlan &plan, const Batch &batch, LightingUniforms *u );
	bool ShadowInfo( const viewLight_t *light, bool local, idVec4 &page, idVec4 &params, float projection[16] ) const;
};

MobileLightingStage::MobileLightingStage() : impl( new Impl ) {}
MobileLightingStage::~MobileLightingStage() = default;

void MobileLightingStage::Init() {
#ifdef TDM_VR_OPENXR
	impl->lighting = programManager->LoadFromFiles( "quest_forward", "stages/mobile/forward.vert.glsl", "stages/mobile/forward.frag.glsl" );
	impl->depth = programManager->LoadFromFiles( "quest_shadow_depth", "stages/mobile/shadow.vert.glsl", "stages/mobile/shadow.frag.glsl" );
#endif
}
void MobileLightingStage::Shutdown() {
	EndFrame();
	if ( impl->fbo ) impl->fbo->Destroy();
	if ( impl->atlas ) impl->atlas->PurgeImage();
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
bool MobileLightingStage::HasFrame() const { return !impl->views.empty(); }

void MobileLightingStage::BeginFrame( const emptyCommand_t *commands ) {
	EndFrame();
	if ( !R_UseMobileLighting() || r_skipRender.GetBool() ) return;
	// A missing/failed program must not leave the depth shader active while
	// replaying lighting draws, which paints black geometry without lighting.
	if ( !impl->lighting->EnsureLinked() || !impl->depth->EnsureLinked() ) {
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
	std::vector<int> requested;
	std::vector<std::pair<int,int>> shapes;
	for ( const Shadow &s : impl->shadows ) {
		const float coverage = float(s.light->scissorRect.GetArea()) / std::max(1, s.view->viewport.GetArea());
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
	for ( size_t i = 0; i < pages.size(); ++i ) impl->shadows[i].page = pages[i];
	impl->DrawShadows();
}

void MobileLightingStage::Impl::Collect( const viewDef_t *view ) {
	std::vector<Packet> opaque, translucent;
	ViewPlan &plan = views[view];
	// Prefer a surface's original index buffer over per-light clipped index
	// subsets. Light-volume clipping happens in the forward shader. This lets
	// different lights on one mesh share a draw rather than requiring identical
	// legacy lightTris. Ambiguous/shared vertex allocations keep their own mesh.
	auto meshKey = [](const drawSurf_t *s) {
		return std::make_tuple(uintptr_t(s->space), uintptr_t(s->material),
			s->ambientCache.isStatic != 0, s->ambientCache.frameNumber + 0,
			s->ambientCache.offset, s->ambientCache.size);
	};
	std::map<decltype(meshKey((const drawSurf_t *)nullptr)), const drawSurf_t *> meshes;
	for ( int i = 0; i < view->numDrawSurfs; ++i ) {
		const drawSurf_t *surf = view->drawSurfs[i];
		if ( !surf->indexCache.IsValid() || !surf->ambientCache.IsValid() ) continue;
		auto inserted = meshes.emplace(meshKey(surf), surf);
		if ( !inserted.second ) inserted.first->second = nullptr;
	}
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
						const drawSurf_t *geometry = surf;
						auto mesh = meshes.find(meshKey(surf));
						if ( mesh != meshes.end() && mesh->second ) geometry = mesh->second;
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
	if ( std::none_of(opaque.begin(), opaque.end(), HasNegativeColor) )
		std::stable_sort( opaque.begin(), opaque.end(), [](const Packet &a, const Packet &b) { return GeometryKey(a) < GeometryKey(b); } );
	for ( const auto *list : { &opaque, &translucent } ) {
		for ( const Packet &p : *list ) {
			if ( plan.batches.empty() || plan.batches.back().count == 2 || !Compatible(plan.batches.back().lights[0], p) ) plan.batches.push_back({});
			Batch &b = plan.batches.back();
			b.lights[b.count++] = p;
			++packets;
		}
	}
	batches += int(plan.batches.size());
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
	if ( shadows.empty() ) return;
	TRACE_GL_SCOPE( "QuestSharedShadowDepth" );
	FrameBuffer *savedRead = frameBuffers->activeFbo;
	FrameBuffer *savedDraw = frameBuffers->activeDrawFbo;
	const viewDef_t *savedView = backEnd.viewDef;
	if ( !atlas ) atlas = globalImages->ImageScratch("_questShadowAtlas");
	if ( !fbo ) {
		fbo = frameBuffers->CreateFromGenerator( "questSharedShadowDepth", [this](FrameBuffer *fb) {
			fb->Init( atlasSize, atlasSize );
			atlas->GenerateAttachment( atlasSize, atlasSize, GL_DEPTH_COMPONENT24, GL_NEAREST );
			fb->AddDepthRenderTexture(atlas);
			GLenum none = GL_NONE;
			qglDrawBuffers(1, &none);
			qglReadBuffer(GL_NONE);
		} );
	}
	fbo->Bind();
	GL_State( GLS_DEPTHFUNC_LESS | GLS_REDMASK | GLS_GREENMASK | GLS_BLUEMASK | GLS_ALPHAMASK );
	qglDisable( GL_STENCIL_TEST );
	qglEnable( GL_SCISSOR_TEST );
#ifdef _GLES
	qglDepthRangef(0.0f, 1.0f);
	qglClearDepthf(1.0f);
#else
	qglDepthRange(0.0, 1.0);
	qglClearDepth(1.0);
#endif
	qglPolygonOffset(0, 0);
	qglDisable(GL_POLYGON_OFFSET_FILL);
	depth->Activate();
	auto u = depth->GetUniformGroup<ShadowUniforms>();
	u->texture.Set(0);
	for ( const Shadow &s : shadows ) {
		backEnd.viewDef = s.view;
		GL_Cull(CT_TWO_SIDED);
		const auto &p = s.page;
		qglScissor(p.x, p.y, (s.light->mobileParallel ? 1 : 3)*p.size, (s.light->localShadows ? 4 : 2)*p.size);
		qglClear(GL_DEPTH_BUFFER_BIT);
		u->lightOrigin.Set(s.light->globalLightOrigin);
		u->parallel.Set(s.light->mobileParallel);
		if ( s.light->mobileParallel ) u->shadowProjection.Set(s.light->mobileShadowProjection);
		for ( int local = 0; local < 2; ++local ) {
			const drawSurf_t *first = local ? s.light->localShadows : s.light->globalShadows;
			if ( !first ) continue;
			std::vector<std::pair<const drawSurf_t *, unsigned>> casters;
			for ( const drawSurf_t *surf = first; surf; surf = surf->nextOnLight ) {
				unsigned faces = 63;
				if ( !s.light->mobileParallel && !surf->geometryBounds.IsCleared() ) {
					idVec3 corners[8]; surf->geometryBounds.ToPoints(corners);
					idBounds bounds; bounds.Clear();
					for ( const idVec3 &corner : corners ) {
						idVec3 world; R_LocalPointToGlobal(surf->space->modelMatrix, corner, world);
						bounds.AddPoint(world - s.light->globalLightOrigin);
					}
					faces = MobileRenderPlan::CubeFaceMask(bounds[0].ToFloatPtr(), bounds[1].ToFloatPtr());
				}
				casters.emplace_back(surf, faces);
			}
			for ( int face = 0; face < (s.light->mobileParallel ? 1 : 6); ++face ) {
				u->face.Set(face);
				int x = p.x + (face % 3)*p.size, y = p.y + (face / 3 + 2*local)*p.size;
				qglViewport(x, y, p.size, p.size);
				qglScissor(x, y, p.size, p.size);
				for ( const auto &caster : casters )
					if ( caster.second & (1u << face) ) DrawCaster(caster.first, u);
			}
		}
	}
	GLSLProgram::Deactivate();
	backEnd.viewDef = savedView;
	savedRead->Bind(); savedDraw->BindDraw();
	GL_State(GLS_DEFAULT);
	qglEnable(GL_STENCIL_TEST);
	qglEnable(GL_CULL_FACE);
	backEnd.glState.faceCulling = -1;
	// ExecutePass resets culling, viewport/scissor and cached state before an eye.
	// Do not call GL_Cull here: savedView may be null on the first XR frame.
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

void MobileLightingStage::Impl::DrawBatch( const viewDef_t *view, ViewPlan &plan, const Batch &batch, LightingUniforms *u ) {
	const Packet *visible[2];
	int count = 0;
	MobileRenderPlan::Rect rect;
	for ( int i = 0; i < batch.count; ++i ) {
		const Packet &p = batch.lights[i];
		const auto &r = plan.lights[p.lightIndex].rect;
		if ( r.Empty() ) continue;
		visible[count++] = &p;
		rect.Add(r);
	}
	if ( count == 0 ) { fullPixels += plan.viewport.Area(); ++culledBatches; return; }
	if ( count == 2 && rect.Area() > plan.lights[visible[0]->lightIndex].rect.Area() +
		plan.lights[visible[1]->lightIndex].rect.Area() ) {
		// A large empty gap makes a fused draw more expensive than two tight
		// light draws. Retain batching for overlapping lights, not distant ones.
		++splitBatches;
		Batch single;
		single.count = 1;
		for ( int i = 0; i < 2; ++i ) {
			single.lights[0] = *visible[i];
			DrawBatch(view, plan, single, u);
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
	const drawInteraction_t &inter = first.inter;
	const drawSurf_t *surf = first.geometry;
	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK |
		(r_fboColorBits.GetInteger() == 64 ? GLS_ALPHAMASK : 0) |
		(first.translucent ? GLS_DEPTHFUNC_LESS : GLS_DEPTHFUNC_EQUAL) );
	ApplyDepthTweaks tweaks(surf);
	GL_Cull(surf->material->GetCullType());
	vertexCache.VertexPosition(surf->ambientCache);
	u->modelMatrix.Set(surf->space->modelMatrix);
	u->modelViewMatrix.Set(surf->space->modelViewMatrix);
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
	idVec3 origins[2]; idVec4 diffuse[2], specular[2], pages[2], params[2], tex[4];
	float projections[32] = {}, shadowProjections[32] = {};
	int lightFlags[2] = {};
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
		const int base = 4 + 5*i;
		// Samplers of different types always have distinct units, even if the
		// inactive branch won't sample one of them (GLES program validation).
		Bind(base, l->lightShader->IsCubicLight() ? globalImages->whiteImage : d.lightImage);
		Bind(base+1, l->lightShader->IsCubicLight() ? d.lightImage : globalImages->whiteCubeMapImage);
		Bind(base+2, d.lightFalloffImage);
		Bind(base+3, ambientDiffuse ? ambientDiffuse : globalImages->whiteCubeMapImage);
		Bind(base+4, ambientSpecular ? ambientSpecular : globalImages->blackCubeMapImage);
	}
	if ( count == 1 ) {
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

void MobileLightingStage::Draw( const viewDef_t *view ) {
	auto it = impl->views.find(view);
	if ( it == impl->views.end() ) return;
	TRACE_GL_SCOPE("QuestForwardLighting");
	frameBuffers->currentRenderFbo->Bind();
	FB_ApplyViewport();
	qglEnable(GL_SCISSOR_TEST);
	impl->lighting->Activate();
	auto u = impl->lighting->GetUniformGroup<LightingUniforms>();
	u->normalTexture.Set(0); u->diffuseTexture.Set(1); u->specularTexture.Set(2); u->parallaxTexture.Set(3);
	u->projection0.Set(4); u->cube0.Set(5); u->falloff0.Set(6); u->ambientDiffuse0.Set(7); u->ambientSpecular0.Set(8);
	u->projection1.Set(9); u->cube1.Set(10); u->falloff1.Set(11); u->ambientDiffuse1.Set(12); u->ambientSpecular1.Set(13);
	u->shadowAtlas.Set(14); u->ssaoTexture.Set(15);
	BindShadowTexture(14);
	ambientOcclusion->BindSSAOTexture(15);
	u->projectionMatrix.Set(view->projectionMatrix);
	u->viewOrigin.Set(view->renderView.vieworg);
	u->clipPlane.Set(view->numClipPlanes ? view->clipPlane[0] : idPlane(0,0,0,0));
	u->renderResolution.Set(frameBuffers->activeFbo->Width(), frameBuffers->activeFbo->Height());
	u->ambientParams.Set(r_ambientMinLevel.GetFloat(), view->IsLightGem() ? 1.0f : r_ambientGamma.GetFloat());
	// Lightgem is a gameplay measurement and historically uses hard shadows.
	u->shadowFilter.Set(!view->IsLightGem() && r_softShadowsQuality.GetBool());
	qglDisable(GL_STENCIL_TEST);
	backEnd.currentScissor = view->scissor;
	FB_ApplyScissor();
	ViewPlan &plan = it->second;
	plan.viewport = RB_QuestViewViewport(view);
	plan.currentScissor = {}; // force the first draw to set actual pixel bounds
	for ( auto &light : plan.lights ) light.rect = RB_QuestLightScissor(view, light.light, plan.viewport);
	for ( const Batch &batch : plan.batches ) impl->DrawBatch(view, plan, batch, u);
	FB_ApplyScissor();
	qglEnable(GL_STENCIL_TEST);
	qglStencilFunc(GL_ALWAYS, 128, 255);
	GL_Cull(CT_FRONT_SIDED);
	GL_SelectTexture(0);
	GLSLProgram::Deactivate();
}
