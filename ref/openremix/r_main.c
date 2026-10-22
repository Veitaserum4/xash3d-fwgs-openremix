/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_main.c - Lifecycle implementation & frame orchestration
 */

#include "r_local.h"
#include "r_remix.h"
#include "r_textures.h"
#include "r_bsp.h"
#include "r_studio.h"
#include "r_ui.h"
#include "r_sprite.h"
#include "r_lights.h"
#include "r_xashrt.h"

// Menu/loading warm-up: set when R_RenderScene ran this frame
static qboolean s_sceneRendered = false;
static cvar_t  *rt_menu_warmup  = NULL;
static remixapi_LightHandle s_warmupLight = NULL;

/*
 * Runtime settings exposed to the game menu (mainui "Ray Tracing" page).
 * Pushed to OpenRemix only when they differ from the runtime's defaults at startup, or when
 * changed, so settings saved from the F12 overlay are not overridden on every launch.
 */
typedef struct
{
	const char *cvar;
	const char *defaultValue;
	const char *description;
	cvar_t     *ptr;
	char        applied[32];
} rt_setting_t;

static rt_setting_t s_settings[] =
{
	{ "rt_upscaler",  "3", "Upscaler: 0 off, 1 auto, 2 FSR 3.1, 3 DLSS" },
	{ "rt_vsync",     "0", "Vertical sync" },
	{ "rt_framegen",  "0", "Frame generation" },
};

// Fog cvars are owned by r_lights.c; changes re-apply the volumetric configuration
static const char *s_fogCvars[] = { "rt_fog", "rt_fog_distance", "rt_fog_scattering" };
static char s_fogApplied[ARRAYSIZE( s_fogCvars )][32];

static void R_Settings_Push( rt_setting_t *s )
{
	static const char *upscalers[] = { "off", "auto", "fsr31", "dlss" };
	const char *key = NULL, *value = NULL;

	if( !Q_strcmp( s->cvar, "rt_upscaler" ) )
	{
		key   = "rtx.native.pathtrace.upscaler";
		value = upscalers[bound( 0, (int)s->ptr->value, (int)ARRAYSIZE( upscalers ) - 1 )];
	}
	else if( !Q_strcmp( s->cvar, "rt_vsync" ) )
	{
		key   = "rtx.native.vsync";
		value = s->ptr->value > 0.0f ? "1" : "0";
	}
	else if( !Q_strcmp( s->cvar, "rt_framegen" ) )
	{
		key   = "rtx.native.frameGeneration";
		value = s->ptr->value > 0.0f ? "1" : "0";
	}

	if( key && g_remix.SetConfigVariable )
	{
		remixapi_ErrorCode err = g_remix.SetConfigVariable( key, value );
		if( err != REMIXAPI_ERROR_CODE_SUCCESS )
			gEngfuncs.Con_Printf( S_WARN "ref_openremix: %s = %s rejected: %s\n", key, value, R_Remix_GetErrorString( err ) );
	}
}

static void R_Settings_Init( void )
{
	for( size_t i = 0; i < ARRAYSIZE( s_settings ); i++ )
	{
		rt_setting_t *s = &s_settings[i];
		s->ptr = gEngfuncs.Cvar_Get( s->cvar, s->defaultValue, FCVAR_ARCHIVE, s->description );
		// Leave the runtime's own (possibly overlay-saved) value alone unless the user chose otherwise
		Q_strncpy( s->applied, s->defaultValue, sizeof( s->applied ) );
	}

	for( size_t i = 0; i < ARRAYSIZE( s_fogCvars ); i++ )
	{
		cvar_t *c = gEngfuncs.pfnGetCvarPointer( s_fogCvars[i] );
		Q_strncpy( s_fogApplied[i], c ? c->string : "", sizeof( s_fogApplied[i] ) );
	}
}

/*
 * R_Settings_Sync - Apply menu/console changes to OpenRemix (called once per frame)
 */
