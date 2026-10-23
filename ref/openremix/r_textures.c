/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_textures.c - Master texture and PBR material pipeline implementation
 */

#include "r_local.h"
#include "r_remix.h"
#include "r_textures.h"
#include "r_xashrt.h"
#include "r_autopbr.h"
#include "r_ui.h"

remix_texture_t g_remixTextures[MAX_REMIX_TEXTURES];
static int      s_nextTexNum = 1;
static remixapi_MaterialHandle s_defaultMaterial = NULL;
static remixapi_TextureHandle  s_defaultTexture  = NULL;

#define MAX_GLASS_TEXTURES 256
static char s_glassTextures[MAX_GLASS_TEXTURES][32];
static int  s_numGlassTextures = 0;

static cvar_t *rt_emis         = NULL;
static cvar_t *rt_glass        = NULL;
static cvar_t *rt_fx_emis      = NULL;
static cvar_t *rt_glass_texture = NULL;
static cvar_t *rt_chrome        = NULL;
static cvar_t *rt_liquid_emis   = NULL;
static cvar_t *rt_texture_nearest = NULL;

// Set while R_Textures_CheckSettings remakes the material of this texture slot (0: none)
static int s_rebuildTexnum = 0;

static void R_Textures_FreePixels( remix_texture_t *entry )
{
	if( entry->pixels )
		Mem_Free( entry->pixels );
	if( entry->pixelsName )
		Mem_Free( entry->pixelsName );
	entry->pixels     = NULL;
	entry->pixelsName = NULL;
}

// Game textures' sampling: linear, or nearest (rt_texture_nearest) for the original's crisp texels
static int R_Textures_FilterMode( void )
{
	return rt_texture_nearest && rt_texture_nearest->value > 0.0f ? 0 : 1;
}
static cvar_t *rt_chrome_roughness = NULL;
static cvar_t *rt_chrome_reflectance = NULL;
static cvar_t *rt_water         = NULL;
static cvar_t *rt_water_dist    = NULL;
static cvar_t *rt_water_ior     = NULL;

// Absorption of the last water body created (the underwater camera medium)
static qboolean s_hasWaterMedium = false;
static float    s_waterColor[3]  = { 0.6f, 0.8f, 0.85f };
static float    s_waterDist      = 300.0f;
static cvar_t  *rt_water_fog     = NULL;

// The fog parameters of the image being uploaded (rgbdata_t fogParams: GoldSrc water keeps
// its fog colour and density in the palette), NULL when it has none
static const byte *s_uploadFog = NULL;

static void R_Textures_DestroyFx( remix_texture_t *entry );
static cvar_t *rt_refr_glass   = NULL;
static cvar_t *rt_glass_dist   = NULL;
static cvar_t *rt_glass_thin   = NULL;
static cvar_t *rt_glass_tint_r = NULL;
static cvar_t *rt_glass_tint_g = NULL;
static cvar_t *rt_glass_tint_b = NULL;

static void R_Textures_GetBaseName( const char *src, char *dst, size_t dstSize );

// 64-bit FNV-1a Hash
static inline uint64_t R_HashString64( const char *str )
{
	uint64_t hash = 14695981039346656037ULL;
	if( !str ) return hash;
	while( *str )
	{
		hash ^= (uint64_t)(unsigned char)(*str++);
		hash *= 1099511628211ULL;
	}
	return hash ? hash : 1ULL;
}

/*
 * Create fallback 16x16 white texture and diffuse material
 */
static void R_Textures_CreateDefaultMaterial( void )
{
	if( s_defaultMaterial || !g_remix.CreateTexture || !g_remix.CreateMaterial )
		return;

	const uint32_t w = 16, h = 16;
	uint32_t whitePixels[16 * 16];
	for( int i = 0; i < 16 * 16; i++ )
		whitePixels[i] = 0xFFFFFFFF; // Opaque white

	uint64_t texHash = R_HashString64( "__openremix_default_white__" );
	remixapi_TextureInfoGenerateMipsEXT mipsExt;
	memset( &mipsExt, 0, sizeof( mipsExt ) );
	mipsExt.sType    = REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT;
	mipsExt.generate = 1;

	remixapi_TextureInfo texInfo;
	memset( &texInfo, 0, sizeof( texInfo ) );
	texInfo.sType     = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
	texInfo.pNext     = &mipsExt;
	texInfo.hash      = texHash;
	texInfo.width     = w;
	texInfo.height    = h;
	texInfo.depth     = 1;
	texInfo.mipLevels = 1;
	texInfo.format    = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
	texInfo.data      = whitePixels;
	texInfo.dataSize  = sizeof( whitePixels );

	remixapi_ErrorCode tErr = g_remix.CreateTexture( &texInfo, &s_defaultTexture );
	if( tErr != REMIXAPI_ERROR_CODE_SUCCESS )
	{
		gEngfuncs.Con_Reportf( S_ERROR "ref_openremix: Default CreateTexture failed: %u\n", (unsigned)tErr );
	}

	wchar_t hashStr[32];
	_snwprintf( hashStr, sizeof( hashStr ) / sizeof( wchar_t ), L"0x%llx", (unsigned long long)texHash );

	remixapi_MaterialInfoOpaqueEXT opaqueExt;
	memset( &opaqueExt, 0, sizeof( opaqueExt ) );
	opaqueExt.sType             = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
	opaqueExt.roughnessConstant = 0.85f;
	opaqueExt.metallicConstant  = 0.0f;
	opaqueExt.opacityConstant   = 1.0f;
	opaqueExt.useDrawCallAlphaState = 1;
	opaqueExt.alphaTestType     = 7; // ALWAYS (0 = NEVER would discard every texel)
	opaqueExt.albedoConstant.x  = 1.0f;
	opaqueExt.albedoConstant.y  = 1.0f;
	opaqueExt.albedoConstant.z  = 1.0f;

	uint64_t matHash = texHash ^ 0x9E3779B97F4A7C15ULL;
	remixapi_MaterialInfo matInfo;
	memset( &matInfo, 0, sizeof( matInfo ) );
	matInfo.sType         = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
	matInfo.pNext         = &opaqueExt;
	matInfo.hash          = matHash;
	matInfo.albedoTexture = hashStr;
	matInfo.filterMode    = 1;
	matInfo.wrapModeU     = 1;
	matInfo.wrapModeV     = 1;

	remixapi_ErrorCode mErr = g_remix.CreateMaterial( &matInfo, &s_defaultMaterial );
	if( mErr != REMIXAPI_ERROR_CODE_SUCCESS )
	{
		gEngfuncs.Con_Reportf( S_ERROR "ref_openremix: Default CreateMaterial failed: %u\n", (unsigned)mErr );
	}
}

static remixapi_MaterialHandle s_glassMaterial = NULL;

/*
 * R_Textures_GlassTextured - Glass shows its texture (opaque-PBR coverage blend) instead of
 * being clear refractive transmission
 */
/*
 * R_Textures_GetWaterMedium - Absorption of the map's water, for the underwater camera
 */
qboolean R_Textures_GetWaterMedium( float color[3], float *distance )
{
	if( !s_hasWaterMedium )
		return false;
	VectorCopy( s_waterColor, color );
	*distance = s_waterDist;
	return true;
}

qboolean R_Textures_GlassTextured( void )
{
	return rt_glass_texture && rt_glass_texture->value > 0.0f;
}

float R_Textures_GlassOpacity( void )
{
	return R_Textures_GlassTextured() ? bound( 0.01f, rt_glass_texture->value, 1.0f ) : 1.0f;
}

remixapi_MaterialHandle R_Textures_GetGlassMaterial( void )
{
	if( s_glassMaterial )
		return s_glassMaterial;

	if( !g_remix.CreateMaterial )
		return NULL;

	remixapi_MaterialInfoTranslucentEXT transExt;
	memset( &transExt, 0, sizeof( transExt ) );
	transExt.sType                   = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
	transExt.pNext                   = NULL;
	transExt.transmittanceTexture    = NULL;
	transExt.refractiveIndex         = ( rt_refr_glass ? rt_refr_glass->value : 1.52f );
	if( transExt.refractiveIndex < 1.0f ) transExt.refractiveIndex = 1.52f;

	float tintR = ( rt_glass_tint_r ? rt_glass_tint_r->value : 0.98f );
	float tintG = ( rt_glass_tint_g ? rt_glass_tint_g->value : 0.99f );
	float tintB = ( rt_glass_tint_b ? rt_glass_tint_b->value : 1.0f );
	transExt.transmittanceColor.x    = tintR;
	transExt.transmittanceColor.y    = tintG;
	transExt.transmittanceColor.z    = tintB;

	float dist = ( rt_glass_dist ? rt_glass_dist->value : 2000.0f );
	if( dist <= 0.0f ) dist = 2000.0f;
	transExt.transmittanceMeasurementDistance = dist;

	if( rt_glass_thin && rt_glass_thin->value > 0.0f )
	{
		transExt.thinWallThickness_hasvalue = 1;
		transExt.thinWallThickness_value    = 0.005f;
	}
	else
	{
		transExt.thinWallThickness_hasvalue = 0;
	}
	transExt.useDiffuseLayer         = 0;

	uint64_t matHash = R_HashString64( "__openremix_shared_glass_material__" );
	remixapi_MaterialInfo matInfo;
	memset( &matInfo, 0, sizeof( matInfo ) );
	matInfo.sType         = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
	matInfo.pNext         = &transExt;
	matInfo.hash          = matHash;
	matInfo.albedoTexture = NULL;
	matInfo.filterMode    = 1;
	matInfo.wrapModeU     = 1;
	matInfo.wrapModeV     = 1;

	remixapi_ErrorCode err = g_remix.CreateMaterial( &matInfo, &s_glassMaterial );
	if( err != REMIXAPI_ERROR_CODE_SUCCESS )
	{
		gEngfuncs.Con_Reportf( S_ERROR "ref_openremix: CreateMaterial for shared glass failed: %u\n", (unsigned)err );
		s_glassMaterial = NULL;
	}
	else
	{
		gEngfuncs.Con_Printf( "^2ref_openremix: Created shared ray-traced glass material (handle %p)^7\n", (void *)s_glassMaterial );
	}

	return s_glassMaterial;
}

