/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_lights.c - Light extraction from BSP entities, dynamic lights, player flashlight, and volumetrics
 */

#include "r_local.h"
#include "r_lights.h"
#include "r_remix.h"
#include "r_xashrt.h"

lights_state_t g_lights;

// Lighting CVars
static cvar_t *rt_sun              = NULL;
static cvar_t *rt_sun_volumetric   = NULL;
static cvar_t *rt_ies              = NULL;

/*
 * IES photometric profiles (IESNA LM-63) for shaped lights: how bright a light is at each angle
 * from its axis. rt/ies/flashlight.ies and rt/ies/spotlight.ies replace these built-in ones.
 */
enum { IES_FLASHLIGHT, IES_SPOTLIGHT, IES_COUNT };

// A handheld LED torch with a reflector: a flat hotspot with a steep edge (6-10 degrees), a
// dimmer spill holding the reflector's faint ring (14 degrees), cut off hard at 35 degrees
static const char s_iesFlashlight[] =
	"IESNA:LM-63-2002\n"
	"[TEST] ref_openremix built-in\n"
	"[LUMINAIRE] Flashlight (approximation)\n"
	"TILT=NONE\n"
	"1 -1 1 21 1 1 1 0 0 0\n"
	"1 1 3\n"
	"0 3 5 6 7 8 9 10 12 13 14 15 17 20 25 30 32 34 35 36 90\n"
	"0\n"
	"1000 1000 990 960 820 520 280 200 185 200 215 190 170 160 150 135 110 50 15 0 0\n";

// A ceiling spot or downlight: broad and smooth, gone by 80 degrees
static const char s_iesSpotlight[] =
	"IESNA:LM-63-2002\n"
	"[TEST] ref_openremix built-in\n"
	"[LUMINAIRE] Spotlight (approximation)\n"
	"TILT=NONE\n"
	"1 -1 1 10 1 1 1 0 0 0\n"
	"1 1 60\n"
	"0 10 20 30 40 50 60 70 80 90\n"
	"0\n"
	"1000 970 880 700 450 220 80 20 0 0\n";

static struct
{
	byte    *file; // the rt/ies override, if any
	size_t   size;
	qboolean loaded;
} s_ies[IES_COUNT];

/*
 * The flashlight's cookie: a texture projected along its beam, the detail a profile cannot hold.
 * rt/cookies/flashlight.png (or .tga/.jpg/.bmp/.dds) next to the executable replaces the built-in one.
 */
#define FLASHLIGHT_COOKIE_SIZE 256
#define FLASHLIGHT_COOKIE_FOV  72.0f // its edge where the beam profile ends (36 degrees off axis)

static cvar_t *rt_flsh_cookie = NULL;
static struct
{
	wchar_t                path[512]; // what the light names: a file, or the built-in texture's hash
	remixapi_TextureHandle builtin;
	qboolean               ready;
} s_flashlightCookie;

static float R_Lights_CookieNoise( float x, float y )
{
	// Smooth value noise on an integer lattice
	const int ix = (int)floorf( x ), iy = (int)floorf( y );
	const float fx = x - ix, fy = y - iy;
	float corner[4];
	for( int i = 0; i < 4; i++ )
	{
		uint32_t h = (uint32_t)( ix + ( i & 1 )) * 73856093u ^ (uint32_t)( iy + ( i >> 1 )) * 19349663u;
		h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
		corner[i] = ( h & 0xffff ) / 65535.0f;
	}
	const float sx = fx * fx * ( 3.0f - 2.0f * fx ), sy = fy * fy * ( 3.0f - 2.0f * fy );
	const float top = corner[0] + ( corner[1] - corner[0] ) * sx, bottom = corner[2] + ( corner[3] - corner[2] ) * sx;
	return top + ( bottom - top ) * sy;
}

// A ring around the centre: 1 at radius c, falling off over w
static float R_Lights_CookieBand( float r, float c, float w )
{
	const float d = ( r - c ) / w;
	return expf( -d * d );
}

/*
 * The built-in cookie: an LED torch's beam as projected, with a slightly dim die at the centre,
 * faint reflector rings and soft, uneven blotches; the beam profile does the radial falloff
 */
