/*
 * r_decals.h - Decals on brush surfaces (blood, bullet holes, scorch marks, sprays)
 */
#ifndef R_DECALS_H
#define R_DECALS_H

#include "r_local.h"
#include "r_remix.h"

void R_Decals_Init( void );
void R_Decals_NewMap( void );
// Draws a brush model's decals: the world (identity transform) or a brush entity's
void R_Decals_DrawModel( const model_t *mod, const remixapi_Transform *transform, uint64_t instanceId, qboolean isWorld );

// Engine ref_interface_t hooks
void R_DecalShoot( int textureIndex, int entityIndex, int modelIndex, vec3_t pos, int flags, float scale );
void R_DecalRemoveAll( int textureIndex );
int  R_CreateDecalList( struct decallist_s *pList );
void R_ClearAllDecals( qboolean includePermanent );

#endif // R_DECALS_H
