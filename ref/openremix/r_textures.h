/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_textures.h - Master texture & material pipeline header
 */

#pragma once
#ifndef R_TEXTURES_H
#define R_TEXTURES_H

#include "r_local.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_REMIX_TEXTURES 4096

typedef struct remix_texture_s
{
	char                    name[64];
	uint32_t                width;
	uint32_t                height;
	uint32_t                flags;
	uint64_t                texHash;
	remixapi_TextureHandle  texHandle;

	uint64_t                matHash;
	remixapi_MaterialHandle matHandle;

	// Additive sprite variant: alpha = brightness, self-illuminated (beams, glows)
	remixapi_TextureHandle  fxTexHandle;
	remixapi_MaterialHandle fxMatHandle;

	// Self-illuminated material on the original texture (additive brushes), created on demand
	remixapi_MaterialHandle emisMatHandle;

	// Emission mask generated from the texture's brightest areas
	remixapi_TextureHandle  lumaTexHandle;

	qboolean                registered;
	qboolean                isCutout;
	qboolean                isWater;
	qboolean                isTranslucent;
	qboolean                isGlass;
	qboolean                isSky;

	// Mean colour, linear (sky environment light)
	float                   avgColor[3];
} remix_texture_t;

extern remix_texture_t g_remixTextures[MAX_REMIX_TEXTURES];

void                    R_Textures_Init( void );
void                    R_Textures_Shutdown( void );
void                    R_Textures_ClearMap( void );

qboolean                R_Textures_IsGlass( const char *name );
remixapi_MaterialHandle R_Textures_GetGlassMaterial( void );
qboolean                R_Textures_GlassTextured( void );
qboolean                R_Textures_GetWaterMedium( float color[3], float *distance );
float                   R_Textures_GlassOpacity( void );
int                     R_Textures_Find( const char *name );
const char             *R_Textures_GetName( unsigned int texnum );
remixapi_MaterialHandle R_Textures_GetMaterial( unsigned int texnum );
remixapi_MaterialHandle R_Textures_GetFxMaterial( unsigned int texnum );
remixapi_MaterialHandle R_Textures_GetEmissiveMaterial( unsigned int texnum );
remixapi_MaterialHandle R_Textures_CreateSkyMaterial( unsigned int texnum, float brightness, qboolean repeat );
qboolean                R_Textures_GetAverageColor( unsigned int texnum, float color[3] );

// Engine ref_interface_t hooks
int                     GL_LoadTexture_Hook( const char *name, const byte *buf, size_t size, int flags );
int                     GL_LoadTextureFromBuffer_Hook( const char *name, rgbdata_t *pic, texFlags_t flags, qboolean update );
int                     GL_CreateTexture_Hook( const char *name, int width, int height, const void *buffer, texFlags_t flags );
int                     GL_FindTexture_Hook( const char *name );
const char             *GL_TextureName_Hook( unsigned int texnum );
void                    GL_FreeTexture_Hook( unsigned int texnum );
void                    GL_UpdateTexture_Hook( int texnum, int cols, int rows, int width, int height, const byte *buffer, pixformat_t fmt );
void                    Mod_StudioLoadTextures_Hook( model_t *mod, void *data );

// Geometry material bindings
remixapi_MaterialHandle R_Textures_GetOrCreateForBSP( texture_t *tx, const model_t *mod );
remixapi_MaterialHandle R_Textures_GetOrCreateForStudio( mstudiotexture_t *ptexture, const studiohdr_t *phdr );

#ifdef __cplusplus
}
#endif

#endif // R_TEXTURES_H