static void R_Settings_Sync( void )
{
	for( size_t i = 0; i < ARRAYSIZE( s_settings ); i++ )
	{
		rt_setting_t *s = &s_settings[i];
		if( !s->ptr || !Q_strcmp( s->ptr->string, s->applied ) )
			continue;
		Q_strncpy( s->applied, s->ptr->string, sizeof( s->applied ) );
		R_Settings_Push( s );
	}

	qboolean fogChanged = false;
	for( size_t i = 0; i < ARRAYSIZE( s_fogCvars ); i++ )
	{
		cvar_t *c = gEngfuncs.pfnGetCvarPointer( s_fogCvars[i] );
		if( c && Q_strcmp( c->string, s_fogApplied[i] ) )
		{
			Q_strncpy( s_fogApplied[i], c->string, sizeof( s_fogApplied[i] ) );
			fogChanged = true;
		}
	}
	if( fogChanged )
		R_Lights_UpdateVolumetrics();
}

const char *R_GetConfigName( void )
{
	return "openremix";
}

qboolean R_Init( void )
{
	if( tr.initialized )
		return true;

	gEngfuncs.Con_Printf( "^2ref_openremix: Initializing OpenRemix Native Renderer...^7\n" );

	// Allocate memory pool for renderer
	r_temppool = Mem_AllocPool( "OpenRemix Zone" );
	if( !r_temppool )
	{
		gEngfuncs.Con_Reportf( S_ERROR "ref_openremix: Failed to allocate memory pool\n" );
		return false;
	}

	// 1. Create native window via engine video subsystem in software mode (clean window surface, no GL/WGL context)
	if( !gEngfuncs.R_Init_Video( REF_SOFTWARE ) )
	{
		gEngfuncs.Con_Reportf( S_ERROR "ref_openremix: R_Init_Video failed\n" );
		Mem_FreePool( &r_temppool );
		return false;
	}

	// 2. Retrieve Win32 window HWND from video subsystem
	void *hwnd = NULL;
	ref_window_type_t wtype = gEngfuncs.R_GetWindowHandle( &hwnd, REF_WINDOW_TYPE_WIN32 );
	if( wtype != REF_WINDOW_TYPE_WIN32 || !hwnd )
	{
		gEngfuncs.Con_Reportf( S_ERROR "ref_openremix: Failed to acquire Win32 window HWND\n" );
		gEngfuncs.R_Free_Video();
		Mem_FreePool( &r_temppool );
		return false;
	}

	// 3. Dynamically load remix.dll and initialize Remix C API dispatch table
	const char *custom_path = ( gEngfuncs.pfnGetCvarString ) ? gEngfuncs.pfnGetCvarString( "r_openremix_path" ) : "";
	if( !Remix_LoadAndInitialize( custom_path ) )
	{
		gEngfuncs.Con_Reportf( S_ERROR "ref_openremix: Failed to load and initialize remix.dll\n" );
		gEngfuncs.R_Free_Video();
		Mem_FreePool( &r_temppool );
		return false;
	}

	// 4. Start OpenRemix path tracing runtime bound to the window HWND
	if( !R_Remix_Startup( hwnd ) )
	{
		gEngfuncs.Con_Reportf( S_ERROR "ref_openremix: Failed to startup OpenRemix runtime on HWND\n" );
		Remix_ShutdownAndUnload();
		gEngfuncs.R_Free_Video();
		Mem_FreePool( &r_temppool );
		return false;
	}

	// 5. Configure global path tracer coordinates and units
	if( g_remix.SetConfigVariable )
	{
		// GoldSrc units: 1 unit = 0.0254 meters (1 inch)
		g_remix.SetConfigVariable( "rtx.native.pathtrace.sceneScale", "0.0254" );
		// Unlimited physically based falloff (disable artificial 16-unit cutoff)
		g_remix.SetConfigVariable( "rtx.native.pathtrace.localRange", "0" );
		// GoldSrc coordinates: Z-up (0: X, 1: Y, 2: Z)
		g_remix.SetConfigVariable( "rtx.native.pathtrace.volumetricUpAxis", "2" );
		// Enable viewmodel ray-traced lighting
		g_remix.SetConfigVariable( "rtx.native.pathtrace.viewModel", "1" );
		g_remix.SetConfigVariable( "rtx.native.pathtrace.viewModelScale", "1.0" );
	}

	// Ensure mouse centering in windowed mode and raw input for full 360-degree rotation
	if( gEngfuncs.Cvar_SetValue )
	{
		gEngfuncs.Cvar_SetValue( "_windowed_mouse", 0.0f );
		gEngfuncs.Cvar_SetValue( "m_rawinput", 1.0f );
	}
	if( gEngfuncs.Cbuf_AddText )
	{
		gEngfuncs.Cbuf_AddText( "_windowed_mouse 0\n" );
		gEngfuncs.Cbuf_AddText( "m_rawinput 1\n" );
		gEngfuncs.Cbuf_AddText( "bind F12 openremix_menu\n" );
		// Keep "~" WAD textures paletted so their fullbright pixels become the emission mask
		// (latched: takes effect from the next map load)
		gEngfuncs.Cbuf_AddText( "r_allow_wad3_luma 1\n" );
	}

	// 6. Cache engine parameters
	tr.world            = (struct world_static_s *)ENGINE_GET_PARM( PARM_GET_WORLD_PTR );
	tr.palette          = (color24 *)ENGINE_GET_PARM( PARM_GET_PALETTE_PTR );
	tr.viewent          = (cl_entity_t *)ENGINE_GET_PARM( PARM_GET_VIEWENT_PTR );
	tr.texgammatable    = (byte *)ENGINE_GET_PARM( PARM_GET_TEXGAMMATABLE_PTR );
	tr.lightgammatable  = (uint16_t *)ENGINE_GET_PARM( PARM_GET_LIGHTGAMMATABLE_PTR );
	tr.screengammatable = (uint16_t *)ENGINE_GET_PARM( PARM_GET_SCREENGAMMATABLE_PTR );
	tr.lineargammatable = (uint16_t *)ENGINE_GET_PARM( PARM_GET_LINEARGAMMATABLE_PTR );
	tr.elights          = (dlight_t *)ENGINE_GET_PARM( PARM_GET_ELIGHTS_PTR );

	// 7. Initialize rendering subsystems
	R_Textures_Init();
	R_BSP_Init();
	R_StudioInit();
	R_UI_Init();
	R_Sprite_Init();
	R_Lights_Init();

	rt_menu_warmup = gEngfuncs.Cvar_Get( "rt_menu_warmup", "1", FCVAR_ARCHIVE, "Prepare the path tracer in the main menu instead of on first map load" );
	R_Settings_Init();

	tr.initialized = true;
	tr.framecount = 0;

	gEngfuncs.Con_Printf( "^2ref_openremix: Backend successfully initialized with HWND %p^7\n", hwnd );
	return true;
}

