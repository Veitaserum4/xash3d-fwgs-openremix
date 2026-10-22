/*
 * r_sky.h - Skybox for OpenRemix's sky camera
 */

#ifndef R_SKY_H
#define R_SKY_H

#include "r_local.h"

void R_Sky_Init( void );
void R_Sky_Setup( int *skyboxTextures ); // ref_interface_t R_SetupSky; NULL unloads
void R_Sky_SetClouds( int solidskyTexture, int alphaskyTexture ); // ref_interface_t R_SetSkyCloudsTextures
void R_Sky_Shutdown( void );
void R_Sky_SetupCamera( const remixapi_CameraInfoParameterizedEXT *world );
void R_Sky_Draw( void );
qboolean R_Sky_GetEnvironment( float radiance[3] );

#endif // R_SKY_H