static remixapi_TextureHandle R_Lights_BuildFlashlightCookie( uint64_t hash )
{
	const int n = FLASHLIGHT_COOKIE_SIZE;
	byte *pixels = (byte *)malloc( (size_t)n * n * 4 );
	if( !pixels || !g_remix.CreateTexture )
	{
		free( pixels );
		return NULL;
	}

	for( int y = 0; y < n; y++ )
	{
		for( int x = 0; x < n; x++ )
		{
			const float u = ( x + 0.5f ) / n * 2.0f - 1.0f, v = ( y + 0.5f ) / n * 2.0f - 1.0f;
			const float r0 = sqrtf( u * u + v * v ); // 1 at the beam's edge
			const float r = r0 + 0.025f * ( R_Lights_CookieNoise( u * 4.0f + 7.0f, v * 4.0f + 2.0f ) - 0.5f ); // rings wobble

			float value = 1.0f;
			value *= 1.0f - 0.15f * R_Lights_CookieBand( r0, 0.0f, 0.04f );   // the LED die
			value *= 1.0f + 0.06f * R_Lights_CookieBand( r, 0.16f, 0.04f );    // the reflector's halo
			value *= 1.0f - 0.12f * R_Lights_CookieBand( r, 0.26f, 0.045f );   // the gap before the spill
			value *= 1.0f - 0.06f * R_Lights_CookieBand( r, 0.55f, 0.05f ) - 0.05f * R_Lights_CookieBand( r, 0.78f, 0.06f ); // spill rings
			value *= 0.90f + 0.10f * R_Lights_CookieNoise( u * 5.0f + 11.0f, v * 5.0f + 3.0f ); // blotches
			value *= 0.96f + 0.04f * R_Lights_CookieNoise( u * 20.0f, v * 20.0f );              // grain
			value *= 1.0f - Q_max( 0.0f, ( r0 - 0.96f ) / 0.04f );                             // soft rim
			value = bound( 0.0f, value, 1.0f );

			// The spill's reflector light is a little warmer than the hotspot
			const float warm = Q_min( r0, 1.0f ) * 0.06f;
			byte *p = &pixels[( y * n + x ) * 4];
			p[0] = (byte)( 255.0f * value );
			p[1] = (byte)( 255.0f * value * ( 1.0f - warm * 0.5f ));
			p[2] = (byte)( 255.0f * value * ( 1.0f - warm ));
			p[3] = 255;
		}
	}

	remixapi_TextureInfo info;
	memset( &info, 0, sizeof( info ));
	info.sType     = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
	info.hash      = hash;
	info.width     = n;
	info.height    = n;
	info.depth     = 1;
	info.mipLevels = 1;
	info.format    = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
	info.data      = pixels;
	info.dataSize  = (uint64_t)n * n * 4;

	remixapi_TextureHandle handle = NULL;
	if( g_remix.CreateTexture( &info, &handle ) != REMIXAPI_ERROR_CODE_SUCCESS )
		handle = NULL;
	free( pixels );
	return handle;
}

/*
 * R_Lights_GetFlashlightCookie - The flashlight's cookie, facing up along the view; false with
 * rt_flsh_cookie 0 or without one
 */
static qboolean R_Lights_GetFlashlightCookie( remixapi_LightInfoCookieEXT *ext )
{
	if( !rt_flsh_cookie || rt_flsh_cookie->value <= 0.0f )
		return false;

	if( !s_flashlightCookie.ready )
	{
		s_flashlightCookie.ready = true;
		s_flashlightCookie.path[0] = 0;
#if defined( _WIN32 )
		static const char *const files[] = { "flashlight.png", "flashlight.tga", "flashlight.jpg", "flashlight.bmp", "flashlight.dds" };
		char exe[MAX_PATH];
		DWORD len = GetModuleFileNameA( NULL, exe, sizeof( exe ));
		char *slash = len ? strrchr( exe, '\\' ) : NULL;
		for( size_t i = 0; slash && i < ARRAYSIZE( files ) && !s_flashlightCookie.path[0]; i++ )
		{
			char full[MAX_PATH];
			Q_snprintf( full, sizeof( full ), "%.*s\\rt\\cookies\\%s", (int)( slash - exe ), exe, files[i] );
			FILE *f = fopen( full, "rb" );
			if( f )
			{
				fclose( f );
				MultiByteToWideChar( CP_ACP, 0, full, -1, s_flashlightCookie.path, ARRAYSIZE( s_flashlightCookie.path ));
				gEngfuncs.Con_Printf( "ref_openremix: flashlight cookie rt/cookies/%s\n", files[i] );
			}
		}
#endif
		if( !s_flashlightCookie.path[0] )
		{
			const uint64_t hash = 0x464C53484B4F4F43ULL; // "FLSHKOOC"
			s_flashlightCookie.builtin = R_Lights_BuildFlashlightCookie( hash );
			if( s_flashlightCookie.builtin )
				_snwprintf( s_flashlightCookie.path, ARRAYSIZE( s_flashlightCookie.path ), L"0x%llx", (unsigned long long)hash );
		}
	}
	if( !s_flashlightCookie.path[0] )
		return false;

	memset( ext, 0, sizeof( *ext ));
	ext->sType      = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_COOKIE_EXT;
	ext->texture    = s_flashlightCookie.path;
	ext->up.x       = RI.vup[0];
	ext->up.y       = RI.vup[1];
	ext->up.z       = RI.vup[2];
	ext->fovDegrees = FLASHLIGHT_COOKIE_FOV;
	return true;
}

/*
 * R_Lights_GetIes - The profile for a kind of light, as a LightInfo extension; false with rt_ies 0
 */
static qboolean R_Lights_GetIes( int which, remixapi_LightInfoIesProfileEXT *ext )
{
	if( !rt_ies || rt_ies->value <= 0.0f )
		return false;

	static const char *const files[IES_COUNT] = { "rt/ies/flashlight.ies", "rt/ies/spotlight.ies" };
	if( !s_ies[which].loaded )
	{
		s_ies[which].loaded = true;
		s_ies[which].file = R_XRT_ReadFile( files[which], &s_ies[which].size );
		if( s_ies[which].file )
			gEngfuncs.Con_Printf( "ref_openremix: IES profile %s\n", files[which] );
	}

	memset( ext, 0, sizeof( *ext ));
	ext->sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_IES_PROFILE_EXT;
	if( s_ies[which].file )
	{
		ext->data = (const char *)s_ies[which].file;
		ext->size = s_ies[which].size;
	}
	else
	{
		ext->data = which == IES_FLASHLIGHT ? s_iesFlashlight : s_iesSpotlight;
		ext->size = which == IES_FLASHLIGHT ? sizeof( s_iesFlashlight ) - 1 : sizeof( s_iesSpotlight ) - 1;
	}
	return true;
}
static cvar_t *rt_sun_diameter     = NULL;
static cvar_t *rt_flsh             = NULL;
static cvar_t *rt_flsh_radius      = NULL;
static cvar_t *rt_flsh_angle       = NULL;
static cvar_t *rt_flsh_r           = NULL;
static cvar_t *rt_flsh_u           = NULL;
static cvar_t *rt_light_s          = NULL;
static cvar_t *rt_light_d          = NULL;
static cvar_t *rt_light_e          = NULL;
static cvar_t *rt_light_radius     = NULL;
static cvar_t *rt_light_range      = NULL;
static cvar_t *rt_emis             = NULL;
static cvar_t *rt_fog              = NULL;
static cvar_t *rt_fog_distance     = NULL;
static cvar_t *rt_fog_scattering   = NULL;

