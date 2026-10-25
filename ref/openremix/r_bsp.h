/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_bsp.h - BSP geometry extraction, batching, and instancing interface
 */

#pragma once
#ifndef R_BSP_H
#define R_BSP_H

#include "r_local.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_BSP_SUBMODELS 512

#ifndef MAX_VISIBLE_PACKET
#define MAX_VISIBLE_PACKET 1024
#endif

typedef struct bsp_submodel_mesh_s
{
	model_t            *model;        // Engine submodel pointer
	char                name[16];     // e.g. "*1", "*2"
	remixapi_MeshHandle meshHandle;   // OpenRemix BLAS mesh handle
	remixapi_MeshHandle sidesMeshHandle; // With the water's sides (EF_WATERSIDES), made on first use
	remixapi_MeshHandle altMeshHandle;   // With the alternate textures (+A...: a pressed button), made on first use
	qboolean            altChecked;      // altMeshHandle was tried (NULL: the submodel has no alternate textures)
	uint64_t            hash;         // Unique 64-bit hash
	qboolean            isGlass;      // True if submodel represents translucent glass
} bsp_submodel_mesh_t;

typedef struct bsp_state_s
{
	remixapi_MeshHandle worldMesh;
	uint64_t            worldMeshHash;

	bsp_submodel_mesh_t submodels[MAX_BSP_SUBMODELS];
	uint32_t            numSubmodels;

	// Entity tracking list for the current frame
	cl_entity_t        *solid_entities[MAX_VISIBLE_PACKET];
	uint32_t            num_solid_entities;
	cl_entity_t        *trans_entities[MAX_VISIBLE_PACKET];
	uint32_t            num_trans_entities;
	cl_entity_t        *beam_entities[MAX_VISIBLE_PACKET]; // server env_beams (ET_BEAM), drawn by CL_DrawBeams
	uint32_t            num_beam_entities;
} bsp_state_t;

extern bsp_state_t g_bspState;

// Subsystem Lifecycle
void        R_BSP_Init( void );
void        R_BSP_Shutdown( void );
void        R_BSP_NewMap( model_t *worldmodel );
void        R_BSP_Teardown( void );

// Frame Rendering & Instancing
void        R_BSP_DrawWorld( void );
void        R_BSP_DrawBrushEntity( cl_entity_t *ent );
void        R_BSP_DrawEntities( void );
// The point is inside a drawn water brush entity (func_water), below its surface
qboolean    R_BSP_PointInWaterEntity( const vec3_t p );

// Entity List Collection (invoked by R_AddEntity)
qboolean    R_BSP_AddEntity( cl_entity_t *clent, int type );
void        R_BSP_ClearEntities( void );

// Submodel Query
remixapi_MeshHandle R_BSP_GetSubmodelMesh( model_t *mod, qboolean waterSides, qboolean alternate );

#ifdef __cplusplus
}
#endif

#endif // R_BSP_H
