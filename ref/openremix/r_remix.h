/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_remix.h - Dynamic Loader and Interface Dispatch Table Header
 */

#pragma once
#ifndef R_REMIX_H
#define R_REMIX_H

#include "r_local.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ref_remix_state_s
{
	HMODULE            hRemixDll;
	qboolean           isLoaded;
	qboolean           isStarted;
	wchar_t            dllPath[MAX_PATH];
	wchar_t            dllDir[MAX_PATH];
	uint64_t           apiVersion;
	char               lastError[512];
} ref_remix_state_t;

extern remixapi_Interface g_remix;
extern ref_remix_state_t  g_remixState;

qboolean    R_Remix_LoadLibrary( void );
qboolean    R_Remix_Startup( void *hwnd );
void        R_Remix_Shutdown( void );
void        R_Remix_UnloadLibrary( void );
qboolean    R_Remix_IsLoaded( void );
qboolean    R_Remix_IsStarted( void );
const char *R_Remix_GetErrorString( remixapi_ErrorCode errorCode );
void        R_Remix_ToggleUI( void );
void        R_Remix_UpdateCursorClipping( void );

// Compatibility aliases
qboolean    Remix_LoadAndInitialize( const char *custom_path );
void        Remix_ShutdownAndUnload( void );

#ifdef __cplusplus
}
#endif

#endif // R_REMIX_H
