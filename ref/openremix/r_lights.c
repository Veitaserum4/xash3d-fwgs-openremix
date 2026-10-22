/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_lights.c - Light extraction from BSP entities, dynamic lights, player flashlight, and volumetrics
 */

#include "r_local.h"
#include "r_lights.h"
#include "r_remix.h"

lights_state_t g_lights;

// Lighting CVars
static cvar_t *rt_sun              = NULL;
static cvar_t *rt_sun_diameter     = NULL;
static cvar_t *rt_flsh             = NULL;
static cvar_t *rt_flsh_radius      = NULL;
static cvar_t *rt_flsh_angle       = NULL;
static cvar_t *rt_flsh_r           = NULL;
static cvar_t *rt_flsh_u           = NULL;
static cvar_t *rt_light_s          = NULL;
static cvar_t *rt_light_d          = NULL;
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
		rt_sun             = gEngfuncs.Cvar_Get( "rt_sun", "2000", FCVAR_ARCHIVE, "Sun directional light intensity" );
		rt_sun_diameter    = gEngfuncs.Cvar_Get( "rt_sun_diameter", "0.53", FCVAR_ARCHIVE, "Sun angular diameter in degrees" );
		rt_flsh            = gEngfuncs.Cvar_Get( "rt_flsh", "800", FCVAR_ARCHIVE, "Player flashlight intensity" );
		rt_flsh_radius     = gEngfuncs.Cvar_Get( "rt_flsh_radius", "2.0", FCVAR_ARCHIVE, "Player flashlight source radius" );
		rt_flsh_angle      = gEngfuncs.Cvar_Get( "rt_flsh_angle", "25", FCVAR_ARCHIVE, "Player flashlight outer cone angle in degrees" );
		rt_flsh_r          = gEngfuncs.Cvar_Get( "rt_flsh_r", "2.5", FCVAR_ARCHIVE, "Player flashlight right offset" );
		rt_flsh_u          = gEngfuncs.Cvar_Get( "rt_flsh_u", "-2.5", FCVAR_ARCHIVE, "Player flashlight up offset" );
		rt_light_s         = gEngfuncs.Cvar_Get( "rt_light_s", "1000", FCVAR_ARCHIVE, "Map static light intensity scale" );
		rt_light_d         = gEngfuncs.Cvar_Get( "rt_light_d", "1000", FCVAR_ARCHIVE, "Dynamic lights (explosions/projectiles) intensity scale" );
		rt_light_radius    = gEngfuncs.Cvar_Get( "rt_light_radius", "3.0", FCVAR_ARCHIVE, "Map static light source radius in GoldSrc units" );
		rt_light_range     = gEngfuncs.Cvar_Get( "rt_light_range", "0", FCVAR_ARCHIVE, "Local light cutoff range (0 = unlimited physical falloff)" );
		rt_emis            = gEngfuncs.Cvar_Get( "rt_emis", "1.0", FCVAR_ARCHIVE, "Texture light emission multiplier" );
		rt_fog             = gEngfuncs.Cvar_Get( "rt_fog", "0", FCVAR_ARCHIVE, "Enable OpenRemix path-traced volumetric fog" );
		rt_fog_distance    = gEngfuncs.Cvar_Get( "rt_fog_distance", "500.0", FCVAR_ARCHIVE, "Volumetric fog sight distance in meters" );
		rt_fog_scattering  = gEngfuncs.Cvar_Get( "rt_fog_scattering", "0.3", FCVAR_ARCHIVE, "Volumetric fog light scattering multiplier" );

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

	R_Lights_UpdateVolumetrics();
	gEngfuncs.Con_Printf( "^2ref_openremix: Light and volumetric subsystem initialized^7\n" );
}

/*
====================
R_Lights_Shutdown
====================
*/
void R_Lights_Shutdown( void )
{
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
R_Lights_UpdateVolumetrics
====================
*/
void R_Lights_UpdateVolumetrics( void )
{
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
		g_remix.SetConfigVariable( "rtx.native.pathtrace.volumetricOverride", "1" );
		g_remix.SetConfigVariable( "rtx.native.pathtrace.volumetricUpAxis", "2" ); // Z-up
		g_remix.SetConfigVariable( "rtx.native.pathtrace.volumetricMaxDistance", "250.0" );

		if( rt_fog_distance )
		{
			char distBuf[32];
			Q_snprintf( distBuf, sizeof( distBuf ), "%.1f", rt_fog_distance->value );
			g_remix.SetConfigVariable( "rtx.native.pathtrace.volumetricDistance", distBuf );
		}

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

	float s_scale = ( rt_light_s ? rt_light_s->value : 1000.0f );
	if( s_scale <= 0.0f ) s_scale = 1000.0f;

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

					remixapi_LightInfoSphereEXT sphereExt;
					memset( &sphereExt, 0, sizeof( sphereExt ) );
					sphereExt.sType                   = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
					sphereExt.pNext                   = NULL;
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
					float sun_scale = ( rt_sun ? rt_sun->value : 2000.0f );
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
					distantExt.volumetricRadianceScale = 1.0f;

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

	R_Lights_UpdateVolumetrics();
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

	float flsh_intensity = ( rt_flsh ? rt_flsh->value : 800.0f );
	if( flsh_intensity <= 0.0f ) flsh_intensity = 800.0f;

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
static void R_Lights_SubmitPointDlight( const dlight_t *l, int index, float d_scale )
{
	if( !l || l->dark || l->radius <= 0.0f || index < 0 || index >= MAX_ACTIVE_DLIGHTS )
		return;

	// Scale intensity based on radius and color (GoldSrc dlights have radius ~ 80..300)
	float falloff_mult = l->radius * 0.025f; // Quake to metric
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

			R_Lights_SubmitPointDlight( l, i, d_scale );
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
			R_Lights_SubmitPointDlight( l, R_OPENREMIX_MAX_DLIGHTS + i, d_scale );
		}
	}
}

