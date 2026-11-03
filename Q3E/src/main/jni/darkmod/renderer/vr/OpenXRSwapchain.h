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

#ifdef TDM_VR_OPENXR

#include "renderer/vr/xr_common.h"

class FrameBuffer;
class idImage;

/*
A color swapchain of the OpenXR runtime. Every image of the swapchain is wrapped into an engine
FrameBuffer so that the renderer can draw straight into it.
*/
class XrGlSwapchain {
public:
	XrGlSwapchain() = default;
	~XrGlSwapchain();

	bool Create( XrSession session, const char *name, int64_t format, int width, int height );
	void Destroy();

	// Acquires the next image and waits until the compositor is done with it.
	bool Acquire();
	// Hands the current image back to the compositor.
	void Release();
	bool IsAcquired() const { return acquired; }

	FrameBuffer *CurrentFrameBuffer() const;
	XrSwapchainSubImage CurrentSubImage() const;

	int Width() const { return width; }
	int Height() const { return height; }

private:
	XrSwapchain swapchain = XR_NULL_HANDLE;
	int width = 0;
	int height = 0;
	uint32_t currentIndex = 0;
	bool acquired = false;
	idList<FrameBuffer *> fbos;
	idList<idImage *> images;
};

#endif // TDM_VR_OPENXR