// Compatibility internal CVars required by client_amd64.dll
static cvar_t *_rt_labcoat         = NULL;
static cvar_t *rt_labcoat_force    = NULL;
static cvar_t *rt_labcoat_model    = NULL;
static cvar_t *_rt_chapter         = NULL;
static cvar_t *_rt_chaptershow     = NULL;
static cvar_t *_rt_flsh_key        = NULL;
static cvar_t *_rt_skipframe       = NULL;
static cvar_t *_rt_dlss_available  = NULL;

/*
====================
R_LightMakeHash
====================
*/
static uint64_t R_LightMakeHash( uint32_t index, const vec3_t origin )
{
	uint64_t h = 0xcbf29ce484222325ULL;
	uint32_t data[4];
	data[0] = index;
	memcpy( &data[1], origin, sizeof( float ) * 3 );

	const uint8_t *b = (const uint8_t *)data;
	for( size_t i = 0; i < sizeof( data ); i++ )
	{
		h ^= b[i];
		h *= 0x100000001b3ULL;
	}
	return h ? h : 1ULL;
}

/*
====================
R_Lights_Init
====================
*/
void R_Lights_Init( void )
{
	memset( &g_lights, 0, sizeof( g_lights ) );

	if( gEngfuncs.Cvar_Get )
	{
		rt_flsh_cookie     = gEngfuncs.Cvar_Get( "rt_flsh_cookie", "1", FCVAR_ARCHIVE, "Project a texture along the flashlight's beam (rt/cookies/flashlight.png replaces the built-in one)" );
		rt_ies             = gEngfuncs.Cvar_Get( "rt_ies", "1", FCVAR_ARCHIVE, "IES light profiles for the flashlight and spotlights (rt/ies/*.ies replace the built-in ones)" );
		rt_sun             = gEngfuncs.Cvar_Get( "rt_sun", "50", FCVAR_ARCHIVE, "Sun irradiance for a white light_environment at brightness 255" );
		rt_sun_volumetric  = gEngfuncs.Cvar_Get( "rt_sun_volumetric", "4", FCVAR_ARCHIVE, "How much more the sun scatters in volumetric fog (light shafts); applies on map load" );
		rt_sun_diameter    = gEngfuncs.Cvar_Get( "rt_sun_diameter", "0.53", FCVAR_ARCHIVE, "Sun angular diameter in degrees" );
		rt_flsh            = gEngfuncs.Cvar_Get( "rt_flsh", "8000", FCVAR_ARCHIVE, "Player flashlight intensity" );
		rt_flsh_radius     = gEngfuncs.Cvar_Get( "rt_flsh_radius", "2.0", FCVAR_ARCHIVE, "Player flashlight source radius" );
		rt_flsh_angle      = gEngfuncs.Cvar_Get( "rt_flsh_angle", "25", FCVAR_ARCHIVE, "Player flashlight outer cone angle in degrees" );
		rt_flsh_r          = gEngfuncs.Cvar_Get( "rt_flsh_r", "2.5", FCVAR_ARCHIVE, "Player flashlight right offset" );
		rt_flsh_u          = gEngfuncs.Cvar_Get( "rt_flsh_u", "-2.5", FCVAR_ARCHIVE, "Player flashlight up offset" );
		rt_light_s         = gEngfuncs.Cvar_Get( "rt_light_s", "500", FCVAR_ARCHIVE, "Map static light intensity scale" );
		rt_light_d         = gEngfuncs.Cvar_Get( "rt_light_d", "1000", FCVAR_ARCHIVE, "Dynamic lights (explosions/projectiles) intensity scale" );
		rt_light_e         = gEngfuncs.Cvar_Get( "rt_light_e", "5000", FCVAR_ARCHIVE, "Entity lights (controller orbs, Nihilanth's spheres) intensity" );
		rt_light_radius    = gEngfuncs.Cvar_Get( "rt_light_radius", "3.0", FCVAR_ARCHIVE, "Map static light source radius in GoldSrc units" );
		rt_light_range     = gEngfuncs.Cvar_Get( "rt_light_range", "0", FCVAR_ARCHIVE, "Local light cutoff range (0 = unlimited physical falloff)" );
		rt_emis            = gEngfuncs.Cvar_Get( "rt_emis", "1.0", FCVAR_ARCHIVE, "Texture light emission multiplier" );
		rt_fog             = gEngfuncs.Cvar_Get( "rt_fog", "1", FCVAR_ARCHIVE, "Enable OpenRemix path-traced volumetric fog" );
		rt_fog_distance    = gEngfuncs.Cvar_Get( "rt_fog_distance", "2000.0", FCVAR_ARCHIVE, "Volumetric fog sight distance in meters" );
		rt_fog_scattering  = gEngfuncs.Cvar_Get( "rt_fog_scattering", "1.0", FCVAR_ARCHIVE, "Volumetric fog light scattering multiplier" );

		// Register client compatibility variables so game client does not warn each frame
		_rt_labcoat        = gEngfuncs.Cvar_Get( "_rt_labcoat", "0", 0, "Internal labcoat player model state" );
		rt_labcoat_force   = gEngfuncs.Cvar_Get( "rt_labcoat_force", "0", 0, "Force labcoat model" );
		rt_labcoat_model   = gEngfuncs.Cvar_Get( "rt_labcoat_model", "models/player.mdl", 0, "Labcoat model override" );
		_rt_chapter        = gEngfuncs.Cvar_Get( "_rt_chapter", "", 0, "Current chapter name" );
		_rt_chaptershow    = gEngfuncs.Cvar_Get( "_rt_chaptershow", "0", 0, "Show chapter title" );
		_rt_flsh_key       = gEngfuncs.Cvar_Get( "_rt_flsh_key", "-1", 0, "DLight key of the player's flashlight" );
		_rt_skipframe      = gEngfuncs.Cvar_Get( "_rt_skipframe", "0", 0, "Skip frame flag" );
		_rt_dlss_available = gEngfuncs.Cvar_Get( "_rt_dlss_available", "1", 0, "DLSS availability flag" );
	}

	// Nothing is pushed to OpenRemix's settings here: its profile (the overlay's, or Xash3D's defaults when
	// none is saved) decides, and fog and light range settings only apply when changed (R_Settings_Sync)
	R_Lights_ApplyFogState();
	gEngfuncs.Con_Printf( "^2ref_openremix: Light and volumetric subsystem initialized^7\n" );
}

