/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_local.h - Master internal renderer definitions & declarations
 */

#pragma once
#ifndef R_LOCAL_H
#define R_LOCAL_H

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include "port.h"

// Remix C API headers
#include "remix/remix_c.h"
#include "remix/remix_native_ext.h"

// Xash3D engine headers
#include "xash3d_types.h"
#include "const.h"
#include "cvardef.h"
#include "com_strings.h"
#include "crtlib.h"
#include "enginefeatures.h"
#ifndef PM_GLASS_IGNORE
#define PM_GLASS_IGNORE 0x00000004
#endif
#ifndef PM_STUDIO_IGNORE
#define PM_STUDIO_IGNORE 0x00000008
#endif
#include "ref_params.h"
#include "pmove.h"
#include "xash3d_mathlib.h"
#include "ref_api.h"
#include "com_model.h"
#include "studio.h"
#include "r_efx.h"

#ifndef M_PI_F
#define M_PI_F 3.14159265358979323846f
#endif

//
// Engine Global Variables (cached in GetRefAPI)
//
extern ref_api_t          gEngfuncs;
extern ref_globals_t     *gpGlobals;
extern ref_client_t      *gp_cl;
extern ref_host_t        *gp_host;
extern struct movevars_s *gp_movevars;
extern dlight_t          *gp_dlights;

// Engine shared cvars
DECLARE_ENGINE_SHARED_CVAR_LIST()

//
// Renderer Memory Management
//
extern poolhandle_t r_temppool;

#define Mem_Malloc( pool, size )       gEngfuncs._Mem_Alloc( pool, size, false, __FILE__, __LINE__ )
#define Mem_Calloc( pool, size )       gEngfuncs._Mem_Alloc( pool, size, true, __FILE__, __LINE__ )
#define Mem_Realloc( pool, ptr, size ) gEngfuncs._Mem_Realloc( pool, ptr, size, true, __FILE__, __LINE__ )
#define Mem_Free( mem )                gEngfuncs._Mem_Free( mem, __FILE__, __LINE__ )
#define Mem_AllocPool( name )          gEngfuncs._Mem_AllocPool( name, 0, __FILE__, __LINE__ )
#define Mem_FreePool( pool )           gEngfuncs._Mem_FreePool( pool, __FILE__, __LINE__ )

#define Assert( x ) if( !( x )) gEngfuncs.Host_Error( "assert failed at %s:%i\n", __FILE__, __LINE__ )
#define ENGINE_GET_PARM_ (*gEngfuncs.EngineGetParm)
#define ENGINE_GET_PARM( parm ) ENGINE_GET_PARM_( (parm), 0 )

//
// Renderer Global State
//
typedef struct ref_state_s
{
	qboolean                 initialized;
	uint32_t                 framecount;
	double                   realtime;
	double                   frametime;
	model_t                 *worldmodel;
	struct world_static_s   *world;
	color24                 *palette;
	cl_entity_t             *viewent;
	byte                    *texgammatable;
	uint16_t                *lightgammatable;
	uint16_t                *screengammatable;
	uint16_t                *lineargammatable;
	dlight_t                *elights;
	qboolean                 is2DMode;
	float                    offset2d[2];
	float                    blend;
	qboolean                 fFlipViewModel;
	int                      framenum;
	cl_entity_t             *entities;
	uint32_t                 max_entities;
} ref_state_t;

extern ref_state_t tr;

//
// Current Viewpass & Render Instance State
//
typedef struct ref_instance_s
{
	ref_viewpass_t           rvp;
	vec3_t                   vieworg;
	vec3_t                   viewangles;
	vec3_t                   vforward;
	vec3_t                   vright;
	vec3_t                   vup;
	float                    fov_x;
	float                    fov_y;
	int                      viewport[4];
	cl_entity_t             *currententity;
	model_t                 *currentmodel;
	matrix4x4                objectMatrix;
} ref_instance_t;

extern ref_instance_t RI;

// Helpers
#define CL_IsViewEntityLocalPlayer() ( gp_cl && ( gp_cl->viewentity == ( gp_cl->playernum + 1 ) ) )
int CL_FxBlend( cl_entity_t *e );

static inline cl_entity_t *CL_GetEntityByIndex( int index )
{
	if( index < 0 || !tr.entities || (uint32_t)index >= tr.max_entities )
		return NULL;

	return &tr.entities[index];
}

// Subsystem Headers
#include "r_remix.h"
#include "r_textures.h"
#include "r_bsp.h"
#include "r_studio.h"
#include "r_ui.h"
#include "r_sprite.h"
#include "r_lights.h"

//
// Subsystem Function Prototypes
//

// r_main.c - Lifecycle & Frame Orchestration
qboolean    R_Init( void );
void        R_Shutdown( void );
void        R_NewMap( void );
void        R_BeginFrame( qboolean clearScene );
void        R_RenderScene( void );
void        R_EndFrame( void );
void        R_Set2DMode( qboolean enable );
void        R_Set2DOffset( float x, float y );
void        R_ClearScreen( void );
void        R_SetupCamera( const ref_viewpass_t *rvp );
void        GL_RenderFrame( const struct ref_viewpass_s *rvp );
void        GL_BackendStartFrame( void );
void        GL_BackendEndFrame( void );
const char *R_GetConfigName( void );

// r_context.c - Master Function Table & Export
extern const ref_interface_t gReffuncs;
int EXPORT GetRefAPI( int version, ref_interface_t *funcs, ref_api_t *engfuncs, ref_globals_t *globals );

#endif // R_LOCAL_H
