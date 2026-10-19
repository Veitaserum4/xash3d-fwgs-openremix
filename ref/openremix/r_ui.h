/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_ui.h - 2D HUD, Menus & UI Compositing Subsystem Header
 */

#pragma once
#ifndef R_UI_H
#define R_UI_H

#include "r_local.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_UI_VERTICES 32768
#define MAX_UI_INDICES  65536
#define MAX_UI_COMMANDS 2048
#define MAX_UI_TEXTURES 4096

// Texture entry metadata
typedef struct ui_texture_entry_s
{
	qboolean         registered;
	uint32_t         width;
	uint32_t         height;
	remixapi_Format  format;
	char             name[MAX_QPATH];
} ui_texture_entry_t;

// Complete 2D UI Subsystem State
typedef struct ui_state_s
{
	qboolean                 initialized;
	remixapi_UIVertex        vertices[MAX_UI_VERTICES];
	uint32_t                 vertexCount;
	uint32_t                 indices[MAX_UI_INDICES];
	uint32_t                 indexCount;
	remixapi_UIDrawCommand   commands[MAX_UI_COMMANDS];
	uint32_t                 commandCount;

	// Active drawing state
	uint32_t                 currentColor;     // Packed 0xAABBGGRR
	remixapi_UITextureHandle currentTextureId; // 0 = white quad
	uint32_t                 currentFlags;     // Blend & filter flags

	// Texture registry table
	ui_texture_entry_t       textures[MAX_UI_TEXTURES];
} ui_state_t;

extern ui_state_t g_ui;

// Subsystem Lifecycle
void     R_UI_Init( void );
void     R_UI_Shutdown( void );
void     R_UI_BeginFrame( void );
void     R_UI_EndFrame( void );
void     R_UI_Clear( void );

// Texture Management Hooks
int      R_UI_RegisterTexture( int texnum, const char *name, int width, int height, const void *pixels, remixapi_Format format );
void     R_UI_UpdateTextureRegion( int texnum, int x, int y, int width, int height, const void *pixels, remixapi_Format format, int rowPitch );
void     R_UI_FreeTexture( int texnum );
qboolean R_UI_GetTextureDimensions( int texnum, int *width, int *height );

// Engine Callback Implementations
void     R_UI_Set2DMode( qboolean enable );
void     R_UI_Set2DOffset( float x, float y );
void     R_UI_DrawStretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, int texnum );
void     R_UI_FillRGBA( int rendermode, float x, float y, float w, float h, byte r, byte g, byte b, byte a );
void     R_UI_DrawPoly( const float *verts, int numverts, int texnum );
void     R_UI_DrawCharacter( int x, int y, int number, int r, int g, int b, int texnum, int charW, int charH );

// 2D TriAPI Emulation Hooks
void     R_UI_TriRenderMode( int mode );
void     R_UI_TriBegin( int primitiveCode );
void     R_UI_TriEnd( void );
void     R_UI_TriColor4ub( byte r, byte g, byte b, byte a );
void     R_UI_TriColor4f( float r, float g, float b, float a );
void     R_UI_TriTexCoord2f( float u, float v );
void     R_UI_TriVertex3f( float x, float y, float z );
void     R_UI_TriVertex3fv( const float *worldPnt );

#ifdef __cplusplus
}
#endif

#endif // R_UI_H