/*
====================
R_Lights_Shutdown
====================
*/
void R_Lights_Shutdown( void )
{
	for( int i = 0; i < IES_COUNT; i++ )
	{
		free( s_ies[i].file );
		memset( &s_ies[i], 0, sizeof( s_ies[i] ));
	}
	if( s_flashlightCookie.builtin && g_remix.DestroyTexture )
		g_remix.DestroyTexture( s_flashlightCookie.builtin );
	memset( &s_flashlightCookie, 0, sizeof( s_flashlightCookie ));

	if( g_remix.DestroyLight )
	{
		for( uint32_t i = 0; i < g_lights.static_lights_count; i++ )
		{
			if( g_lights.static_lights[i].handle )
			{
				g_remix.DestroyLight( g_lights.static_lights[i].handle );
				g_lights.static_lights[i].handle = NULL;
			}
		}

		if( g_lights.sun_handle )
		{
			g_remix.DestroyLight( g_lights.sun_handle );
			g_lights.sun_handle = NULL;
		}

		if( g_lights.flashlight_handle )
		{
			g_remix.DestroyLight( g_lights.flashlight_handle );
			g_lights.flashlight_handle = NULL;
		}

		for( int i = 0; i < MAX_ACTIVE_DLIGHTS; i++ )
		{
			if( g_lights.dlight_handles[i] )
			{
				g_remix.DestroyLight( g_lights.dlight_handles[i] );
				g_lights.dlight_handles[i] = NULL;
			}
		}
	}

	memset( &g_lights, 0, sizeof( g_lights ) );
}

/*
====================
R_Lights_ApplyFogState

Half-Life sets no fog the path tracer could scatter in, so volumetrics (light shafts) would
stay off with the override off. XashRT keeps a thin medium of its own: so does this, handed to
OpenRemix as the game's fog (remixapi_SetFogState) so that the profile's override stays the
user's. rt_fog_distance: the sight distance in meters (5% of the light left), as in the override
====================
*/
void R_Lights_ApplyFogState( void )
{
	if( !g_remix.SetFogState )
		return;

	remixapi_FogInfo fog;
	memset( &fog, 0, sizeof( fog ) );
	fog.sType = REMIXAPI_STRUCT_TYPE_FOG_INFO;
	if( rt_fog && rt_fog->value > 0.0f )
	{
		const float meters = ( rt_fog_distance && rt_fog_distance->value > 1.0f ) ? rt_fog_distance->value : 2000.0f;
		fog.mode    = 1; // exp: the density is the extinction per world unit
		fog.color.x = fog.color.y = fog.color.z = 1.0f;
		fog.density = 3.0f / ( meters / 0.0254f ); // -ln( 0.05 ) = 3
	}
	g_remix.SetFogState( &fog );
}

/*
====================
R_Lights_UpdateVolumetrics

Applies changed fog and light range settings (R_Settings_Sync): the profile keeps the rest
====================
*/
void R_Lights_UpdateVolumetrics( void )
{
	R_Lights_ApplyFogState();

	if( !g_remix.SetConfigVariable )
		return;

	if( rt_light_range && rt_light_range->value > 0.0f )
	{
		char rangeBuf[32];
		Q_snprintf( rangeBuf, sizeof( rangeBuf ), "%.1f", rt_light_range->value );
		g_remix.SetConfigVariable( "rtx.native.pathtrace.localRange", rangeBuf );
	}
	else
	{
		g_remix.SetConfigVariable( "rtx.native.pathtrace.localRange", "0" );
	}

	if( rt_fog && rt_fog->value > 0 )
	{
		g_remix.SetConfigVariable( "rtx.native.pathtrace.volumetrics", "1" );

		if( rt_fog_scattering )
		{
			char scatBuf[32];
			Q_snprintf( scatBuf, sizeof( scatBuf ), "%.2f", rt_fog_scattering->value );
			g_remix.SetConfigVariable( "rtx.native.pathtrace.volumetricScattering", scatBuf );
		}
	}
	else
	{
		g_remix.SetConfigVariable( "rtx.native.pathtrace.volumetrics", "0" );
	}
}