static void R_Textures_LoadMaterialsTxt( void )
{
	s_numGlassTextures = 0;
	R_AutoPBR_ClearTypes();

	// Populate built-in standard Half-Life glass textures first
	static const char *s_builtinGlass[] = {
		"GLASS_BRIGHT",
		"GLASS_DARK",
		"GLASS_MED",
		"GLASSBLUE1",
		"GLASSBLUE2",
		"GLASSGREEEN",
		"GLASSGREEN",
		"C1A1_WINDOW",
		"LAB1_GLSS1",
		"0GLASSBRICK",
		"1GLASSBRICK",
		"2GLASSBRICK",
		"3GLASSBRICK",
		"4GLASSBRICK"
	};
	for( size_t i = 0; i < sizeof( s_builtinGlass ) / sizeof( s_builtinGlass[0] ); i++ )
	{
		if( s_numGlassTextures < MAX_GLASS_TEXTURES )
		{
			Q_strncpy( s_glassTextures[s_numGlassTextures++], s_builtinGlass[i], 32 );
		}
	}

	// Try loading sound/materials.txt through engine filesystem or direct file
	fs_offset_t fileSize = 0;
	byte *fileBuf = NULL;

	if( gEngfuncs.fsapi && gEngfuncs.fsapi->LoadFileMalloc )
	{
		fileBuf = gEngfuncs.fsapi->LoadFileMalloc( "sound/materials.txt", &fileSize, false );
	}

	if( !fileBuf || fileSize <= 0 )
	{
		FILE *f = fopen( "valve/sound/materials.txt", "rb" );
		if( !f ) f = fopen( "sound/materials.txt", "rb" );
		if( f )
		{
			fseek( f, 0, SEEK_END );
			long sz = ftell( f );
			fseek( f, 0, SEEK_SET );
			if( sz > 0 )
			{
				fileBuf = (byte *)malloc( sz + 1 );
				if( fileBuf )
				{
					fread( fileBuf, 1, sz, f );
					fileBuf[sz] = '\0';
					fileSize = sz;
				}
			}
			fclose( f );
		}
	}

	if( fileBuf && fileSize > 0 )
	{
		const char *pos = (const char *)fileBuf;
		const char *end = pos + fileSize;

		while( pos < end )
		{
			// Skip whitespace
			while( pos < end && ( *pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n' ) )
				pos++;
			if( pos >= end ) break;

			// Skip comments
			if( pos + 1 < end && pos[0] == '/' && pos[1] == '/' )
			{
				while( pos < end && *pos != '\n' ) pos++;
				continue;
			}

			char matType = *pos++;
			while( pos < end && ( *pos == ' ' || *pos == '\t' ) ) pos++;

			char texName[64];
			int nameLen = 0;
			while( pos < end && *pos != ' ' && *pos != '\t' && *pos != '\r' && *pos != '\n' && nameLen < (int)sizeof( texName ) - 1 )
			{
				texName[nameLen++] = *pos++;
			}
			texName[nameLen] = '\0';
			R_AutoPBR_AddType( matType, texName );

			if( ( matType == 'Y' || matType == 'y' ) && nameLen > 0 )
			{
				// Check if already in list
				qboolean exists = false;
				for( int k = 0; k < s_numGlassTextures; k++ )
				{
					if( !Q_stricmp( s_glassTextures[k], texName ) )
					{
						exists = true;
						break;
					}
				}
				if( !exists && s_numGlassTextures < MAX_GLASS_TEXTURES )
				{
					Q_strncpy( s_glassTextures[s_numGlassTextures++], texName, 32 );
				}
			}

			// Advance to end of line
			while( pos < end && *pos != '\n' ) pos++;
		}

		free( fileBuf );
	}

	gEngfuncs.Con_Printf( "^2ref_openremix: Loaded %d glass textures from materials.txt & database^7\n",
	                      s_numGlassTextures );
}

qboolean R_Textures_IsGlass( const char *name )
{
	if( !name || !*name )
		return false;

	if( rt_glass && rt_glass->value <= 0.0f )
		return false;

	char baseName[64];
	R_Textures_GetBaseName( name, baseName, sizeof( baseName ) );

	// Strip leading '{' if any (e.g. {glass)
	const char *cleanName = baseName;
	if( cleanName[0] == '{' )
		cleanName++;

	// 1. Check sound/materials.txt glass texture list ('Y')
	for( int i = 0; i < s_numGlassTextures; i++ )
	{
		if( !Q_strnicmp( cleanName, s_glassTextures[i], 12 ) )
			return true;
	}

	// 2. Case-insensitive heuristic check for glass / window keywords
	char lower[64];
	Q_strncpy( lower, cleanName, sizeof( lower ) );
	for( char *p = lower; *p; p++ )
	{
		if( *p >= 'A' && *p <= 'Z' )
			*p += ( 'a' - 'A' );
	}

	if( strstr( lower, "glass" ) || strstr( lower, "window" ) ||
	    strstr( lower, "_gls" ) || strstr( lower, "glss" ) )
	{
		return true;
	}

	return false;
}

void R_Textures_Init( void )
{
	memset( g_remixTextures, 0, sizeof( g_remixTextures ) );
	s_nextTexNum = 1;
	s_defaultMaterial = NULL;
	s_defaultTexture  = NULL;

	if( gEngfuncs.Cvar_Get )
	{
		rt_emis         = gEngfuncs.Cvar_Get( "rt_emis", "1.0", FCVAR_ARCHIVE, "Texture light emission multiplier" );
		rt_glass        = gEngfuncs.Cvar_Get( "rt_glass", "1", FCVAR_ARCHIVE, "Enable OpenRemix ray-traced glass translucency" );
		rt_water        = gEngfuncs.Cvar_Get( "rt_water", "1", FCVAR_ARCHIVE, "Ray-traced water bodies (refraction, absorption, Snell's window); applies on map load" );
		rt_water_dist   = gEngfuncs.Cvar_Get( "rt_water_dist", "300", FCVAR_ARCHIVE, "Water clarity: distance in units over which the water colour is absorbed once" );
		rt_water_ior    = gEngfuncs.Cvar_Get( "rt_water_ior", "1.333", FCVAR_ARCHIVE, "Water index of refraction" );
		rt_glass_texture = gEngfuncs.Cvar_Get( "rt_glass_texture", "0.9", FCVAR_ARCHIVE, "Glass texture opacity: 0 = clear refractive glass, >0 = textured glass with reflections (applies on map load)" );
		rt_water_fog    = gEngfuncs.Cvar_Get( "rt_water_fog", "1", FCVAR_ARCHIVE, "Underwater fog density, relative to Half-Life's (0 = clear water); applies on map load" );
		rt_liquid_emis  = gEngfuncs.Cvar_Get( "rt_liquid_emis", "1", FCVAR_ARCHIVE, "Glow of glowing liquids such as toxic waste; applies on map load" );
		rt_chrome       = gEngfuncs.Cvar_Get( "rt_chrome", "1", FCVAR_ARCHIVE, "Model chrome as path-traced metal (0 = Half-Life's fake environment map); applies on model load" );
		rt_chrome_roughness = gEngfuncs.Cvar_Get( "rt_chrome_roughness", "0.3", FCVAR_ARCHIVE, "Roughness of path-traced model chrome; applies on model load" );
		rt_chrome_reflectance = gEngfuncs.Cvar_Get( "rt_chrome_reflectance", "0.6", FCVAR_ARCHIVE, "Reflectance of path-traced model chrome (steel ~0.6, polished silver ~0.95); applies on model load" );
		rt_fx_emis      = gEngfuncs.Cvar_Get( "rt_fx_emis", "4.0", FCVAR_ARCHIVE, "Emission intensity of path-traced additive effects (beams, glow sprites); applies on texture load" );
		rt_refr_glass   = gEngfuncs.Cvar_Get( "rt_refr_glass", "1.52", FCVAR_ARCHIVE, "Glass index of refraction (matching xash-rt)" );
		rt_glass_dist   = gEngfuncs.Cvar_Get( "rt_glass_dist", "2000.0", FCVAR_ARCHIVE, "Glass transmittance measurement distance" );
		rt_glass_thin   = gEngfuncs.Cvar_Get( "rt_glass_thin", "0", FCVAR_ARCHIVE, "Force thin-wall glass sheet mode without refraction" );
		rt_glass_tint_r = gEngfuncs.Cvar_Get( "rt_glass_tint_r", "0.98", FCVAR_ARCHIVE, "Glass transmittance color Red" );
		rt_glass_tint_g = gEngfuncs.Cvar_Get( "rt_glass_tint_g", "0.99", FCVAR_ARCHIVE, "Glass transmittance color Green" );
		rt_glass_tint_b = gEngfuncs.Cvar_Get( "rt_glass_tint_b", "1.00", FCVAR_ARCHIVE, "Glass transmittance color Blue" );
		rt_texture_nearest = gEngfuncs.Cvar_Get( "rt_texture_nearest", "0", FCVAR_ARCHIVE, "Nearest-neighbour texture filtering (crisp texels, as Half-Life without filtering)" );
		R_AutoPBR_Init();
	}

	R_Textures_LoadMaterialsTxt();
	R_XRT_Init();
	R_Textures_CreateDefaultMaterial();
	R_Textures_GetGlassMaterial();
}


void R_Textures_ClearMap( void )
{
	for( int i = 1; i < MAX_REMIX_TEXTURES; i++ )
	{
		remix_texture_t *entry = &g_remixTextures[i];
		if( !entry->registered )
			continue;

		if( entry->matHandle && g_remix.DestroyMaterial )
		{
			g_remix.DestroyMaterial( entry->matHandle );
			entry->matHandle = NULL;
		}

		if( entry->texHandle && g_remix.DestroyTexture )
		{
			g_remix.DestroyTexture( entry->texHandle );
			entry->texHandle = NULL;
		}

		R_Textures_DestroyFx( entry );
		R_Textures_FreePixels( entry );
		entry->registered = false;
	}

	s_nextTexNum = 1;
	tr.particleTexture = 0;
	R_Textures_CreateDefaultMaterial();
	R_Textures_GetGlassMaterial();
}

void R_Textures_Shutdown( void )
{
	R_XRT_Shutdown();
	R_Textures_ClearMap();

	if( s_defaultMaterial && g_remix.DestroyMaterial )
	{
		g_remix.DestroyMaterial( s_defaultMaterial );
		s_defaultMaterial = NULL;
	}
	if( s_glassMaterial && g_remix.DestroyMaterial )
	{
		g_remix.DestroyMaterial( s_glassMaterial );
		s_glassMaterial = NULL;
	}
	if( s_defaultTexture && g_remix.DestroyTexture )
	{
		g_remix.DestroyTexture( s_defaultTexture );
		s_defaultTexture = NULL;
	}
}

static void R_Textures_GetBaseName( const char *src, char *dst, size_t dstSize )
{
	if( !src || !*src )
	{
		dst[0] = '\0';
		return;
	}

	const char *colon = strrchr( src, ':' );
	const char *slash = strrchr( src, '/' );
	const char *backslash = strrchr( src, '\\' );

	const char *start = src;
	if( colon && colon + 1 > start ) start = colon + 1;
	if( slash && slash + 1 > start ) start = slash + 1;
	if( backslash && backslash + 1 > start ) start = backslash + 1;

	Q_strncpy( dst, start, dstSize );

	char *dot = strrchr( dst, '.' );
	if( dot )
		*dot = '\0';
}

/*
 * R_Textures_FindExact - Full-name lookup only. Used for upload de-duplication, where the
 * base-name fallback would alias unrelated textures (e.g. every "#HUD/x(one:00).spr" frame)
 */
static int R_Textures_FindExact( const char *name )
{
	if( !name || !*name )
		return 0;

	for( int i = 1; i < MAX_REMIX_TEXTURES; i++ )
	{
		if( g_remixTextures[i].registered && !Q_stricmp( g_remixTextures[i].name, name ) )
			return i;
	}

	return 0;
}

int R_Textures_Find( const char *name )
{
	if( !name || !*name )
		return 0;

	// 1. Exact match
	int exact = R_Textures_FindExact( name );
	if( exact )
		return exact;

	// Engine-generated names (sprite frames, "#maps/..." miptex, "*particle") must match exactly
	if( name[0] == '#' || name[0] == '*' )
		return 0;

	// 2. Base name match (e.g. matching "c2a4e_w1b" against "#maps/c1a0.bsp:c2a4e_w1b.mip")
	char baseSearch[64];
	R_Textures_GetBaseName( name, baseSearch, sizeof( baseSearch ) );

	if( baseSearch[0] != '\0' )
	{
		for( int i = 1; i < MAX_REMIX_TEXTURES; i++ )
		{
			if( !g_remixTextures[i].registered )
				continue;

			char baseReg[64];
			R_Textures_GetBaseName( g_remixTextures[i].name, baseReg, sizeof( baseReg ) );

			if( !Q_stricmp( baseReg, baseSearch ) )
				return i;
		}
	}

	return 0;
}

const char *R_Textures_GetName( unsigned int texnum )
{
	if( texnum > 0 && texnum < MAX_REMIX_TEXTURES && g_remixTextures[texnum].registered )
		return g_remixTextures[texnum].name;
	return NULL;
}

/*
 * R_Textures_DestroyFx - Release a texture's additive FX variant
 */
static void R_Textures_DestroyFx( remix_texture_t *entry )
{
	if( entry->fxMatHandle && g_remix.DestroyMaterial )
		g_remix.DestroyMaterial( entry->fxMatHandle );
	if( entry->fxBrightMatHandle && g_remix.DestroyMaterial )
		g_remix.DestroyMaterial( entry->fxBrightMatHandle );
	entry->fxBrightMatHandle = NULL;
	if( entry->fxTexHandle && g_remix.DestroyTexture )
		g_remix.DestroyTexture( entry->fxTexHandle );
	if( entry->emisMatHandle && g_remix.DestroyMaterial )
		g_remix.DestroyMaterial( entry->emisMatHandle );
	if( entry->lumaTexHandle && g_remix.DestroyTexture )
		g_remix.DestroyTexture( entry->lumaTexHandle );
	entry->lumaTexHandle = NULL;
	if( entry->normalTexHandle && g_remix.DestroyTexture )
		g_remix.DestroyTexture( entry->normalTexHandle );
	entry->normalTexHandle = NULL;
	entry->fxMatHandle   = NULL;
	entry->fxTexHandle   = NULL;
	entry->emisMatHandle = NULL;
}

/*
 * R_Textures_CreateFx - Build the path-traced stand-in for an additive sprite frame.
 * The path tracer only blends by coverage, so alpha becomes the texel's brightness
 * (black = fully transparent) and RGB is normalized; coverage blending then gives
 * c + dst * (1 - max(c)), close to additive. The material is self-illuminated so the
 * effect glows with its texture * vertex colour and lights the scene.
 */
static void R_Textures_CreateFx( remix_texture_t *entry, const char *name, uint32_t width, uint32_t height, const byte *rgbaData )
{
	if( !g_remix.CreateTexture || !g_remix.CreateMaterial )
		return;

	uint32_t numPixels = width * height;
	byte *fx = (byte *)Mem_Malloc( r_temppool, numPixels * 4 );
	if( !fx )
		return;

	for( uint32_t i = 0; i < numPixels; i++ )
	{
		const byte *src = &rgbaData[i * 4];
		byte m = Q_max( src[0], Q_max( src[1], src[2] ) );
		// Respect texels the sprite itself marks transparent
		m = (byte)( ( (uint32_t)m * src[3] ) / 255 );

		if( m == 0 )
		{
			fx[i * 4 + 0] = fx[i * 4 + 1] = fx[i * 4 + 2] = fx[i * 4 + 3] = 0;
			continue;
		}

		for( int c = 0; c < 3; c++ )
			fx[i * 4 + c] = (byte)Q_min( 255, ( (uint32_t)src[c] * 255 ) / Q_max( 1, Q_max( src[0], Q_max( src[1], src[2] ) ) ) );
		fx[i * 4 + 3] = m;
	}

	uint64_t fxHash = entry->texHash ^ 0xF00DFACE0BEA0000ULL;

	remixapi_TextureInfoGenerateMipsEXT mipsExt;
	memset( &mipsExt, 0, sizeof( mipsExt ) );
	mipsExt.sType    = REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT;
	mipsExt.generate = 1;

	remixapi_TextureInfo texInfo;
	memset( &texInfo, 0, sizeof( texInfo ) );
	texInfo.sType     = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
	texInfo.pNext     = &mipsExt;
	texInfo.hash      = fxHash;
	texInfo.width     = width;
	texInfo.height    = height;
	texInfo.depth     = 1;
	texInfo.mipLevels = 1;
	texInfo.format    = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
	texInfo.data      = fx;
	texInfo.dataSize  = (uint64_t)numPixels * 4;

	remixapi_ErrorCode err = g_remix.CreateTexture( &texInfo, &entry->fxTexHandle );
	Mem_Free( fx );
	if( err != REMIXAPI_ERROR_CODE_SUCCESS )
	{
		gEngfuncs.Con_Reportf( S_WARN "ref_openremix: CreateTexture (fx) failed for %s (code %u)\n", name, (unsigned)err );
		entry->fxTexHandle = NULL;
		return;
	}

	wchar_t hashStr[32];
	_snwprintf( hashStr, sizeof( hashStr ) / sizeof( wchar_t ), L"0x%llx", (unsigned long long)fxHash );

	remixapi_MaterialInfoSelfIlluminationEXT selfExt;
	memset( &selfExt, 0, sizeof( selfExt ) );
	selfExt.sType     = REMIXAPI_STRUCT_TYPE_MATERIAL_SELF_ILLUMINATION_EXT;
	selfExt.mask      = REMIXAPI_SELF_ILLUMINATION_MASK_NONE_EXT;
	selfExt.maskScale = 1.0f;

	remixapi_MaterialInfoOpaqueEXT opaqueExt;
	memset( &opaqueExt, 0, sizeof( opaqueExt ) );
	opaqueExt.sType                 = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
	opaqueExt.pNext                 = &selfExt;
	opaqueExt.albedoConstant.x      = 1.0f;
	opaqueExt.albedoConstant.y      = 1.0f;
	opaqueExt.albedoConstant.z      = 1.0f;
	opaqueExt.opacityConstant       = 1.0f;
	opaqueExt.useDrawCallAlphaState = 1;
	opaqueExt.alphaTestType         = 7; // ALWAYS

	float intensity = rt_fx_emis ? rt_fx_emis->value : 4.0f;
	if( intensity <= 0.0f )
		intensity = 0.001f;

	remixapi_MaterialInfo matInfo;
	memset( &matInfo, 0, sizeof( matInfo ) );
	matInfo.sType                   = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
	matInfo.pNext                   = &opaqueExt;
	matInfo.hash                    = fxHash ^ 0x9E3779B97F4A7C15ULL;
	matInfo.albedoTexture           = hashStr;
	matInfo.emissiveIntensity       = intensity;
	matInfo.emissiveColorConstant.x = 1.0f;
	matInfo.emissiveColorConstant.y = 1.0f;
	matInfo.emissiveColorConstant.z = 1.0f;
	matInfo.filterMode              = R_Textures_FilterMode();
	matInfo.wrapModeU               = 1;
	matInfo.wrapModeV               = 1;

	err = g_remix.CreateMaterial( &matInfo, &entry->fxMatHandle );
	if( err != REMIXAPI_ERROR_CODE_SUCCESS )
	{
		gEngfuncs.Con_Reportf( S_WARN "ref_openremix: CreateMaterial (fx) failed for %s (code %u)\n", name, (unsigned)err );
		entry->fxMatHandle = NULL;
	}
}

/*
 * R_Textures_CreateSkyMaterial - A skybox face's material: the sky layer shows texture * albedo
 * constant, so the constant is the sky's brightness against the lit world. Skybox faces are
 * clamped at the edges so they do not bleed into each other; scrolling cloud layers repeat.
 * The caller destroys it.
 */
remixapi_MaterialHandle R_Textures_CreateSkyMaterial( unsigned int texnum, float brightness, qboolean repeat )
{
	if( texnum == 0 || texnum >= MAX_REMIX_TEXTURES || !g_remixTextures[texnum].registered ||
	    !g_remixTextures[texnum].texHandle || !g_remix.CreateMaterial )
		return NULL;

	const remix_texture_t *entry = &g_remixTextures[texnum];
	wchar_t hashStr[32];
	_snwprintf( hashStr, sizeof( hashStr ) / sizeof( wchar_t ), L"0x%llx", (unsigned long long)entry->texHash );

	remixapi_MaterialInfoOpaqueEXT opaqueExt;
	memset( &opaqueExt, 0, sizeof( opaqueExt ) );
	opaqueExt.sType            = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
	opaqueExt.albedoConstant.x = brightness;
	opaqueExt.albedoConstant.y = brightness;
	opaqueExt.albedoConstant.z = brightness;
	opaqueExt.opacityConstant  = 1.0f;
	opaqueExt.alphaTestType    = 7; // ALWAYS

	remixapi_MaterialInfo matInfo;
	memset( &matInfo, 0, sizeof( matInfo ) );
	matInfo.sType         = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
	matInfo.pNext         = &opaqueExt;
	matInfo.hash          = entry->texHash ^ 0x5C1B0F0E5C1B0F0EULL ^ (uint64_t)( brightness * 1000.0f ) ^ ( repeat ? 0x8000000000000000ULL : 0 );
	matInfo.albedoTexture = hashStr;
	matInfo.filterMode    = 1; // Linear
	matInfo.wrapModeU     = repeat ? 1 : 3; // Wrap or clamp
	matInfo.wrapModeV     = repeat ? 1 : 3;

	remixapi_MaterialHandle handle = NULL;
	if( g_remix.CreateMaterial( &matInfo, &handle ) != REMIXAPI_ERROR_CODE_SUCCESS )
		return NULL;
	return handle;
}

/*
 * R_Textures_GetAverageColor - The texture's mean linear colour
 */
qboolean R_Textures_GetAverageColor( unsigned int texnum, float color[3] )
{
	if( texnum == 0 || texnum >= MAX_REMIX_TEXTURES || !g_remixTextures[texnum].registered )
		return false;
	VectorCopy( g_remixTextures[texnum].avgColor, color );
	return true;
}

/*
 * R_Textures_GetFxMaterialScaled - The additive variant glowing `scale` times as bright (sparks and
 * tracers outshine glows and beams), made on first use; the plain variant at scale 1
 */
remixapi_MaterialHandle R_Textures_GetFxMaterialScaled( unsigned int texnum, float scale )
{
	if( texnum == 0 || texnum >= MAX_REMIX_TEXTURES || !g_remixTextures[texnum].registered )
		return NULL;
	remix_texture_t *entry = &g_remixTextures[texnum];
	if( scale == 1.0f || !entry->fxTexHandle || !g_remix.CreateMaterial )
		return entry->fxMatHandle;
	if( entry->fxBrightMatHandle && entry->fxBrightScale == scale )
		return entry->fxBrightMatHandle;

	if( entry->fxBrightMatHandle && g_remix.DestroyMaterial )
		g_remix.DestroyMaterial( entry->fxBrightMatHandle );
	entry->fxBrightMatHandle = NULL;

	// The same texture as the plain variant (R_Textures_CreateFx)
	const uint64_t fxHash = entry->texHash ^ 0xF00DFACE0BEA0000ULL;
	wchar_t hashStr[32];
	_snwprintf( hashStr, sizeof( hashStr ) / sizeof( wchar_t ), L"0x%llx", (unsigned long long)fxHash );

	remixapi_MaterialInfoSelfIlluminationEXT selfExt;
	memset( &selfExt, 0, sizeof( selfExt ) );
	selfExt.sType     = REMIXAPI_STRUCT_TYPE_MATERIAL_SELF_ILLUMINATION_EXT;
	selfExt.mask      = REMIXAPI_SELF_ILLUMINATION_MASK_NONE_EXT;
	selfExt.maskScale = 1.0f;

	remixapi_MaterialInfoOpaqueEXT opaqueExt;
	memset( &opaqueExt, 0, sizeof( opaqueExt ) );
	opaqueExt.sType                 = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
	opaqueExt.pNext                 = &selfExt;
	opaqueExt.albedoConstant.x      = 1.0f;
	opaqueExt.albedoConstant.y      = 1.0f;
	opaqueExt.albedoConstant.z      = 1.0f;
	opaqueExt.opacityConstant       = 1.0f;
	opaqueExt.useDrawCallAlphaState = 1;
	opaqueExt.alphaTestType         = 7; // ALWAYS

	const float base = rt_fx_emis && rt_fx_emis->value > 0.0f ? rt_fx_emis->value : 0.001f;

	remixapi_MaterialInfo matInfo;
	memset( &matInfo, 0, sizeof( matInfo ) );
	matInfo.sType                   = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
	matInfo.pNext                   = &opaqueExt;
	matInfo.hash                    = fxHash ^ 0x9E3779B97F4A7C15ULL ^ 0x0B51A4E5CA1ED000ULL;
	matInfo.albedoTexture           = hashStr;
	matInfo.emissiveIntensity       = base * scale;
	matInfo.emissiveColorConstant.x = 1.0f;
	matInfo.emissiveColorConstant.y = 1.0f;
	matInfo.emissiveColorConstant.z = 1.0f;
	matInfo.filterMode              = R_Textures_FilterMode();
	matInfo.wrapModeU               = 1;
	matInfo.wrapModeV               = 1;

	if( g_remix.CreateMaterial( &matInfo, &entry->fxBrightMatHandle ) != REMIXAPI_ERROR_CODE_SUCCESS )
		entry->fxBrightMatHandle = NULL;
	entry->fxBrightScale = scale;
	return entry->fxBrightMatHandle ? entry->fxBrightMatHandle : entry->fxMatHandle;
}

/*
 * R_Textures_GetFxMaterial - Self-illuminated additive variant, or NULL if the texture has none
 */
remixapi_MaterialHandle R_Textures_GetFxMaterial( unsigned int texnum )
{
	if( texnum > 0 && texnum < MAX_REMIX_TEXTURES && g_remixTextures[texnum].registered )
		return g_remixTextures[texnum].fxMatHandle;
	return NULL;
}

/*
 * R_Textures_GetEmissiveMaterial - Self-illuminated material sampling the texture as uploaded.
 * Used for additive brush entities: coverage comes from the draw's texture factor (renderamt)
 */
remixapi_MaterialHandle R_Textures_GetEmissiveMaterial( unsigned int texnum )
{
	if( texnum == 0 || texnum >= MAX_REMIX_TEXTURES || !g_remixTextures[texnum].registered )
		return NULL;

	remix_texture_t *entry = &g_remixTextures[texnum];
	if( entry->emisMatHandle || !entry->texHandle || !g_remix.CreateMaterial )
		return entry->emisMatHandle;

	wchar_t hashStr[32];
	_snwprintf( hashStr, sizeof( hashStr ) / sizeof( wchar_t ), L"0x%llx", (unsigned long long)entry->texHash );

	remixapi_MaterialInfoSelfIlluminationEXT selfExt;
	memset( &selfExt, 0, sizeof( selfExt ) );
	selfExt.sType     = REMIXAPI_STRUCT_TYPE_MATERIAL_SELF_ILLUMINATION_EXT;
	selfExt.mask      = REMIXAPI_SELF_ILLUMINATION_MASK_NONE_EXT;
	selfExt.maskScale = 1.0f;

	remixapi_MaterialInfoOpaqueEXT opaqueExt;
	memset( &opaqueExt, 0, sizeof( opaqueExt ) );
	opaqueExt.sType                 = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
	opaqueExt.pNext                 = &selfExt;
	opaqueExt.albedoConstant.x      = 1.0f;
	opaqueExt.albedoConstant.y      = 1.0f;
	opaqueExt.albedoConstant.z      = 1.0f;
	opaqueExt.opacityConstant       = 1.0f;
	opaqueExt.useDrawCallAlphaState = 1;
	opaqueExt.alphaTestType         = 7; // ALWAYS

	float intensity = rt_fx_emis ? rt_fx_emis->value : 4.0f;
	if( intensity <= 0.0f )
		intensity = 0.001f;

	remixapi_MaterialInfo matInfo;
	memset( &matInfo, 0, sizeof( matInfo ) );
	matInfo.sType                   = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
	matInfo.pNext                   = &opaqueExt;
	matInfo.hash                    = entry->texHash ^ 0xE3155E1F00000000ULL;
	matInfo.albedoTexture           = hashStr;
	matInfo.emissiveIntensity       = intensity;
	matInfo.emissiveColorConstant.x = 1.0f;
	matInfo.emissiveColorConstant.y = 1.0f;
	matInfo.emissiveColorConstant.z = 1.0f;
	matInfo.filterMode              = R_Textures_FilterMode();
	matInfo.wrapModeU               = 1;
	matInfo.wrapModeV               = 1;

	if( g_remix.CreateMaterial( &matInfo, &entry->emisMatHandle ) != REMIXAPI_ERROR_CODE_SUCCESS )
	{
		gEngfuncs.Con_Reportf( S_WARN "ref_openremix: CreateMaterial (emissive) failed for %s\n", entry->name );
		entry->emisMatHandle = NULL;
	}

	return entry->emisMatHandle;
}

remixapi_MaterialHandle R_Textures_GetMaterial( unsigned int texnum )
{
	if( texnum > 0 && texnum < MAX_REMIX_TEXTURES && g_remixTextures[texnum].registered )
	{
		if( g_remixTextures[texnum].matHandle )
			return g_remixTextures[texnum].matHandle;
	}
	return s_defaultMaterial;
}

/*
 * R_Textures_CreateBrightMask - Emission mask for emissive textures with no authored mask:
 * the texture's own colour where it is near its brightest (lamp panels, lit screen
 * content), fading to black below (fixture frames, bezels, the surrounding wall)
 */
static qboolean R_Textures_CreateBrightMask( remix_texture_t *entry, const byte *rgba, uint32_t width, uint32_t height,
                                             float strength, uint64_t hash, wchar_t *out, size_t outLen )
{
	if( !g_remix.CreateTexture || !rgba )
		return false;

	const size_t numPixels = (size_t)width * height;
	float maxLum = 0.0f;
	for( size_t i = 0; i < numPixels; i++ )
	{
		float lum = 0.2126f * rgba[i * 4 + 0] + 0.7152f * rgba[i * 4 + 1] + 0.0722f * rgba[i * 4 + 2];
		maxLum = Q_max( maxLum, lum );
	}
	if( maxLum < 16.0f )
		return false;

	byte *mask = (byte *)Mem_Malloc( r_temppool, numPixels * 4 );
	if( !mask )
		return false;

	// Everything else keeps a faint base glow of about its normally lit brightness
	// (1 / strength), so a lamp's dimmer surround doesn't turn into a black ring
	const float base = bound( 0.0f, 1.0f / Q_max( strength, 1.0f ), 0.25f );
	const float lo = 0.55f * maxLum, hi = 0.85f * maxLum;
	for( size_t i = 0; i < numPixels; i++ )
	{
		float lum = 0.2126f * rgba[i * 4 + 0] + 0.7152f * rgba[i * 4 + 1] + 0.0722f * rgba[i * 4 + 2];
		float t = bound( 0.0f, ( lum - lo ) / ( hi - lo ), 1.0f );
		t = t * t * ( 3.0f - 2.0f * t ); // smoothstep
		t = base + ( 1.0f - base ) * t;
		for( int c = 0; c < 3; c++ )
			mask[i * 4 + c] = (byte)( rgba[i * 4 + c] * t );
		mask[i * 4 + 3] = 255;
	}

	remixapi_TextureInfo info;
	memset( &info, 0, sizeof( info ) );
	info.sType     = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
	info.hash      = hash;
	info.width     = width;
	info.height    = height;
	info.depth     = 1;
	info.mipLevels = 1;
	info.format    = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
	info.data      = mask;
	info.dataSize  = (uint64_t)numPixels * 4;

	if( entry->lumaTexHandle && g_remix.DestroyTexture )
		g_remix.DestroyTexture( entry->lumaTexHandle );
	entry->lumaTexHandle = NULL;

	qboolean ok = g_remix.CreateTexture( &info, &entry->lumaTexHandle ) == REMIXAPI_ERROR_CODE_SUCCESS;
	Mem_Free( mask );
	if( ok )
		_snwprintf( out, outLen, L"0x%llx", (unsigned long long)hash );
	return ok;
}

/*
 * Upload RGBA pixel data to OpenRemix and construct Material
 */
static int R_Textures_UploadRGBA( const char *name, uint32_t width, uint32_t height, const byte *rgbaData, uint32_t flags )
{
	if( !name || width == 0 || height == 0 || !rgbaData )
		return 0;

	// Check if already registered (a rebuild names its slot: entry names are truncated)
	const qboolean rebuild = s_rebuildTexnum > 0;
	int existing = rebuild ? s_rebuildTexnum : R_Textures_FindExact( name );
	if( existing > 0 && g_remixTextures[existing].matHandle && !rebuild )
		return existing;

	int texnum = existing > 0 ? existing : s_nextTexNum++;
	if( s_nextTexNum >= MAX_REMIX_TEXTURES )
		s_nextTexNum = 1;

	char baseName[64];
	R_Textures_GetBaseName( name, baseName, sizeof( baseName ) );

	remix_texture_t *entry = &g_remixTextures[texnum];
	Q_strncpy( entry->name, name, sizeof( entry->name ) );
	entry->width         = width;
	entry->height        = height;
	entry->flags         = flags;
	entry->isCutout      = ( baseName[0] == '{' );
	// Map textures ("...name.mip") only: the engine's built-in textures are named with a '*' too
	// ("*particle", "*white"), and as water they lost their image
	const size_t nameLength = Q_strlen( name );
	entry->isWater       = ( baseName[0] == '!' || baseName[0] == '*' ) && nameLength > 4 && !Q_stricmp( name + nameLength - 4, ".mip" );
	entry->isSky         = ( !Q_strnicmp( baseName, "sky", 3 ) || !Q_strnicmp( baseName, "scrollsky", 9 ) );
	entry->isGlass       = R_Textures_IsGlass( baseName );

	// Mean linear colour, over at most about 4096 sampled texels
	{
		const uint32_t numPixels = width * height;
		const uint32_t step = Q_max( 1u, numPixels / 4096u );
		double sum[3] = { 0.0, 0.0, 0.0 };
		uint32_t samples = 0;
		for( uint32_t i = 0; i < numPixels; i += step, samples++ )
			for( int c = 0; c < 3; c++ )
				sum[c] += pow( rgbaData[i * 4 + c] / 255.0, 2.2 );
		for( int c = 0; c < 3; c++ )
			entry->avgColor[c] = samples ? (float)( sum[c] / samples ) : 0.0f;
	}

	// XashRT content (rt/): material overrides and replacement textures
	xrt_material_t xrtMat;
	qboolean hasXrt = R_XRT_GetMaterial( name, &xrtMat );
	if( hasXrt && xrtMat.glass )
		entry->isGlass = true;
	entry->registered    = true;

	// 1. Create OpenRemix 3D Texture
	uint64_t texHash = R_HashString64( name );
	entry->texHash   = texHash;

	remixapi_TextureInfoGenerateMipsEXT mipsExt;
	memset( &mipsExt, 0, sizeof( mipsExt ) );
	mipsExt.sType    = REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT;
	mipsExt.generate = 1;

	const byte *uploadData = rgbaData;
	byte *sanitizedBuffer = NULL;
	if( entry->isCutout && rgbaData && width > 0 && height > 0 )
	{
		uint32_t numPixels = width * height;
		sanitizedBuffer = (byte *)Mem_Malloc( r_temppool, numPixels * 4 );
		if( sanitizedBuffer )
		{
			memcpy( sanitizedBuffer, rgbaData, numPixels * 4 );
			for( uint32_t i = 0; i < numPixels; i++ )
			{
				byte r = sanitizedBuffer[i * 4 + 0];
				byte g = sanitizedBuffer[i * 4 + 1];
				byte b = sanitizedBuffer[i * 4 + 2];
				byte a = sanitizedBuffer[i * 4 + 3];

				// Half-Life 1-bit alpha test: blue palette index 255 (0, 0, 255) or 0 alpha is transparent
				if( a == 0 || ( r == 0 && g == 0 && b == 255 ) )
					sanitizedBuffer[i * 4 + 3] = 0;
				else
					sanitizedBuffer[i * 4 + 3] = 255;
			}
			uploadData = sanitizedBuffer;
		}
	}

	remixapi_TextureInfo texInfo;
	memset( &texInfo, 0, sizeof( texInfo ) );
	texInfo.sType     = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
	texInfo.pNext     = &mipsExt;
	texInfo.hash      = texHash;
	texInfo.width     = width;
	texInfo.height    = height;
	texInfo.depth     = 1;
	texInfo.mipLevels = 1;
	texInfo.format    = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
	texInfo.data      = uploadData;
	texInfo.dataSize  = (uint64_t)width * height * 4;

	// A rebuild remakes the material alone: the texture stays
	if( g_remix.CreateTexture && !rebuild )
	{
		remixapi_ErrorCode err = g_remix.CreateTexture( &texInfo, &entry->texHandle );
		if( err != REMIXAPI_ERROR_CODE_SUCCESS )
		{
			gEngfuncs.Con_Reportf( S_WARN "ref_openremix: CreateTexture failed for %s (code %u)\n", name, (unsigned)err );
		}
	}

	if( sanitizedBuffer )
	{
		Mem_Free( sanitizedBuffer );
		sanitizedBuffer = NULL;
	}

	wchar_t hashStr[32];
	_snwprintf( hashStr, sizeof( hashStr ) / sizeof( wchar_t ), L"0x%llx", (unsigned long long)texHash );

	uint64_t matHash = texHash ^ 0x9E3779B97F4A7C15ULL;
	entry->matHash   = matHash;

	float emisMultiplier = ( rt_emis ? rt_emis->value : 1.0f );
	if( emisMultiplier < 0.0f ) emisMultiplier = 0.0f;

	// 2. Create OpenRemix Material
	remixapi_MaterialInfoTranslucentEXT transExt;
	remixapi_MaterialInfoOpaqueEXT opaqueExt;

	// Clear glass: refractive transmission with no texture. Textured glass: the texture at
	// rt_glass_texture opacity, smooth so it reflects (GoldSrc-style, no refraction).
	qboolean clearGlass = entry->isGlass && !R_Textures_GlassTextured();

	// Water ("!" / "*" textures, XashRT isWater): a refractive body that absorbs with depth;
	// seen from below it shows Snell's window and total internal reflection
	qboolean waterBody = ( entry->isWater || ( hasXrt && xrtMat.water ) ) && !( hasXrt && xrtMat.opaque ) && !entry->isSky && !clearGlass &&
	                     rt_water && rt_water->value > 0.0f;
	remixapi_MaterialInfoTranslucentEXT waterExt;
	remixapi_MaterialNormalEncodingEXT waterNormalEnc;
	remixapi_MaterialNormalScrollEXT waterScroll;
	remixapi_MaterialInfoCausticsEXT waterCaustics;
	wchar_t waterNormal[32];
	qboolean hasWaterNormal = false;

	if( waterBody )
	{
		// Absorption colour: the texture's average, normalised so the brightest channel passes
		// 90% per measurement distance (HL water textures are dark; their hue is what matters)
		double sum[3] = { 0, 0, 0 };
		const size_t numPixels = (size_t)width * height;
		for( size_t i = 0; i < numPixels; i++ )
			for( int c = 0; c < 3; c++ )
				sum[c] += rgbaData[i * 4 + c];
		float avg[3], peak = 1e-3f;
		for( int c = 0; c < 3; c++ )
		{
			avg[c] = (float)( sum[c] / ( numPixels * 255.0 ) );
			peak = Q_max( peak, avg[c] );
		}

		memset( &waterExt, 0, sizeof( waterExt ) );
		waterExt.sType           = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
		waterExt.refractiveIndex = ( rt_water_ior && rt_water_ior->value >= 1.0f ) ? rt_water_ior->value : 1.333f;
		waterExt.transmittanceColor.x = bound( 0.05f, avg[0] / peak * 0.9f, 0.98f );
		waterExt.transmittanceColor.y = bound( 0.05f, avg[1] / peak * 0.9f, 0.98f );
		waterExt.transmittanceColor.z = bound( 0.05f, avg[2] / peak * 0.9f, 0.98f );
		waterExt.transmittanceMeasurementDistance = ( rt_water_dist && rt_water_dist->value > 1.0f ) ? rt_water_dist->value : 300.0f;

		s_hasWaterMedium = true;
		s_waterColor[0] = waterExt.transmittanceColor.x;
		s_waterColor[1] = waterExt.transmittanceColor.y;
		s_waterColor[2] = waterExt.transmittanceColor.z;
		s_waterDist     = waterExt.transmittanceMeasurementDistance;

		// Under water: Half-Life's fog (GL_EXP, density fogParams[3] * 0.000025 per unit), its
		// colour the medium's. Colour^(d / D) falls like exp(-density d) for the colour's mean
		// when D = -ln(mean) / density
		const float fogScale = rt_water_fog ? rt_water_fog->value : 1.0f;
		if( s_uploadFog && s_uploadFog[3] > 0 && ( s_uploadFog[0] || s_uploadFog[1] || s_uploadFog[2] ) && fogScale > 0.0f )
		{
			float mean = 0.0f;
			for( int c = 0; c < 3; c++ )
			{
				s_waterColor[c] = bound( 0.02f, s_uploadFog[c] / 255.0f, 0.98f );
				mean += s_waterColor[c] / 3.0f;
			}
			const float density = s_uploadFog[3] * 0.000025f * fogScale;
			s_waterDist = -logf( mean ) / density;
		}

		// Waves: XashRT's water normal map, scrolled in three layers like Source's water
		hasWaterNormal = R_XRT_GetFileTexture( "rt/WaterNormal_n.ktx2", waterNormal, ARRAYSIZE( waterNormal ) );
		if( hasWaterNormal )
		{
			memset( &waterScroll, 0, sizeof( waterScroll ) );
			waterScroll.sType     = REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_SCROLL_EXT;
			waterScroll.layers    = 3;
			waterScroll.base[0]   = 0.02f;  waterScroll.base[1]   = 0.01f;
			waterScroll.layer1[0] = -0.015f; waterScroll.layer1[1] = 0.02f;
			waterScroll.layer2[0] = 0.01f;  waterScroll.layer2[1] = -0.025f;

			memset( &waterNormalEnc, 0, sizeof( waterNormalEnc ) );
			waterNormalEnc.sType    = REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_ENCODING_EXT;
			waterNormalEnc.pNext    = &waterScroll;
			waterNormalEnc.encoding = 5; // Source water: scrolled tangent-space map
			waterExt.pNext = &waterNormalEnc;
		}

		// Light through the water draws fake caustics on what it lights (rt_caustics)
		memset( &waterCaustics, 0, sizeof( waterCaustics ) );
		waterCaustics.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_CAUSTICS_EXT;
		waterCaustics.pNext = waterExt.pNext;
		waterExt.pNext      = &waterCaustics;
	}

	// XashRT PBR maps: _orm packs roughness in green and metallic in blue (RayTracedGL1's
	// NULL_ROUGHNESS_METALLIC swizzle); _n is a tangent-space (OpenGL convention) normal map
	wchar_t xrtOrm[32], xrtNormal[32];
	qboolean hasOrm    = !clearGlass && !waterBody && R_XRT_GetReplacementTexture( name, "_orm", xrtOrm, ARRAYSIZE( xrtOrm ) );
	qboolean hasNormal = !clearGlass && !waterBody && R_XRT_GetReplacementTexture( name, "_n", xrtNormal, ARRAYSIZE( xrtNormal ) );

	// XashRT's replacement image for the texture
	wchar_t xrtAlbedo[32];
	const qboolean hasXrtAlbedo = R_XRT_GetReplacementTexture( name, "", xrtAlbedo, ARRAYSIZE( xrtAlbedo ) );

	// Studio textures (#<model>/<texture>.mdl) carry the model's texture flags; others carry TF_*.
	// Chrome is our fallback: XashRT's material for the texture, when it has one, comes first
	const size_t nameLen = Q_strlen( name );
	const qboolean studioTexture = nameLen > 4 && !Q_stricmp( name + nameLen - 4, ".mdl" );
	const qboolean chrome = rt_chrome && rt_chrome->value > 0.0f && !clearGlass && !waterBody &&
	                        FBitSet( flags, STUDIO_NF_CHROME ) && studioTexture &&
	                        !hasXrt && !hasOrm && !hasNormal && !hasXrtAlbedo;

	// Auto PBR (r_autopbr.c) for map and model textures with nothing authored: XashRT's values
	// and maps win channel by channel, and none of it touches a texture XashRT replaces (the
	// generated maps would not fit its image). USD mods replace the whole material by its hash
	float autoMetallic = 0.0f, autoRoughness = 0.85f;
	qboolean autoBumps = false, autoMaterial = false;
	wchar_t autoNormal[32];
	qboolean hasAutoNormal = false;
	const qboolean autoPbr = R_AutoPBR_Enabled() && ( studioTexture || ( nameLen > 4 && !Q_stricmp( name + nameLen - 4, ".mip" ))) &&
	                         !clearGlass && !waterBody && !entry->isGlass && !entry->isSky && !entry->isWater && !chrome && !hasXrtAlbedo;
	if( entry->normalTexHandle && g_remix.DestroyTexture )
		g_remix.DestroyTexture( entry->normalTexHandle );
	entry->normalTexHandle = NULL;
	if( autoPbr )
	{
		autoMaterial = !hasOrm && R_AutoPBR_Classify( baseName, studioTexture, &autoMetallic, &autoRoughness, &autoBumps );
		if( !autoMaterial )
			autoBumps = true;
		// Not over cutouts' see-through texels, whose colour is no surface
		if( autoBumps && !hasNormal && !entry->isCutout )
			hasAutoNormal = R_AutoPBR_CreateNormalMap( rgbaData, width, height, texHash ^ 0x4E4F524D414C4D50ULL, &entry->normalTexHandle,
			                                           autoNormal, ARRAYSIZE( autoNormal ));
	}
	remixapi_MaterialPbrRemapEXT pbrRemap;
	remixapi_MaterialNormalEncodingEXT normalEnc;
	memset( &pbrRemap, 0, sizeof( pbrRemap ) );
	pbrRemap.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_REMAP_EXT;
	pbrRemap.roughness.channel  = 1;
	pbrRemap.roughness.exponent = 1.0f;
	pbrRemap.roughness.scale    = 1.0f;
	pbrRemap.metallic.channel   = 2;
	pbrRemap.metallic.exponent  = 1.0f;
	pbrRemap.metallic.scale     = 1.0f;
	memset( &normalEnc, 0, sizeof( normalEnc ) );
	normalEnc.sType    = REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_ENCODING_EXT;
	normalEnc.encoding = 3; // tangent GL

	if( clearGlass )
	{
		memset( &transExt, 0, sizeof( transExt ) );
		transExt.sType                   = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
		transExt.pNext                   = NULL;
		// For pure ray-traced refractive glass, transmittanceTexture must be NULL so Beer-Lambert
		// does not absorb all background light based on dark legacy diffuse pixel colors.
		transExt.transmittanceTexture    = NULL;
		transExt.refractiveIndex         = ( rt_refr_glass ? rt_refr_glass->value : 1.52f );
		if( transExt.refractiveIndex < 1.0f ) transExt.refractiveIndex = 1.52f;

		float tintR = ( rt_glass_tint_r ? rt_glass_tint_r->value : 0.98f );
		float tintG = ( rt_glass_tint_g ? rt_glass_tint_g->value : 0.99f );
		float tintB = ( rt_glass_tint_b ? rt_glass_tint_b->value : 1.0f );
		transExt.transmittanceColor.x    = tintR;
		transExt.transmittanceColor.y    = tintG;
		transExt.transmittanceColor.z    = tintB;

		float dist = ( rt_glass_dist ? rt_glass_dist->value : 2000.0f );
		if( dist <= 0.0f ) dist = 2000.0f;
		transExt.transmittanceMeasurementDistance = dist;

		if( rt_glass_thin && rt_glass_thin->value > 0.0f )
		{
			transExt.thinWallThickness_hasvalue = 1;
			transExt.thinWallThickness_value    = 0.005f;
		}
		else
		{
			transExt.thinWallThickness_hasvalue = 0;
		}
		transExt.useDiffuseLayer         = 0;
	}
	else
	{
		memset( &opaqueExt, 0, sizeof( opaqueExt ) );
		opaqueExt.sType             = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
		opaqueExt.roughnessConstant = 0.85f;
		opaqueExt.metallicConstant  = 0.0f;
		opaqueExt.opacityConstant   = 1.0f;
		opaqueExt.useDrawCallAlphaState = 1;
		opaqueExt.alphaTestType     = 7; // ALWAYS (0 = NEVER would discard every texel)
		opaqueExt.albedoConstant.x  = 1.0f;
		opaqueExt.albedoConstant.y  = 1.0f;
		opaqueExt.albedoConstant.z  = 1.0f;

		if( entry->isCutout )
		{
			opaqueExt.alphaTestType       = 4; // GREATER
			opaqueExt.alphaReferenceValue = 127;
		}

		if( entry->isGlass )
		{
			opaqueExt.opacityConstant   = R_Textures_GlassOpacity();
			opaqueExt.roughnessConstant = 0.05f;
		}

		if( autoMaterial )
		{
			opaqueExt.metallicConstant  = autoMetallic;
			opaqueExt.roughnessConstant = autoRoughness;
		}

		if( hasXrt && xrtMat.metallic >= 0.0f )
			opaqueExt.metallicConstant = xrtMat.metallic;
		if( hasXrt && xrtMat.roughness >= 0.0f )
			opaqueExt.roughnessConstant = xrtMat.roughness;

		// Model chrome (STUDIO_NF_CHROME) faked reflections with view-dependent texture coordinates;
		// the path tracer reflects the scene, so it becomes metal tinted by the texture's mean colour
		// at rt_chrome_reflectance (only where XashRT has no material for it, see above)
		if( chrome )
		{
			const float reflectance = rt_chrome_reflectance ? bound( 0.0f, rt_chrome_reflectance->value, 1.0f ) : 0.6f;
			const float peak = Q_max( entry->avgColor[0], Q_max( entry->avgColor[1], entry->avgColor[2] ));
			const float scale = peak > 0.001f ? reflectance / peak : 0.0f;
			opaqueExt.albedoConstant.x  = peak > 0.001f ? entry->avgColor[0] * scale : reflectance;
			opaqueExt.albedoConstant.y  = peak > 0.001f ? entry->avgColor[1] * scale : reflectance;
			opaqueExt.albedoConstant.z  = peak > 0.001f ? entry->avgColor[2] * scale : reflectance;
			opaqueExt.metallicConstant  = 1.0f;
			opaqueExt.roughnessConstant = rt_chrome_roughness ? bound( 0.0f, rt_chrome_roughness->value, 1.0f ) : 0.3f;
		}

		if( hasOrm )
		{
			opaqueExt.roughnessTexture = xrtOrm;
			opaqueExt.metallicTexture  = xrtOrm;
			pbrRemap.pNext   = hasNormal ? (void *)&normalEnc : NULL;
			opaqueExt.pNext  = &pbrRemap;
		}
		else if( hasNormal || hasAutoNormal )
		{
			opaqueExt.pNext = &normalEnc;
		}
	}

	wchar_t maskHashStr[32];

	wchar_t xrtEmissive[32];
	const wchar_t *albedoPath = hasXrtAlbedo ? xrtAlbedo : hashStr;

	remixapi_MaterialInfo matInfo;
	memset( &matInfo, 0, sizeof( matInfo ) );
	matInfo.sType         = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
	matInfo.pNext         = clearGlass ? (void *)&transExt : waterBody ? (void *)&waterExt : (void *)&opaqueExt;
	matInfo.hash          = matHash;
	matInfo.albedoTexture = ( clearGlass || waterBody || chrome ) ? NULL : albedoPath;
	if( waterBody && hasWaterNormal )
		matInfo.normalTexture = waterNormal;
	else if( hasNormal )
		matInfo.normalTexture = xrtNormal;
	else if( hasAutoNormal )
		matInfo.normalTexture = autoNormal;
	matInfo.filterMode    = R_Textures_FilterMode();
	matInfo.wrapModeU     = 1; // Repeat
	matInfo.wrapModeV     = 1; // Repeat

	if( !entry->isGlass && !waterBody && R_XRT_Active() )
	{
		// XashRT: emissiveMult / texture lights, masked by the _e map when one exists
		qboolean hasEmissiveMap = R_XRT_GetReplacementTexture( name, "_e", xrtEmissive, ARRAYSIZE( xrtEmissive ) );
		float strength = ( hasXrt && xrtMat.emissive > 0.0f ) ? xrtMat.emissive : hasEmissiveMap ? 10.0f : 0.0f;
		if( strength > 0.0f && emisMultiplier > 0.0f )
		{
			matInfo.emissiveColorConstant.x = 1.0f;
			matInfo.emissiveColorConstant.y = 1.0f;
			matInfo.emissiveColorConstant.z = 1.0f;
			matInfo.emissiveIntensity       = strength * emisMultiplier;
			// XashRT's _e map, else the texture's brightest areas (lamp panels, screen content).
			// GoldSrc's fullbright palette range is no guide: some lights are entirely
			// fullbright (frame included), others have none. Glowing liquids (toxic waste) are
			// GoldSrc's fullbright turbulent surfaces: the whole texture, at about its own
			// brightness rather than a lamp's
			if( entry->isWater )
			{
				matInfo.emissiveIntensity = ( rt_liquid_emis ? Q_max( rt_liquid_emis->value, 0.0f ) : 1.0f ) * emisMultiplier;
				matInfo.emissiveTexture   = albedoPath;
			}
			else if( hasEmissiveMap )
				matInfo.emissiveTexture = xrtEmissive;
			else if( R_Textures_CreateBrightMask( entry, rgbaData, width, height, strength * emisMultiplier, texHash ^ 0x4252494748544D4BULL, maskHashStr, ARRAYSIZE( maskHashStr ) ) )
				matInfo.emissiveTexture = maskHashStr;
			else
				matInfo.emissiveTexture = albedoPath;
		}
	}
	else if( !entry->isGlass && !waterBody )
	{
		// Detect light fixtures and screens to enable self-illumination emission in the path tracer
		char lowerBase[64];
		Q_strncpy( lowerBase, baseName, sizeof( lowerBase ) );
		for( char *p = lowerBase; *p; p++ )
		{
			if( *p >= 'A' && *p <= 'Z' )
				*p += ( 'a' - 'A' );
		}

		qboolean isLightFixture = false;
		qboolean isScreen = false;

		if( strstr( lowerBase, "light" ) || strstr( lowerBase, "lght" ) ||
		    strstr( lowerBase, "spot" ) || strstr( lowerBase, "lamp" ) ||
		    strstr( lowerBase, "fifts_lgt" ) || strstr( lowerBase, "fifties_lgt" ) )
		{
			isLightFixture = true;
		}
		else if( strstr( lowerBase, "~" ) || strstr( lowerBase, "scrn" ) ||
		         strstr( lowerBase, "comp" ) || strstr( lowerBase, "crt" ) )
		{
			isScreen = true;
		}

		if( isLightFixture && emisMultiplier > 0.0f )
		{
			matInfo.emissiveColorConstant.x = 1.0f;
			matInfo.emissiveColorConstant.y = 1.0f;
			matInfo.emissiveColorConstant.z = 1.0f;
			matInfo.emissiveIntensity       = 150.0f * emisMultiplier;
			matInfo.emissiveTexture         = R_Textures_CreateBrightMask( entry, rgbaData, width, height, matInfo.emissiveIntensity, texHash ^ 0x4252494748544D4BULL, maskHashStr, ARRAYSIZE( maskHashStr ) ) ? maskHashStr : hashStr;
		}
		else if( isScreen && emisMultiplier > 0.0f )
		{
			matInfo.emissiveColorConstant.x = 1.0f;
			matInfo.emissiveColorConstant.y = 1.0f;
			matInfo.emissiveColorConstant.z = 1.0f;
			matInfo.emissiveIntensity       = 10.0f * emisMultiplier;
			matInfo.emissiveTexture         = R_Textures_CreateBrightMask( entry, rgbaData, width, height, matInfo.emissiveIntensity, texHash ^ 0x4252494748544D4BULL, maskHashStr, ARRAYSIZE( maskHashStr ) ) ? maskHashStr : hashStr;
		}
	}

	// Optional PBR suffix detection: check for <name>_n, <name>_r, <name>_m, <name>_e
	char pbrPath[128];
	wchar_t normHashStr[32] = { 0 };
	wchar_t emisHashStr[32] = { 0 };

	// Normal map check
	Q_snprintf( pbrPath, sizeof( pbrPath ), "textures/%s_n.tga", name );
	rgbdata_t *pbrPic = gEngfuncs.FS_LoadImage ? gEngfuncs.FS_LoadImage( pbrPath, NULL, 0 ) : NULL;
	if( !pbrPic )
	{
		Q_snprintf( pbrPath, sizeof( pbrPath ), "textures/%s_norm.tga", name );
		pbrPic = gEngfuncs.FS_LoadImage ? gEngfuncs.FS_LoadImage( pbrPath, NULL, 0 ) : NULL;
	}
	if( pbrPic )
	{
		if( pbrPic->type != PF_RGBA_32 && gEngfuncs.Image_Process )
			gEngfuncs.Image_Process( &pbrPic, 0, 0, IMAGE_FORCE_RGBA, 0.0f );

		if( pbrPic && pbrPic->buffer )
		{
			uint64_t normHash = R_HashString64( pbrPath );
			_snwprintf( normHashStr, sizeof( normHashStr ) / sizeof( wchar_t ), L"0x%llx", (unsigned long long)normHash );

			remixapi_TextureInfo normTexInfo = texInfo;
			normTexInfo.hash     = normHash;
			normTexInfo.width    = pbrPic->width;
			normTexInfo.height   = pbrPic->height;
			normTexInfo.data     = pbrPic->buffer;
			normTexInfo.dataSize = (uint64_t)pbrPic->width * pbrPic->height * 4;

			remixapi_TextureHandle hNorm = NULL;
			if( g_remix.CreateTexture )
				g_remix.CreateTexture( &normTexInfo, &hNorm );

			matInfo.normalTexture = normHashStr;
		}
		if( pbrPic ) gEngfuncs.FS_FreeImage( pbrPic );
	}

	// PBR Emissive map check
	Q_snprintf( pbrPath, sizeof( pbrPath ), "textures/%s_e.tga", name );
	rgbdata_t *emisPic = gEngfuncs.FS_LoadImage ? gEngfuncs.FS_LoadImage( pbrPath, NULL, 0 ) : NULL;
	if( !emisPic )
	{
		Q_snprintf( pbrPath, sizeof( pbrPath ), "textures/%s_luma.tga", name );
		emisPic = gEngfuncs.FS_LoadImage ? gEngfuncs.FS_LoadImage( pbrPath, NULL, 0 ) : NULL;
	}
	if( emisPic )
	{
		if( emisPic->type != PF_RGBA_32 && gEngfuncs.Image_Process )
			gEngfuncs.Image_Process( &emisPic, 0, 0, IMAGE_FORCE_RGBA, 0.0f );

		if( emisPic && emisPic->buffer )
		{
			uint64_t emisHash = R_HashString64( pbrPath );
			_snwprintf( emisHashStr, sizeof( emisHashStr ) / sizeof( wchar_t ), L"0x%llx", (unsigned long long)emisHash );

			remixapi_TextureInfo emisTexInfo = texInfo;
			emisTexInfo.hash     = emisHash;
			emisTexInfo.width    = emisPic->width;
			emisTexInfo.height   = emisPic->height;
			emisTexInfo.data     = emisPic->buffer;
			emisTexInfo.dataSize = (uint64_t)emisPic->width * emisPic->height * 4;

			remixapi_TextureHandle hEmis = NULL;
			if( g_remix.CreateTexture )
				g_remix.CreateTexture( &emisTexInfo, &hEmis );

			matInfo.emissiveTexture = emisHashStr;
			matInfo.emissiveIntensity = 40.0f * ( emisMultiplier > 0.0f ? emisMultiplier : 1.0f );
			matInfo.emissiveColorConstant.x = 1.0f;
			matInfo.emissiveColorConstant.y = 1.0f;
			matInfo.emissiveColorConstant.z = 1.0f;
		}
		if( emisPic ) gEngfuncs.FS_FreeImage( emisPic );
	}

	if( g_remix.CreateMaterial )
	{
		remixapi_ErrorCode err = g_remix.CreateMaterial( &matInfo, &entry->matHandle );
		if( err != REMIXAPI_ERROR_CODE_SUCCESS )
		{
			gEngfuncs.Con_Reportf( S_WARN "ref_openremix: CreateMaterial failed for %s (code %u)\n", name, (unsigned)err );
			entry->matHandle = s_defaultMaterial;
		}
		else if( clearGlass )
		{
			gEngfuncs.Con_Printf( "^2ref_openremix: Created ray-traced glass material for '%s' (IOR=%.2f, dist=%.1f)^7\n",
			                      name, transExt.refractiveIndex, transExt.transmittanceMeasurementDistance );
		}
	}

	// World sprite frames ("#sprites/x.spr(...).spr", not "#HUD/...") get an additive FX variant
	R_Textures_DestroyFx( entry );
	if( name[0] == '#' && Q_strnicmp( name, "#HUD/", 5 ) && Q_stristr( name, ".spr" ) )
		R_Textures_CreateFx( entry, name, width, height, rgbaData );

	if( rebuild )
		return texnum;

	// Map and model textures keep their pixels: their material is remade (by the same hash,
	// which OpenRemix swaps in wherever it is drawn) when a setting it depends on changes
	R_Textures_FreePixels( entry );
	if( studioTexture || ( nameLen > 4 && !Q_stricmp( name + nameLen - 4, ".mip" )))
	{
		entry->pixels = (byte *)Mem_Malloc( r_temppool, (size_t)width * height * 4 );
		entry->pixelsName = (char *)Mem_Malloc( r_temppool, nameLen + 1 );
		if( entry->pixels && entry->pixelsName )
		{
			memcpy( entry->pixels, rgbaData, (size_t)width * height * 4 );
			memcpy( entry->pixelsName, name, nameLen + 1 );
		}
		else
			R_Textures_FreePixels( entry );
	}

	// 3. Register with 2D UI system so HUD and menus can draw this texture
	R_UI_RegisterTexture( texnum, name, (int)width, (int)height, rgbaData, REMIXAPI_FORMAT_R8G8B8A8_UNORM );

	return texnum;
}

/*
 * R_Textures_CheckSettings - Remakes map and model materials (from their kept pixels) when auto
 * PBR or texture filtering changes, so the change shows at once instead of on the next map load
 */
void R_Textures_CheckSettings( void )
{
	qboolean changed = R_AutoPBR_SettingsChanged();
	if( rt_texture_nearest && FBitSet( rt_texture_nearest->flags, FCVAR_CHANGED ))
	{
		ClearBits( rt_texture_nearest->flags, FCVAR_CHANGED );
		changed = true;
	}
	if( !changed )
		return;

	int rebuilt = 0;
	for( int i = 1; i < MAX_REMIX_TEXTURES; i++ )
	{
		remix_texture_t *entry = &g_remixTextures[i];
		if( !entry->registered || !entry->pixels || !entry->pixelsName || !entry->matHandle )
			continue;
		s_rebuildTexnum = i;
		R_Textures_UploadRGBA( entry->pixelsName, entry->width, entry->height, entry->pixels, entry->flags );
		rebuilt++;
	}
	s_rebuildTexnum = 0;
	if( rebuilt )
		gEngfuncs.Con_Reportf( "ref_openremix: rebuilt %d materials for the new texture settings\n", rebuilt );
}

/*
 * R_Textures_ForceOpaque - Images without IMAGE_HAS_ALPHA are opaque. Paletted BMPs take
 * "alpha" from the palette's reserved byte (usually 0); the GL renderer uploads them as RGB,
 * so do the same or additive UI art (menu buttons) is invisible.
 */
static void R_Textures_ForceOpaque( rgbdata_t *pic, int originalFlags )
{
	if( FBitSet( originalFlags, IMAGE_HAS_ALPHA ) || !pic || !pic->buffer || pic->type != PF_RGBA_32 )
		return;

	size_t numPixels = (size_t)pic->width * pic->height;
	for( size_t i = 0; i < numPixels; i++ )
		pic->buffer[i * 4 + 3] = 255;
}

/*
 * GL_LoadTexture_Hook - Loads texture from disk or raw memory buffer via Engine FS
 */
int GL_LoadTexture_Hook( const char *name, const byte *buf, size_t size, int flags )
{
	if( !name || !*name )
		return 0;

	// Check if already loaded
	int existing = R_Textures_FindExact( name );
	if( existing > 0 && g_remixTextures[existing].matHandle )
		return existing;

	if( !gEngfuncs.FS_LoadImage )
		return 0;

	gEngfuncs.Image_SetForceFlags( 0 );
	rgbdata_t *pic = gEngfuncs.FS_LoadImage( name, buf, size );
	if( !pic )
		return 0;

	int originalFlags = pic->flags;
	if( pic->type != PF_RGBA_32 && gEngfuncs.Image_Process )
	{
		gEngfuncs.Image_Process( &pic, 0, 0, IMAGE_FORCE_RGBA, 0.0f );
	}

	if( !pic || !pic->buffer || pic->width <= 0 || pic->height <= 0 )
	{
		if( pic && gEngfuncs.FS_FreeImage ) gEngfuncs.FS_FreeImage( pic );
		return 0;
	}

	R_Textures_ForceOpaque( pic, originalFlags );

	s_uploadFog = pic->fogParams;
	int texnum = R_Textures_UploadRGBA( name, pic->width, pic->height, pic->buffer, flags );
	s_uploadFog = NULL;

	if( gEngfuncs.FS_FreeImage )
		gEngfuncs.FS_FreeImage( pic );

	return texnum;
}

/*
 * GL_LoadTextureFromBuffer_Hook - Loads texture from engine rgbdata_t (e.g. WAD miptex)
 */
int GL_LoadTextureFromBuffer_Hook( const char *name, rgbdata_t *pic, texFlags_t flags, qboolean update )
{
	(void)update;
	if( !pic || !pic->buffer || pic->width <= 0 || pic->height <= 0 )
		return 0;

	// Convert paletted or 24-bit to 32-bit RGBA if needed
	rgbdata_t *workingPic = pic;
	qboolean freedCopy = false;

	if( workingPic->type != PF_RGBA_32 && gEngfuncs.FS_CopyImage && gEngfuncs.Image_Process )
	{
		workingPic = gEngfuncs.FS_CopyImage( pic );
		if( workingPic )
		{
			gEngfuncs.Image_Process( &workingPic, 0, 0, IMAGE_FORCE_RGBA, 0.0f );
			R_Textures_ForceOpaque( workingPic, pic->flags );
			freedCopy = true;
		}
		else
		{
			workingPic = pic;
		}
	}

	s_uploadFog = pic->fogParams;
	int texnum = R_Textures_UploadRGBA( name, workingPic->width, workingPic->height, workingPic->buffer, flags );
	s_uploadFog = NULL;

	if( freedCopy && gEngfuncs.FS_FreeImage )
		gEngfuncs.FS_FreeImage( workingPic );

	if( !Q_strcmp( name, REF_PARTICLE_TEXTURE ) )
		tr.particleTexture = texnum;

	return texnum;
}

/*
 * GL_CreateTexture_Hook - Direct creation from raw buffer
 */
int GL_CreateTexture_Hook( const char *name, int width, int height, const void *buffer, texFlags_t flags )
{
	if( !buffer || width <= 0 || height <= 0 )
		return 0;

	int texnum = R_Textures_UploadRGBA( name, (uint32_t)width, (uint32_t)height, (const byte *)buffer, flags );
	if( !Q_strcmp( name, REF_PARTICLE_TEXTURE ) )
		tr.particleTexture = texnum;

	return texnum;
}

int GL_FindTexture_Hook( const char *name )
{
	return R_Textures_Find( name );
}

const char *GL_TextureName_Hook( unsigned int texnum )
{
	return R_Textures_GetName( texnum );
}

void GL_FreeTexture_Hook( unsigned int texnum )
{
	if( texnum > 0 && texnum < MAX_REMIX_TEXTURES )
	{
		remix_texture_t *entry = &g_remixTextures[texnum];
		if( entry->registered )
		{
			if( entry->matHandle && g_remix.DestroyMaterial )
			{
				g_remix.DestroyMaterial( entry->matHandle );
				entry->matHandle = NULL;
			}
			if( entry->texHandle && g_remix.DestroyTexture )
			{
				g_remix.DestroyTexture( entry->texHandle );
				entry->texHandle = NULL;
			}
			R_Textures_DestroyFx( entry );
			R_Textures_FreePixels( entry );
			entry->registered = false;
		}
	}
	R_UI_FreeTexture( (int)texnum );
}

void GL_UpdateTexture_Hook( int texnum, int cols, int rows, int width, int height, const byte *buffer, pixformat_t fmt )
{
	remixapi_Format rfmt = ( fmt == PF_BGRA_32 ) ? REMIXAPI_FORMAT_B8G8R8A8_UNORM : REMIXAPI_FORMAT_R8G8B8A8_UNORM;
	R_UI_UpdateTextureRegion( texnum, cols, rows, width, height, buffer, rfmt, width * 4 );
}

/*
 * Mod_StudioLoadTextures_Hook - Called by engine when studio model is loaded
 */
// Model being loaded by Mod_StudioLoadTextures_Hook, used to name its textures
static char s_studioModelName[128];

void Mod_StudioLoadTextures_Hook( model_t *mod, void *data )
{
	studiohdr_t *phdr = (studiohdr_t *)data;
	if( !phdr || phdr->textureindex <= 0 || phdr->numtextures <= 0 )
		return;

	// Same naming as the GL renderer: "#models/barney/BX_Chrome1.mdl"
	Q_strncpy( s_studioModelName, mod ? mod->name : "", sizeof( s_studioModelName ) );
	COM_StripExtension( s_studioModelName );

	mstudiotexture_t *ptexture = (mstudiotexture_t *)((byte *)phdr + phdr->textureindex);
	for( int i = 0; i < phdr->numtextures; i++ )
	{
		R_Textures_GetOrCreateForStudio( &ptexture[i], phdr );
	}

	s_studioModelName[0] = '\0';
}

/*
 * R_Textures_GetOrCreateForBSP - Retrieves or creates OpenRemix material for BSP surface bucket
 */
remixapi_MaterialHandle R_Textures_GetOrCreateForBSP( texture_t *tx, const model_t *mod )
{
	if( !tx )
		return s_defaultMaterial;

	// 1. Check if already loaded with valid material
	if( tx->gl_texturenum > 0 && tx->gl_texturenum < MAX_REMIX_TEXTURES )
	{
		if( g_remixTextures[tx->gl_texturenum].matHandle )
			return g_remixTextures[tx->gl_texturenum].matHandle;
	}

	// 2. Search by name or base name in registry
	int existing = R_Textures_Find( tx->name );
	if( existing > 0 && g_remixTextures[existing].matHandle )
	{
		tx->gl_texturenum = existing;
		return g_remixTextures[existing].matHandle;
	}

	// 3. Try searching with map name prefix e.g. #maps/<map>.bsp:<name>.mip
	const model_t *world = tr.worldmodel ? tr.worldmodel : mod;
	const char *mapName = ( world && world->name && world->name[0] != '*' ) ? world->name : ( ( mod && mod->name && mod->name[0] != '*' ) ? mod->name : NULL );
	if( mapName )
	{
		char mapTexName[128];
		Q_snprintf( mapTexName, sizeof( mapTexName ), "#%s:%s.mip", mapName, tx->name );
		existing = R_Textures_Find( mapTexName );
		if( existing > 0 && g_remixTextures[existing].matHandle )
		{
			tx->gl_texturenum = existing;
			return g_remixTextures[existing].matHandle;
		}

		if( gEngfuncs.FS_LoadImage )
		{
			rgbdata_t *pic = gEngfuncs.FS_LoadImage( mapTexName, NULL, 0 );
			if( pic )
			{
				if( pic->type != PF_RGBA_32 && gEngfuncs.Image_Process )
					gEngfuncs.Image_Process( &pic, 0, 0, IMAGE_FORCE_RGBA, 0.0f );

				if( pic && pic->buffer )
				{
					int texnum = R_Textures_UploadRGBA( mapTexName, pic->width, pic->height, pic->buffer, 0 );
					tx->gl_texturenum = texnum;
					gEngfuncs.FS_FreeImage( pic );
					if( texnum > 0 && g_remixTextures[texnum].matHandle )
						return g_remixTextures[texnum].matHandle;
				}
				else if( pic )
				{
					gEngfuncs.FS_FreeImage( pic );
				}
			}
		}
	}

	// 4. Try to load from disk / WAD via engine
	if( gEngfuncs.FS_LoadImage )
	{
		rgbdata_t *pic = gEngfuncs.FS_LoadImage( tx->name, NULL, 0 );
		if( pic )
		{
			if( pic->type != PF_RGBA_32 && gEngfuncs.Image_Process )
				gEngfuncs.Image_Process( &pic, 0, 0, IMAGE_FORCE_RGBA, 0.0f );

			if( pic && pic->buffer )
			{
				int texnum = R_Textures_UploadRGBA( tx->name, pic->width, pic->height, pic->buffer, 0 );
				tx->gl_texturenum = texnum;
				gEngfuncs.FS_FreeImage( pic );
				if( texnum > 0 && g_remixTextures[texnum].matHandle )
					return g_remixTextures[texnum].matHandle;
			}
			else if( pic )
			{
				gEngfuncs.FS_FreeImage( pic );
			}
		}
	}

	return s_defaultMaterial;
}

/*
 * R_Textures_GetOrCreateForStudio - Decodes 8-bit paletted studio texture and creates OpenRemix material
 */
remixapi_MaterialHandle R_Textures_GetOrCreateForStudio( mstudiotexture_t *ptexture, const studiohdr_t *phdr )
{
	if( !ptexture || !phdr )
		return s_defaultMaterial;

	// After loading, index holds our texture number. While loading (s_studioModelName set)
	// it is still the offset of the pixel data, which for small models is a valid-looking
	// texture number of something else entirely (HEV textures on headcrabs): never trust it then
	if( !s_studioModelName[0] && ptexture->index > 0 && ptexture->index < MAX_REMIX_TEXTURES )
	{
		if( g_remixTextures[ptexture->index].matHandle )
			return g_remixTextures[ptexture->index].matHandle;
	}

	char texName[192];
	if( s_studioModelName[0] )
	{
		char base[64];
		COM_FileBase( ptexture->name, base, sizeof( base ) );
		Q_snprintf( texName, sizeof( texName ), "#%s/%s.mdl", s_studioModelName, base );
	}
	else
	{
		Q_strncpy( texName, ptexture->name, sizeof( texName ) );
	}

	int existing = R_Textures_FindExact( texName );
	if( existing > 0 && g_remixTextures[existing].matHandle )
	{
		ptexture->index = existing;
		return g_remixTextures[existing].matHandle;
	}

	if( ptexture->width <= 0 || ptexture->height <= 0 )
		return s_defaultMaterial;

	// Decode 8-bit paletted pixels into RGBA32
	const byte *pixels = (const byte *)phdr + ptexture->index;
	const byte *palette = (const byte *)phdr + ptexture->index + ( ptexture->width * ptexture->height );

	uint32_t numPixels = (uint32_t)( ptexture->width * ptexture->height );
	byte *rgba = (byte *)Mem_Malloc( r_temppool, numPixels * 4 );
	if( !rgba )
		return s_defaultMaterial;

	const qboolean isMasked = ( ptexture->flags & STUDIO_NF_MASKED ) != 0;
	for( uint32_t i = 0; i < numPixels; i++ )
	{
		byte palIdx = pixels[i];
		rgba[i * 4 + 0] = palette[palIdx * 3 + 0];
		rgba[i * 4 + 1] = palette[palIdx * 3 + 1];
		rgba[i * 4 + 2] = palette[palIdx * 3 + 2];
		rgba[i * 4 + 3] = ( isMasked && palIdx == 255 ) ? 0 : 255;
	}

	int texnum = R_Textures_UploadRGBA( texName, (uint32_t)ptexture->width, (uint32_t)ptexture->height, rgba, ptexture->flags );
	Mem_Free( rgba );

	ptexture->index = texnum;

	if( texnum > 0 && g_remixTextures[texnum].matHandle )
		return g_remixTextures[texnum].matHandle;

	return s_defaultMaterial;
}
