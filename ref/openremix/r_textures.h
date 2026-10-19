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

	qboolean                registered;
	qboolean                isCutout;
	qboolean                isWater;
	qboolean                isTranslucent;
	qboolean                isSky;
} remix_texture_t;

extern remix_texture_t g_remixTextures[MAX_REMIX_TEXTURES];

void                    R_Textures_Init( void );
void                    R_Textures_Shutdown( void );
void                    R_Textures_ClearMap( void );

int                     R_Textures_Find( const char *name );
const char             *R_Textures_GetName( unsigned int texnum );
remixapi_MaterialHandle R_Textures_GetMaterial( unsigned int texnum );

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
