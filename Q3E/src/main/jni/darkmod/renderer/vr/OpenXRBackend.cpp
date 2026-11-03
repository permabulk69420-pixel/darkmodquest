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

 The overall design (one frontend pass with a combined view, the backend replaying the commands for each eye
 with fresh matrices, UI drawn into a separate overlay) follows "The Dark Mod VR" by Holger Frydrych (fholger),
 https://github.com/fholger/thedarkmodvr

******************************************************************************/
#include "precompiled.h"
#pragma hdrstop

#include "renderer/vr/OpenXRBackend.h"

OpenXRBackend xrBackendInstance;
OpenXRBackend *vrBackend = &xrBackendInstance;

#ifdef TDM_VR_OPENXR

#include <mutex>
#include <vector>
#include <string>
#include <stdarg.h>
#include <unistd.h>
#include <android/log.h>

#include "renderer/tr_local.h"
#include "renderer/frontend/RenderWorld_local.h"
#include "renderer/backend/FrameBuffer.h"
#include "renderer/backend/FrameBufferManager.h"
#include "renderer/backend/RenderBackend.h"
#include "framework/GamepadInput.h"
#include "renderer/vr/xr_common.h"
#include "renderer/vr/OpenXRSwapchain.h"
#include "renderer/vr/OpenXRInput.h"

// provided by sys/android
extern bool Sys_VR_IsRequested( void );
extern void *Sys_VR_GetJavaVM( void );
extern void *Sys_VR_GetActivity( void );
extern void GLimp_GetEGLHandles( void **display, void **config, void **context );

// provided by renderer/backend/tr_backend.cpp
bool RB_Bloom( bloomCommand_t *cmd );
void RB_CopyRender( const void *data );

idCVar vr_renderScale( "vr_renderScale", "0.65", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
	"Resolution of the eye buffers relative to the headset's recommended resolution (takes effect on restart)", 0.25f, 2.0f );
idCVar vr_refreshRate( "vr_refreshRate", "72", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
	"Display refresh rate to request from the headset, e.g. 72, 80, 90 or 120 on Quest (takes effect on restart)" );
idCVar vr_uiOverlayHeight( "vr_uiOverlayHeight", "1.8", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
	"Height of the HUD/menu screen in metres" );
idCVar vr_uiOverlayDistance( "vr_uiOverlayDistance", "2.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
	"Distance of the HUD/menu screen from the player in metres" );
idCVar vr_uiOverlayVerticalOffset( "vr_uiOverlayVerticalOffset", "-0.2", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
	"Vertical offset of the HUD/menu screen in metres" );

namespace {

// the command chain links "base" commands; every command in it starts with the fields of emptyCommand_t
inline const emptyCommand_t *NextCommand( const emptyCommand_t *cmd ) {
	return static_cast<const emptyCommand_t *>( cmd->next );
}

const int UI_WIDTH = 1920;
const int UI_HEIGHT = 1440;	// 4:3, like the 640x480 virtual screen of the GUIs

XrInstance g_instanceForErrors = XR_NULL_HANDLE;

}

const float XR_GameUnitsToMetres = 0.02309f;
const float XR_MetresToGameUnits = 1.0f / XR_GameUnitsToMetres;

void XR_Log( const char *fmt, ... ) {
	char buf[1024];
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	__android_log_print( ANDROID_LOG_INFO, "TDM-XR", "%s", buf );
	if ( common ) {
		common->Printf( "XR: %s\n", buf );
	}
}

bool XR_Check( XrResult result, const char *what, XrInstance instance ) {
	if ( XR_SUCCEEDED( result ) ) {
		return true;
	}
	char name[XR_MAX_RESULT_STRING_SIZE] = "?";
	XrInstance inst = instance != XR_NULL_HANDLE ? instance : g_instanceForErrors;
	if ( inst != XR_NULL_HANDLE ) {
		xrResultToString( inst, result, name );
	}
	XR_Log( "error %d (%s) while %s", (int)result, name, what );
	return false;
}

idVec3 XR_Vec3( const XrVector3f &v ) {
	return idVec3( -v.z, -v.x, v.y ) * XR_MetresToGameUnits;
}

idQuat XR_Quat( const XrQuaternionf &q ) {
	return idQuat( q.z, q.x, -q.y, q.w );
}

/*
=============================================================================================
Implementation
=============================================================================================
*/
struct OpenXRBackend::Impl {
	bool initialized = false;
	bool sessionRunning = false;
	bool sessionFocused = false;
	bool frameBegun = false;
	bool shouldRender = false;
	bool padBindingsChecked = false;

	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace localSpace = XR_NULL_HANDLE;	// seated tracking space
	XrSpace viewSpace = XR_NULL_HANDLE;		// moves with the head
	XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;

	int64_t swapchainFormat = 0;
	int eyeWidth = 0;
	int eyeHeight = 0;
	XrGlSwapchain eyeSwapchains[2];
	XrGlSwapchain uiSwapchain;

	// pose data
	XrView renderViews[2];			// poses of the frame being rendered
	bool haveRenderViews = false;
	XrView predictedViews[2];		// poses the frontend is working with
	bool havePredictedViews = false;
	XrTime predictedDisplayTime = 0;
	XrDuration displayPeriod = 0;

	// HUD placement
	bool menuAnchored = false;
	XrPosef menuPose;

	OpenXRInput input;
	std::mutex inputMutex;

	// extension functions and availability
	bool hasRefreshRate = false;
	bool hasPerfSettings = false;
	bool hasThreadSettings = false;
	PFN_xrEnumerateDisplayRefreshRatesFB pfnEnumerateRefreshRates = nullptr;
	PFN_xrRequestDisplayRefreshRateFB pfnRequestRefreshRate = nullptr;
	PFN_xrPerfSettingsSetPerformanceLevelEXT pfnSetPerfLevel = nullptr;
	PFN_xrSetAndroidApplicationThreadKHR pfnSetAppThread = nullptr;

