/*****************************************************************************
The Dark Mod GPL Source Code

This file is part of the The Dark Mod Source Code, originally based
on the Doom 3 GPL Source Code as published in 2011.

The Dark Mod Source Code is free software: you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the License,
or (at your option) any later version. For details, see LICENSE.TXT.

Project: The Dark Mod (http://www.thedarkmod.com/)

******************************************************************************/
#pragma once

class FrameBuffer;
class GLSLProgram;

class TonemapStage {
public:
	void Init();
	void Shutdown();

	void ApplyTonemap( FrameBuffer *destinationFbo, idImage *sourceTexture );
	bool ApplyTonemapArray(FrameBuffer *destinationFbo,GLuint texture,int layer);

private:
	GLSLProgram *tonemapShader = nullptr;
	GLSLProgram *arrayShader = nullptr;
	uint64_t arrayCheckedGeneration = 0;
	bool arrayReady = false;
	void Apply(FrameBuffer *destinationFbo,idImage *sourceTexture,GLuint arrayTexture,int layer,GLSLProgram *shader);

	struct Uniforms;
};
