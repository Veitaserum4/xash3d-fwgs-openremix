/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_lights.h - Light parsing, dynamic lights, flashlights, and volumetric lighting
 */

#pragma once
#ifndef R_LIGHTS_H
#define R_LIGHTS_H

#include "r_local.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_STATIC_MAP_LIGHTS 2048
#define MAX_ACTIVE_DLIGHTS    128
#define R_OPENREMIX_MAX_DLIGHTS 32

typedef struct parsed_light_s
{
	vec3_t               origin;
	vec3_t               direction;
	remixapi_Float3D     radiance;
	float                intensity;
	float                radius;
	qboolean             is_spot;
	float                cone_angle;
	float                cone_softness;
	int                  style;
	uint64_t             hash;
	remixapi_LightHandle handle;
} parsed_light_t;

typedef struct lights_state_s
{
	parsed_light_t       static_lights[MAX_STATIC_MAP_LIGHTS];
	uint32_t             static_lights_count;

	qboolean             has_sun;
	remixapi_Float3D     sun_radiance;
	vec3_t               sun_direction;
	float                sun_diameter;
	uint64_t             sun_hash;
	remixapi_LightHandle sun_handle;

	// Dynamic flashlight
	remixapi_LightHandle flashlight_handle;
	qboolean             flashlight_active;

	// Dynamic light pool
	remixapi_LightHandle dlight_handles[MAX_ACTIVE_DLIGHTS];
} lights_state_t;

extern lights_state_t g_lights;

// Subsystem API
void R_Lights_Init( void );
void R_Lights_Shutdown( void );
void R_Lights_NewMap( const char *entities );
void R_Lights_DrawFrame( void );
void R_Lights_UpdateVolumetrics( void );

#ifdef __cplusplus
}
#endif

#endif // R_LIGHTS_H
