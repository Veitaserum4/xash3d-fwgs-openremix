/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_xashrt.h - XashRT content support (rt/ folder from the xash-rt resources package):
 *              texture material overrides, KTX2 replacement textures, per-map glTF scenes
 */

#ifndef R_XASHRT_H
#define R_XASHRT_H

#include "r_local.h"

#ifdef __cplusplus
extern "C" {
#endif

// Per-texture material overrides from rt/data/textures.json
typedef struct xrt_material_s
{
	qboolean mirror;         // perfect chrome (metallic 1, roughness 0)
	qboolean glass;          // ray-traced glass
	qboolean water;          // water body
	qboolean opaque;         // forceOpaque: liquids that glow (slime), not water bodies
	float    metallic;       // < 0 when unset
	float    roughness;      // < 0 when unset
	float    emissive;       // emission strength (already scaled), < 0 when unset
} xrt_material_t;

void     R_XRT_Init( void );
void     R_XRT_Shutdown( void );

// Texture overrides; texname is the engine texture name (e.g. "#maps/c1a0.bsp:lab1_cmpm3.mip")
qboolean R_XRT_Active( void );
qboolean R_XRT_GetMaterial( const char *texname, xrt_material_t *out );
// Loads rt/mat/<key><suffix>.ktx2 once and writes its Remix texture hash path ("0x...") to out
qboolean R_XRT_GetReplacementTexture( const char *texname, const char *suffix, wchar_t *out, size_t outLen );
// A file under rt/ (next to the executable), NUL-terminated; free() it. NULL if absent
byte    *R_XRT_ReadFile( const char *path, size_t *outSize );
// Loads any KTX2 under rt/ (e.g. "rt/WaterNormal_n.ktx2") once
qboolean R_XRT_GetFileTexture( const char *path, wchar_t *out, size_t outLen );
void     R_XRT_TextureKey( const char *texname, char *out, size_t outSize );

// Per-map glTF scene (mirrors, added geometry, hand-placed lights)
void     R_XRT_NewMap( const model_t *worldmodel );
void     R_XRT_DrawScene( void );

#ifdef __cplusplus
}
#endif

#endif // R_XASHRT_H
