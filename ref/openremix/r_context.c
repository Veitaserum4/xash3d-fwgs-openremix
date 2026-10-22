/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_context.c - GetRefAPI entry point & callback table definition
 */

#include "r_local.h"
#include "r_textures.h"
#include "r_ui.h"

// Cached engine globals
ref_api_t          gEngfuncs;
ref_globals_t     *gpGlobals = NULL;
ref_client_t      *gp_cl = NULL;
ref_host_t        *gp_host = NULL;
struct movevars_s *gp_movevars = NULL;
dlight_t          *gp_dlights = NULL;

// Global renderer state instances
ref_state_t    tr;
ref_instance_t RI;
poolhandle_t   r_temppool = 0;

// Engine shared cvars
DEFINE_ENGINE_SHARED_CVAR_LIST()

// Renderer-specific cvars
static CVAR_DEFINE( r_openremix_raytracing, "r_openremix_raytracing", "1", FCVAR_ARCHIVE, "Enable ray tracing via OpenRemix" );
static CVAR_DEFINE( r_openremix_debug, "r_openremix_debug", "0", 0, "Enable OpenRemix debug logging" );
static CVAR_DEFINE( r_openremix_path, "r_openremix_path", "", FCVAR_ARCHIVE, "Custom path to remix.dll" );

//
// Stubs for engine callbacks
//
static void GL_SetupAttributes_Stub( int safegl )
{
	(void)safegl;
}

static void GL_InitExtensions_Stub( void )
{
}

static void GL_ClearExtensions_Stub( void )
{
}

static void R_GammaChanged_Stub( qboolean do_reset_gamma )
{
	(void)do_reset_gamma;
}

static void R_PushScene_Stub( void )
{
}

static void R_PopScene_Stub( void )
{
}

static void R_AllowFog_Stub( qboolean allow )
{
	(void)allow;
}


static void R_ProcessEntData_Stub( qboolean allocate, cl_entity_t *entities, unsigned int max_entities )
{
	if( !allocate )
	{
		tr.entities = NULL;
		tr.max_entities = 0;
	}
	else
	{
		tr.entities = entities;
		tr.max_entities = max_entities;
	}
}

static void R_ShowTextures_Stub( void )
{
}

static const byte *R_GetTextureOriginalBuffer_Stub( unsigned int idx )
{
	(void)idx;
	return NULL;
}

static void GL_ProcessTexture_Stub( int texnum, float gamma, int topColor, int bottomColor )
{
	(void)texnum;
	(void)gamma;
	(void)topColor;
	(void)bottomColor;
}

static void R_SetupSky_Stub( int *skyboxTextures )
{
	(void)skyboxTextures;
}

static int WorldToScreen_Stub( const vec3_t world, vec3_t screen )
{
	(void)world;
	VectorClear( screen );
	return 0;
}

static qboolean VID_ScreenShot_Stub( const char *filename, int shot_type )
{
	(void)filename;
	(void)shot_type;
	return false;
}

static qboolean VID_CubemapShot_Stub( const char *base, uint size, const float *vieworg, qboolean skyshot )
{
	(void)base;
	(void)size;
	(void)vieworg;
	(void)skyshot;
	return false;
}

static colorVec R_LightPoint_Stub( const float *p )
{
	(void)p;
	colorVec c = { 128, 128, 128, 255 };
	return c;
}

static void R_DecalShoot_Stub( int textureIndex, int entityIndex, int modelIndex, vec3_t pos, int flags, float scale )
{
	(void)textureIndex;
	(void)entityIndex;
	(void)modelIndex;
	(void)pos;
	(void)flags;
	(void)scale;
}

static void R_DecalRemoveAll_Stub( int texture )
{
	(void)texture;
}

static int R_CreateDecalList_Stub( struct decallist_s *pList )
{
	(void)pList;
	return 0;
}

static void R_ClearAllDecals_Stub( qboolean includePermanent )
{
	(void)includePermanent;
}

static void R_SetSkyCloudsTextures_Stub( int solidskyTexture, int alphaskyTexture )
{
	(void)solidskyTexture;
	(void)alphaskyTexture;
}

static void GL_SubdivideSurface_Stub( model_t *mod, msurface_t *fa )
{
	(void)mod;
	(void)fa;
}

static void CL_RunLightStyles_Stub( lightstyle_t *ls )
{
	(void)ls;
}

static qboolean Mod_ProcessRenderData_Stub( model_t *mod, qboolean create, const byte *buffer, size_t buffersize )
{
	(void)mod;
	(void)create;
	(void)buffer;
	(void)buffersize;
	return true;
}