void R_Shutdown( void )
{
	if( !tr.initialized )
		return;

	gEngfuncs.Con_Printf( "ref_openremix: Shutting down...\n" );

	if( s_warmupLight && g_remix.DestroyLight )
		g_remix.DestroyLight( s_warmupLight );
	s_warmupLight = NULL;

	// Shutdown rendering subsystems
	R_Lights_Shutdown();
	R_Sprite_Shutdown();
	R_UI_Shutdown();
	R_StudioShutdown();
	R_BSP_Shutdown();
	R_Textures_Shutdown();

	// Shutdown Remix renderer
	R_Remix_Shutdown();

	// Unload library and clear dispatch table
	Remix_ShutdownAndUnload();

	// Free memory pool and video subsystem
	Mem_FreePool( &r_temppool );
	gEngfuncs.R_Free_Video();

	memset( &tr, 0, sizeof( tr ) );
	tr.initialized = false;
}

void R_NewMap( void )
{
	if( !tr.initialized )
		return;

	if( gp_cl && gp_cl->models )
	{
		tr.worldmodel = gp_cl->models[1];
	}

	// Teardown previous map geometry & build new static world + submodels
	// Note: We do NOT clear textures here because Mod_LoadModel loads the map's textures
	// before calling R_NewMap(). Clearing them here would destroy newly loaded textures.
	R_BSP_NewMap( tr.worldmodel );
	R_StudioNewMap();
	R_Sprite_NewMap();
	R_Lights_NewMap( tr.worldmodel ? tr.worldmodel->entities : NULL );
	R_XRT_NewMap( tr.worldmodel );

	// Request DXVK / OpenRemix memory compaction and VRAM cleanup
	if( g_remix.RequestVramCompaction )
		g_remix.RequestVramCompaction();
	if( g_remix.RequestTextureVramFree )
		g_remix.RequestTextureVramFree();

	gEngfuncs.Con_DPrintf( "ref_openremix: R_NewMap complete for %s\n", tr.worldmodel ? tr.worldmodel->name : "none" );
}