	// ---- setup ----
	bool Initialize();
	bool CreateInstance();
	bool CreateSessionAndSpaces();
	bool CreateSwapchains();
	void Shutdown();

	// ---- session ----
	void HandleEvents();
	void OnSessionStateChanged( const XrEventDataSessionStateChanged &e );
	void OnSessionBegun();
	void ApplyFallbackPadBindings();

	// ---- frame ----
	bool BeginFrame();
	void EndFrame( bool submitLayers, bool hasWorldView );
	bool LocateViews( XrTime time, XrView views[2] );
	void RenderFrame( const frameData_t *fd );

	// ---- rendering ----
	enum Pass { PASS_LEFT = 0, PASS_RIGHT = 1, PASS_UI = 2 };
	void ExecutePass( const frameData_t *fd, Pass pass, FrameBuffer *target );
	void UpdateViewsForEye( const emptyCommand_t *cmds, int eye );
	void RepositionView( viewDef_t *vd, int eye );
	static bool IsWorldView( const viewDef_t *vd );
	static bool FrameHasWorldView( const frameData_t *fd );
};

OpenXRBackend::OpenXRBackend() {
	impl = new Impl;
}

bool OpenXRBackend::Impl::IsWorldView( const viewDef_t *vd ) {
	// 2D GUI overlays and the 3D windows inside GUIs (renderDef) belong to the UI, only real map views are stereo
	return vd->viewEntitys != nullptr && vd->renderWorld != nullptr && !vd->renderWorld->mapName.IsEmpty();
}

bool OpenXRBackend::Impl::FrameHasWorldView( const frameData_t *fd ) {
	for ( const emptyCommand_t *cmd = fd->cmdHead; cmd; cmd = NextCommand( cmd ) ) {
		if ( cmd->commandId == RC_DRAW_VIEW ) {
			const viewDef_t *vd = ( (const drawSurfsCommand_t *)cmd )->viewDef;
			if ( IsWorldView( vd ) && !vd->IsLightGem() ) {
				return true;
			}
		}
	}
	return false;
}

