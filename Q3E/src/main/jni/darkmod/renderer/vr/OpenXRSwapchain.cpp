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
#include "precompiled.h"
#pragma hdrstop

#ifdef TDM_VR_OPENXR

#include "renderer/tr_local.h"
#include "renderer/backend/FrameBuffer.h"
#include "renderer/backend/FrameBufferManager.h"
#include "renderer/vr/OpenXRSwapchain.h"

namespace {

// A GL texture that belongs to the OpenXR runtime, wrapped so that it can be attached to an engine FrameBuffer.
// The texture must never be deleted by us.
class ExternalGlImage : public idImage {
public:
	ExternalGlImage( GLuint glTexture, int w, int h ) {
		texnum = glTexture;
		uploadWidth = w;
		uploadHeight = h;
		imgName = "xr swapchain image";
	}
	virtual void PurgeImage() override {
		texnum = TEXTURE_NOT_LOADED;
	}
};

}

XrGlSwapchain::~XrGlSwapchain() {
	Destroy();
}

bool XrGlSwapchain::Create( XrSession session, const char *name, int64_t format, int w, int h ) {
	Destroy();

	XrSwapchainCreateInfo createInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	createInfo.format = format;
	createInfo.sampleCount = 1;
	createInfo.width = w;
	createInfo.height = h;
	createInfo.faceCount = 1;
	createInfo.arraySize = 1;
	createInfo.mipCount = 1;
	if ( !XR_Check( xrCreateSwapchain( session, &createInfo, &swapchain ), "creating swapchain", XR_NULL_HANDLE ) ) {
		swapchain = XR_NULL_HANDLE;
		return false;
	}
	width = w;
	height = h;

	uint32_t imageCount = 0;
	if ( !XR_Check( xrEnumerateSwapchainImages( swapchain, 0, &imageCount, nullptr ), "counting swapchain images", XR_NULL_HANDLE ) ) {
		Destroy();
		return false;
	}
	idList<XrSwapchainImageOpenGLESKHR> glImages;
	glImages.SetNum( imageCount );
	for ( int i = 0; i < glImages.Num(); ++i ) {
		glImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
		glImages[i].next = nullptr;
		glImages[i].image = 0;
	}
	if ( !XR_Check( xrEnumerateSwapchainImages( swapchain, imageCount, &imageCount,
			reinterpret_cast<XrSwapchainImageBaseHeader *>( glImages.Ptr() ) ), "enumerating swapchain images", XR_NULL_HANDLE ) ) {
		Destroy();
		return false;
	}

	for ( uint32_t i = 0; i < imageCount; ++i ) {
		idImage *image = new ExternalGlImage( glImages[i].image, w, h );
		images.Append( image );

		idStr fboName = idStr::Fmt( "%s %d", name, (int)i );
		// the generator is run lazily the first time the FrameBuffer is bound, and again after every FrameBufferManager::PurgeAll
		FrameBuffer *fbo = frameBuffers->CreateFromGenerator( fboName, [image, w, h]( FrameBuffer *f ) {
			f->Init( w, h );
			f->AddColorRenderTexture( 0, image );
		} );
		fbos.Append( fbo );
	}

	XR_Log( "Created swapchain '%s': %dx%d, %d images", name, w, h, (int)imageCount );
	return true;
}

void XrGlSwapchain::Destroy() {
	if ( acquired ) {
		Release();
	}
	for ( FrameBuffer *fbo : fbos ) {
		fbo->Destroy();
		// make sure a later PurgeAll+Bind never touches the deleted image
		fbo->SetGenerator( []( FrameBuffer * ) {} );
	}
	fbos.Clear();
	for ( idImage *image : images ) {
		image->PurgeImage();
		delete image;
	}
	images.Clear();
	if ( swapchain != XR_NULL_HANDLE ) {
		xrDestroySwapchain( swapchain );
		swapchain = XR_NULL_HANDLE;
	}
	width = height = 0;
}

bool XrGlSwapchain::Acquire() {
	if ( swapchain == XR_NULL_HANDLE ) {
		return false;
	}
	if ( acquired ) {
		Release();
	}

	if ( !XR_Check( xrAcquireSwapchainImage( swapchain, nullptr, &currentIndex ), "acquiring swapchain image", XR_NULL_HANDLE ) ) {
		return false;
	}
	acquired = true;

	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = 1000000000;	// 1s
	XrResult result = xrWaitSwapchainImage( swapchain, &waitInfo );
	if ( !XR_SUCCEEDED( result ) || result == XR_TIMEOUT_EXPIRED ) {
		XR_Log( "waiting for swapchain image failed (%d)", (int)result );
		Release();
		return false;
	}
	return true;
}

void XrGlSwapchain::Release() {
	if ( !acquired ) {
		return;
	}
	// some runtimes complain about stale GL errors when releasing the image
	qglGetError();
	XR_Check( xrReleaseSwapchainImage( swapchain, nullptr ), "releasing swapchain image", XR_NULL_HANDLE );
	acquired = false;
}

FrameBuffer *XrGlSwapchain::CurrentFrameBuffer() const {
	assert( acquired );
	return fbos[currentIndex];
}

XrSwapchainSubImage XrGlSwapchain::CurrentSubImage() const {
	XrSwapchainSubImage sub = {};
	sub.swapchain = swapchain;
	sub.imageRect.offset.x = 0;
	sub.imageRect.offset.y = 0;
	sub.imageRect.extent.width = width;
	sub.imageRect.extent.height = height;
	sub.imageArrayIndex = 0;
	return sub;
}

#endif // TDM_VR_OPENXR