static intptr_t RefGetParm_Hook( int parm, int arg )
{
	switch( parm )
	{
	case PARM_TEX_WIDTH:
	case PARM_TEX_SRC_WIDTH:
		if( arg > 0 && arg < MAX_REMIX_TEXTURES && g_remixTextures[arg].registered )
			return (intptr_t)g_remixTextures[arg].width;
		if( arg > 0 && arg < MAX_UI_TEXTURES && g_ui.textures[arg].registered )
			return (intptr_t)g_ui.textures[arg].width;
		return 0;

	case PARM_TEX_HEIGHT:
	case PARM_TEX_SRC_HEIGHT:
		if( arg > 0 && arg < MAX_REMIX_TEXTURES && g_remixTextures[arg].registered )
			return (intptr_t)g_remixTextures[arg].height;
		if( arg > 0 && arg < MAX_UI_TEXTURES && g_ui.textures[arg].registered )
			return (intptr_t)g_ui.textures[arg].height;
		return 0;

	case PARM_TEX_TEXNUM:
		return (intptr_t)arg;

	case PARM_TEX_FLAGS:
		if( arg > 0 && arg < MAX_REMIX_TEXTURES && g_remixTextures[arg].registered )
			return (intptr_t)g_remixTextures[arg].flags;
		return 0;

	case PARM_GL_CONTEXT_TYPE:
		return CONTEXT_TYPE_SOFTWARE;

	default:
		return 0;
	}
}

static void R_GetDetailScaleForTexture_Stub( int texture, float *xScale, float *yScale )
{
	(void)texture;
	if( xScale ) *xScale = 1.0f;
	if( yScale ) *yScale = 1.0f;
}

static void R_SetDetailScaleForTexture_Stub( int texture, float xScale, float yScale )
{
	(void)texture;
	(void)xScale;
	(void)yScale;
}

static const byte *GL_TextureData_Stub( unsigned int texnum )
{
	(void)texnum;
	return NULL;
}

static void R_OverrideTextureSourceSize_Stub( unsigned int texnum, unsigned int srcWidth, unsigned int srcHeight )
{
	(void)texnum;
	(void)srcWidth;
	(void)srcHeight;
}

static void GL_Bind_Hook( int tmu, unsigned int texnum )
{
	(void)tmu;
	if( texnum > 0 && texnum < MAX_UI_TEXTURES && g_ui.textures[texnum].registered )
		g_ui.currentTextureId = (remixapi_UITextureHandle)texnum;
	else
		g_ui.currentTextureId = 0;

	extern unsigned int g_currentBoundTexture;
	g_currentBoundTexture = texnum;
}

static void GL_OrthoBounds_Stub( const float *mins, const float *maxs )
{
	(void)mins;
	(void)maxs;
}

static qboolean R_SpeedsMessage_Stub( char *out, size_t size )
{
	(void)out;
	(void)size;
	return false;
}

static byte *Mod_GetCurrentVis_Stub( void )
{
	return NULL;
}

static qboolean R_SetDisplayTransform_Stub( ref_screen_rotation_t rotate, int x, int y, float unused1, float unused2 )
{
	(void)rotate;
	(void)x;
	(void)y;
	(void)unused1;
	(void)unused2;
	return true;
}

static void TriCullFace_Stub( TRICULLSTYLE mode )
{
	(void)mode;
}

static void R_FillRenderAPI_Stub( struct render_api_s *api )
{
	(void)api;
}

static void VGUI_SetupDrawing_Stub( qboolean rect )
{
	(void)rect;
}