/*
=============================================================================================
Setup
=============================================================================================
*/
bool OpenXRBackend::Impl::CreateInstance() {
	// the loader needs the JVM and the activity before anything else
	PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
	xrGetInstanceProcAddr( XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction *>( &initializeLoader ) );
	if ( initializeLoader ) {
		XrLoaderInitInfoAndroidKHR loaderInit = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
		loaderInit.applicationVM = Sys_VR_GetJavaVM();
		loaderInit.applicationContext = Sys_VR_GetActivity();
		if ( !XR_Check( initializeLoader( reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>( &loaderInit ) ), "initializing the loader", XR_NULL_HANDLE ) ) {
			return false;
		}
	}

	uint32_t extCount = 0;
	xrEnumerateInstanceExtensionProperties( nullptr, 0, &extCount, nullptr );
	std::vector<XrExtensionProperties> extensions( extCount, XrExtensionProperties{ XR_TYPE_EXTENSION_PROPERTIES } );
	xrEnumerateInstanceExtensionProperties( nullptr, extCount, &extCount, extensions.data() );
	auto hasExtension = [&]( const char *name ) {
		for ( const XrExtensionProperties &e : extensions ) {
			if ( strcmp( e.extensionName, name ) == 0 ) {
				return true;
			}
		}
		return false;
	};

	if ( !hasExtension( XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME ) || !hasExtension( XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME ) ) {
		XR_Log( "the OpenXR runtime does not support OpenGL ES on Android (%u extensions found)", extCount );
		return false;
	}

	std::vector<const char *> enabled;
	enabled.push_back( XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME );
	enabled.push_back( XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME );
	hasRefreshRate = hasExtension( XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME );
	if ( hasRefreshRate ) {
		enabled.push_back( XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME );
	}
	hasPerfSettings = hasExtension( XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME );
	if ( hasPerfSettings ) {
		enabled.push_back( XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME );
	}
	hasThreadSettings = hasExtension( XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME );
	if ( hasThreadSettings ) {
		enabled.push_back( XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME );
	}

	XrInstanceCreateInfoAndroidKHR androidInfo = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	androidInfo.applicationVM = Sys_VR_GetJavaVM();
	androidInfo.applicationActivity = Sys_VR_GetActivity();

	XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
	createInfo.next = &androidInfo;
	createInfo.createFlags = 0;
	idStr::Copynz( createInfo.applicationInfo.applicationName, "The Dark Mod", sizeof( createInfo.applicationInfo.applicationName ) );
	createInfo.applicationInfo.applicationVersion = 1;
	idStr::Copynz( createInfo.applicationInfo.engineName, "idTech 4", sizeof( createInfo.applicationInfo.engineName ) );
	createInfo.applicationInfo.engineVersion = 1;
	createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	createInfo.enabledExtensionCount = (uint32_t)enabled.size();
	createInfo.enabledExtensionNames = enabled.data();
	if ( !XR_Check( xrCreateInstance( &createInfo, &instance ), "creating the instance", XR_NULL_HANDLE ) ) {
		instance = XR_NULL_HANDLE;
		return false;
	}
	g_instanceForErrors = instance;

	XrInstanceProperties props = { XR_TYPE_INSTANCE_PROPERTIES };
	if ( XR_SUCCEEDED( xrGetInstanceProperties( instance, &props ) ) ) {
		XR_Log( "runtime: %s %u.%u.%u", props.runtimeName,
			(unsigned)XR_VERSION_MAJOR( props.runtimeVersion ), (unsigned)XR_VERSION_MINOR( props.runtimeVersion ), (unsigned)XR_VERSION_PATCH( props.runtimeVersion ) );
	}

	XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if ( !XR_Check( xrGetSystem( instance, &systemInfo, &systemId ), "getting the system", instance ) ) {
		return false;
	}
	XrSystemProperties sysProps = { XR_TYPE_SYSTEM_PROPERTIES };
	if ( XR_SUCCEEDED( xrGetSystemProperties( instance, systemId, &sysProps ) ) ) {
		XR_Log( "system: %s", sysProps.systemName );
	}

	// extension functions are not exported by the loader
	xrGetInstanceProcAddr( instance, "xrEnumerateDisplayRefreshRatesFB", reinterpret_cast<PFN_xrVoidFunction *>( &pfnEnumerateRefreshRates ) );
	xrGetInstanceProcAddr( instance, "xrRequestDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction *>( &pfnRequestRefreshRate ) );
	xrGetInstanceProcAddr( instance, "xrPerfSettingsSetPerformanceLevelEXT", reinterpret_cast<PFN_xrVoidFunction *>( &pfnSetPerfLevel ) );
	xrGetInstanceProcAddr( instance, "xrSetAndroidApplicationThreadKHR", reinterpret_cast<PFN_xrVoidFunction *>( &pfnSetAppThread ) );
	return true;
}

bool OpenXRBackend::Impl::CreateSessionAndSpaces() {
	// must be called before a session is created, although we do not need the result
	PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
	xrGetInstanceProcAddr( instance, "xrGetOpenGLESGraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction *>( &getRequirements ) );
	if ( !getRequirements ) {
		XR_Log( "xrGetOpenGLESGraphicsRequirementsKHR is not available" );
		return false;
	}
	XrGraphicsRequirementsOpenGLESKHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
	if ( !XR_Check( getRequirements( instance, systemId, &requirements ), "querying OpenGL ES requirements", instance ) ) {
		return false;
	}
	XR_Log( "OpenGL ES %u.%u - %u.%u required",
		(unsigned)XR_VERSION_MAJOR( requirements.minApiVersionSupported ), (unsigned)XR_VERSION_MINOR( requirements.minApiVersionSupported ),
		(unsigned)XR_VERSION_MAJOR( requirements.maxApiVersionSupported ), (unsigned)XR_VERSION_MINOR( requirements.maxApiVersionSupported ) );

	void *display = nullptr, *config = nullptr, *context = nullptr;
	GLimp_GetEGLHandles( &display, &config, &context );

	XrGraphicsBindingOpenGLESAndroidKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
	binding.display = (EGLDisplay)display;
	binding.config = (EGLConfig)config;
	binding.context = (EGLContext)context;

	XrSessionCreateInfo sessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
	sessionInfo.next = &binding;
	sessionInfo.createFlags = 0;
	sessionInfo.systemId = systemId;
	if ( !XR_Check( xrCreateSession( instance, &sessionInfo, &session ), "creating the session", instance ) ) {
		session = XR_NULL_HANDLE;
		return false;
	}
	XR_Log( "session created" );

	XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if ( !XR_Check( xrCreateReferenceSpace( session, &spaceInfo, &localSpace ), "creating the local space", instance ) ) {
		return false;
	}
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if ( !XR_Check( xrCreateReferenceSpace( session, &spaceInfo, &viewSpace ), "creating the view space", instance ) ) {
		return false;
	}
	return true;
}

bool OpenXRBackend::Impl::CreateSwapchains() {
	uint32_t formatCount = 0;
	xrEnumerateSwapchainFormats( session, 0, &formatCount, nullptr );
	std::vector<int64_t> formats( formatCount );
	xrEnumerateSwapchainFormats( session, formatCount, &formatCount, formats.data() );

	// The engine's final tonemapping pass already produces gamma-corrected colors, so we want a plain
	// (non-sRGB) format that the compositor displays as is.
	const int64_t wanted[] = { GL_RGBA8, GL_SRGB8_ALPHA8, GL_RGBA16F };
	swapchainFormat = 0;
	for ( int64_t w : wanted ) {
		for ( int64_t f : formats ) {
			if ( f == w ) {
				swapchainFormat = f;
				break;
			}
		}
		if ( swapchainFormat ) {
			break;
		}
	}
	if ( swapchainFormat == 0 ) {
		XR_Log( "no suitable swapchain format (%u offered)", formatCount );
		return false;
	}
	XR_Log( "swapchain format: 0x%x", (unsigned)swapchainFormat );

	XrViewConfigurationView views[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
	uint32_t viewCount = 0;
	if ( !XR_Check( xrEnumerateViewConfigurationViews( instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, views ),
			"getting the view configuration", instance ) || viewCount != 2 ) {
		return false;
	}
	XR_Log( "recommended eye resolution: %ux%u (max %ux%u)", views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight,
		views[0].maxImageRectWidth, views[0].maxImageRectHeight );

	const float scale = vr_renderScale.GetFloat();
	eyeWidth = idMath::Imin( (int)( views[0].recommendedImageRectWidth * scale ) & ~1, (int)views[0].maxImageRectWidth );
	eyeHeight = idMath::Imin( (int)( views[0].recommendedImageRectHeight * scale ) & ~1, (int)views[0].maxImageRectHeight );
	if ( eyeWidth < 256 || eyeHeight < 256 ) {
		eyeWidth = views[0].recommendedImageRectWidth;
		eyeHeight = views[0].recommendedImageRectHeight;
	}
	XR_Log( "eye buffer resolution: %dx%d (render scale %.2f)", eyeWidth, eyeHeight, scale );

	if ( !eyeSwapchains[0].Create( session, "xr left eye", swapchainFormat, eyeWidth, eyeHeight ) ||
		 !eyeSwapchains[1].Create( session, "xr right eye", swapchainFormat, eyeWidth, eyeHeight ) ||
		 !uiSwapchain.Create( session, "xr ui", swapchainFormat, UI_WIDTH, UI_HEIGHT ) ) {
		return false;
	}

	// The renderer sizes its main framebuffers from the "video size". Make it match the eye buffers.
	glConfig.vidWidth = eyeWidth;
	glConfig.vidHeight = eyeHeight;
	r_fboResolution.SetModified();
	return true;
}

bool OpenXRBackend::Impl::Initialize() {
	XR_Log( "-------- initializing VR --------" );
	if ( !CreateInstance() || !CreateSessionAndSpaces() || !CreateSwapchains() ) {
		Shutdown();
		return false;
	}
	input.Init( instance, session );

	// The GUI is drawn at 640x480 virtual pixels and has no aspect ratio fixup: it fills the whole overlay, which is 4:3.
	// Scissors computed by the frontend are valid for the combined view only and are of no use for the individual eyes.
	cvarSystem->SetCVarBool( "r_useScissor", false );
	cvarSystem->SetCVarInteger( "r_aspectRatio", 0 );

	initialized = true;
	XR_Log( "-------- VR initialized --------" );
	return true;
}

void OpenXRBackend::Impl::Shutdown() {
	input.Destroy();
	eyeSwapchains[0].Destroy();
	eyeSwapchains[1].Destroy();
	uiSwapchain.Destroy();
	if ( viewSpace != XR_NULL_HANDLE ) {
		xrDestroySpace( viewSpace );
		viewSpace = XR_NULL_HANDLE;
	}
	if ( localSpace != XR_NULL_HANDLE ) {
		xrDestroySpace( localSpace );
		localSpace = XR_NULL_HANDLE;
	}
	if ( session != XR_NULL_HANDLE ) {
		xrDestroySession( session );
		session = XR_NULL_HANDLE;
	}
	if ( instance != XR_NULL_HANDLE ) {
		xrDestroyInstance( instance );
		instance = XR_NULL_HANDLE;
		g_instanceForErrors = XR_NULL_HANDLE;
	}
	initialized = sessionRunning = sessionFocused = frameBegun = false;
	haveRenderViews = havePredictedViews = false;
	predictedDisplayTime = 0;
}

/*
=============================================================================================
Session
=============================================================================================
*/
void OpenXRBackend::Impl::OnSessionBegun() {
	if ( hasRefreshRate && pfnRequestRefreshRate && pfnEnumerateRefreshRates ) {
		uint32_t count = 0;
		pfnEnumerateRefreshRates( session, 0, &count, nullptr );
		std::vector<float> rates( count );
		if ( count > 0 && XR_SUCCEEDED( pfnEnumerateRefreshRates( session, count, &count, rates.data() ) ) ) {
			const float wantedRate = (float)vr_refreshRate.GetInteger();
			bool supported = false;
			for ( float r : rates ) {
				supported = supported || fabsf( r - wantedRate ) < 0.5f;
			}
			if ( supported ) {
				XR_Check( pfnRequestRefreshRate( session, wantedRate ), "requesting the display refresh rate", instance );
				XR_Log( "requested display refresh rate: %.0f Hz", wantedRate );
			} else {
				XR_Log( "refresh rate %.0f Hz is not supported by this headset", wantedRate );
			}
		}
	}
	if ( hasPerfSettings && pfnSetPerfLevel ) {
		pfnSetPerfLevel( session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT );
		pfnSetPerfLevel( session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT );
	}
	if ( hasThreadSettings && pfnSetAppThread ) {
		// this is the thread that owns the GL context and submits frames
		pfnSetAppThread( session, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, (uint32_t)gettid() );
	}
}

void OpenXRBackend::Impl::OnSessionStateChanged( const XrEventDataSessionStateChanged &e ) {
	XR_Log( "session state %d -> %d", (int)sessionState, (int)e.state );
	sessionState = e.state;
	sessionFocused = ( e.state == XR_SESSION_STATE_FOCUSED );

	switch ( e.state ) {
	case XR_SESSION_STATE_READY: {
		XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
		beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		if ( XR_Check( xrBeginSession( session, &beginInfo ), "beginning the session", instance ) ) {
			sessionRunning = true;
			OnSessionBegun();
		}
		break;
	}
	case XR_SESSION_STATE_STOPPING:
		sessionRunning = false;
		haveRenderViews = havePredictedViews = false;
		predictedDisplayTime = 0;
		XR_Check( xrEndSession( session ), "ending the session", instance );
		break;
	case XR_SESSION_STATE_EXITING:
	case XR_SESSION_STATE_LOSS_PENDING:
		sessionRunning = false;
		XR_Log( "the headset asked us to exit" );
		cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "quit\n" );
		break;
	default:
		break;
	}
}

void OpenXRBackend::Impl::HandleEvents() {
	for ( ;; ) {
		XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
		if ( xrPollEvent( instance, &event ) != XR_SUCCESS ) {
			break;
		}
		switch ( event.type ) {
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
			OnSessionStateChanged( *reinterpret_cast<XrEventDataSessionStateChanged *>( &event ) );
			break;
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			XR_Log( "instance loss pending" );
			cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "quit\n" );
			break;
		default:
			break;
		}
	}
}

// If the game data does not come with gamepad bindings, give the controllers a useful default layout.
void OpenXRBackend::Impl::ApplyFallbackPadBindings() {
	if ( padBindingsChecked || !common->IsInitialized() ) {
		return;
	}
	padBindingsChecked = true;
	if ( idGamepadInput::HasAnyBinding() ) {
		return;
	}
	XR_Log( "no gamepad bindings found, applying the default VR controller layout" );
	static const char *binds[][2] = {
		{ "PAD_A", "_jump" },
		{ "PAD_B", "_crouch" },
		{ "PAD_X", "_inventory_use" },
		{ "PAD_Y", "_ready" },
		{ "PAD_R2", "_attack" },
		{ "PAD_L2", "_frob" },
		{ "PAD_L1", "_lean_left" },
		{ "PAD_R1", "_lean_right" },
		{ "PAD_L3", "_speed" },
		{ "PAD_R3", "_creep" },
		{ "PAD_UP", "_inventory_prev" },
		{ "PAD_DOWN", "_inventory_next" },
		{ "PAD_START", "escape" },	// opens the main menu
	};
	for ( const auto &b : binds ) {
		cmdSystem->BufferCommandText( CMD_EXEC_NOW, va( "bindPadButton PRESS %s %s\n", b[0], b[1] ) );
	}
}

/*
=============================================================================================
Frame
=============================================================================================
*/
bool OpenXRBackend::Impl::LocateViews( XrTime time, XrView views[2] ) {
	XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = time;
	locateInfo.space = localSpace;
	XrViewState viewState = { XR_TYPE_VIEW_STATE };
	XrView located[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	uint32_t count = 0;
	XrResult result = xrLocateViews( session, &locateInfo, &viewState, 2, &count, located );
	if ( !XR_SUCCEEDED( result ) || count != 2 || !( viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT ) ) {
		return false;
	}
	views[0] = located[0];
	views[1] = located[1];
	return true;
}

bool OpenXRBackend::Impl::BeginFrame() {
	if ( !sessionRunning ) {
		return false;
	}

	XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState frameState = { XR_TYPE_FRAME_STATE };
	if ( !XR_Check( xrWaitFrame( session, &waitInfo, &frameState ), "waiting for the next frame", instance ) ) {
		return false;
	}
	predictedDisplayTime = frameState.predictedDisplayTime;
	displayPeriod = frameState.predictedDisplayPeriod > 0 ? frameState.predictedDisplayPeriod : 13888889;	// fall back to 72 Hz
	shouldRender = frameState.shouldRender != XR_FALSE;

	XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
	if ( !XR_Check( xrBeginFrame( session, &beginInfo ), "beginning the frame", instance ) ) {
		return false;
	}
	frameBegun = true;

	if ( LocateViews( predictedDisplayTime, renderViews ) ) {
		haveRenderViews = true;
	}
	return true;
}

void OpenXRBackend::Impl::EndFrame( bool submitLayers, bool hasWorldView ) {
	if ( !frameBegun ) {
		return;
	}
	frameBegun = false;

	XrCompositionLayerProjectionView projectionViews[2] = {
		{ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
		{ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
	};
	XrCompositionLayerProjection projectionLayer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerQuad uiLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	const XrCompositionLayerBaseHeader *layers[2];
	uint32_t layerCount = 0;

	if ( submitLayers && haveRenderViews ) {
		for ( int eye = 0; eye < 2; ++eye ) {
			projectionViews[eye].pose = renderViews[eye].pose;
			projectionViews[eye].fov = renderViews[eye].fov;
			projectionViews[eye].subImage = eyeSwapchains[eye].CurrentSubImage();
		}
		projectionLayer.layerFlags = 0;
		projectionLayer.space = localSpace;
		projectionLayer.viewCount = 2;
		projectionLayer.views = projectionViews;
		layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>( &projectionLayer );

		// HUD: attached to the head while playing, floating in front of the player (but not following) in menus
		const float height = vr_uiOverlayHeight.GetFloat();
		uiLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		uiLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		uiLayer.subImage = uiSwapchain.CurrentSubImage();
		uiLayer.size.height = height;
		uiLayer.size.width = height * ( (float)UI_WIDTH / (float)UI_HEIGHT );
		if ( hasWorldView ) {
			menuAnchored = false;
			uiLayer.space = viewSpace;
			uiLayer.pose.orientation.w = 1.0f;
			uiLayer.pose.position.x = 0.0f;
			uiLayer.pose.position.y = vr_uiOverlayVerticalOffset.GetFloat();
			uiLayer.pose.position.z = -vr_uiOverlayDistance.GetFloat();
		} else {
			if ( !menuAnchored ) {
				// place the screen in front of wherever the player is looking right now, level with the horizon
				const XrPosef &p = renderViews[0].pose;
				const XrQuaternionf &q = p.orientation;
				// forward vector of the head (-z rotated by q)
				const float fx = -2.0f * ( q.x * q.z + q.w * q.y );
				const float fz = -( 1.0f - 2.0f * ( q.x * q.x + q.y * q.y ) );
				const float yaw = atan2f( -fx, -fz );
				const float len = sqrtf( fx * fx + fz * fz );
				const float hx = len > 0.001f ? fx / len : 0.0f;
				const float hz = len > 0.001f ? fz / len : -1.0f;
				const float dist = vr_uiOverlayDistance.GetFloat();
				menuPose.orientation.x = 0.0f;
				menuPose.orientation.y = sinf( yaw * 0.5f );
				menuPose.orientation.z = 0.0f;
				menuPose.orientation.w = cosf( yaw * 0.5f );
				menuPose.position.x = p.position.x + hx * dist;
				menuPose.position.y = p.position.y + vr_uiOverlayVerticalOffset.GetFloat();
				menuPose.position.z = p.position.z + hz * dist;
				menuAnchored = true;
			}
			uiLayer.space = localSpace;
			uiLayer.pose = menuPose;
		}
		layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>( &uiLayer );
	}

	XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
	endInfo.displayTime = predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = layerCount;
	endInfo.layers = layers;
	XR_Check( xrEndFrame( session, &endInfo ), "submitting the frame", instance );
}

/*
=============================================================================================
Rendering
=============================================================================================
*/
static void XR_BuildProjection( const XrFovf &fov, float zNear, float m[16] ) {
	const float l = tanf( fov.angleLeft );
	const float r = tanf( fov.angleRight );
	const float u = tanf( fov.angleUp );
	const float d = tanf( fov.angleDown );
	const float w = r - l;
	const float h = u - d;

	// same "far plane at infinity" layout as R_ComputeProjection, but for an asymmetric frustum
	m[0] = 2.0f / w;			m[4] = 0.0f;				m[8] = ( r + l ) / w;		m[12] = 0.0f;
	m[1] = 0.0f;				m[5] = 2.0f / h;			m[9] = ( u + d ) / h;		m[13] = 0.0f;
	m[2] = 0.0f;				m[6] = 0.0f;				m[10] = -0.999f;			m[14] = -2.0f * zNear;
	m[3] = 0.0f;				m[7] = 0.0f;				m[11] = -1.0f;				m[15] = 0.0f;
}

/*
Moves a view to the pose of the given eye. Views derived from the player's view are handled as follows:
- the main view and the xray view sit at the eye
- the portal sky only turns with the head (it is infinitely far away)
- mirror views show the eye as reflected through the mirror(s)
*/
void OpenXRBackend::Impl::RepositionView( viewDef_t *vd, int eye ) {
	renderView_t &rv = vd->renderView;

	// head pose of this eye relative to the tracking origin
	const idVec3 eyePos = XR_Vec3( renderViews[eye].pose.position );
	const idMat3 eyeAxis = XR_Quat( renderViews[eye].pose.orientation ).ToMat3();

	idVec3 origin = rv.initialVieworg + eyePos * rv.initialViewaxis;
	idMat3 axis = eyeAxis * rv.initialViewaxis;

	if ( rv.fixedOrigin ) {
		origin = rv.vieworg;
	} else if ( rv.isMirror ) {
		// apply the mirrors from the outermost one to the one that produced this view
		const viewDef_t *chain[8];
		int numMirrors = 0;
		for ( const viewDef_t *v = vd; v && v->renderView.isMirror && numMirrors < 8; v = v->superView ) {
			chain[numMirrors++] = v;
		}
		for ( int i = numMirrors - 1; i >= 0; --i ) {
			orientation_t surface = chain[i]->renderView.mirrorSurface;
			orientation_t camera = chain[i]->renderView.mirrorCamera;
			idVec3 mirroredOrigin;
			R_MirrorPoint( origin, &surface, &camera, mirroredOrigin );
			origin = mirroredOrigin;
			idMat3 mirroredAxis;
			for ( int k = 0; k < 3; ++k ) {
				R_MirrorVector( axis[k], &surface, &camera, mirroredAxis[k] );
			}
			axis = mirroredAxis;
		}
	}

	rv.vieworg = origin;
	rv.viewaxis = axis;
	rv.eyeorg[0] = rv.eyeorg[1] = origin;

	// projection of this eye
	float zNear = r_znear.GetFloat();
	if ( rv.cramZNear ) {
		zNear *= 0.25f;
	}
	XR_BuildProjection( renderViews[eye].fov, zNear, vd->projectionMatrix );

	// view matrices. R_SetViewMatrix resets the whole world space entity, so keep everything but the matrices.
	viewEntity_t savedWorld = vd->worldSpace;
	R_SetViewMatrix( *vd );
	memcpy( savedWorld.modelViewMatrix, vd->worldSpace.modelViewMatrix, sizeof( savedWorld.modelViewMatrix ) );
	vd->worldSpace = savedWorld;

	idRenderMatrix::Transpose( *(idRenderMatrix *)vd->projectionMatrix, vd->projectionRenderMatrix );
	idRenderMatrix::Multiply( vd->projectionRenderMatrix, vd->viewRenderMatrix, vd->worldSpace.mvp );

	// all entities (and the GUIs on surfaces, which are not in the entity list) need their modelview matrix updated
	for ( viewEntity_t *ve = vd->viewEntitys; ve; ve = ve->next ) {
		myGlMultMatrix( ve->modelMatrix, vd->worldSpace.modelViewMatrix, ve->modelViewMatrix );
	}
	for ( int i = 0; i < vd->numDrawSurfs; ++i ) {
		drawSurf_t *surf = vd->drawSurfs[i];
		if ( surf->dsFlags & DSF_GUI_SURF ) {
			viewEntity_t *space = const_cast<viewEntity_t *>( surf->space );
			myGlMultMatrix( space->modelMatrix, vd->worldSpace.modelViewMatrix, space->modelViewMatrix );
		}
	}
}

void OpenXRBackend::Impl::UpdateViewsForEye( const emptyCommand_t *cmds, int eye ) {
	// Views are listed in the order they are drawn, i.e. subviews come before the views that show them.
	// A mirror's parent is always processed first because the mirror state is derived from the parents' data only.
	for ( const emptyCommand_t *cmd = cmds; cmd; cmd = NextCommand( cmd ) ) {
		if ( cmd->commandId != RC_DRAW_VIEW ) {
			continue;
		}
		viewDef_t *vd = const_cast<viewDef_t *>( ( (const drawSurfsCommand_t *)cmd )->viewDef );
		if ( !IsWorldView( vd ) || vd->IsLightGem() ) {
			continue;
		}
		renderView_t &rv = vd->renderView;
		if ( !rv.followsHead ) {
			if ( vd->isSubview || rv.viewID != VID_PLAYER_VIEW ) {
				continue;	// e.g. security camera views have their own pose
			}
			// The frontend did not get a head pose for this view (first frames of a session). Treat the view as the body view.
			rv.initialVieworg = rv.vieworg;
			rv.initialViewaxis = rv.viewaxis;
			rv.followsHead = true;
		}
		RepositionView( vd, eye );
	}
}

/*
Same as RB_ExecuteBackEndCommands, but only does the part of the work that belongs to the given pass:
- eye passes draw the 3D views (and everything that works on them, up to and including the tonemapping)
- the UI pass draws the 2D views (HUD, menus, console). When the frame is flagged as "render2D" (e.g. cutscenes)
  it draws the 3D views as well.
*/
void OpenXRBackend::Impl::ExecutePass( const frameData_t *fd, Pass pass, FrameBuffer *target ) {
	const bool isEye = ( pass != PASS_UI );
	const bool drawWorld = isEye || fd->render2D;
	const bool drawUI = !isEye;
	const bool drawLightgem = ( pass == PASS_LEFT ) || ( pass == PASS_UI && fd->render2D );

	// render this pass into the swapchain image instead of the window
	frameBuffers->defaultFbo = target;
	frameBuffers->currentRenderFbo = target;
	target->Bind();
	frameBuffers->tonemapNotYet = drawWorld && r_tonemap.GetBool();

	GL_ViewportRelative( 0, 0, 1, 1 );
	GL_ScissorRelative( 0, 0, 1, 1 );
	if ( isEye ) {
		qglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	} else {
		qglClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	}
	qglClear( GL_COLOR_BUFFER_BIT );

	if ( isEye ) {
		UpdateViewsForEye( fd->cmdHead, pass );
	}

	RB_SetDefaultGLState();

	// true: views are drawn into the primary framebuffer (which can be multisampled)
	// false: the main rendering is done and we are drawing straight into the target
	bool drawToPrimary = drawWorld;
	bool outputColorIsBackground = false;

	for ( const emptyCommand_t *cmds = fd->cmdHead; cmds; cmds = NextCommand( cmds ) ) {
		switch ( cmds->commandId ) {
		case RC_NOP:
			break;
		case RC_DRAW_VIEW: {
			backEnd.viewDef = ( (const drawSurfsCommand_t *)cmds )->viewDef;
			const bool isv3d = IsWorldView( backEnd.viewDef );
			if ( isv3d ? !drawWorld : !drawUI ) {
				break;
			}
			if ( drawToPrimary ) {
				if ( isv3d ) {
					frameBuffers->EnterPrimary();
				} else {
					frameBuffers->LeavePrimary();
					FB_DebugShowContents();
					drawToPrimary = false;
				}
			}
			renderBackend->DrawView( backEnd.viewDef, outputColorIsBackground );
			GL_CheckErrors();
			outputColorIsBackground = backEnd.viewDef->outputColorIsBackground;
			break;
		}
		case RC_DRAW_LIGHTGEM:
			if ( drawLightgem ) {
				backEnd.viewDef = ( (const drawLightgemCommand_t *)cmds )->viewDef;
				renderBackend->DrawLightgem( backEnd.viewDef, ( (const drawLightgemCommand_t *)cmds )->dataBuffer );
			}
			break;
		case RC_SET_BUFFER:
			// there is no window to select a buffer of
			break;
		case RC_BLOOM:
			if ( drawWorld && RB_Bloom( (bloomCommand_t *)cmds ) ) {
				FB_DebugShowContents();
				frameBuffers->LeavePrimary();
			}
			break;
		case RC_COPY_RENDER:
			if ( drawWorld ) {
				RB_CopyRender( cmds );
			}
			break;
		case RC_TONEMAP: {
			if ( !drawWorld ) {
				break;
			}
			const bool forceOutputToBlack = ( (const tonemapCommand_t *)cmds )->forceOutputToBlack;
			frameBuffers->LeavePrimary();
			if ( !forceOutputToBlack ) {
				renderBackend->Tonemap();
			} else {
				frameBuffers->defaultFbo->Bind();
				qglClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
				qglClear( GL_COLOR_BUFFER_BIT );
			}
			drawToPrimary = false;
			frameBuffers->tonemapNotYet = false;
			frameBuffers->LeavePrimary( false );
			break;
		}
		default:
			common->Error( "OpenXRBackend::ExecutePass: bad commandId" );
			break;
		}
	}

	// go back to the default texture so nothing is left bound
	qglBindTexture( GL_TEXTURE_2D, 0 );
	backEnd.glState.tmu[0].current2DMap = -1;
}

void OpenXRBackend::Impl::RenderFrame( const frameData_t *fd ) {
	if ( !BeginFrame() ) {
		return;
	}

	bool rendered = false;
	const bool hasWorldView = FrameHasWorldView( fd ) && !fd->render2D;

	if ( shouldRender && haveRenderViews &&
		 eyeSwapchains[0].Acquire() && eyeSwapchains[1].Acquire() && uiSwapchain.Acquire() ) {
		FrameBuffer *realDefault = frameBuffers->defaultFbo;

		ExecutePass( fd, PASS_LEFT, eyeSwapchains[0].CurrentFrameBuffer() );
		ExecutePass( fd, PASS_RIGHT, eyeSwapchains[1].CurrentFrameBuffer() );
		ExecutePass( fd, PASS_UI, uiSwapchain.CurrentFrameBuffer() );

		frameBuffers->defaultFbo = realDefault;
		frameBuffers->currentRenderFbo = realDefault;
		realDefault->Bind();

		qglFlush();
		rendered = true;
	}

	eyeSwapchains[0].Release();
	eyeSwapchains[1].Release();
	uiSwapchain.Release();
	EndFrame( rendered, hasWorldView );
}

/*
=============================================================================================
Public interface
=============================================================================================
*/
void OpenXRBackend::Init() {
	if ( impl->initialized || !Sys_VR_IsRequested() ) {
		return;
	}
	if ( !impl->Initialize() ) {
		XR_Log( "VR initialization failed, continuing without VR" );
	}
}

void OpenXRBackend::Destroy() {
	if ( impl->initialized ) {
		impl->Shutdown();
	}
}

bool OpenXRBackend::IsActive() const {
	return impl->initialized;
}

void OpenXRBackend::PrepareFrame() {
	if ( !impl->initialized ) {
		return;
	}
	impl->HandleEvents();
	impl->ApplyFallbackPadBindings();

	if ( !impl->sessionRunning || impl->predictedDisplayTime == 0 ) {
		return;
	}
	// The display time we know is that of the frame currently being drawn by the backend (or the one before it).
	// The frontend prepares the frame after the next one.
	XrView views[2];
	if ( impl->LocateViews( impl->predictedDisplayTime + 2 * impl->displayPeriod, views ) ) {
		impl->predictedViews[0] = views[0];
		impl->predictedViews[1] = views[1];
		impl->havePredictedViews = true;
	}
}

void OpenXRBackend::AdjustRenderView( renderView_t *view ) {
	if ( !impl->initialized || !impl->havePredictedViews ) {
		return;
	}

	const XrView &left = impl->predictedViews[0];
	const XrView &right = impl->predictedViews[1];
	const idVec3 leftEyePos = XR_Vec3( left.pose.position );
	const idVec3 rightEyePos = XR_Vec3( right.pose.position );
	const idQuat leftEyeRot = XR_Quat( left.pose.orientation );
	const idQuat rightEyeRot = XR_Quat( right.pose.orientation );
	const idMat3 leftEyeMat = leftEyeRot.ToMat3();
	const idMat3 rightEyeMat = rightEyeRot.ToMat3();

	// The combined view has to enclose both eye frustums.
	const float maxHorizFov = Max( Max( idMath::Fabs( left.fov.angleLeft ), idMath::Fabs( left.fov.angleRight ) ),
								   Max( idMath::Fabs( right.fov.angleLeft ), idMath::Fabs( right.fov.angleRight ) ) );
	const float maxVertFov = Max( Max( idMath::Fabs( left.fov.angleUp ), idMath::Fabs( left.fov.angleDown ) ),
								  Max( idMath::Fabs( right.fov.angleUp ), idMath::Fabs( right.fov.angleDown ) ) );
	// canted displays: the eyes do not look into exactly the same direction
	const float cantedAngle = idMath::Fabs( idMath::ACos( idMath::ClampFloat( -1.0f, 1.0f, leftEyeMat[0] * rightEyeMat[0] ) ) );
	// add some slack, since the view will have moved a little by the time it is drawn, but stay below 180 degrees
	view->fov_x = Min( 178.0f, RAD2DEG( cantedAngle + 2.0f * maxHorizFov ) + 5.0f );
	view->fov_y = Min( 178.0f, RAD2DEG( 2.0f * maxVertFov ) + 5.0f );

	idQuat viewOrientation = 0.5f * ( leftEyeRot + rightEyeRot );
	viewOrientation.Normalize();
	const idMat3 viewAxis = viewOrientation.ToMat3();
	idVec3 viewOrigin = 0.5f * ( leftEyePos + rightEyePos );
	// move the combined origin back a bit, so both eye frustums fit into the combined frustum
	const float eyeDistance = ( rightEyePos - leftEyePos ).Length();
	const float offset = 0.5f * eyeDistance / idMath::Tan( DEG2RAD( view->fov_x * 0.5f ) );
	viewOrigin -= viewAxis[0] * offset;

	view->initialViewaxis = view->viewaxis;
	view->initialVieworg = view->vieworg;
	view->followsHead = true;
	view->fixedOrigin = false;
	view->isMirror = false;
	view->vieworg += viewOrigin * view->viewaxis;
	view->viewaxis = viewAxis * view->viewaxis;
	view->eyeorg[0] = view->initialVieworg + leftEyePos * view->initialViewaxis;
	view->eyeorg[1] = view->initialVieworg + rightEyePos * view->initialViewaxis;

	// the near plane of the combined view must not cut off anything an eye can see
	const idVec3 leftNearZ = leftEyePos + idMath::Cos( left.fov.angleLeft ) * leftEyeMat[0] - idMath::Sin( left.fov.angleLeft ) * leftEyeMat[1];
	const idVec3 rightNearZ = rightEyePos + idMath::Cos( right.fov.angleRight ) * rightEyeMat[0] - idMath::Sin( right.fov.angleRight ) * rightEyeMat[1];
	const float nearZ = ( 0.5f * ( leftNearZ + rightNearZ ) - viewOrigin ).Length();
	const float znear = r_znear.GetFloat();
	view->nearZOffset = Max( nearZ - znear, -0.9f * znear );
}

void OpenXRBackend::RenderStereoView( const frameData_t *frameData ) {
	if ( !impl->initialized ) {
		return;
	}
	if ( !impl->sessionRunning ) {
		// nothing to draw to right now (the headset is not worn, the system menu is up, ...): don't spin
		Sys_Sleep( 8 );
		return;
	}
	impl->RenderFrame( frameData );
}

bool OpenXRBackend::GetHeadOrientation( idQuat &orientation ) const {
	if ( !impl->initialized || !impl->havePredictedViews ) {
		return false;
	}
	idQuat q = XR_Quat( impl->predictedViews[0].pose.orientation ) + XR_Quat( impl->predictedViews[1].pose.orientation );
	q.Normalize();
	orientation = q;
	return true;
}

bool OpenXRBackend::PollControllers( vrControllerState_t &state ) {
	memset( &state, 0, sizeof( state ) );
	if ( !impl->initialized || !impl->sessionRunning ) {
		return false;
	}
	std::lock_guard<std::mutex> lock( impl->inputMutex );
	return impl->input.Poll( impl->session, state );
}

#else // !TDM_VR_OPENXR

// VR is only available on Android (Meta Quest). Everywhere else the VR backend is inert.
struct OpenXRBackend::Impl {};

OpenXRBackend::OpenXRBackend() : impl( nullptr ) {}
void OpenXRBackend::Init() {}
void OpenXRBackend::Destroy() {}
bool OpenXRBackend::IsActive() const { return false; }
void OpenXRBackend::PrepareFrame() {}
void OpenXRBackend::AdjustRenderView( struct renderView_s * ) {}
void OpenXRBackend::RenderStereoView( const struct frameData_s * ) {}
bool OpenXRBackend::GetHeadOrientation( idQuat & ) const { return false; }
bool OpenXRBackend::PollControllers( vrControllerState_t &state ) { memset( &state, 0, sizeof( state ) ); return false; }

#endif // TDM_VR_OPENXR
