/*
 * r_autopbr.c - PBR values and normal maps inferred for textures without authored ones
 *
 * Metallic and roughness come from the texture's material type in sound/materials.txt (the
 * footstep table: Valve's own word on what each texture is), else from keywords in its name.
 * Normal maps take the image's brightness as height. Only textures with nothing authored get
 * them: XashRT's materials and maps come first, and USD mods replace the material whole (its
 * hash, which this never changes).
 */

#include "r_local.h"
#include "r_autopbr.h"
#include "r_remix.h"

#define MAX_MATERIAL_TYPES 2048
#define TYPE_NAME_LEN      13 // CBTEXTURENAMEMAX: names compare on their first 12 characters

static cvar_t *rt_autopbr         = NULL;
static cvar_t *rt_autopbr_normals = NULL;

static struct
{
	char type;
	char name[TYPE_NAME_LEN];
} s_types[MAX_MATERIAL_TYPES];
static int s_numTypes = 0;

typedef struct
{
	float       metallic, roughness;
	qboolean    bumps;
	const char *keywords[32];
} autopbr_category_t;

// Metals stay partly dielectric: Half-Life's metal textures have their shading painted in, so
// as pure metal they would turn dark. First match wins. Keywords: "abc" anywhere in the name,
// "^abc" at the start of one of its words (letter runs), "=abc" a whole word
static const autopbr_category_t s_categories[] =
{
	// Lettering, pictures and screens: no relief from their image
	{ 0.0f, 0.75f, false, { "sign", "poster", "picture", "paper", "=names", "=map", "clock", "=sgn", NULL } },
	{ 0.2f, 0.35f, false, { "^comp", "^cmp", "scrn", "screen", "flickermon", "=mon", NULL } },
	{ 0.2f, 0.75f, true,  { "rust", NULL } },
	{ 0.0f, 0.25f, true,  { "=wet", NULL } },
	{ 0.6f, 0.4f,  true,  { "metal", "mtl", "steel", "chrome", "pipe", "duct", "^vnt", "vent", "grate", "^rail", "ladder",
	                        "^elev", "gear", "^tnk", "tank", "barrel", "freezer", "^reac", "ngin", "engine", "flatbed",
	                        "^gun", "=clip", "scope", NULL } },
	{ 0.0f, 0.3f,  true,  { "tile", "ceramic", "porcelain", NULL } },
	{ 0.0f, 0.7f,  true,  { "wood", "crate", "plank", "=wd", NULL } },
	{ 0.0f, 0.5f,  true,  { "face", "=head", "hand", "=arm", "skin", "=leg", "chest", "torso", "flesh", NULL } },
	{ 0.0f, 0.35f, true,  { "plastic", NULL } },
	{ 0.0f, 0.9f,  true,  { "rock", "=rk", "dirt", "^grnd", "ground", "^grvl", "gravel", "sand", "=mud", NULL } },
	{ 0.0f, 0.85f, true,  { "crete", "brick", "plaster", NULL } },
	{ 0.0f, 0.9f,  true,  { "cloth", "fabric", "carpet", "=rug", "curtain", NULL } },
};

void R_AutoPBR_Init( void )
{
	rt_autopbr         = gEngfuncs.Cvar_Get( "rt_autopbr", "0", FCVAR_ARCHIVE, "Experimental: infer PBR materials (metal, roughness, normal maps) for textures without XashRT or mod ones; applies on map load" );
	rt_autopbr_normals = gEngfuncs.Cvar_Get( "rt_autopbr_normals", "1", FCVAR_ARCHIVE, "Strength of normal maps generated from textures (0 = none); applies on map load" );
}

qboolean R_AutoPBR_Enabled( void )
{
	return rt_autopbr && rt_autopbr->value > 0.0f;
}

void R_AutoPBR_ClearTypes( void )
{
	s_numTypes = 0;
}

// GoldSrc's texture name for its material type: no "-0"/"+0" animation prefix, no '{', '!' or '~'
static const char *R_AutoPBR_TypeName( const char *name )
{
	if( name[0] == '-' || name[0] == '+' )
		name += name[1] ? 2 : 1;
	if( name[0] == '{' || name[0] == '!' || name[0] == '~' || name[0] == ' ' )
		name++;
	return name;
}

void R_AutoPBR_AddType( char type, const char *name )
{
	if( s_numTypes >= MAX_MATERIAL_TYPES || !name || !*name )
		return;
	s_types[s_numTypes].type = (char)Q_toupper( type );
	Q_strncpy( s_types[s_numTypes].name, R_AutoPBR_TypeName( name ), sizeof( s_types[s_numTypes].name ));
	s_numTypes++;
}

static char R_AutoPBR_FindType( const char *baseName )
{
	char name[TYPE_NAME_LEN];
	Q_strncpy( name, R_AutoPBR_TypeName( baseName ), sizeof( name ));
	for( int i = 0; i < s_numTypes; i++ )
	{
		if( !Q_stricmp( s_types[i].name, name ))
			return s_types[i].type;
	}
	return 0;
}

static qboolean R_AutoPBR_MatchesKeyword( const char *lower, const char *keyword )
{
	if( keyword[0] != '^' && keyword[0] != '=' )
		return Q_strstr( lower, keyword ) != NULL;

	const qboolean whole = keyword[0] == '=';
	const size_t len = Q_strlen( keyword + 1 );
	for( const char *p = lower; *p; )
	{
		while( *p && !( *p >= 'a' && *p <= 'z' ))
			p++;
		const char *word = p;
		while( *p >= 'a' && *p <= 'z' )
			p++;
		const size_t wordLen = p - word;
		if( wordLen >= len && !Q_strncmp( word, keyword + 1, len ) && ( !whole || wordLen == len ))
			return true;
	}
	return false;
}

