/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_textures.c - Master texture and PBR material pipeline implementation
 */

#include "r_local.h"
#include "r_remix.h"
#include "r_textures.h"
#include "r_ui.h"

remix_texture_t g_remixTextures[MAX_REMIX_TEXTURES];
static int      s_nextTexNum = 1;
static remixapi_MaterialHandle s_defaultMaterial = NULL;
static remixapi_TextureHandle  s_defaultTexture  = NULL;
static cvar_t                 *rt_emis           = NULL;

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

void R_Textures_Init( void )
{
	memset( g_remixTextures, 0, sizeof( g_remixTextures ) );
	s_nextTexNum = 1;
	s_defaultMaterial = NULL;
	s_defaultTexture  = NULL;

	if( gEngfuncs.Cvar_Get )
		rt_emis = gEngfuncs.Cvar_Get( "rt_emis", "1.0", FCVAR_ARCHIVE, "Texture light emission multiplier" );

	R_Textures_CreateDefaultMaterial();
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

		entry->registered = false;
	}

	s_nextTexNum = 1;
	R_Textures_CreateDefaultMaterial();
}

void R_Textures_Shutdown( void )
{
	R_Textures_ClearMap();

	if( s_defaultMaterial && g_remix.DestroyMaterial )
	{
		g_remix.DestroyMaterial( s_defaultMaterial );
		s_defaultMaterial = NULL;
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

int R_Textures_Find( const char *name )
{
	if( !name || !*name )
		return 0;

	// 1. Exact match
	for( int i = 1; i < MAX_REMIX_TEXTURES; i++ )
	{
		if( g_remixTextures[i].registered && !Q_stricmp( g_remixTextures[i].name, name ) )
			return i;
	}

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
 * Upload RGBA pixel data to OpenRemix and construct Material
 */
static int R_Textures_UploadRGBA( const char *name, uint32_t width, uint32_t height, const byte *rgbaData, uint32_t flags )
{
	if( !name || width == 0 || height == 0 || !rgbaData )
		return 0;

	// Check if already registered
	int existing = R_Textures_Find( name );
	if( existing > 0 && g_remixTextures[existing].matHandle )
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
	entry->isWater       = ( baseName[0] == '!' || baseName[0] == '*' );
	entry->isSky         = ( !Q_strnicmp( baseName, "sky", 3 ) || !Q_strnicmp( baseName, "scrollsky", 9 ) );
	entry->registered    = true;

	// 1. Create OpenRemix 3D Texture
	uint64_t texHash = R_HashString64( name );
	entry->texHash   = texHash;

	remixapi_TextureInfoGenerateMipsEXT mipsExt;
	memset( &mipsExt, 0, sizeof( mipsExt ) );
	mipsExt.sType    = REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT;
	mipsExt.generate = 1;

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
	texInfo.data      = rgbaData;
	texInfo.dataSize  = (uint64_t)width * height * 4;

	if( g_remix.CreateTexture )
	{
		remixapi_ErrorCode err = g_remix.CreateTexture( &texInfo, &entry->texHandle );
		if( err != REMIXAPI_ERROR_CODE_SUCCESS )
		{
			gEngfuncs.Con_Reportf( S_WARN "ref_openremix: CreateTexture failed for %s (code %u)\n", name, (unsigned)err );
		}
	}

	// 2. Create OpenRemix Material
	remixapi_MaterialInfoOpaqueEXT opaqueExt;
	memset( &opaqueExt, 0, sizeof( opaqueExt ) );
	opaqueExt.sType             = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
	opaqueExt.roughnessConstant = 0.85f;
	opaqueExt.metallicConstant  = 0.0f;
	opaqueExt.opacityConstant   = 1.0f;
	opaqueExt.alphaTestType     = 7; // ALWAYS (0 = NEVER would discard every texel)
	opaqueExt.albedoConstant.x  = 1.0f;
	opaqueExt.albedoConstant.y  = 1.0f;
	opaqueExt.albedoConstant.z  = 1.0f;

	if( entry->isCutout )
	{
		opaqueExt.alphaTestType       = 4; // GREATER
		opaqueExt.alphaReferenceValue = 127;
	}

	wchar_t hashStr[32];
	_snwprintf( hashStr, sizeof( hashStr ) / sizeof( wchar_t ), L"0x%llx", (unsigned long long)texHash );

	uint64_t matHash = texHash ^ 0x9E3779B97F4A7C15ULL;
	entry->matHash   = matHash;

	remixapi_MaterialInfo matInfo;
	memset( &matInfo, 0, sizeof( matInfo ) );
	matInfo.sType         = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
	matInfo.pNext         = &opaqueExt;
	matInfo.hash          = matHash;
	matInfo.albedoTexture = hashStr;
	matInfo.filterMode    = 1; // Linear
	matInfo.wrapModeU     = 1; // Repeat
	matInfo.wrapModeV     = 1; // Repeat

	// Detect light fixtures and screens to enable self-illumination emission in the path tracer
	char lowerBase[64];
	Q_strncpy( lowerBase, baseName, sizeof( lowerBase ) );
	for( char *p = lowerBase; *p; p++ )
	{
		if( *p >= 'A' && *p <= 'Z' )
			*p += ( 'a' - 'A' );
	}

	float emisMultiplier = ( rt_emis ? rt_emis->value : 1.0f );
	if( emisMultiplier < 0.0f ) emisMultiplier = 0.0f;

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
		matInfo.emissiveIntensity       = 40.0f * emisMultiplier;
		matInfo.emissiveTexture         = hashStr;
	}
	else if( isScreen && emisMultiplier > 0.0f )
	{
		matInfo.emissiveColorConstant.x = 1.0f;
		matInfo.emissiveColorConstant.y = 1.0f;
		matInfo.emissiveColorConstant.z = 1.0f;
		matInfo.emissiveIntensity       = 10.0f * emisMultiplier;
		matInfo.emissiveTexture         = hashStr;
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
	}

	// 3. Register with 2D UI system so HUD and menus can draw this texture
	R_UI_RegisterTexture( texnum, name, (int)width, (int)height, rgbaData, REMIXAPI_FORMAT_R8G8B8A8_UNORM );

	return texnum;
}

/*
 * GL_LoadTexture_Hook - Loads texture from disk or raw memory buffer via Engine FS
 */
int GL_LoadTexture_Hook( const char *name, const byte *buf, size_t size, int flags )
{
	if( !name || !*name )
		return 0;

	// Check if already loaded
	int existing = R_Textures_Find( name );
	if( existing > 0 && g_remixTextures[existing].matHandle )
		return existing;

	if( !gEngfuncs.FS_LoadImage )
		return 0;

	gEngfuncs.Image_SetForceFlags( 0 );
	rgbdata_t *pic = gEngfuncs.FS_LoadImage( name, buf, size );
	if( !pic )
		return 0;

	if( pic->type != PF_RGBA_32 && gEngfuncs.Image_Process )
	{
		gEngfuncs.Image_Process( &pic, 0, 0, IMAGE_FORCE_RGBA, 0.0f );
	}

	if( !pic || !pic->buffer || pic->width <= 0 || pic->height <= 0 )
	{
		if( pic && gEngfuncs.FS_FreeImage ) gEngfuncs.FS_FreeImage( pic );
		return 0;
	}

	int texnum = R_Textures_UploadRGBA( name, pic->width, pic->height, pic->buffer, flags );

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
			freedCopy = true;
		}
		else
		{
			workingPic = pic;
		}
	}

	int texnum = R_Textures_UploadRGBA( name, workingPic->width, workingPic->height, workingPic->buffer, flags );

	if( freedCopy && gEngfuncs.FS_FreeImage )
		gEngfuncs.FS_FreeImage( workingPic );

	return texnum;
}