void R_BeginFrame( qboolean clearScene )
{
	(void)clearScene;

	if( !tr.initialized )
		return;

	// Pump OpenRemix platform events (handles SDL3 input, window messages, ImGui event processing)
	if( g_remix.PumpEvents )
	{
		g_remix.PumpEvents();
	}

	// Update cursor confinement state
	R_Remix_UpdateCursorClipping();

	R_Settings_Sync();

	tr.realtime  = gp_host ? gp_host->realtime : 0.0;
	tr.frametime = gp_host ? gp_host->frametime : 0.0;
	tr.framecount++;

	// Entity lists are cleared by the engine via R_ClearScene inside CL_EmitEntities,
	// which runs BEFORE R_BeginFrame. Clearing here would discard every queued entity.
	R_Sprite_BeginFrame();
	R_UI_BeginFrame();
	s_sceneRendered = false;
}

/*
 * R_SubmitWarmupScene - Keep the path tracer compiling while in the menu.
 * OpenRemix holds path tracer preparation until it sees a perspective world camera, and
 * keys its pipelines on whether local lights exist. Without a map neither is submitted,
 * so every shader compiled when the first map loaded. Submit a placeholder camera and a
 * negligible sphere light so the same pipeline set the world uses is built here instead.
 */
static void R_SubmitWarmupScene( void )
{
	R_SetupCamera( NULL );

	if( !g_remix.CreateLight || !g_remix.DrawLightInstance )
		return;

	if( !s_warmupLight )
	{
		remixapi_LightInfoSphereEXT sphereExt;
		memset( &sphereExt, 0, sizeof( sphereExt ) );
		sphereExt.sType      = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
		sphereExt.position.x = -64.0f; // behind the placeholder camera
		sphereExt.radius     = 1.0f;

		remixapi_LightInfo lightInfo;
		memset( &lightInfo, 0, sizeof( lightInfo ) );
		lightInfo.sType      = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
		lightInfo.pNext      = &sphereExt;
		lightInfo.hash       = 0x5741524D55504C54ULL; // "WARMUPLT"
		lightInfo.radiance.x = lightInfo.radiance.y = lightInfo.radiance.z = 0.001f;

		if( g_remix.CreateLight( &lightInfo, &s_warmupLight ) != REMIXAPI_ERROR_CODE_SUCCESS )
			s_warmupLight = NULL;
	}

	if( s_warmupLight )
		g_remix.DrawLightInstance( s_warmupLight );
}

void GL_RenderFrame( const struct ref_viewpass_s *rvp )
{
	if( !tr.initialized || !rvp )
		return;

	RI.rvp = *rvp;
	VectorCopy( rvp->vieworigin, RI.vieworg );
	VectorCopy( rvp->viewangles, RI.viewangles );

	R_RenderScene();
}

