/*****************************************************************************
                    The Dark Mod GPL Source Code

 This file is part of the The Dark Mod Source Code, originally based
 on the Doom 3 GPL Source Code as published in 2011.

 The Dark Mod Source Code is free software: you can redistribute it
 and/or modify it under the terms of the GNU General Public License as
 published by the Free Software Foundation, either version 3 of the License,
 or (at your option) any later version. For details, see LICENSE.TXT.

 Project: The Dark Mod (http://www.thedarkmod.com/)

 VR support for Android / Meta Quest through OpenXR.
 The design follows "The Dark Mod VR" by Holger Frydrych (fholger): the frontend runs once with a combined
 view that encloses both eyes, and the backend replays the recorded commands for each eye with fresh matrices.

******************************************************************************/
#pragma once

struct renderView_s;
struct frameData_s;

/*
State of the Touch controllers, laid out like a standard gamepad.
buttons[] is indexed by padNum_t (framework/GamepadInput.h), axes are left stick x/y and right stick x/y in [-1, 1].
*/
struct vrControllerState_t {
	enum { MAX_BUTTONS = 32 };
	bool	buttons[MAX_BUTTONS];
	float	axes[4];
};

/*
Public interface of the VR backend. Everything that deals with OpenXR is hidden in the implementation
so that the rest of the engine does not need to see any OpenXR headers.
All functions are safe to call when VR is not in use, they then do nothing / report "not active".
*/
class OpenXRBackend {
public:
	OpenXRBackend();

	// Creates the XR instance, session and swapchains if the application was launched in VR mode.
	// Must be called with the GL context current (from RenderBackend::Init).
	void Init();
	void Destroy();

	// true if the application runs in VR mode and the OpenXR session exists
	bool IsActive() const;

	// Main thread, once per frame, before the frontend is started for the next frame:
	// handles OpenXR events and predicts the head pose the frontend should work with.
	void PrepareFrame();

	// Frontend: applies the predicted head pose to the given (body) view, turning it into the
	// combined stereo view. Does nothing if no head pose is available.
	void AdjustRenderView( struct renderView_s *view );

	// Backend: draws the whole frame (both eyes and the UI overlay) and submits it to the compositor.
	void RenderStereoView( const struct frameData_s *frameData );

	// Predicted orientation of the head relative to the tracking space, in idTech axes.
	// Returns false if there is no valid head pose.
	bool GetHeadOrientation( idQuat &orientation ) const;

	// Reads the current state of the controllers. Returns false if input is not available.
	bool PollControllers( vrControllerState_t &state );

private:
	struct Impl;
	Impl *impl;
};

extern OpenXRBackend *vrBackend;