/*
 * GL_CreateTexture_Hook - Direct creation from raw buffer
 */
int GL_CreateTexture_Hook( const char *name, int width, int height, const void *buffer, texFlags_t flags )
{
	if( !buffer || width <= 0 || height <= 0 )
		return 0;

	return R_Textures_UploadRGBA( name, (uint32_t)width, (uint32_t)height, (const byte *)buffer, flags );
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
void Mod_StudioLoadTextures_Hook( model_t *mod, void *data )
{
	studiohdr_t *phdr = (studiohdr_t *)data;
	if( !phdr || phdr->textureindex <= 0 || phdr->numtextures <= 0 )
		return;

	mstudiotexture_t *ptexture = (mstudiotexture_t *)((byte *)phdr + phdr->textureindex);
	for( int i = 0; i < phdr->numtextures; i++ )
	{
		R_Textures_GetOrCreateForStudio( &ptexture[i], phdr );
	}
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
	if( mod && mod->name )
	{
		char mapTexName[128];
		Q_snprintf( mapTexName, sizeof( mapTexName ), "#%s:%s.mip", mod->name, tx->name );
		existing = R_Textures_Find( mapTexName );
		if( existing > 0 && g_remixTextures[existing].matHandle )
		{
			tx->gl_texturenum = existing;
			return g_remixTextures[existing].matHandle;
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

	if( ptexture->index > 0 && ptexture->index < MAX_REMIX_TEXTURES )
	{
		if( g_remixTextures[ptexture->index].matHandle )
			return g_remixTextures[ptexture->index].matHandle;
	}

	int existing = R_Textures_Find( ptexture->name );
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

	int texnum = R_Textures_UploadRGBA( ptexture->name, (uint32_t)ptexture->width, (uint32_t)ptexture->height, rgba, ptexture->flags );
	Mem_Free( rgba );

	ptexture->index = texnum;

	if( texnum > 0 && g_remixTextures[texnum].matHandle )
		return g_remixTextures[texnum].matHandle;

	return s_defaultMaterial;
}
