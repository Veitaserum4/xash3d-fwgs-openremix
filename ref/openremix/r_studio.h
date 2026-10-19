/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_studio.h - Studio model animation & rendering declarations
 */

#pragma once
#ifndef R_STUDIO_H
#define R_STUDIO_H

#include "r_local.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_STUDIO_SCRATCH_VERTS   (MAXSTUDIOVERTS * 4)
#define MAX_STUDIO_SCRATCH_INDICES (MAXSTUDIOVERTS * 6)
#define EVENT_CLIENT               5000

typedef struct player_model_s
{
	char    name[MAX_OSPATH];
	char    modelname[MAX_OSPATH];
	model_t *model;
} player_model_t;

// Studio drawing state
typedef struct studio_draw_state_s
{
	double               time;
	double               frametime;
	int                  framecount;
	qboolean             interpolate;
	int                  rendermode;
	float                blend;

	// Bone transforms (model space -> world space)
	matrix3x4            rotationmatrix;
	matrix3x4            bonestransform[MAXSTUDIOBONES];
	matrix3x4            lighttransform[MAXSTUDIOBONES];

	// Cached bones for follow/blending
	matrix3x4            cached_bonestransform[MAXSTUDIOBONES];
	matrix3x4            cached_lighttransform[MAXSTUDIOBONES];
	char                 cached_bonenames[MAXSTUDIOBONES][32];
	int                  cached_numbones;

	// Evaluated vertex geometry
	vec3_t               verts[MAXSTUDIOVERTS];
	vec3_t               norms[MAXSTUDIOVERTS];
	vec2_t               chrome[MAXSTUDIOVERTS];
	int                  normaltable[MAXSTUDIOVERTS];

	// Player model management
	player_model_t       player_models[MAX_CLIENTS];
} studio_draw_state_t;

extern studio_draw_state_t g_studio;

// Subsystem API functions
void     R_StudioInit( void );
void     R_StudioShutdown( void );
void     R_StudioNewMap( void );
void     R_DrawStudioModel( cl_entity_t *e );
void     R_DrawViewModel( void );

// Engine callback interface
int      R_StudioDrawModel( int flags );
int      R_StudioDrawPlayer( int flags, entity_state_t *pplayer );
float    R_StudioEstimateFrame( cl_entity_t *e, mstudioseqdesc_t *pseqdesc, double time );
void     R_StudioLerpMovement( cl_entity_t *e, double time, vec3_t origin, vec3_t angles );
qboolean R_StudioFillAPI( struct engine_studio_api_s *api, struct r_studio_interface_s *pDefaultDraw );
void     R_StudioSetDrawInterface( struct r_studio_interface_s *pDraw );

#ifdef __cplusplus
}
#endif

#endif // R_STUDIO_H