void R_SetupCamera( const ref_viewpass_t *rvp )
{
	vec3_t forward, right, up;
	if( rvp )
	{
		AngleVectors( rvp->viewangles, forward, right, up );
	}
	else
	{
		VectorSet( forward, 1.0f, 0.0f, 0.0f );
		VectorSet( right, 0.0f, 1.0f, 0.0f );
		VectorSet( up, 0.0f, 0.0f, 1.0f );
	}

	VectorCopy( forward, RI.vforward );
	VectorCopy( right, RI.vright );
	VectorCopy( up, RI.vup );

	float aspect = 1.0f;
	if( gpGlobals && gpGlobals->height > 0 )
	{
		aspect = (float)gpGlobals->width / (float)gpGlobals->height;
	}
	if( aspect <= 0.0f )
		aspect = 1.0f;

	float fovY = rvp ? rvp->fov_y : 75.0f;
	if( fovY <= 0.0f || fovY >= 180.0f )
	{
		float fovX = ( rvp && rvp->fov_x > 0.0f ) ? rvp->fov_x : 90.0f;
		fovY = (float)( 2.0 * atan( tan( DEG2RAD( fovX ) * 0.5 ) / (double)aspect ) * ( 180.0 / M_PI ) );
	}

	float nearPlane = 1.0f;
	float farPlane = ( gp_movevars && gp_movevars->zmax > 0.0f ) ? ( gp_movevars->zmax * 1.73f ) : 8192.0f;

	remixapi_CameraInfoParameterizedEXT paramCamera = { 0 };
	paramCamera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
	paramCamera.pNext = NULL;
	if( rvp )
	{
		paramCamera.position.x = rvp->vieworigin[0];
		paramCamera.position.y = rvp->vieworigin[1];
		paramCamera.position.z = rvp->vieworigin[2];
	}
	paramCamera.forward.x = forward[0];
	paramCamera.forward.y = forward[1];
	paramCamera.forward.z = forward[2];
	paramCamera.right.x   = right[0];
	paramCamera.right.y   = right[1];
	paramCamera.right.z   = right[2];
	paramCamera.up.x      = up[0];
	paramCamera.up.y      = up[1];
	paramCamera.up.z      = up[2];
	paramCamera.fovYInDegrees = fovY;
	paramCamera.aspect        = aspect;
	paramCamera.nearPlane     = nearPlane;
	paramCamera.farPlane      = farPlane;

	remixapi_CameraInfo cameraInfo = { 0 };
	cameraInfo.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
	cameraInfo.pNext = &paramCamera;
	cameraInfo.type  = REMIXAPI_CAMERA_TYPE_WORLD;

	if( g_remix.SetupCamera )
	{
		remixapi_ErrorCode err = g_remix.SetupCamera( &cameraInfo );
		if( err != REMIXAPI_ERROR_CODE_SUCCESS )
		{
			static int s_camErrors = 0;
			if( ++s_camErrors <= 5 )
			{
				gEngfuncs.Con_Reportf( S_ERROR "ref_openremix: SetupCamera failed: %s (%d)\n",
				                       R_Remix_GetErrorString( err ), (int)err );
			}
		}

		// Also configure viewmodel camera so OpenRemix applyViewModel() handles first-person weapons
		remixapi_CameraInfoParameterizedEXT paramVMCamera = paramCamera;
		paramVMCamera.fovYInDegrees = 70.0f;
		paramVMCamera.nearPlane     = 0.1f;
		paramVMCamera.farPlane      = 256.0f;

		remixapi_CameraInfo vmCameraInfo = { 0 };
		vmCameraInfo.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
		vmCameraInfo.pNext = &paramVMCamera;
		vmCameraInfo.type  = REMIXAPI_CAMERA_TYPE_VIEW_MODEL;

		g_remix.SetupCamera( &vmCameraInfo );
	}
}