/*
====================
R_Lights_NewMap
====================
*/
void R_Lights_NewMap( const char *entities )
{
	R_Lights_Shutdown();

	if( !entities || !*entities )
		return;

	enum
	{
		LT_CLASS_UNKNOWN,
		LT_CLASS_POINT,
		LT_CLASS_SPOT,
		LT_CLASS_SUN
	} classname = LT_CLASS_UNKNOWN;

	enum
	{
		LT_HAS_CLASS     = ( 1 << 0 ),
		LT_HAS_ORIGIN    = ( 1 << 1 ),
		LT_HAS_COLOR     = ( 1 << 2 ),
		LT_HAS_PITCH     = ( 1 << 3 ),
		LT_HAS_ANGLE     = ( 1 << 4 ),
		LT_HAS_ALLANGLES = ( 1 << 5 ),
		LT_HAS_CONE      = ( 1 << 6 ),
		LT_HAS_CONE2     = ( 1 << 7 ),
		LT_HAS_STYLE     = ( 1 << 8 ),
		LT_HAS_SKY       = ( 1 << 9 )
	};

	struct
	{
		vec3_t origin;
		vec3_t allangles;
		vec3_t color;
		float  intensity;
		float  pitch;
		float  angle;
		float  cone;
		float  cone2;
		int    style;
	} light;

	uint32_t has = 0;
	char *pos = (char *)entities;
	char key[128];
	char value[256];

	float s_scale = ( rt_light_s ? rt_light_s->value : 500.0f );
	if( s_scale <= 0.0f ) s_scale = 500.0f;

	for( ;; )
	{
		pos = COM_ParseFile( pos, key, sizeof( key ) );
		if( !pos )
			break;

		if( key[0] == '{' )
		{
			classname = LT_CLASS_UNKNOWN;
			has = 0;
			memset( &light, 0, sizeof( light ) );
			light.intensity = 1.0f;
			light.cone = 30.0f;
			light.cone2 = 45.0f;
			continue;
		}

		if( key[0] == '}' )
		{
			if( !( has & LT_HAS_CLASS ) )
				continue;

			//
			// 1. Point and Spot Lights
			//
			if( ( classname == LT_CLASS_POINT || classname == LT_CLASS_SPOT ) && !( has & LT_HAS_SKY ) )
			{
				if( ( has & LT_HAS_ORIGIN ) && ( has & LT_HAS_COLOR ) && g_lights.static_lights_count < MAX_STATIC_MAP_LIGHTS )
				{
					parsed_light_t *dst = &g_lights.static_lights[g_lights.static_lights_count];
					memset( dst, 0, sizeof( *dst ) );

					VectorCopy( light.origin, dst->origin );
					dst->radiance.x = ( light.color[0] / 255.0f ) * light.intensity * s_scale;
					dst->radiance.y = ( light.color[1] / 255.0f ) * light.intensity * s_scale;
					dst->radiance.z = ( light.color[2] / 255.0f ) * light.intensity * s_scale;
					dst->intensity = light.intensity;
					dst->style = ( has & LT_HAS_STYLE ) ? light.style : 0;
					dst->is_spot = ( classname == LT_CLASS_SPOT );

					vec3_t direction = { 0, 0, -1 };
					if( dst->is_spot )
					{
						if( has & LT_HAS_ALLANGLES )
						{
							AngleVectors( light.allangles, direction, NULL, NULL );
						}
						else if( ( has & LT_HAS_PITCH ) || ( has & LT_HAS_ANGLE ) )
						{
							vec3_t angles = { ( has & LT_HAS_PITCH ) ? -light.pitch : 0.0f, ( has & LT_HAS_ANGLE ) ? light.angle : 0.0f, 0.0f };
							AngleVectors( angles, direction, NULL, NULL );
						}
						VectorNormalize( direction );
						VectorCopy( direction, dst->direction );

						dst->cone_angle = ( has & LT_HAS_CONE ) ? light.cone : 30.0f;
						float outer = ( has & LT_HAS_CONE2 ) ? light.cone2 : ( dst->cone_angle + 15.0f );
						dst->cone_softness = ( outer > dst->cone_angle ) ? ( ( outer - dst->cone_angle ) / outer ) : 0.2f;
					}

					dst->hash = R_LightMakeHash( g_lights.static_lights_count, dst->origin );

					// Build Remix Sphere/Spot Light definition
					float srcRadius = ( rt_light_radius ? rt_light_radius->value : 8.0f );
					if( srcRadius < 1.0f ) srcRadius = 1.0f;

					remixapi_LightInfoIesProfileEXT iesExt;
					const qboolean ies = dst->is_spot && R_Lights_GetIes( IES_SPOTLIGHT, &iesExt );

					remixapi_LightInfoSphereEXT sphereExt;
					memset( &sphereExt, 0, sizeof( sphereExt ) );
					sphereExt.sType                   = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
					sphereExt.pNext                   = ies ? &iesExt : NULL;
					sphereExt.position.x             = dst->origin[0];
					sphereExt.position.y             = dst->origin[1];
					sphereExt.position.z             = dst->origin[2];
					sphereExt.radius                  = dst->is_spot ? ( srcRadius * 0.6f ) : srcRadius;
					sphereExt.volumetricRadianceScale = 1.0f;

					if( dst->is_spot )
					{
						sphereExt.shaping_hasvalue = 1;
						sphereExt.shaping_value.direction.x      = dst->direction[0];
						sphereExt.shaping_value.direction.y      = dst->direction[1];
						sphereExt.shaping_value.direction.z      = dst->direction[2];
						sphereExt.shaping_value.coneAngleDegrees = dst->cone_angle;
						sphereExt.shaping_value.coneSoftness     = dst->cone_softness;
						sphereExt.shaping_value.focusExponent    = 1.0f;
					}

					remixapi_LightInfo lightInfo;
					memset( &lightInfo, 0, sizeof( lightInfo ) );
					lightInfo.sType     = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
					lightInfo.pNext     = &sphereExt;
					lightInfo.hash      = dst->hash;
					lightInfo.radiance  = dst->radiance;
					lightInfo.isDynamic = 0;

					if( g_remix.CreateLightBatched && g_remix.CreateLightBatched( &lightInfo, &dst->handle ) == REMIXAPI_ERROR_CODE_SUCCESS )
					{
						g_lights.static_lights_count++;
					}
				}
			}
			//
			// 2. Environment / Sun Light
			//
			else if( ( classname == LT_CLASS_SUN || ( has & LT_HAS_SKY ) ) && !g_lights.has_sun )
			{
				if( has & LT_HAS_COLOR )
				{
					g_lights.has_sun = true;
					float sun_scale = ( rt_sun ? rt_sun->value : 50.0f );
					g_lights.sun_radiance.x = ( light.color[0] / 255.0f ) * light.intensity * sun_scale;
					g_lights.sun_radiance.y = ( light.color[1] / 255.0f ) * light.intensity * sun_scale;
					g_lights.sun_radiance.z = ( light.color[2] / 255.0f ) * light.intensity * sun_scale;

					float pitch = ( has & LT_HAS_PITCH ) ? light.pitch : 45.0f;
					float angle = ( has & LT_HAS_ANGLE ) ? light.angle : 0.0f;
					vec3_t angles = { -pitch, angle, 0.0f };
					AngleVectors( angles, g_lights.sun_direction, NULL, NULL );
					VectorNormalize( g_lights.sun_direction );

					g_lights.sun_diameter = ( rt_sun_diameter ? rt_sun_diameter->value : 0.53f );
					g_lights.sun_hash = 0x53554E4C49474854ULL; // "SUNLIGHT"

					remixapi_LightInfoDistantEXT distantExt;
					memset( &distantExt, 0, sizeof( distantExt ) );
					distantExt.sType                   = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
					distantExt.pNext                   = NULL;
					distantExt.direction.x             = g_lights.sun_direction[0];
					distantExt.direction.y             = g_lights.sun_direction[1];
					distantExt.direction.z             = g_lights.sun_direction[2];
					distantExt.angularDiameterDegrees  = g_lights.sun_diameter;
					distantExt.volumetricRadianceScale = rt_sun_volumetric ? Q_max( rt_sun_volumetric->value, 0.0f ) : 1.0f;

					remixapi_LightInfo sunInfo;
					memset( &sunInfo, 0, sizeof( sunInfo ) );
					sunInfo.sType     = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
					sunInfo.pNext     = &distantExt;
					sunInfo.hash      = g_lights.sun_hash;
					sunInfo.radiance  = g_lights.sun_radiance;
					sunInfo.isDynamic = 0;

					if( g_remix.CreateLightBatched )
						g_remix.CreateLightBatched( &sunInfo, &g_lights.sun_handle );
				}
			}

			continue;
		}

		pos = COM_ParseFile( pos, value, sizeof( value ) );
		if( !pos )
			break;

		if( !Q_strcmp( key, "origin" ) )
		{
			if( sscanf( value, "%f %f %f", &light.origin[0], &light.origin[1], &light.origin[2] ) == 3 )
				has |= LT_HAS_ORIGIN;
		}
		else if( !Q_strcmp( key, "classname" ) )
		{
			if( !Q_strcmp( value, "light" ) )
			{
				classname = LT_CLASS_POINT;
				has |= LT_HAS_CLASS;
			}
			else if( !Q_strcmp( value, "light_spot" ) )
			{
				classname = LT_CLASS_SPOT;
				has |= LT_HAS_CLASS;
			}
			else if( !Q_strcmp( value, "light_environment" ) )
			{
				classname = LT_CLASS_SUN;
				has |= LT_HAS_CLASS;
			}
		}
		else if( !Q_strcmp( key, "_light" ) )
		{
			float scale = 1.0f;
			int c = sscanf( value, "%f %f %f %f", &light.color[0], &light.color[1], &light.color[2], &scale );
			if( c == 1 )
			{
				light.color[1] = light.color[2] = light.color[0];
				light.intensity = 1.0f;
				has |= LT_HAS_COLOR;
			}
			else if( c == 3 )
			{
				light.intensity = 1.0f;
				has |= LT_HAS_COLOR;
			}
			else if( c == 4 )
			{
				light.intensity = scale / 255.0f;
				has |= LT_HAS_COLOR;
			}
		}
		else if( !Q_strcmp( key, "angles" ) )
		{
			if( sscanf( value, "%f %f %f", &light.allangles[0], &light.allangles[1], &light.allangles[2] ) == 3 )
				has |= LT_HAS_ALLANGLES;
		}
		else if( !Q_strcmp( key, "pitch" ) )
		{
			if( sscanf( value, "%f", &light.pitch ) == 1 )
				has |= LT_HAS_PITCH;
		}
		else if( !Q_strcmp( key, "angle" ) )
		{
			if( sscanf( value, "%f", &light.angle ) == 1 )
				has |= LT_HAS_ANGLE;
		}
		else if( !Q_strcmp( key, "_cone" ) )
		{
			if( sscanf( value, "%f", &light.cone ) == 1 )
				has |= LT_HAS_CONE;
		}
		else if( !Q_strcmp( key, "_cone2" ) )
		{
			if( sscanf( value, "%f", &light.cone2 ) == 1 )
				has |= LT_HAS_CONE2;
		}
		else if( !Q_strcmp( key, "style" ) )
		{
			if( sscanf( value, "%d", &light.style ) == 1 )
				has |= LT_HAS_STYLE;
		}
		else if( !Q_strcmp( key, "_sky" ) )
		{
			int s = 0;
			if( sscanf( value, "%d", &s ) == 1 && s != 0 )
				has |= LT_HAS_SKY;
		}
	}

	R_Lights_ApplyFogState();
	gEngfuncs.Con_Printf( "^2ref_openremix: Extracted %u static map lights (Sun: %s)^7\n",
		g_lights.static_lights_count, g_lights.has_sun ? "Yes" : "No" );
}

