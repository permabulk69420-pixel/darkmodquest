/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#pragma once
#include <memory>
#include "renderer/tr_local.h"
class FrameBuffer;
void R_QuestCopyImage(GLuint source, GLenum sourceTarget, GLuint target, GLenum targetType,
	int width, int height, int layer);

// Layered primary targets and per-eye copies of render-to-texture images let
// commands advance in stereo order without mixing left/right mirror or sky captures.
class MobileMultiviewTargets {
public:
	MobileMultiviewTargets();
	~MobileMultiviewTargets();
	bool BeginFrame(const emptyCommand_t *commands);
	void EndFrame();
	void Shutdown();
	void SelectEye(int eye);
	FrameBuffer *Layered() const;
	GLuint SSAOTexture() const;
	bool IsPerEyeImage(idImage *image) const;
private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
