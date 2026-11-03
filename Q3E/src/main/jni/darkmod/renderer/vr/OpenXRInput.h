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
#include "renderer/vr/OpenXRBackend.h"

/*
Reads the Quest Touch controllers through OpenXR actions and presents them like a standard gamepad,
so that the engine's existing gamepad handling (movement, smooth turning, GUI cursor, bindings) can be reused:

	left stick			move
	right stick x		turn (smooth)
	right stick y		flick up/down -> PAD_UP / PAD_DOWN
	A, B, X, Y			PAD_A, PAD_B, PAD_X, PAD_Y
	grips				PAD_L1 / PAD_R1
	triggers			PAD_L2 / PAD_R2
	stick clicks		PAD_L3 / PAD_R3
	left menu button	PAD_START
*/
class OpenXRInput {
public:
	bool Init( XrInstance instance, XrSession session );
	void Destroy();

	// Fills 'state' with the current state of the controls.
	// Returns false (and a neutral state) while the application does not have input focus.
	bool Poll( XrSession session, vrControllerState_t &state );

private:
	XrInstance instance = XR_NULL_HANDLE;
	XrActionSet actionSet = XR_NULL_HANDLE;
	bool attached = false;

	XrAction moveAction = XR_NULL_HANDLE;
	XrAction turnAction = XR_NULL_HANDLE;
	XrAction leftTrigger = XR_NULL_HANDLE;
	XrAction rightTrigger = XR_NULL_HANDLE;
	XrAction leftSqueeze = XR_NULL_HANDLE;
	XrAction rightSqueeze = XR_NULL_HANDLE;
	XrAction buttonA = XR_NULL_HANDLE;
	XrAction buttonB = XR_NULL_HANDLE;
	XrAction buttonX = XR_NULL_HANDLE;
	XrAction buttonY = XR_NULL_HANDLE;
	XrAction leftStickClick = XR_NULL_HANDLE;
	XrAction rightStickClick = XR_NULL_HANDLE;
	XrAction menuButton = XR_NULL_HANDLE;

	// previous digital states, used for hysteresis on analog inputs
	bool prevButtons[vrControllerState_t::MAX_BUTTONS] = {};

	XrAction CreateAction( const char *name, const char *localizedName, XrActionType type );
	XrPath ToPath( const char *path ) const;
	bool GetBool( XrSession session, XrAction action, bool &out ) const;
	bool GetFloat( XrSession session, XrAction action, float &out ) const;
	bool GetVec2( XrSession session, XrAction action, float &x, float &y ) const;
	bool HysteresisButton( int button, float value, float pressAt, float releaseAt );
};

#endif // TDM_VR_OPENXR