/*
====================
R_Lights_SubmitFlashlight
====================
*/
static void R_Lights_SubmitFlashlight( void )
{
	if( !g_remix.CreateLight )
		return;

	float flsh_intensity = ( rt_flsh ? rt_flsh->value : 8000.0f );
	if( flsh_intensity <= 0.0f ) flsh_intensity = 8000.0f;

	float src_radius = ( rt_flsh_radius ? rt_flsh_radius->value : 2.0f );
	if( src_radius < 0.1f ) src_radius = 2.0f;

	float cone_angle = ( rt_flsh_angle ? rt_flsh_angle->value : 25.0f );
	if( cone_angle < 5.0f ) cone_angle = 25.0f;

	float offset_r = ( rt_flsh_r ? rt_flsh_r->value : 2.5f );
	float offset_u = ( rt_flsh_u ? rt_flsh_u->value : -2.5f );

	// Calculate flashlight origin: chest/shoulder mounted, slightly forward of eye position
	vec3_t origin;
	VectorCopy( RI.vieworg, origin );
	VectorMA( origin, 3.0f, RI.vforward, origin );
	VectorMA( origin, offset_r, RI.vright, origin );
	VectorMA( origin, offset_u, RI.vup, origin );

	remixapi_LightInfoSphereEXT sphereExt;
	memset( &sphereExt, 0, sizeof( sphereExt ) );
	sphereExt.sType                   = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
	sphereExt.pNext                   = NULL;
	sphereExt.position.x             = origin[0];
	sphereExt.position.y             = origin[1];
	sphereExt.position.z             = origin[2];
	sphereExt.radius                  = src_radius;
	sphereExt.volumetricRadianceScale = 1.5f;

	sphereExt.shaping_hasvalue               = 1;
	sphereExt.shaping_value.direction.x      = RI.vforward[0];
	sphereExt.shaping_value.direction.y      = RI.vforward[1];
	sphereExt.shaping_value.direction.z      = RI.vforward[2];
	sphereExt.shaping_value.coneAngleDegrees = cone_angle;
	sphereExt.shaping_value.coneSoftness     = 0.35f;
	sphereExt.shaping_value.focusExponent    = 1.0f;

	// With an IES profile the beam is the profile's: the cone only bounds it
	remixapi_LightInfoIesProfileEXT iesExt;
	if( R_Lights_GetIes( IES_FLASHLIGHT, &iesExt ))
	{
		sphereExt.pNext                          = &iesExt;
		sphereExt.shaping_value.coneAngleDegrees = 60.0f;
		sphereExt.shaping_value.coneSoftness     = 0.0f;
		sphereExt.shaping_value.focusExponent    = 0.0f;
	}

	// The cookie's texture across the beam, turning with the view
	remixapi_LightInfoCookieEXT cookieExt;
	if( R_Lights_GetFlashlightCookie( &cookieExt ))
	{
		cookieExt.pNext  = sphereExt.pNext;
		sphereExt.pNext  = &cookieExt;
	}

	remixapi_LightInfo lightInfo;
	memset( &lightInfo, 0, sizeof( lightInfo ) );
	lightInfo.sType     = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
	lightInfo.pNext     = &sphereExt;
	lightInfo.hash      = 0xF1A541168700ULL; // "FLASHLGT"
	// Crisp cool LED tint (220, 243, 255)
	lightInfo.radiance.x = ( 220.0f / 255.0f ) * flsh_intensity;
	lightInfo.radiance.y = ( 243.0f / 255.0f ) * flsh_intensity;
	lightInfo.radiance.z = ( 255.0f / 255.0f ) * flsh_intensity;
	lightInfo.isDynamic = 1;
	lightInfo.ignoreViewModel = 0;

	if( g_remix.CreateLight( &lightInfo, &g_lights.flashlight_handle ) == REMIXAPI_ERROR_CODE_SUCCESS )
	{
		if( g_remix.DrawLightInstance && g_lights.flashlight_handle )
			g_remix.DrawLightInstance( g_lights.flashlight_handle );
		g_lights.flashlight_active = true;
	}
}

