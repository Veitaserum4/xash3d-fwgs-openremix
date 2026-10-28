/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_sprite.h - Sprites, Beams & Temp Entities Subsystem Header
 */

#pragma once
#ifndef R_SPRITE_H
#define R_SPRITE_H

#include "r_local.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_DYNAMIC_MESH_SLOTS 512

// Subsystem Lifecycle
void     R_Sprite_Init( void );
void     R_Sprite_Shutdown( void );
void     R_Sprite_NewMap( void );
void     R_Sprite_BeginFrame( void );

// Sprite Model Drawing Callback
void     R_DrawSpriteModel( cl_entity_t *e );

// Beams Drawing Callbacks
void     CL_DrawBeams( int fTrans, BEAM *active_beams );
void     R_BeamDraw( BEAM *pbeam, float frametime );

// Particles & Tracers Callbacks
void     CL_DrawParticles( double frametime, particle_t *particles, float partsize );
void     CL_DrawTracers( double frametime, particle_t *tracers );

// Occlusion and glare blend calculation
qboolean R_SpriteOccluded( cl_entity_t *e, vec3_t origin, float *pscale );

// Dynamic Mesh Buffer Slot
typedef struct dynamic_mesh_slot_s
{
	remixapi_MeshHandle   handle;
	uint64_t              hash;
} dynamic_mesh_slot_t;

dynamic_mesh_slot_t    *R_AllocDynamicMeshSlot( void );
remixapi_MaterialHandle R_GetSpriteMaterial( int texnum );
int                     R_Sprite_TracedClass( void );
void                    R_SetupSpriteBlendState( remixapi_InstanceInfoBlendEXT *pBlend,
                                                 remixapi_InstanceIdentityEXT *pIdent,
                                                 remixapi_InstanceRasterStateEXT *pRaster,
                                                 uint64_t instanceId,
                                                 int rendermode );

extern unsigned int g_currentBoundTexture;

#ifdef __cplusplus
}
#endif

#endif // R_SPRITE_H