/*
====================
CL_FxBlend
====================
*/
int CL_FxBlend( cl_entity_t *e )
{
	if( !e )
		return 0;

	int blend = 0;
	float offset = ((int)e->index) * 363.0f; // Use ent index to de-sync these fx
	double curtime = gp_cl ? gp_cl->time : tr.realtime;

	switch( e->curstate.renderfx )
	{
	case kRenderFxPulseSlowWide:
		blend = e->curstate.renderamt + 0x40 * sin( curtime * 2 + offset );
		break;
	case kRenderFxPulseFastWide:
		blend = e->curstate.renderamt + 0x40 * sin( curtime * 8 + offset );
		break;
	case kRenderFxPulseSlow:
		blend = e->curstate.renderamt + 0x10 * sin( curtime * 2 + offset );
		break;
	case kRenderFxPulseFast:
		blend = e->curstate.renderamt + 0x10 * sin( curtime * 8 + offset );
		break;
	case kRenderFxFadeSlow:
		if( !FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ) )
		{
			if( e->curstate.renderamt > 0 )
				e->curstate.renderamt -= 1;
			else
				e->curstate.renderamt = 0;
		}
		blend = e->curstate.renderamt;
		break;
	case kRenderFxFadeFast:
		if( !FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ) )
		{
			if( e->curstate.renderamt > 3 )
				e->curstate.renderamt -= 4;
			else
				e->curstate.renderamt = 0;
		}
		blend = e->curstate.renderamt;
		break;
	case kRenderFxSolidSlow:
		if( !FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ) )
		{
			if( e->curstate.renderamt < 255 )
				e->curstate.renderamt += 1;
			else
				e->curstate.renderamt = 255;
		}
		blend = e->curstate.renderamt;
		break;
	case kRenderFxSolidFast:
		if( !FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ) )
		{
			if( e->curstate.renderamt < 252 )
				e->curstate.renderamt += 4;
			else
				e->curstate.renderamt = 255;
		}
		blend = e->curstate.renderamt;
		break;
	case kRenderFxStrobeSlow:
		blend = 20 * sin( curtime * 4 + offset );
		if( blend < 0 ) blend = 0;
		else blend = e->curstate.renderamt;
		break;
	case kRenderFxStrobeFast:
		blend = 20 * sin( curtime * 16 + offset );
		if( blend < 0 ) blend = 0;
		else blend = e->curstate.renderamt;
		break;
	case kRenderFxStrobeFaster:
		blend = 20 * sin( curtime * 36 + offset );
		if( blend < 0 ) blend = 0;
		else blend = e->curstate.renderamt;
		break;
	case kRenderFxFlickerSlow:
		blend = 20 * ( sin( curtime * 2 ) + sin( curtime * 17 + offset ) );
		if( blend < 0 ) blend = 0;
		else blend = e->curstate.renderamt;
		break;
	case kRenderFxFlickerFast:
		blend = 20 * ( sin( curtime * 16 ) + sin( curtime * 23 + offset ) );
		if( blend < 0 ) blend = 0;
		else blend = e->curstate.renderamt;
		break;
	case kRenderFxHologram:
	case kRenderFxDistort:
	{
		vec3_t tmp;
		VectorCopy( e->origin, tmp );
		VectorSubtract( tmp, RI.rvp.vieworigin, tmp );
		float dist = DotProduct( tmp, RI.vforward );

		if( e->curstate.renderfx == kRenderFxDistort )
			dist = 1.0f;

		if( dist <= 0.0f )
		{
			blend = 0;
		}
		else
		{
			e->curstate.renderamt = 180;
			if( dist <= 100.0f )
				blend = e->curstate.renderamt;
			else
				blend = (int)( ( 1.0f - ( dist - 100.0f ) * ( 1.0f / 400.0f ) ) * e->curstate.renderamt );
			if( gEngfuncs.COM_RandomLong )
				blend += gEngfuncs.COM_RandomLong( -32, 31 );
		}
		break;
	}
	default:
		blend = e->curstate.renderamt;
		break;
	}

	blend = bound( 0, blend, 255 );
	return blend;
}