/*
====================
R_Lights_SubmitPointDlight
====================
*/
static void R_Lights_SubmitPointDlight( const dlight_t *l, int index, float d_scale, qboolean entityLight )
{
	if( !l || l->dark || l->radius <= 0.0f || index < 0 || index >= MAX_ACTIVE_DLIGHTS )
		return;

	// Scale intensity based on radius and color (GoldSrc dlights have radius ~ 80..300). An entity
	// light's radius only says how far it tints its model (controller orbs send 16): its strength
	// is rt_light_e's
	float falloff_mult = entityLight ? 1.0f : l->radius * 0.025f; // Quake to metric
	if( falloff_mult < 1.0f ) falloff_mult = 1.0f;

	remixapi_LightInfoSphereEXT sphereExt;
	memset( &sphereExt, 0, sizeof( sphereExt ) );
	sphereExt.sType                   = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
	sphereExt.pNext                   = NULL;
	sphereExt.position.x             = l->origin[0];
	sphereExt.position.y             = l->origin[1];
	sphereExt.position.z             = l->origin[2];
	sphereExt.radius                  = ( rt_light_radius ? rt_light_radius->value : 4.0f );
	if( sphereExt.radius < 1.0f ) sphereExt.radius = 1.0f;
	sphereExt.volumetricRadianceScale = 1.0f;
	sphereExt.shaping_hasvalue        = 0;

	remixapi_LightInfo lightInfo;
	memset( &lightInfo, 0, sizeof( lightInfo ) );
	lightInfo.sType     = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
	lightInfo.pNext     = &sphereExt;
	lightInfo.hash      = 0x444C494748540000ULL | (uint64_t)( index & 0xFFFF ); // "DLIGHT" + index
	lightInfo.radiance.x = ( (float)l->color.r / 255.0f ) * d_scale * falloff_mult;
	lightInfo.radiance.y = ( (float)l->color.g / 255.0f ) * d_scale * falloff_mult;
	lightInfo.radiance.z = ( (float)l->color.b / 255.0f ) * d_scale * falloff_mult;
	lightInfo.isDynamic = 1;

	if( g_remix.CreateLight )
	{
		if( g_remix.CreateLight( &lightInfo, &g_lights.dlight_handles[index] ) == REMIXAPI_ERROR_CODE_SUCCESS )
		{
			if( g_remix.DrawLightInstance && g_lights.dlight_handles[index] )
				g_remix.DrawLightInstance( g_lights.dlight_handles[index] );
		}
	}
}

