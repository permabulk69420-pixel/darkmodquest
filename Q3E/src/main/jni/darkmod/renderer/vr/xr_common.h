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

// Common definitions for the OpenXR (Android / Meta Quest) VR backend.
// Only the VR sources include this, the rest of the engine never sees OpenXR.

#ifdef TDM_VR_OPENXR

#include <jni.h>
#include <EGL/egl.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// Logs to the engine log (and thereby to logcat). Safe to use before the engine console exists.
void XR_Log( const char *fmt, ... );

// Returns true if the call succeeded. Failures are logged together with the OpenXR error name.
bool XR_Check( XrResult result, const char *what, XrInstance instance );

// Unit scale between OpenXR (metres) and the game world.
// The Dark Mod: 1 unit = 2.309 cm
extern const float XR_GameUnitsToMetres;
extern const float XR_MetresToGameUnits;

// Conversions from OpenXR (x right, y up, -z forward) to idTech (x forward, y left, z up).
// Positions are also converted from metres to game units.
idVec3 XR_Vec3( const XrVector3f &v );
idQuat XR_Quat( const XrQuaternionf &q );

#endif // TDM_VR_OPENXR
