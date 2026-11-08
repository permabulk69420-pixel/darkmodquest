/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#pragma once
#ifdef TDM_VR_OPENXR
#include <memory>
#include "renderer/vr/xr_common.h"
struct viewDef_s;
struct MobileStereoView;

// Owns real runtime motion/depth swapchains and previous rendered geometry.
// The existing OpenXR session, eye swapchains, tracking and HUD stay in charge.
class OpenXRSpaceWarp {
public:
	OpenXRSpaceWarp();
	~OpenXRSpaceWarp();
	bool Initialize(XrSession session, int width, int height, const int64_t *formats, unsigned count);
	void Shutdown();
	void BeginFrame();
	void ResetHistory();
	bool Render(const viewDef_s *view, const MobileStereoView &stereo, const XrPosef &appSpacePose,
		XrTime displayTime, float nearMetres);
	bool Chain(XrCompositionLayerProjectionView (&views)[2], XrCompositionLayerSpaceWarpInfoFB (&info)[2]) const;
	void CompleteFrame(bool submitted, XrResult result = XR_SUCCESS);
	void GetDiagnostics(char *buffer, int size, bool extensionAvailable) const;
private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
extern idCVar vr_spaceWarp;
#endif
