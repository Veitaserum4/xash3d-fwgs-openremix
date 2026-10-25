/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_xashrt.c - XashRT content support
 *
 * Reads the "rt/" folder shipped with xash-rt (hl1-rt-resources.zip), relative to the game's
 * working directory:
 *   rt/data/textures.json      per-texture material overrides (chrome, glass, emission)
 *   rt/mat/<texname>[_e].ktx2  replacement albedo / emissive textures
 *   rt/scenes/<map>/<map>.gltf added geometry (mirrors, props) and hand-placed lights
 */

#include <stdio.h>
#include <stdlib.h>
#include "r_local.h"
#include "r_remix.h"
#include "r_textures.h"
#include "r_xashrt.h"

static cvar_t *rt_xrt              = NULL;
static cvar_t *rt_xrt_emis         = NULL;
static cvar_t *rt_xrt_light_emis   = NULL;
static cvar_t *rt_xrt_scene_lights = NULL;
static cvar_t *rt_xrt_volumetric   = NULL;

static inline uint64_t R_XRT_Hash64( const char *str )
{
	uint64_t hash = 14695981039346656037ULL;
	if( !str ) return hash;
	while( *str )
	{
		hash ^= (uint64_t)(unsigned char)( *str++ );
		hash *= 1099511628211ULL;
	}
	return hash ? hash : 1ULL;
}

static FILE *R_XRT_Open( const char *path )
{
	FILE *f = fopen( path, "rb" );
#if defined( _WIN32 )
	// rt/ sits next to the executable, like XashRT; the working directory may differ
	if( !f )
	{
		char full[MAX_PATH];
		DWORD len = GetModuleFileNameA( NULL, full, sizeof( full ) );
		char *slash = len ? strrchr( full, '\\' ) : NULL;
		if( slash && (size_t)( slash - full ) + 2 + strlen( path ) < sizeof( full ) )
		{
			Q_strncpy( slash + 1, path, sizeof( full ) - ( slash + 1 - full ) );
			f = fopen( full, "rb" );
		}
	}
#endif
	return f;
}

byte *R_XRT_ReadFile( const char *path, size_t *outSize )
{
	FILE *f = R_XRT_Open( path );
	if( !f )
		return NULL;

	fseek( f, 0, SEEK_END );
	long size = ftell( f );
	fseek( f, 0, SEEK_SET );
	if( size <= 0 )
	{
		fclose( f );
		return NULL;
	}

	byte *data = (byte *)malloc( (size_t)size + 1 );
	if( data && fread( data, 1, (size_t)size, f ) != (size_t)size )
	{
		free( data );
		data = NULL;
	}
	fclose( f );

	if( data )
	{
		data[size] = 0;
		*outSize = (size_t)size;
	}
	return data;
}

/*
==============================================================================
Minimal JSON parser (objects, arrays, strings, numbers, bools; // and block comments)
==============================================================================
*/
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype_t;

typedef struct jval_s
{
	jtype_t         type;
	double          num;
	char           *str;
	int             count;
	struct jval_s **items;
	char          **keys;
} jval_t;

typedef struct { const char *p, *end; } jcur_t;

static void J_Free( jval_t *v )
{
	if( !v ) return;
	for( int i = 0; i < v->count; i++ )
	{
		J_Free( v->items[i] );
		if( v->keys ) free( v->keys[i] );
	}
	free( v->items );
	free( v->keys );
	free( v->str );
	free( v );
}

