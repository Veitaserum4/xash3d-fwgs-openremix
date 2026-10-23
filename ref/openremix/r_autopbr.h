/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_autopbr.h - PBR values and normal maps inferred for textures without authored ones
 */

#pragma once
#ifndef R_AUTOPBR_H
#define R_AUTOPBR_H

#include "r_local.h"

#ifdef __cplusplus
extern "C" {
#endif

void     R_AutoPBR_Init( void );
qboolean R_AutoPBR_Enabled( void );

// Texture material types from sound/materials.txt (footstep types: M metal, V vent, ...)
void     R_AutoPBR_ClearTypes( void );
void     R_AutoPBR_AddType( char type, const char *name );

// Metallic and roughness for a texture (its base name; studio: a model's texture), from
// materials.txt, else its name. False when neither says. `bumps`: whether a normal map
// generated from its image suits it (not lettering, screens or pictures)
qboolean R_AutoPBR_Classify( const char *baseName, qboolean studio, float *metallic, float *roughness, qboolean *bumps );

// Tangent-space (OpenGL) normal map from the image's brightness as height, at rt_autopbr_normals
// strength; uploaded as `hash`, its texture path written to `path`. False when off or failed
qboolean R_AutoPBR_CreateNormalMap( const byte *rgba, uint32_t width, uint32_t height, uint64_t hash,
                                    remixapi_TextureHandle *handle, wchar_t *path, size_t pathLen );

#ifdef __cplusplus
}
#endif

#endif // R_AUTOPBR_H