qboolean R_AutoPBR_Classify( const char *baseName, qboolean studio, float *metallic, float *roughness, qboolean *bumps )
{
	*bumps = true;
	if( !baseName || !*baseName )
		return false;

	// The footstep table names world textures only
	const char type = studio ? 0 : R_AutoPBR_FindType( baseName );
	switch( type )
	{
	case 'M': *metallic = 0.6f; *roughness = 0.4f;  return true; // metal
	case 'V': *metallic = 0.6f; *roughness = 0.45f; return true; // vent
	case 'G': *metallic = 0.6f; *roughness = 0.5f;  return true; // grate
	case 'T': *metallic = 0.0f; *roughness = 0.3f;  return true; // tile
	case 'W': *metallic = 0.0f; *roughness = 0.7f;  return true; // wood
	case 'C': *metallic = 0.0f; *roughness = 0.85f; return true; // concrete
	case 'D': *metallic = 0.0f; *roughness = 0.95f; return true; // dirt
	case 'S': *metallic = 0.0f; *roughness = 0.3f;  return true; // slosh: wet
	case 'F': *metallic = 0.0f; *roughness = 0.5f;  return true; // flesh
	case 'P': *metallic = 0.2f; *roughness = 0.35f; *bumps = false; return true; // computer
	case 'Y': return false; // glass has its own material
	}

	char lower[64];
	Q_strncpy( lower, baseName, sizeof( lower ));
	for( char *p = lower; *p; p++ )
		*p = (char)Q_tolower( *p );

	for( size_t c = 0; c < ARRAYSIZE( s_categories ); c++ )
	{
		for( const char *const *k = s_categories[c].keywords; *k; k++ )
		{
			if( R_AutoPBR_MatchesKeyword( lower, *k ))
			{
				*metallic  = s_categories[c].metallic;
				*roughness = s_categories[c].roughness;
				*bumps     = s_categories[c].bumps;
				return true;
			}
		}
	}
	return false;
}

qboolean R_AutoPBR_CreateNormalMap( const byte *rgba, uint32_t width, uint32_t height, uint64_t hash,
                                    remixapi_TextureHandle *handle, wchar_t *path, size_t pathLen )
{
	const float strength = rt_autopbr_normals ? rt_autopbr_normals->value : 1.0f;
	if( strength <= 0.0f || !rgba || width < 4 || height < 4 || !g_remix.CreateTexture )
		return false;

	const size_t numPixels = (size_t)width * height;
	byte *height8 = (byte *)Mem_Malloc( r_temppool, numPixels );
	byte *normals = (byte *)Mem_Malloc( r_temppool, numPixels * 4 );
	if( !height8 || !normals )
	{
		if( height8 ) Mem_Free( height8 );
		if( normals ) Mem_Free( normals );
		return false;
	}

	for( size_t i = 0; i < numPixels; i++ )
		height8[i] = (byte)(( rgba[i * 4 + 0] * 77 + rgba[i * 4 + 1] * 150 + rgba[i * 4 + 2] * 29 ) >> 8 );

	// Sobel gradients, wrapping as the texture tiles. Rows run down the image, the OpenGL
	// convention's tangent-space y up it: a rise down the rows tilts the normal toward +y
	const float scale = bound( 0.0f, strength, 5.0f ) * 2.5f / 1020.0f;
	for( uint32_t y = 0; y < height; y++ )
	{
		const uint32_t up = ( y + height - 1 ) % height * width, row = y * width, down = ( y + 1 ) % height * width;
		for( uint32_t x = 0; x < width; x++ )
		{
			const uint32_t left = ( x + width - 1 ) % width, right = ( x + 1 ) % width;
			const int dx = ( height8[up + right] + 2 * height8[row + right] + height8[down + right] ) -
			               ( height8[up + left] + 2 * height8[row + left] + height8[down + left] );
			const int dy = ( height8[down + left] + 2 * height8[down + x] + height8[down + right] ) -
			               ( height8[up + left] + 2 * height8[up + x] + height8[up + right] );
			float n[3] = { -dx * scale, dy * scale, 1.0f };
			const float inv = 1.0f / sqrtf( n[0] * n[0] + n[1] * n[1] + 1.0f );
			byte *out = &normals[( row + x ) * 4];
			for( int c = 0; c < 3; c++ )
				out[c] = (byte)( n[c] * inv * 127.5f + 127.5f );
			out[3] = 255;
		}
	}

	remixapi_TextureInfoGenerateMipsEXT mipsExt;
	memset( &mipsExt, 0, sizeof( mipsExt ));
	mipsExt.sType    = REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT;
	mipsExt.generate = 1;

	remixapi_TextureInfo info;
	memset( &info, 0, sizeof( info ));
	info.sType     = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
	info.pNext     = &mipsExt;
	info.hash      = hash;
	info.width     = width;
	info.height    = height;
	info.depth     = 1;
	info.mipLevels = 1;
	info.format    = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
	info.data      = normals;
	info.dataSize  = (uint64_t)numPixels * 4;

	const qboolean ok = g_remix.CreateTexture( &info, handle ) == REMIXAPI_ERROR_CODE_SUCCESS;
	Mem_Free( height8 );
	Mem_Free( normals );
	if( !ok )
	{
		*handle = NULL;
		return false;
	}
	_snwprintf( path, pathLen, L"0x%llx", (unsigned long long)hash );
	return true;
}