void R_RenderScene( void )
{
	if( !tr.initialized )
		return;

	s_sceneRendered = true;

	// Reset dynamic mesh allocator at start of scene
	R_Sprite_BeginFrame();

	// Setup camera basis using GoldSrc coordinates
	R_SetupCamera( &RI.rvp );

	// Draw static world mesh
	R_BSP_DrawWorld();

	// Store efrags for static and fragmented entities in the world (doors, func_wall grates, etc.)
	if( tr.worldmodel && tr.worldmodel->leafs && gEngfuncs.R_StoreEfrags )
	{
		extern int g_bspSceneCount;
		// R_StoreEfrags skips ents with visframe == framecount and sets it after adding;
		// pass a distinct value so our AddEntity dedupe (keyed on g_bspSceneCount) still runs.
		const int efragFrame = -g_bspSceneCount;
		for( int i = 0; i <= tr.worldmodel->numleafs; i++ )
		{
			mleaf_t *pleaf = &tr.worldmodel->leafs[i];
			if( pleaf->efrags )
				gEngfuncs.R_StoreEfrags( &pleaf->efrags, efragFrame );
		}
	}

	// Draw moving and static brush entities (doors, triggers, platforms, grates)
	R_BSP_DrawEntities();

	// Draw 3D normal triangles from client DLL (world-space resonance cascade effects, beams)
	if( FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) && gEngfuncs.pfnDrawNormalTriangles )
	{
		gEngfuncs.pfnDrawNormalTriangles();
		R_UI_TriFlush();
	}

	// Draw studio models and sprites queued for this frame
	for( uint32_t i = 0; i < g_bspState.num_solid_entities; i++ )
	{
		cl_entity_t *e = g_bspState.solid_entities[i];
		if( !e || !e->model )
			continue;

		if( e->model->type == mod_studio )
		{
			R_DrawStudioModel( e );
		}
		else if( e->model->type == mod_sprite )
		{
			R_DrawSpriteModel( e );
		}
	}

	for( uint32_t i = 0; i < g_bspState.num_trans_entities; i++ )
	{
		cl_entity_t *e = g_bspState.trans_entities[i];
		if( !e || !e->model )
			continue;

		if( e->model->type == mod_studio )
		{
			R_DrawStudioModel( e );
		}
		else if( e->model->type == mod_sprite )
		{
			R_DrawSpriteModel( e );
		}
	}

	// Draw 3D transparent triangles from client DLL (shockwaves, energy rings, custom client effects)
	if( FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) && gEngfuncs.pfnDrawTransparentTriangles )
	{
		gEngfuncs.pfnDrawTransparentTriangles();
		R_UI_TriFlush();
	}

	// Draw local player viewmodel
	R_DrawViewModel();

	// Draw particle systems, beams, and special effects
	if( gEngfuncs.CL_DrawEFX )
	{
		gEngfuncs.CL_DrawEFX( (float)tr.frametime, false );
		R_UI_TriFlush();
		gEngfuncs.CL_DrawEFX( (float)tr.frametime, true );
		R_UI_TriFlush();
	}

	// Final flush for any remaining 3D TriAPI geometry
	R_UI_TriFlush();

	// Submit static map lights, sun, and dynamic lights (flashlight, explosions)
	R_Lights_DrawFrame();

	// XashRT map additions: mirrors, props and hand-placed lights
	R_XRT_DrawScene();
}

void R_Set2DMode( qboolean enable )
{
	tr.is2DMode = enable;
}

void R_Set2DOffset( float x, float y )
{
	tr.offset2d[0] = x;
	tr.offset2d[1] = y;
}

void R_ClearScreen( void )
{
}

void GL_BackendStartFrame( void )
{
}

void GL_BackendEndFrame( void )
{
}

void R_EndFrame( void )
{
	if( !tr.initialized )
		return;

	if( !s_sceneRendered && !( rt_menu_warmup && rt_menu_warmup->value <= 0.0f ) )
		R_SubmitWarmupScene();

	// Submit 2D UI draw list (HUD, menus, console, VGUI)
	R_UI_EndFrame();

	R_Set2DMode( false );

	if( g_remix.Present )
	{
		remixapi_PresentInfo presentInfo = { 0 };
		presentInfo.sType        = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
		presentInfo.hwndOverride = NULL;

		remixapi_ErrorCode err = g_remix.Present( &presentInfo );
		if( err != REMIXAPI_ERROR_CODE_SUCCESS )
		{
			gEngfuncs.Con_Reportf( S_WARN "ref_openremix: Present returned error: %s (code %d)\n",
			                      R_Remix_GetErrorString( err ), (int)err );
		}
	}
}
