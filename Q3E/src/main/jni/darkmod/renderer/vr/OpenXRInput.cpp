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

#include "framework/GamepadInput.h"
#include "renderer/vr/OpenXRInput.h"

XrPath OpenXRInput::ToPath( const char *path ) const {
	XrPath result = XR_NULL_PATH;
	XR_Check( xrStringToPath( instance, path, &result ), "converting path", instance );
	return result;
}

XrAction OpenXRInput::CreateAction( const char *name, const char *localizedName, XrActionType type ) {
	XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
	info.actionType = type;
	info.countSubactionPaths = 0;
	info.subactionPaths = nullptr;
	idStr::Copynz( info.actionName, name, sizeof( info.actionName ) );
	idStr::Copynz( info.localizedActionName, localizedName, sizeof( info.localizedActionName ) );
	XrAction action = XR_NULL_HANDLE;
	XR_Check( xrCreateAction( actionSet, &info, &action ), name, instance );
	return action;
}

bool OpenXRInput::Init( XrInstance inst, XrSession session ) {
	instance = inst;

	XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
	idStr::Copynz( setInfo.actionSetName, "gameplay", sizeof( setInfo.actionSetName ) );
	idStr::Copynz( setInfo.localizedActionSetName, "Gameplay", sizeof( setInfo.localizedActionSetName ) );
	setInfo.priority = 0;
	if ( !XR_Check( xrCreateActionSet( instance, &setInfo, &actionSet ), "creating action set", instance ) ) {
		return false;
	}

	moveAction = CreateAction( "move", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT );
	turnAction = CreateAction( "turn", "Turn", XR_ACTION_TYPE_VECTOR2F_INPUT );
	leftTrigger = CreateAction( "left_trigger", "Left trigger", XR_ACTION_TYPE_FLOAT_INPUT );
	rightTrigger = CreateAction( "right_trigger", "Right trigger", XR_ACTION_TYPE_FLOAT_INPUT );
	leftSqueeze = CreateAction( "left_squeeze", "Left grip", XR_ACTION_TYPE_FLOAT_INPUT );
	rightSqueeze = CreateAction( "right_squeeze", "Right grip", XR_ACTION_TYPE_FLOAT_INPUT );
	buttonA = CreateAction( "button_a", "A button", XR_ACTION_TYPE_BOOLEAN_INPUT );
	buttonB = CreateAction( "button_b", "B button", XR_ACTION_TYPE_BOOLEAN_INPUT );
	buttonX = CreateAction( "button_x", "X button", XR_ACTION_TYPE_BOOLEAN_INPUT );
	buttonY = CreateAction( "button_y", "Y button", XR_ACTION_TYPE_BOOLEAN_INPUT );
	leftStickClick = CreateAction( "left_stick_click", "Left stick click", XR_ACTION_TYPE_BOOLEAN_INPUT );
	rightStickClick = CreateAction( "right_stick_click", "Right stick click", XR_ACTION_TYPE_BOOLEAN_INPUT );
	menuButton = CreateAction( "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT );

	struct Binding {
		XrAction action;
		const char *path;
	};
	const Binding touchBindings[] = {
		{ moveAction,		"/user/hand/left/input/thumbstick" },
		{ turnAction,		"/user/hand/right/input/thumbstick" },
		{ leftTrigger,		"/user/hand/left/input/trigger/value" },
		{ rightTrigger,		"/user/hand/right/input/trigger/value" },
		{ leftSqueeze,		"/user/hand/left/input/squeeze/value" },
		{ rightSqueeze,		"/user/hand/right/input/squeeze/value" },
		{ buttonA,			"/user/hand/right/input/a/click" },
		{ buttonB,			"/user/hand/right/input/b/click" },
		{ buttonX,			"/user/hand/left/input/x/click" },
		{ buttonY,			"/user/hand/left/input/y/click" },
		{ leftStickClick,	"/user/hand/left/input/thumbstick/click" },
		{ rightStickClick,	"/user/hand/right/input/thumbstick/click" },
		{ menuButton,		"/user/hand/left/input/menu/click" },
	};
	idList<XrActionSuggestedBinding> suggested;
	for ( const Binding &b : touchBindings ) {
		if ( b.action == XR_NULL_HANDLE ) {
			continue;
		}
		XrActionSuggestedBinding sb;
		sb.action = b.action;
		sb.binding = ToPath( b.path );
		suggested.Append( sb );
	}

	// note: the Quest 3 controllers are served through the same profile as the older Touch controllers
	XrInteractionProfileSuggestedBinding profile = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	profile.interactionProfile = ToPath( "/interaction_profiles/oculus/touch_controller" );
	profile.countSuggestedBindings = suggested.Num();
	profile.suggestedBindings = suggested.Ptr();
	if ( !XR_Check( xrSuggestInteractionProfileBindings( instance, &profile ), "suggesting touch controller bindings", instance ) ) {
		return false;
	}

	XrSessionActionSetsAttachInfo attachInfo = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &actionSet;
	if ( !XR_Check( xrAttachSessionActionSets( session, &attachInfo ), "attaching action sets", instance ) ) {
		return false;
	}
	attached = true;
	XR_Log( "Touch controller input initialized" );
	return true;
}

void OpenXRInput::Destroy() {
	// actions are destroyed along with their action set
	if ( actionSet != XR_NULL_HANDLE ) {
		xrDestroyActionSet( actionSet );
		actionSet = XR_NULL_HANDLE;
	}
	attached = false;
}

bool OpenXRInput::GetBool( XrSession session, XrAction action, bool &out ) const {
	XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = action;
	XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
	if ( action == XR_NULL_HANDLE || !XR_SUCCEEDED( xrGetActionStateBoolean( session, &info, &state ) ) || !state.isActive ) {
		out = false;
		return false;
	}
	out = state.currentState != XR_FALSE;
	return true;
}

bool OpenXRInput::GetFloat( XrSession session, XrAction action, float &out ) const {
	XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = action;
	XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
	if ( action == XR_NULL_HANDLE || !XR_SUCCEEDED( xrGetActionStateFloat( session, &info, &state ) ) || !state.isActive ) {
		out = 0.0f;
		return false;
	}
	out = state.currentState;
	return true;
}

bool OpenXRInput::GetVec2( XrSession session, XrAction action, float &x, float &y ) const {
	XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = action;
	XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
	if ( action == XR_NULL_HANDLE || !XR_SUCCEEDED( xrGetActionStateVector2f( session, &info, &state ) ) || !state.isActive ) {
		x = y = 0.0f;
		return false;
	}
	x = state.currentState.x;
	y = state.currentState.y;
	return true;
}

// analog value to digital button with hysteresis, so a trigger resting at the threshold does not flutter
bool OpenXRInput::HysteresisButton( int button, float value, float pressAt, float releaseAt ) {
	bool &prev = prevButtons[button];
	if ( prev ) {
		prev = value > releaseAt;
	} else {
		prev = value >= pressAt;
	}
	return prev;
}

bool OpenXRInput::Poll( XrSession session, vrControllerState_t &state ) {
	memset( &state, 0, sizeof( state ) );
	if ( !attached ) {
		return false;
	}

	XrActiveActionSet active = {};
	active.actionSet = actionSet;
	active.subactionPath = XR_NULL_PATH;
	XrActionsSyncInfo syncInfo = { XR_TYPE_ACTIONS_SYNC_INFO };
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &active;
	XrResult result = xrSyncActions( session, &syncInfo );
	if ( result != XR_SUCCESS ) {
		// most likely XR_SESSION_NOT_FOCUSED: the system menu is open or the app is in the background
		memset( prevButtons, 0, sizeof( prevButtons ) );
		return false;
	}

	float lx, ly, rx, ry;
	GetVec2( session, moveAction, lx, ly );
	GetVec2( session, turnAction, rx, ry );
	state.axes[0] = lx;
	state.axes[1] = ly;
	state.axes[2] = rx;
	// vertical movement of the right stick must not pitch the view (the headset does that),
	// so it is only used as a pair of digital buttons
	state.axes[3] = 0.0f;
	state.buttons[PAD_UP] = HysteresisButton( PAD_UP, ry, 0.7f, 0.4f );
	state.buttons[PAD_DOWN] = HysteresisButton( PAD_DOWN, -ry, 0.7f, 0.4f );

	bool b;
	GetBool( session, buttonA, b );	state.buttons[PAD_A] = b;
	GetBool( session, buttonB, b );	state.buttons[PAD_B] = b;
	GetBool( session, buttonX, b );	state.buttons[PAD_X] = b;
	GetBool( session, buttonY, b );	state.buttons[PAD_Y] = b;
	GetBool( session, leftStickClick, b );	state.buttons[PAD_L3] = b;
	GetBool( session, rightStickClick, b );	state.buttons[PAD_R3] = b;
	GetBool( session, menuButton, b );	state.buttons[PAD_START] = b;

	float f;
	GetFloat( session, leftTrigger, f );	state.buttons[PAD_L2] = HysteresisButton( PAD_L2, f, 0.6f, 0.3f );
	GetFloat( session, rightTrigger, f );	state.buttons[PAD_R2] = HysteresisButton( PAD_R2, f, 0.6f, 0.3f );
	GetFloat( session, leftSqueeze, f );	state.buttons[PAD_L1] = HysteresisButton( PAD_L1, f, 0.7f, 0.4f );
	GetFloat( session, rightSqueeze, f );	state.buttons[PAD_R1] = HysteresisButton( PAD_R1, f, 0.7f, 0.4f );

	// diagnostics: report the first button changes in the log (helps to check the controller mapping on a headset)
	static bool announced = false;
	static int reported = 0;
	static bool lastReported[vrControllerState_t::MAX_BUTTONS] = {};
	if ( !announced ) {
		announced = true;
		XR_Log( "controller input is active" );
	}
	for ( int i = 0; i < PAD_NUM_BUTTONS && reported < 100; ++i ) {
		if ( state.buttons[i] != lastReported[i] ) {
			lastReported[i] = state.buttons[i];
			++reported;
			XR_Log( "pad button %s %s", idGamepadInput::ButtonToString( i ).c_str(), state.buttons[i] ? "pressed" : "released" );
		}
	}
	return true;
}

#endif // TDM_VR_OPENXR
