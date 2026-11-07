/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#pragma once

// Late OpenXR poses for one view. Ordinary CPU-composed modelview matrices are
// used by both depth and lighting, preserving GL_EQUAL and later material passes.
struct MobileStereoView {
	float projection[2][16];
	float worldView[2][16];
	idVec3 origin[2];
	GLuint ssaoTexture = 0;
	void ModelViews(const viewEntity_t *space,float result[2][16]) const {
		for(int eye=0;eye<2;++eye) myGlMultMatrix(space->modelMatrix,worldView[eye],result[eye]);
	}
};
