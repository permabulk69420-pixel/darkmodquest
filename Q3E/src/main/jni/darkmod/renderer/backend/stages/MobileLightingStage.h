/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#pragma once

#include "renderer/tr_local.h"
#include "renderer/backend/MobileRenderPlan.h"
#include <memory>

// Quest's forward renderer. The combined-view frontend is retained; all
// eye-independent work is prepared before the OpenXR eye command replays.
bool R_UseMobileLighting();
bool R_QuestFogDisabled( const viewDef_t *view );
bool R_QuestSkipFogSurface( const viewDef_t *view, const srfTriangles_t *tri, const idMaterial *material );
bool R_QuestSkipFogVolume( const viewDef_t *view, const idMaterial *material );
bool R_UseQuestLightScissors();
MobileRenderPlan::Rect RB_QuestViewViewport( const viewDef_t *view );
MobileRenderPlan::Rect RB_QuestLightScissor( const viewDef_t *view, const viewLight_t *light,
	const MobileRenderPlan::Rect &viewport );

class MobileLightingStage {
public:
	MobileLightingStage();
	~MobileLightingStage();
	void Init();
	void Shutdown();
	void BeginFrame( const emptyCommand_t *commands );
	void EndFrame();
	bool HasView( const viewDef_t *view ) const;
	bool CanDrawMultiview( const viewDef_t *view, const class MobileMultiviewTargets &targets ) const;
	bool HasFrame() const;
	bool PrepareMultiview();
	void GetDiagnosticsText( char *buffer, int size ) const; // render thread only
	void GetFoveationDiagnostics(char *buffer,int size) const;
	void Draw( const viewDef_t *view, const struct MobileStereoView *stereo = nullptr );
	// Volumetric lights sample the same shared depth as opaque/translucent
	// surfaces, so there is no second shadow renderer or unshadowed light leak.
	bool GetShadow( const viewDef_t *view, const viewLight_t *light, idVec4 &page,
		idVec4 &params, idMat4 &projection ) const;
	void BindShadowTexture( int unit );

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