static void J_SkipWs( jcur_t *c )
{
	while( c->p < c->end )
	{
		if( *c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n' )
			c->p++;
		else if( c->p + 1 < c->end && c->p[0] == '/' && c->p[1] == '/' )
		{
			while( c->p < c->end && *c->p != '\n' ) c->p++;
		}
		else if( c->p + 1 < c->end && c->p[0] == '/' && c->p[1] == '*' )
		{
			c->p += 2;
			while( c->p + 1 < c->end && !( c->p[0] == '*' && c->p[1] == '/' ) ) c->p++;
			c->p += 2;
		}
		else
			break;
	}
}

static char *J_ParseString( jcur_t *c )
{
	if( c->p >= c->end || *c->p != '"' )
		return NULL;
	c->p++;

	size_t cap = 32, len = 0;
	char *s = (char *)malloc( cap );
	while( s && c->p < c->end && *c->p != '"' )
	{
		char ch = *c->p++;
		if( ch == '\\' && c->p < c->end )
		{
			ch = *c->p++;
			switch( ch )
			{
			case 'n': ch = '\n'; break;
			case 't': ch = '\t'; break;
			case 'u': c->p += ( c->end - c->p >= 4 ) ? 4 : ( c->end - c->p ); ch = '?'; break;
			default: break;
			}
		}
		if( len + 2 > cap )
		{
			cap *= 2;
			char *n = (char *)realloc( s, cap );
			if( !n ) { free( s ); return NULL; }
			s = n;
		}
		s[len++] = ch;
	}
	if( s ) s[len] = 0;
	if( c->p < c->end ) c->p++; // closing quote
	return s;
}

static jval_t *J_Parse( jcur_t *c, int depth );

static qboolean J_Append( jval_t *v, jval_t *item, char *key )
{
	jval_t **items = (jval_t **)realloc( v->items, sizeof( jval_t * ) * ( v->count + 1 ) );
	if( !items ) return false;
	v->items = items;
	if( v->type == J_OBJ )
	{
		char **keys = (char **)realloc( v->keys, sizeof( char * ) * ( v->count + 1 ) );
		if( !keys ) return false;
		v->keys = keys;
		v->keys[v->count] = key;
	}
	v->items[v->count++] = item;
	return true;
}

static jval_t *J_Parse( jcur_t *c, int depth )
{
	J_SkipWs( c );
	if( c->p >= c->end || depth > 64 )
		return NULL;

	jval_t *v = (jval_t *)calloc( 1, sizeof( jval_t ) );
	if( !v ) return NULL;

	char ch = *c->p;
	if( ch == '{' || ch == '[' )
	{
		v->type = ( ch == '{' ) ? J_OBJ : J_ARR;
		char close = ( ch == '{' ) ? '}' : ']';
		c->p++;
		for( ;; )
		{
			J_SkipWs( c );
			if( c->p >= c->end ) { J_Free( v ); return NULL; }
			if( *c->p == close ) { c->p++; break; }
			if( *c->p == ',' ) { c->p++; continue; } // tolerate leading/trailing commas

			char *key = NULL;
			if( v->type == J_OBJ )
			{
				key = J_ParseString( c );
				J_SkipWs( c );
				if( !key || c->p >= c->end || *c->p != ':' ) { free( key ); J_Free( v ); return NULL; }
				c->p++;
			}

			jval_t *item = J_Parse( c, depth + 1 );
			if( !item || !J_Append( v, item, key ) ) { free( key ); J_Free( item ); J_Free( v ); return NULL; }
		}
	}
	else if( ch == '"' )
	{
		v->type = J_STR;
		v->str = J_ParseString( c );
	}
	else if( !strncmp( c->p, "true", 4 ) )  { v->type = J_BOOL; v->num = 1; c->p += 4; }
	else if( !strncmp( c->p, "false", 5 ) ) { v->type = J_BOOL; v->num = 0; c->p += 5; }
	else if( !strncmp( c->p, "null", 4 ) )  { v->type = J_NULL; c->p += 4; }
	else
	{
		char *endp = NULL;
		v->type = J_NUM;
		v->num = strtod( c->p, &endp );
		if( !endp || endp == c->p ) { J_Free( v ); return NULL; }
		c->p = endp;
	}
	return v;
}

static jval_t *J_ParseText( const char *text, size_t len )
{
	jcur_t c = { text, text + len };
	return J_Parse( &c, 0 );
}

static jval_t *J_Get( const jval_t *v, const char *key )
{
	if( !v || v->type != J_OBJ ) return NULL;
	for( int i = 0; i < v->count; i++ )
		if( v->keys[i] && !strcmp( v->keys[i], key ) )
			return v->items[i];
	return NULL;
}

static jval_t *J_At( const jval_t *v, int i )
{
	return ( v && v->type == J_ARR && i >= 0 && i < v->count ) ? v->items[i] : NULL;
}

static double J_Num( const jval_t *v, double def )
{
	return ( v && ( v->type == J_NUM || v->type == J_BOOL ) ) ? v->num : def;
}

static int J_Int( const jval_t *v, int def )
{
	return (int)J_Num( v, def );
}

static const char *J_Str( const jval_t *v )
{
	return ( v && v->type == J_STR ) ? v->str : NULL;
}

/*
==============================================================================
Texture names and material overrides
==============================================================================
*/
typedef struct
{
	char     key[96];
	qboolean mirror, glass, water, opaque;
	float    metallic, roughness;
	float    emissiveMult, lightIntensity; // < 0 when unset
} xrt_texentry_t;

static xrt_texentry_t *s_texEntries = NULL;
static int             s_numTexEntries = 0;

/*
 * R_XRT_TextureKey - XashRT naming: *.mip textures become "#maps/<name>.mip" regardless of
 * the WAD/BSP they came from; other names (e.g. "#models/barney/BX_Chrome1.mdl") are kept
 */
void R_XRT_TextureKey( const char *texname, char *out, size_t outSize )
{
	if( !texname ) { out[0] = 0; return; }

	const char *ext = COM_FileExtension( texname );
	if( !Q_stricmp( ext, "mip" ) )
	{
		const char *start = texname;
		const char *seps[3] = { strrchr( texname, '/' ), strrchr( texname, '\\' ), strrchr( texname, ':' ) };
		for( int i = 0; i < 3; i++ )
			if( seps[i] && seps[i] + 1 > start )
				start = seps[i] + 1;
		Q_snprintf( out, outSize, "#maps/%s", start );
	}
	else
	{
		Q_strncpy( out, texname, outSize );
	}
}

static const xrt_texentry_t *R_XRT_FindEntry( const char *texname )
{
	if( !s_numTexEntries || !texname )
		return NULL;

	char key[128];
	R_XRT_TextureKey( texname, key, sizeof( key ) );
	for( int i = 0; i < s_numTexEntries; i++ )
		if( !Q_stricmp( s_texEntries[i].key, key ) )
			return &s_texEntries[i];
	return NULL;
}

qboolean R_XRT_Active( void )
{
	return rt_xrt && rt_xrt->value > 0.0f && s_numTexEntries > 0;
}

qboolean R_XRT_GetMaterial( const char *texname, xrt_material_t *out )
{
	if( !R_XRT_Active() )
		return false;

	const xrt_texentry_t *e = R_XRT_FindEntry( texname );
	if( !e )
		return false;

	out->mirror    = e->mirror;
	out->glass     = e->glass;
	out->water     = e->water;
	out->opaque    = e->opaque;
	out->metallic  = e->metallic;
	out->roughness = e->roughness;
	out->emissive  = -1.0f;

	// emissiveMult is the visible glow. lightIntensity is how much light XashRT casts from
	// the surface; Remix lights the room from the same emission, so it only adds a little
	// (it must not dominate, or screens and fixtures wash out to white)
	float emis = -1.0f;
	if( e->emissiveMult >= 0.0f )
		emis = e->emissiveMult * ( rt_xrt_emis ? rt_xrt_emis->value : 100.0f );
	if( e->lightIntensity >= 0.0f )
		emis = Q_max( emis, 0.0f ) + e->lightIntensity * ( rt_xrt_light_emis ? rt_xrt_light_emis->value : 0.05f );
	out->emissive = emis;

	if( e->mirror )
	{
		out->metallic  = 1.0f;
		out->roughness = 0.0f;
	}
	return true;
}

static void R_XRT_LoadTexturesJson( void )
{
	size_t size = 0;
	byte *text = R_XRT_ReadFile( "rt/data/textures.json", &size );
	if( !text )
	{
		gEngfuncs.Con_Printf( "ref_openremix: XashRT content not found (rt/data/textures.json)\n" );
		return;
	}

	jval_t *root = J_ParseText( (const char *)text, size );
	free( text );

	jval_t *arr = J_Get( root, "array" );
	if( !arr || arr->type != J_ARR )
	{
		gEngfuncs.Con_Printf( S_WARN "ref_openremix: rt/data/textures.json could not be parsed\n" );
		J_Free( root );
		return;
	}

	s_texEntries = (xrt_texentry_t *)calloc( arr->count, sizeof( xrt_texentry_t ) );
	for( int i = 0; s_texEntries && i < arr->count; i++ )
	{
		const jval_t *o = arr->items[i];
		const char *name = J_Str( J_Get( o, "textureName" ) );
		if( !name )
			continue;

		xrt_texentry_t *e = &s_texEntries[s_numTexEntries++];
		Q_strncpy( e->key, name, sizeof( e->key ) );
		e->mirror         = J_Num( J_Get( o, "isMirror" ), 0 ) != 0;
		e->glass          = J_Num( J_Get( o, "isGlass" ), 0 ) != 0;
		e->water          = J_Num( J_Get( o, "isWater" ), 0 ) != 0;
		e->opaque         = J_Num( J_Get( o, "forceOpaque" ), 0 ) != 0;
		e->metallic       = (float)J_Num( J_Get( o, "metallicDefault" ), -1 );
		e->roughness      = (float)J_Num( J_Get( o, "roughnessDefault" ), -1 );
		e->emissiveMult   = (float)J_Num( J_Get( o, "emissiveMult" ), -1 );
		e->lightIntensity = (float)J_Num( J_Get( o, "lightIntensity" ), -1 );
	}

	J_Free( root );
	gEngfuncs.Con_Printf( "^2ref_openremix: Loaded %d XashRT texture overrides^7\n", s_numTexEntries );
}

/*
==============================================================================
KTX2 replacement textures
==============================================================================
*/
// Every file looked up, found or not: each texture probes its replacement and _n, _orm and _e
// maps, so a session's maps, models and HUD reach thousands (1024 filled up, and with it the
// water's normal map went missing)
#define MAX_XRT_TEXTURES 16384

typedef struct
{
	uint64_t               hash;
	remixapi_TextureHandle handle;
	qboolean               valid;
} xrt_ktx_t;

static xrt_ktx_t s_ktx[MAX_XRT_TEXTURES];
static int       s_numKtx = 0;
static int       s_ktxLoaded = 0, s_ktxFailed = 0;

static remixapi_Format R_XRT_KtxFormat( uint32_t vkFormat )
{
	switch( vkFormat )
	{
	case 37:  return REMIXAPI_FORMAT_R8G8B8A8_UNORM;
	case 43:  return REMIXAPI_FORMAT_R8G8B8A8_SRGB;
	case 44:  return REMIXAPI_FORMAT_B8G8R8A8_UNORM;
	case 50:  return REMIXAPI_FORMAT_B8G8R8A8_SRGB;
	case 131: return REMIXAPI_FORMAT_BC1_RGB_UNORM;
	case 132: return REMIXAPI_FORMAT_BC1_RGB_SRGB;
	case 137: return REMIXAPI_FORMAT_BC3_UNORM;   // VK_FORMAT_BC3_UNORM_BLOCK
	case 138: return REMIXAPI_FORMAT_BC3_SRGB;
	case 141: return REMIXAPI_FORMAT_BC5_UNORM;   // VK_FORMAT_BC5_UNORM_BLOCK
	case 145: return REMIXAPI_FORMAT_BC7_UNORM;
	case 146: return REMIXAPI_FORMAT_BC7_SRGB;
	default:  return (remixapi_Format)0;
	}
}

static uint32_t R_RD32( const byte *p ) { return (uint32_t)p[0] | ( (uint32_t)p[1] << 8 ) | ( (uint32_t)p[2] << 16 ) | ( (uint32_t)p[3] << 24 ); }
static uint64_t R_RD64( const byte *p ) { return (uint64_t)R_RD32( p ) | ( (uint64_t)R_RD32( p + 4 ) << 32 ); }

static qboolean R_XRT_CreateKtx( const char *path, uint64_t hash, remixapi_TextureHandle *outHandle )
{
	static const byte ident[12] = { 0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n' };

	size_t size = 0;
	byte *data = R_XRT_ReadFile( path, &size );
	if( !data )
		return false;

	qboolean ok = false;
	if( size >= 80 && !memcmp( data, ident, 12 ) && R_RD32( data + 44 ) == 0 /* no supercompression */ )
	{
		remixapi_Format format = R_XRT_KtxFormat( R_RD32( data + 12 ) );
		uint32_t width  = R_RD32( data + 20 );
		uint32_t height = R_RD32( data + 24 );
		uint32_t levels = Q_max( 1u, R_RD32( data + 40 ) );

		if( format && width && height && size >= 80 + (size_t)levels * 24 )
		{
			// Level index is largest-first; pack the chain contiguously, largest first
			uint64_t total = 0;
			qboolean inRange = true;
			for( uint32_t l = 0; l < levels; l++ )
			{
				uint64_t off = R_RD64( data + 80 + l * 24 ), len = R_RD64( data + 80 + l * 24 + 8 );
				if( off + len > size ) inRange = false;
				total += len;
			}

			byte *chain = inRange ? (byte *)malloc( (size_t)total ) : NULL;
			if( chain )
			{
				uint64_t at = 0;
				for( uint32_t l = 0; l < levels; l++ )
				{
					uint64_t off = R_RD64( data + 80 + l * 24 ), len = R_RD64( data + 80 + l * 24 + 8 );
					memcpy( chain + at, data + off, (size_t)len );
					at += len;
				}

				remixapi_TextureInfo info;
				memset( &info, 0, sizeof( info ) );
				info.sType     = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
				info.hash      = hash;
				info.width     = width;
				info.height    = height;
				info.depth     = 1;
				info.mipLevels = levels;
				info.format    = format;
				info.data      = chain;
				info.dataSize  = total;
				ok = g_remix.CreateTexture( &info, outHandle ) == REMIXAPI_ERROR_CODE_SUCCESS;

				if( !ok && levels > 1 )
				{
					// Fall back to the top level only
					info.mipLevels = 1;
					info.dataSize  = R_RD64( data + 80 + 8 );
					ok = g_remix.CreateTexture( &info, outHandle ) == REMIXAPI_ERROR_CODE_SUCCESS;
				}
				free( chain );
			}
		}
	}

	free( data );
	return ok;
}

qboolean R_XRT_GetReplacementTexture( const char *texname, const char *suffix, wchar_t *out, size_t outLen )
{
	if( !texname )
		return false;

	char key[128], path[256];
	R_XRT_TextureKey( texname, key, sizeof( key ) );

	// XashRT ships its own HD replacements for these models (rt/valve/models); their textures
	// only fit those meshes, not the stock models we draw
	static const char *replacedModels[] = { "#models/v_crowbar/" };
	for( size_t i = 0; i < ARRAYSIZE( replacedModels ); i++ )
		if( !Q_strnicmp( key, replacedModels[i], Q_strlen( replacedModels[i] ) ) )
			return false;

	Q_snprintf( path, sizeof( path ), "rt/mat/%s%s.ktx2", key, suffix ? suffix : "" );
	return R_XRT_GetFileTexture( path, out, outLen );
}

qboolean R_XRT_GetFileTexture( const char *path, wchar_t *out, size_t outLen )
{
	if( !rt_xrt || rt_xrt->value <= 0.0f || !path || !g_remix.CreateTexture )
		return false;

	uint64_t hash = R_XRT_Hash64( path );
	xrt_ktx_t *slot = NULL;
	for( int i = 0; i < s_numKtx; i++ )
	{
		if( s_ktx[i].hash == hash )
		{
			slot = &s_ktx[i];
			break;
		}
	}

	if( !slot )
	{
		if( s_numKtx >= MAX_XRT_TEXTURES )
		{
			static qboolean warned = false;
			if( !warned )
				gEngfuncs.Con_Printf( S_WARN "ref_openremix: XashRT texture table full (%d files), later ones are skipped\n", MAX_XRT_TEXTURES );
			warned = true;
			return false;
		}
		slot = &s_ktx[s_numKtx++];
		slot->hash  = hash;
		// Missing files stay cached as invalid; files that exist but fail are reported
		FILE *probe = R_XRT_Open( path );
		if( probe )
		{
			fclose( probe );
			slot->valid = R_XRT_CreateKtx( path, hash, &slot->handle );
			if( slot->valid )
				s_ktxLoaded++;
			else
			{
				s_ktxFailed++;
				gEngfuncs.Con_Printf( S_WARN "ref_openremix: could not load XashRT texture %s\n", path );
			}
		}
	}

	if( !slot->valid )
		return false;

	_snwprintf( out, outLen, L"0x%llx", (unsigned long long)hash );
	return true;
}

/*
==============================================================================
Per-map glTF scenes
==============================================================================
*/
#define MAX_XRT_INSTANCES 1024
#define MAX_XRT_LIGHTS    512
#define MAX_XRT_MATERIALS 512

typedef struct
{
	remixapi_MeshHandle mesh;
	remixapi_Transform  transform;
	uint64_t            id;
} xrt_instance_t;

static struct
{
	jval_t                 *gltf;
	byte                   *bin;
	size_t                  binSize;
	char                    dir[128];

	remixapi_MeshHandle    *meshes;     // per glTF mesh
	int                     numMeshes;
	remixapi_MaterialHandle materials[MAX_XRT_MATERIALS];
	remixapi_MaterialHandle mirror, glass;
	remixapi_TextureHandle  textures[MAX_XRT_MATERIALS];
	int                     numTextures;

	xrt_instance_t          instances[MAX_XRT_INSTANCES];
	int                     numInstances;
	remixapi_LightHandle    lights[MAX_XRT_LIGHTS];
	int                     numLights;
	// What a light needs to be made again when its light style changes
	remixapi_LightInfoSphereEXT lightSphere[MAX_XRT_LIGHTS];
	float                   lightRadiance[MAX_XRT_LIGHTS][3];
	uint64_t                lightHash[MAX_XRT_LIGHTS];
	int                     lightStyle[MAX_XRT_LIGHTS];
	float                   lightApplied[MAX_XRT_LIGHTS];
} s_scene;

static void R_XRT_FreeScene( void )
{
	if( s_scene.meshes && g_remix.DestroyMesh )
		for( int i = 0; i < s_scene.numMeshes; i++ )
			if( s_scene.meshes[i] ) g_remix.DestroyMesh( s_scene.meshes[i] );

	if( g_remix.DestroyMaterial )
	{
		for( int i = 0; i < MAX_XRT_MATERIALS; i++ )
			if( s_scene.materials[i] ) g_remix.DestroyMaterial( s_scene.materials[i] );
		if( s_scene.mirror ) g_remix.DestroyMaterial( s_scene.mirror );
	}

	if( g_remix.DestroyTexture )
		for( int i = 0; i < s_scene.numTextures; i++ )
			if( s_scene.textures[i] ) g_remix.DestroyTexture( s_scene.textures[i] );

	if( g_remix.DestroyLight )
		for( int i = 0; i < s_scene.numLights; i++ )
			if( s_scene.lights[i] ) g_remix.DestroyLight( s_scene.lights[i] );

	free( s_scene.meshes );
	free( s_scene.bin );
	J_Free( s_scene.gltf );
	memset( &s_scene, 0, sizeof( s_scene ) );
}

// Accessor data as floats (count * components), NULL if unsupported
static float *R_XRT_ReadFloats( int accessorIndex, int components, int *outCount )
{
	const jval_t *acc = J_At( J_Get( s_scene.gltf, "accessors" ), accessorIndex );
	const jval_t *view = J_At( J_Get( s_scene.gltf, "bufferViews" ), J_Int( J_Get( acc, "bufferView" ), -1 ) );
	if( !acc || !view || J_Int( J_Get( acc, "componentType" ), 0 ) != 5126 )
		return NULL;

	int count = J_Int( J_Get( acc, "count" ), 0 );
	size_t offset = (size_t)J_Int( J_Get( view, "byteOffset" ), 0 ) + (size_t)J_Int( J_Get( acc, "byteOffset" ), 0 );
	size_t stride = (size_t)J_Int( J_Get( view, "byteStride" ), components * 4 );
	if( count <= 0 || offset + stride * ( count - 1 ) + components * 4 > s_scene.binSize )
		return NULL;

	float *out = (float *)malloc( sizeof( float ) * components * count );
	for( int i = 0; out && i < count; i++ )
		memcpy( &out[i * components], s_scene.bin + offset + stride * i, components * 4 );
	*outCount = count;
	return out;
}

static uint32_t *R_XRT_ReadIndices( int accessorIndex, int *outCount )
{
	const jval_t *acc = J_At( J_Get( s_scene.gltf, "accessors" ), accessorIndex );
	const jval_t *view = J_At( J_Get( s_scene.gltf, "bufferViews" ), J_Int( J_Get( acc, "bufferView" ), -1 ) );
	if( !acc || !view )
		return NULL;

	int type = J_Int( J_Get( acc, "componentType" ), 0 );
	int size = ( type == 5121 ) ? 1 : ( type == 5123 ) ? 2 : ( type == 5125 ) ? 4 : 0;
	int count = J_Int( J_Get( acc, "count" ), 0 );
	size_t offset = (size_t)J_Int( J_Get( view, "byteOffset" ), 0 ) + (size_t)J_Int( J_Get( acc, "byteOffset" ), 0 );
	if( !size || count <= 0 || offset + (size_t)size * count > s_scene.binSize )
		return NULL;

	uint32_t *out = (uint32_t *)malloc( sizeof( uint32_t ) * count );
	const byte *p = s_scene.bin + offset;
	for( int i = 0; out && i < count; i++ )
		out[i] = ( size == 1 ) ? p[i] : ( size == 2 ) ? (uint32_t)( p[i * 2] | ( p[i * 2 + 1] << 8 ) ) : R_RD32( p + i * 4 );
	*outCount = count;
	return out;
}

// Image for a glTF texture index: a file next to the .gltf, or (mat_junction) an engine texture
static qboolean R_XRT_SceneImage( int textureIndex, wchar_t *out, size_t outLen )
{
	const jval_t *tex = J_At( J_Get( s_scene.gltf, "textures" ), textureIndex );
	const jval_t *img = J_At( J_Get( s_scene.gltf, "images" ), J_Int( J_Get( tex, "source" ), -1 ) );
	const char *uri = J_Str( J_Get( img, "uri" ) );
	if( !uri )
		return false;

	// URL-decode (%23 = '#')
	char decoded[256];
	size_t n = 0;
	for( const char *s = uri; *s && n + 1 < sizeof( decoded ); s++ )
	{
		if( s[0] == '%' && s[1] && s[2] )
		{
			char hex[3] = { s[1], s[2], 0 };
			decoded[n++] = (char)strtol( hex, NULL, 16 );
			s += 2;
		}
		else decoded[n++] = *s;
	}
	decoded[n] = 0;

	// "mat_junction/#maps/<name>.mip.tga" references a game texture
	const char *junction = strstr( decoded, "mat_junction/" );
	if( junction )
	{
		char key[128];
		Q_strncpy( key, junction + 13, sizeof( key ) );
		COM_StripExtension( key ); // drop ".tga"
		for( int i = 1; i < MAX_REMIX_TEXTURES; i++ )
		{
			char other[128];
			if( !g_remixTextures[i].registered || !g_remixTextures[i].texHandle )
				continue;
			R_XRT_TextureKey( g_remixTextures[i].name, other, sizeof( other ) );
			if( !Q_stricmp( other, key ) )
			{
				_snwprintf( out, outLen, L"0x%llx", (unsigned long long)g_remixTextures[i].texHash );
				return true;
			}
		}
		return false;
	}

	char path[384];
	Q_snprintf( path, sizeof( path ), "%s/%s", s_scene.dir, decoded );
	size_t size = 0;
	byte *file = R_XRT_ReadFile( path, &size );
	if( !file || !gEngfuncs.FS_LoadImage || s_scene.numTextures >= MAX_XRT_MATERIALS )
	{
		free( file );
		return false;
	}

	rgbdata_t *pic = gEngfuncs.FS_LoadImage( decoded, file, size );
	free( file );
	if( pic && pic->type != PF_RGBA_32 && gEngfuncs.Image_Process )
		gEngfuncs.Image_Process( &pic, 0, 0, IMAGE_FORCE_RGBA, 0.0f );

	qboolean ok = false;
	if( pic && pic->buffer )
	{
		uint64_t hash = R_XRT_Hash64( path );
		remixapi_TextureInfo info;
		memset( &info, 0, sizeof( info ) );
		info.sType     = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
		info.hash      = hash;
		info.width     = pic->width;
		info.height    = pic->height;
		info.depth     = 1;
		info.mipLevels = 1;
		info.format    = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
		info.data      = pic->buffer;
		info.dataSize  = (uint64_t)pic->width * pic->height * 4;
		if( g_remix.CreateTexture( &info, &s_scene.textures[s_scene.numTextures] ) == REMIXAPI_ERROR_CODE_SUCCESS )
		{
			s_scene.numTextures++;
			_snwprintf( out, outLen, L"0x%llx", (unsigned long long)hash );
			ok = true;
		}
	}
	if( pic ) gEngfuncs.FS_FreeImage( pic );
	return ok;
}

static remixapi_MaterialHandle R_XRT_MirrorMaterial( void );

static remixapi_MaterialHandle R_XRT_SceneMaterial( int index )
{
	if( index < 0 || index >= MAX_XRT_MATERIALS )
		return R_Textures_GetMaterial( 0 );
	if( s_scene.materials[index] )
		return s_scene.materials[index];

	const jval_t *m = J_At( J_Get( s_scene.gltf, "materials" ), index );

	// Some mirrors are marked only by their material ("mirr", "mirr_inner", "mirrorlocker")
	const char *matName = J_Str( J_Get( m, "name" ) );
	if( matName && !Q_strnicmp( matName, "mirr", 4 ) )
		return R_XRT_MirrorMaterial();
	const jval_t *pbr = J_Get( m, "pbrMetallicRoughness" );
	const jval_t *color = J_Get( pbr, "baseColorFactor" );
	const jval_t *emis = J_Get( m, "emissiveFactor" );

	wchar_t albedo[32] = { 0 };
	qboolean hasAlbedo = R_XRT_SceneImage( J_Int( J_Get( J_Get( pbr, "baseColorTexture" ), "index" ), -1 ), albedo, ARRAYSIZE( albedo ) );

	remixapi_MaterialInfoOpaqueEXT opaque;
	memset( &opaque, 0, sizeof( opaque ) );
	opaque.sType             = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
	opaque.albedoConstant.x  = (float)J_Num( J_At( color, 0 ), 1.0 );
	opaque.albedoConstant.y  = (float)J_Num( J_At( color, 1 ), 1.0 );
	opaque.albedoConstant.z  = (float)J_Num( J_At( color, 2 ), 1.0 );
	opaque.opacityConstant   = 1.0f;
	opaque.metallicConstant  = (float)J_Num( J_Get( pbr, "metallicFactor" ), 1.0 );  // glTF defaults
	opaque.roughnessConstant = (float)J_Num( J_Get( pbr, "roughnessFactor" ), 1.0 );
	opaque.alphaTestType     = 7; // ALWAYS

	remixapi_MaterialInfo info;
	memset( &info, 0, sizeof( info ) );
	info.sType         = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
	info.pNext         = &opaque;
	info.hash          = R_XRT_Hash64( s_scene.dir ) ^ ( 0x5CE7E00000000000ULL + (uint64_t)index );
	info.albedoTexture = hasAlbedo ? albedo : NULL;
	info.filterMode    = 1;
	info.wrapModeU     = 1;
	info.wrapModeV     = 1;

	float e[3] = { (float)J_Num( J_At( emis, 0 ), 0 ), (float)J_Num( J_At( emis, 1 ), 0 ), (float)J_Num( J_At( emis, 2 ), 0 ) };
	if( e[0] > 0.0f || e[1] > 0.0f || e[2] > 0.0f )
	{
		float strength = (float)J_Num( J_Get( J_Get( J_Get( m, "extensions" ), "KHR_materials_emissive_strength" ), "emissiveStrength" ), 1.0 );
		info.emissiveColorConstant.x = e[0];
		info.emissiveColorConstant.y = e[1];
		info.emissiveColorConstant.z = e[2];
		info.emissiveIntensity = strength * ( rt_xrt_emis ? rt_xrt_emis->value : 100.0f ) * 0.1f;
	}

	if( g_remix.CreateMaterial( &info, &s_scene.materials[index] ) != REMIXAPI_ERROR_CODE_SUCCESS )
		s_scene.materials[index] = NULL;
	return s_scene.materials[index] ? s_scene.materials[index] : R_Textures_GetMaterial( 0 );
}

static remixapi_MaterialHandle R_XRT_MirrorMaterial( void )
{
	if( s_scene.mirror )
		return s_scene.mirror;

	remixapi_MaterialInfoOpaqueEXT opaque;
	memset( &opaque, 0, sizeof( opaque ) );
	opaque.sType             = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
	// Full reflectance; smooth metal qualifies for primary surface replacement (PSR)
	opaque.albedoConstant.x  = opaque.albedoConstant.y = opaque.albedoConstant.z = 1.0f;
	opaque.opacityConstant   = 1.0f;
	opaque.metallicConstant  = 1.0f;
	opaque.roughnessConstant = 0.0f;
	opaque.alphaTestType     = 7;

	remixapi_MaterialInfo info;
	memset( &info, 0, sizeof( info ) );
	info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
	info.pNext = &opaque;
	info.hash  = 0x4D4952524F520001ULL; // "MIRROR"
	if( g_remix.CreateMaterial( &info, &s_scene.mirror ) != REMIXAPI_ERROR_CODE_SUCCESS )
		s_scene.mirror = NULL;
	return s_scene.mirror;
}

static remixapi_MeshHandle R_XRT_SceneMesh( int meshIndex, remixapi_MaterialHandle overrideMat )
{
	const jval_t *mesh = J_At( J_Get( s_scene.gltf, "meshes" ), meshIndex );
	const jval_t *prims = J_Get( mesh, "primitives" );
	if( !prims || prims->type != J_ARR || !prims->count )
		return NULL;

	remixapi_MeshInfoSurfaceTriangles *surfaces = (remixapi_MeshInfoSurfaceTriangles *)calloc( prims->count, sizeof( *surfaces ) );
	remixapi_HardcodedVertex **verts = (remixapi_HardcodedVertex **)calloc( prims->count, sizeof( *verts ) );
	uint32_t **indices = (uint32_t **)calloc( prims->count, sizeof( *indices ) );
	int numSurfaces = 0;

	for( int p = 0; surfaces && verts && indices && p < prims->count; p++ )
	{
		const jval_t *prim = prims->items[p];
		const jval_t *attrs = J_Get( prim, "attributes" );
		if( J_Int( J_Get( prim, "mode" ), 4 ) != 4 )
			continue;

		int nPos = 0, nNrm = 0, nUv = 0, nIdx = 0;
		float *pos = R_XRT_ReadFloats( J_Int( J_Get( attrs, "POSITION" ), -1 ), 3, &nPos );
		float *nrm = R_XRT_ReadFloats( J_Int( J_Get( attrs, "NORMAL" ), -1 ), 3, &nNrm );
		float *uv  = R_XRT_ReadFloats( J_Int( J_Get( attrs, "TEXCOORD_0" ), -1 ), 2, &nUv );
		uint32_t *idx = R_XRT_ReadIndices( J_Int( J_Get( prim, "indices" ), -1 ), &nIdx );

		if( pos && idx && nIdx >= 3 )
		{
			remixapi_HardcodedVertex *v = (remixapi_HardcodedVertex *)calloc( nPos, sizeof( *v ) );
			for( int i = 0; v && i < nPos; i++ )
			{
				VectorCopy( &pos[i * 3], v[i].position );
				if( nrm && i < nNrm ) VectorCopy( &nrm[i * 3], v[i].normal );
				else v[i].normal[2] = 1.0f;
				if( uv && i < nUv ) { v[i].texcoord[0] = uv[i * 2]; v[i].texcoord[1] = uv[i * 2 + 1]; }
				v[i].color = 0xFFFFFFFF;
			}

			if( v )
			{
				remixapi_MeshInfoSurfaceTriangles *st = &surfaces[numSurfaces];
				verts[numSurfaces]   = v;
				indices[numSurfaces] = idx;
				st->vertices_values  = v;
				st->vertices_count   = nPos;
				st->indices_values   = idx;
				st->indices_count    = nIdx - ( nIdx % 3 );
				st->material         = overrideMat ? overrideMat : R_XRT_SceneMaterial( J_Int( J_Get( prim, "material" ), -1 ) );
				numSurfaces++;
				idx = NULL; // owned by the surface now
			}
		}

		free( pos ); free( nrm ); free( uv ); free( idx );
	}

	remixapi_MeshHandle handle = NULL;
	if( numSurfaces )
	{
		char hashName[192];
		Q_snprintf( hashName, sizeof( hashName ), "%s#mesh%d%s", s_scene.dir, meshIndex, overrideMat ? "#ovr" : "" );

		remixapi_MeshInfo info;
		memset( &info, 0, sizeof( info ) );
		info.sType           = REMIXAPI_STRUCT_TYPE_MESH_INFO;
		info.hash            = R_XRT_Hash64( hashName );
		info.surfaces_values = surfaces;
		info.surfaces_count  = numSurfaces;
		if( g_remix.CreateMesh( &info, &handle ) != REMIXAPI_ERROR_CODE_SUCCESS )
			handle = NULL;
	}

	for( int i = 0; i < numSurfaces; i++ )
	{
		free( verts[i] );
		free( indices[i] );
	}
	free( surfaces ); free( verts ); free( indices );
	return handle;
}

// Column-major 4x4 from glTF TRS
static void R_XRT_NodeMatrix( const jval_t *node, float m[16] )
{
	const jval_t *mat = J_Get( node, "matrix" );
	if( mat && mat->type == J_ARR && mat->count == 16 )
	{
		for( int i = 0; i < 16; i++ ) m[i] = (float)J_Num( mat->items[i], 0 );
		return;
	}

	const jval_t *t = J_Get( node, "translation" ), *r = J_Get( node, "rotation" ), *s = J_Get( node, "scale" );
	float tx = (float)J_Num( J_At( t, 0 ), 0 ), ty = (float)J_Num( J_At( t, 1 ), 0 ), tz = (float)J_Num( J_At( t, 2 ), 0 );
	float qx = (float)J_Num( J_At( r, 0 ), 0 ), qy = (float)J_Num( J_At( r, 1 ), 0 ), qz = (float)J_Num( J_At( r, 2 ), 0 ), qw = (float)J_Num( J_At( r, 3 ), 1 );
	float sx = (float)J_Num( J_At( s, 0 ), 1 ), sy = (float)J_Num( J_At( s, 1 ), 1 ), sz = (float)J_Num( J_At( s, 2 ), 1 );

	m[0]  = ( 1 - 2 * ( qy * qy + qz * qz ) ) * sx;
	m[1]  = ( 2 * ( qx * qy + qz * qw ) ) * sx;
	m[2]  = ( 2 * ( qx * qz - qy * qw ) ) * sx;
	m[3]  = 0;
	m[4]  = ( 2 * ( qx * qy - qz * qw ) ) * sy;
	m[5]  = ( 1 - 2 * ( qx * qx + qz * qz ) ) * sy;
	m[6]  = ( 2 * ( qy * qz + qx * qw ) ) * sy;
	m[7]  = 0;
	m[8]  = ( 2 * ( qx * qz + qy * qw ) ) * sz;
	m[9]  = ( 2 * ( qy * qz - qx * qw ) ) * sz;
	m[10] = ( 1 - 2 * ( qx * qx + qy * qy ) ) * sz;
	m[11] = 0;
	m[12] = tx; m[13] = ty; m[14] = tz; m[15] = 1;
}

static void R_XRT_MulMatrix( const float a[16], const float b[16], float out[16] )
{
	float r[16];
	for( int c = 0; c < 4; c++ )
		for( int row = 0; row < 4; row++ )
			r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] + a[1 * 4 + row] * b[c * 4 + 1] +
			                 a[2 * 4 + row] * b[c * 4 + 2] + a[3 * 4 + row] * b[c * 4 + 3];
	memcpy( out, r, sizeof( r ) );
}

static void R_XRT_AddLight( const jval_t *light, const float m[16], uint64_t hash )
{
	if( s_scene.numLights >= MAX_XRT_LIGHTS || !g_remix.CreateLight )
		return;

	const char *type = J_Str( J_Get( light, "type" ) );
	if( !type || !Q_strcmp( type, "directional" ) )
		return; // the map's own light_environment provides the sun

	const jval_t *color = J_Get( light, "color" );
	// Blender exports W * 683 / (4 pi) candela; back to watts, then into map-light radiance units
	float watts = (float)J_Num( J_Get( light, "intensity" ), 1.0 ) / 54.35f;
	float scale = watts * ( rt_xrt_scene_lights ? rt_xrt_scene_lights->value : 8.0f );

	remixapi_LightInfoSphereEXT sphere;
	memset( &sphere, 0, sizeof( sphere ) );
	sphere.sType                   = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
	sphere.position.x              = m[12];
	sphere.position.y              = m[13];
	sphere.position.z              = m[14];
	sphere.radius                  = 2.0f;
	// XashRT marks the lights meant to light its fog (isVolumetric): RayTracedGL1 scatters only
	// the brightest of them in full. Here every marked light scatters rt_xrt_volumetric times more,
	// the others rt_light_volumetric times
	cvar_t *plainScale = gEngfuncs.pfnGetCvarPointer( "rt_light_volumetric" );
	if( J_Num( J_Get( J_Get( light, "extras" ), "isVolumetric" ), 0 ) != 0 && rt_xrt_volumetric )
		sphere.volumetricRadianceScale = Q_max( rt_xrt_volumetric->value, 0.0f );
	else
		sphere.volumetricRadianceScale = plainScale ? Q_max( plainScale->value, 0.0f ) : 1.0f;

	if( !Q_strcmp( type, "spot" ) )
	{
		const jval_t *spot = J_Get( light, "spot" );
		float outer = (float)J_Num( J_Get( spot, "outerConeAngle" ), M_PI_F / 4.0f );
		float inner = (float)J_Num( J_Get( spot, "innerConeAngle" ), 0.0f );

		// glTF lights shine down their local -Z axis
		vec3_t dir = { -m[8], -m[9], -m[10] };
		VectorNormalize( dir );

		sphere.shaping_hasvalue               = 1;
		sphere.shaping_value.direction.x      = dir[0];
		sphere.shaping_value.direction.y      = dir[1];
		sphere.shaping_value.direction.z      = dir[2];
		sphere.shaping_value.coneAngleDegrees = RAD2DEG( outer );
		sphere.shaping_value.coneSoftness     = ( outer > 0.0f ) ? bound( 0.0f, ( outer - inner ) / outer, 1.0f ) : 0.0f;
		sphere.shaping_value.focusExponent    = 1.0f;
	}

	const int slot = s_scene.numLights;
	s_scene.lightSphere[slot] = sphere;

	remixapi_LightInfo info;
	memset( &info, 0, sizeof( info ) );
	info.sType      = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
	info.pNext      = &s_scene.lightSphere[slot];
	info.hash       = hash;
	info.radiance.x = (float)J_Num( J_At( color, 0 ), 1.0 ) * scale;
	info.radiance.y = (float)J_Num( J_At( color, 1 ), 1.0 ) * scale;
	info.radiance.z = (float)J_Num( J_At( color, 2 ), 1.0 ) * scale;

	if( g_remix.CreateLight( &info, &s_scene.lights[slot] ) == REMIXAPI_ERROR_CODE_SUCCESS )
	{
		s_scene.lightRadiance[slot][0] = info.radiance.x;
		s_scene.lightRadiance[slot][1] = info.radiance.y;
		s_scene.lightRadiance[slot][2] = info.radiance.z;
		s_scene.lightHash[slot]        = hash;
		s_scene.lightStyle[slot]       = J_Int( J_Get( J_Get( light, "extras" ), "lightstyle" ), 0 );
		s_scene.lightApplied[slot]     = 1.0f;
		s_scene.numLights++;
	}
}

static void R_XRT_VisitNode( int nodeIndex, const float parent[16], int depth )
{
	const jval_t *node = J_At( J_Get( s_scene.gltf, "nodes" ), nodeIndex );
	if( !node || depth > 32 )
		return;

	const char *name = J_Str( J_Get( node, "name" ) );
	const jval_t *extras = J_Get( node, "extras" );

	// Geometry exported from the engine (world, brush entities) is already drawn; sky-visibility
	// helpers only steer RayTracedGL1's sky
	if( name && ( !Q_strncmp( name, "maps/", 5 ) || name[0] == '*' ) )
		return;
	if( J_Num( J_Get( extras, "isSkyVisibility" ), 0 ) != 0 )
		return;

	float local[16], world[16];
	// The root converts GoldSrc units/Z-up into meters/Y-up: its children are already in map space
	if( name && !Q_strncmp( name, "rtgl1_main_root", 15 ) )
	{
		memset( world, 0, sizeof( world ) );
		world[0] = world[5] = world[10] = world[15] = 1.0f;
	}
	else
	{
		R_XRT_NodeMatrix( node, local );
		R_XRT_MulMatrix( parent, local, world );
	}

	int meshIndex = J_Int( J_Get( node, "mesh" ), -1 );
	if( meshIndex >= 0 && meshIndex < s_scene.numMeshes && s_scene.numInstances < MAX_XRT_INSTANCES )
	{
		qboolean mirror = J_Num( J_Get( extras, "isMirror" ), 0 ) != 0;
		qboolean glass  = J_Num( J_Get( extras, "isGlass" ), 0 ) != 0 || J_Num( J_Get( extras, "isWater" ), 0 ) != 0;
		remixapi_MaterialHandle overrideMat = mirror ? R_XRT_MirrorMaterial() : glass ? R_Textures_GetGlassMaterial() : NULL;

		remixapi_MeshHandle mesh;
		if( overrideMat )
			mesh = R_XRT_SceneMesh( meshIndex, overrideMat ); // own copy with the special material
		else
		{
			if( !s_scene.meshes[meshIndex] )
				s_scene.meshes[meshIndex] = R_XRT_SceneMesh( meshIndex, NULL );
			mesh = s_scene.meshes[meshIndex];
		}

		if( mesh )
		{
			xrt_instance_t *inst = &s_scene.instances[s_scene.numInstances++];
			inst->mesh = mesh;
			inst->id   = R_XRT_Hash64( s_scene.dir ) ^ ( 0x1257A0CE00000000ULL + (uint64_t)nodeIndex );
			for( int r = 0; r < 3; r++ )
				for( int c = 0; c < 4; c++ )
					inst->transform.matrix[r][c] = world[c * 4 + r];
		}
	}

	const jval_t *ext = J_Get( J_Get( node, "extensions" ), "KHR_lights_punctual" );
	if( ext )
	{
		const jval_t *lights = J_Get( J_Get( J_Get( s_scene.gltf, "extensions" ), "KHR_lights_punctual" ), "lights" );
		const jval_t *light = J_At( lights, J_Int( J_Get( ext, "light" ), -1 ) );
		if( light )
			R_XRT_AddLight( light, world, R_XRT_Hash64( s_scene.dir ) ^ ( 0x11647000000000ULL + (uint64_t)nodeIndex ) );
	}

	const jval_t *children = J_Get( node, "children" );
	for( int i = 0; children && i < children->count; i++ )
		R_XRT_VisitNode( J_Int( children->items[i], -1 ), world, depth + 1 );
}

void R_XRT_NewMap( const model_t *worldmodel )
{
	// Meshes created with "own copy" overrides are not tracked per glTF mesh: free them via
	// their instances before tearing down the rest
	if( g_remix.DestroyMesh )
	{
		for( int i = 0; i < s_scene.numInstances; i++ )
		{
			qboolean shared = false;
			for( int m = 0; m < s_scene.numMeshes && !shared; m++ )
				shared = ( s_scene.meshes && s_scene.meshes[m] == s_scene.instances[i].mesh );
			if( !shared && s_scene.instances[i].mesh )
				g_remix.DestroyMesh( s_scene.instances[i].mesh );
		}
	}
	R_XRT_FreeScene();

	if( !rt_xrt || rt_xrt->value <= 0.0f || !worldmodel || !g_remix.CreateMesh )
		return;

	char mapname[64];
	COM_FileBase( worldmodel->name, mapname, sizeof( mapname ) );
	Q_snprintf( s_scene.dir, sizeof( s_scene.dir ), "rt/scenes/%s", mapname );

	char path[256];
	Q_snprintf( path, sizeof( path ), "%s/%s.gltf", s_scene.dir, mapname );
	size_t size = 0;
	byte *text = R_XRT_ReadFile( path, &size );
	if( !text )
		return;

	s_scene.gltf = J_ParseText( (const char *)text, size );
	free( text );

	const char *binUri = J_Str( J_Get( J_At( J_Get( s_scene.gltf, "buffers" ), 0 ), "uri" ) );
	if( binUri )
	{
		Q_snprintf( path, sizeof( path ), "%s/%s", s_scene.dir, binUri );
		s_scene.bin = R_XRT_ReadFile( path, &s_scene.binSize );
	}

	const jval_t *meshes = J_Get( s_scene.gltf, "meshes" );
	if( !s_scene.gltf || !s_scene.bin || !meshes )
	{
		gEngfuncs.Con_Printf( S_WARN "ref_openremix: failed to load XashRT scene %s\n", s_scene.dir );
		R_XRT_FreeScene();
		return;
	}

	s_scene.numMeshes = meshes->count;
	s_scene.meshes = (remixapi_MeshHandle *)calloc( meshes->count, sizeof( remixapi_MeshHandle ) );

	float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	const jval_t *scene = J_At( J_Get( s_scene.gltf, "scenes" ), J_Int( J_Get( s_scene.gltf, "scene" ), 0 ) );
	const jval_t *roots = J_Get( scene, "nodes" );
	for( int i = 0; roots && i < roots->count; i++ )
		R_XRT_VisitNode( J_Int( roots->items[i], -1 ), identity, 0 );

	// Geometry is in Remix now; drop the source data
	free( s_scene.bin );
	s_scene.bin = NULL;

	gEngfuncs.Con_Printf( "^2ref_openremix: XashRT scene %s: %d objects, %d lights (replacement textures: %d loaded, %d failed)^7\n",
	                      mapname, s_scene.numInstances, s_scene.numLights, s_ktxLoaded, s_ktxFailed );
}

void R_XRT_DrawScene( void )
{
	if( !g_remix.DrawInstance || !rt_xrt || rt_xrt->value <= 0.0f )
		return;

	for( int i = 0; i < s_scene.numInstances; i++ )
	{
		const xrt_instance_t *src = &s_scene.instances[i];

		remixapi_InstanceIdentityEXT ident;
		memset( &ident, 0, sizeof( ident ) );
		ident.sType          = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
		ident.instanceId     = src->id;
		ident.classification = REMIXAPI_INSTANCE_STATIC_EXT;
		ident.rasterVisible  = 1;

		remixapi_InstanceInfo inst;
		memset( &inst, 0, sizeof( inst ) );
		inst.sType       = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
		inst.pNext       = &ident;
		inst.mesh        = src->mesh;
		inst.transform   = src->transform;
		inst.doubleSided = 1;
		g_remix.DrawInstance( &inst );
	}

	if( g_remix.DrawLightInstance )
	{
		for( int i = 0; i < s_scene.numLights; i++ )
		{
			// A light that follows a light style (a switch, a flicker) is made again at its brightness
			if( s_scene.lightStyle[i] > 0 && g_remix.CreateLight )
			{
				const float styleScale = R_Lights_StyleScale( s_scene.lightStyle[i] );
				if( styleScale != s_scene.lightApplied[i] )
				{
					remixapi_LightInfo info;
					memset( &info, 0, sizeof( info ) );
					info.sType      = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
					info.pNext      = &s_scene.lightSphere[i];
					info.hash       = s_scene.lightHash[i];
					info.radiance.x = s_scene.lightRadiance[i][0] * styleScale;
					info.radiance.y = s_scene.lightRadiance[i][1] * styleScale;
					info.radiance.z = s_scene.lightRadiance[i][2] * styleScale;

					remixapi_LightHandle handle = NULL;
					if( g_remix.CreateLight( &info, &handle ) == REMIXAPI_ERROR_CODE_SUCCESS )
					{
						if( s_scene.lights[i] && g_remix.DestroyLight )
							g_remix.DestroyLight( s_scene.lights[i] );
						s_scene.lights[i] = handle;
					}
					s_scene.lightApplied[i] = styleScale;
				}
				if( styleScale <= 0.0f )
					continue; // switched off
			}
			g_remix.DrawLightInstance( s_scene.lights[i] );
		}
	}
}

void R_XRT_Init( void )
{
	rt_xrt              = gEngfuncs.Cvar_Get( "rt_xrt", "1", FCVAR_ARCHIVE, "Use XashRT content from the rt/ folder (materials, textures, map additions); applies on map load" );
	rt_xrt_emis         = gEngfuncs.Cvar_Get( "rt_xrt_emis", "100", FCVAR_ARCHIVE, "Emission strength per XashRT emissiveMult" );
	rt_xrt_light_emis   = gEngfuncs.Cvar_Get( "rt_xrt_light_glow", "0.05", FCVAR_ARCHIVE, "Extra emission per XashRT texture lightIntensity" );
	rt_xrt_scene_lights = gEngfuncs.Cvar_Get( "rt_xrt_scene_lights", "8", FCVAR_ARCHIVE, "Brightness of XashRT hand-placed map lights (per watt)" );
	rt_xrt_volumetric   = gEngfuncs.Cvar_Get( "rt_xrt_volumetric", "80", FCVAR_ARCHIVE, "How much more XashRT's fog lights (isVolumetric) scatter in volumetrics; applies on map load" );

	if( !s_texEntries )
		R_XRT_LoadTexturesJson();
}

void R_XRT_Shutdown( void )
{
	R_XRT_NewMap( NULL ); // frees the scene

	if( g_remix.DestroyTexture )
		for( int i = 0; i < s_numKtx; i++ )
			if( s_ktx[i].valid && s_ktx[i].handle ) g_remix.DestroyTexture( s_ktx[i].handle );
	s_numKtx = 0;

	free( s_texEntries );
	s_texEntries = NULL;
	s_numTexEntries = 0;
}