/*
====================
R_Lights_DrawFrame
====================
*/
void R_Lights_DrawFrame( void )
{
	if( !g_remix.DrawLightInstance )
		return;

	// 1. Submit static map lights
	for( uint32_t i = 0; i < g_lights.static_lights_count; i++ )
	{
		if( g_lights.static_lights[i].handle )
			g_remix.DrawLightInstance( g_lights.static_lights[i].handle );
	}

	// 2. Submit sun distant light
	if( g_lights.has_sun && g_lights.sun_handle )
	{
		g_remix.DrawLightInstance( g_lights.sun_handle );
	}

	// 3. Submit Player Flashlight & Dynamic Lights
	double curTime = ( gp_cl ? gp_cl->time : tr.realtime );
	float d_scale = ( rt_light_d ? rt_light_d->value : 1000.0f );
	int flsh_key = ( _rt_flsh_key ? (int)_rt_flsh_key->value : -1 );
	int player_key = gp_cl ? ( gp_cl->playernum + 1 ) : 1;

	qboolean flashlight_drawn = false;

	// Engine dynamic lights: flashlight (key == local player index), muzzle flashes, explosions
	if( gp_dlights )
	{
		for( int i = 0; i < R_OPENREMIX_MAX_DLIGHTS; i++ )
		{
			dlight_t *l = &gp_dlights[i];
			if( l->die < curTime || l->radius <= 0.0f )
				continue;

			if( l->key == player_key || ( flsh_key != -1 && l->key == flsh_key ) )
			{
				R_Lights_SubmitFlashlight();
				flashlight_drawn = true;
				continue;
			}

			R_Lights_SubmitPointDlight( l, i, d_scale, false );
		}
	}

	// Fallback/extra check: if flashlight was not in gp_dlights, check local player EF_DIMLIGHT
	if( !flashlight_drawn )
	{
		cl_entity_t *player = CL_GetEntityByIndex( player_key );
		if( player && ( player->curstate.effects & EF_DIMLIGHT ) )
		{
			R_Lights_SubmitFlashlight();
			flashlight_drawn = true;
		}
		else
		{
			g_lights.flashlight_active = false;
		}
	}

	// Entity lights (EF_DIMLIGHT/EF_BRIGHTLIGHT on studio models)
	if( tr.elights )
	{
		for( int i = 0; i < MAX_ELIGHTS && R_OPENREMIX_MAX_DLIGHTS + i < MAX_ACTIVE_DLIGHTS; i++ )
		{
			dlight_t *l = &tr.elights[i];
			if( l->die < curTime || l->radius <= 0.0f )
				continue;
			R_Lights_SubmitPointDlight( l, R_OPENREMIX_MAX_DLIGHTS + i, rt_light_e ? rt_light_e->value : 5000.0f, true );
		}
	}
}