//
// Master Function Table for Xash3D Engine
//
const ref_interface_t gReffuncs =
{
	.R_Init                = R_Init,
	.R_Shutdown            = R_Shutdown,
	.R_GetConfigName       = R_GetConfigName,
	.R_SetDisplayTransform = R_SetDisplayTransform_Stub,

	.GL_SetupAttributes    = GL_SetupAttributes_Stub,
	.GL_InitExtensions     = GL_InitExtensions_Stub,
	.GL_ClearExtensions    = GL_ClearExtensions_Stub,

	.R_GammaChanged        = R_GammaChanged_Stub,
	.R_BeginFrame          = R_BeginFrame,
	.R_RenderScene         = R_RenderScene,
	.R_EndFrame            = R_EndFrame,
	.R_PushScene           = R_PushScene_Stub,
	.R_PopScene            = R_PopScene_Stub,
	.GL_BackendStartFrame  = GL_BackendStartFrame,
	.GL_BackendEndFrame    = GL_BackendEndFrame,

	.R_ClearScreen         = R_ClearScreen,
	.R_AllowFog            = R_AllowFog_Stub,
	.GL_SetRenderMode      = R_UI_SetRenderMode,

	.R_AddEntity           = R_BSP_AddEntity,
	.R_ProcessEntData      = R_ProcessEntData_Stub,
	.R_ShowTextures        = R_ShowTextures_Stub,

	.R_GetTextureOriginalBuffer = R_GetTextureOriginalBuffer_Stub,
	.GL_LoadTextureFromBuffer   = GL_LoadTextureFromBuffer_Hook,
	.GL_ProcessTexture          = GL_ProcessTexture_Stub,
	.R_SetupSky                 = R_SetupSky_Stub,

	.R_Set2DMode           = R_UI_Set2DMode,
	.R_Set2DOffset         = R_UI_Set2DOffset,
	.R_DrawStretchPic      = R_UI_DrawStretchPic,
	.FillRGBA              = R_UI_FillRGBA,
	.WorldToScreen         = R_UI_TriWorldToScreen,

	.VID_ScreenShot        = VID_ScreenShot_Stub,
	.VID_CubemapShot       = VID_CubemapShot_Stub,
	.R_LightPoint          = R_LightPoint_Stub,

	.R_DecalShoot          = R_DecalShoot_Stub,
	.R_DecalRemoveAll      = R_DecalRemoveAll_Stub,
	.R_CreateDecalList     = R_CreateDecalList_Stub,
	.R_ClearAllDecals      = R_ClearAllDecals_Stub,

	.R_StudioEstimateFrame = R_StudioEstimateFrame,
	.R_StudioLerpMovement  = R_StudioLerpMovement,
	.R_StudioFillAPI       = R_StudioFillAPI,
	.R_StudioSetDrawInterface = R_StudioSetDrawInterface,

	.R_SetSkyCloudsTextures = R_SetSkyCloudsTextures_Stub,
	.GL_SubdivideSurface   = GL_SubdivideSurface_Stub,
	.CL_RunLightStyles     = CL_RunLightStyles_Stub,

	.Mod_ProcessRenderData  = Mod_ProcessRenderData_Stub,
	.Mod_StudioLoadTextures = Mod_StudioLoadTextures_Hook,

	.CL_DrawParticles      = CL_DrawParticles,
	.CL_DrawTracers        = CL_DrawTracers,
	.CL_DrawBeams          = CL_DrawBeams,

	.RefGetParm            = RefGetParm_Hook,

	.R_GetDetailScaleForTexture = R_GetDetailScaleForTexture_Stub,
	.R_SetDetailScaleForTexture = R_SetDetailScaleForTexture_Stub,

	.GL_CreateTexture      = GL_CreateTexture_Hook,
	.GL_FindTexture        = GL_FindTexture_Hook,
	.GL_TextureName        = GL_TextureName_Hook,
	.GL_TextureData        = GL_TextureData_Stub,
	.GL_LoadTexture        = GL_LoadTexture_Hook,
	.GL_FreeTexture        = GL_FreeTexture_Hook,
	.R_OverrideTextureSourceSize = R_OverrideTextureSourceSize_Stub,

	.GL_UpdateTexture      = GL_UpdateTexture_Hook,
	.GL_Bind               = GL_Bind_Hook,

	.GL_RenderFrame        = GL_RenderFrame,
	.GL_OrthoBounds        = GL_OrthoBounds_Stub,
	.R_SpeedsMessage       = R_SpeedsMessage_Stub,
	.Mod_GetCurrentVis     = Mod_GetCurrentVis_Stub,
	.R_NewMap              = R_NewMap,
	.R_ClearScene          = R_BSP_ClearEntities,

	.TriRenderMode         = R_UI_TriRenderMode,
	.Begin                 = R_UI_TriBegin,
	.End                   = R_UI_TriEnd,
	.Color4f               = R_UI_TriColor4f,
	.Color4ub              = R_UI_TriColor4ub,
	.Vertex3fv             = R_UI_TriVertex3fv,
	.Vertex3f              = R_UI_TriVertex3f,
	.CullFace              = R_UI_TriCullFace,

	.R_FillRenderAPI       = R_FillRenderAPI_Stub,
	.R_FillTriAPI          = R_UI_FillTriAPI,
	.VGUI_SetupDrawing     = VGUI_SetupDrawing_Stub,
};

/*
 * GetRefAPI - Primary entry point exported by ref_openremix.dll
 */
int EXPORT GetRefAPI( int version, ref_interface_t *funcs, ref_api_t *engfuncs, ref_globals_t *globals )
{
	if( version != REF_API_VERSION )
		return 0;

	if( !funcs || !engfuncs || !globals )
		return 0;

	// Populate callback interface
	*funcs = gReffuncs;

	// Cache engine services & globals
	gEngfuncs = *engfuncs;
	gpGlobals = globals;

	// Retrieve internal engine pointers
	gp_cl       = (ref_client_t *)ENGINE_GET_PARM( PARM_GET_CLIENT_PTR );
	gp_host     = (ref_host_t *)ENGINE_GET_PARM( PARM_GET_HOST_PTR );
	gp_movevars = (struct movevars_s *)ENGINE_GET_PARM( PARM_GET_MOVEVARS_PTR );
	gp_dlights  = (dlight_t *)ENGINE_GET_PARM( PARM_GET_DLIGHTS_PTR );

	// Retrieve engine shared cvars
	RETRIEVE_ENGINE_SHARED_CVAR_LIST();

	// Register ref_openremix custom cvars
	if( gEngfuncs.Cvar_RegisterVariable )
	{
		gEngfuncs.Cvar_RegisterVariable( &r_openremix_raytracing );
		gEngfuncs.Cvar_RegisterVariable( &r_openremix_debug );
		gEngfuncs.Cvar_RegisterVariable( &r_openremix_path );
	}

	return REF_API_VERSION;
}
