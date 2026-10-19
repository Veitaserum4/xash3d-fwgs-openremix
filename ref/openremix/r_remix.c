/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_remix.c - Dynamic Loader and Handshake Implementation
 */

#include "r_local.h"
#include "r_remix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Global dispatch table instance and loader state
remixapi_Interface g_remix;
ref_remix_state_t  g_remixState;

static cvar_t *r_remix_path = NULL;

static const wchar_t *g_candidatePaths[] = {
	// Game directory paths
	L"remix.dll",
	L"bin\\remix.dll",
	L"ref\\remix.dll",
	L"cl_dlls\\remix.dll",

	// Development workspace relative and absolute paths
	L"_work\\packages\\windows\\bin\\remix.dll",
	L"..\\..\\UnityRTX\\OpenRemix\\_work\\packages\\windows\\bin\\remix.dll",
	L"..\\UnityRTX\\OpenRemix\\_work\\packages\\windows\\bin\\remix.dll",
	L"UnityRTX\\OpenRemix\\_work\\packages\\windows\\bin\\remix.dll",
	L"..\\..\\OpenRemix\\_work\\packages\\windows\\bin\\remix.dll",
	L"c:\\Users\\Jacob\\Desktop\\UnityRTX\\OpenRemix\\_work\\packages\\windows\\bin\\remix.dll",

	NULL
};

const char *R_Remix_GetErrorString( remixapi_ErrorCode errorCode )
{
	switch( errorCode )
	{
	case REMIXAPI_ERROR_CODE_SUCCESS:
		return "Success";
	case REMIXAPI_ERROR_CODE_GENERAL_FAILURE:
		return "General failure";
	case REMIXAPI_ERROR_CODE_LOAD_LIBRARY_FAILURE:
		return "Failed to load library";
	case REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS:
		return "Invalid arguments or structure mismatch";
	case REMIXAPI_ERROR_CODE_GET_PROC_ADDRESS_FAILURE:
		return "Failed to find remixapi_InitializeLibrary entry point";
	case REMIXAPI_ERROR_CODE_ALREADY_EXISTS:
		return "Remix runtime already initialized";
	case REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION:
		return "Incompatible Remix API version (expected 0.1003.0)";
	case REMIXAPI_ERROR_CODE_SET_DLL_DIRECTORY_FAILURE:
		return "Failed to set DLL search directory";
	case REMIXAPI_ERROR_CODE_GET_FULL_PATH_NAME_FAILURE:
		return "Failed to resolve absolute path";
	case REMIXAPI_ERROR_CODE_NOT_INITIALIZED:
		return "Remix runtime not initialized";
	case REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE:
		return "Requested feature unavailable on this platform";
	case REMIXAPI_ERROR_CODE_WRONG_THREAD:
		return "Call executed from wrong thread (must run on window thread)";
	case REMIXAPI_ERROR_CODE_HRESULT_NO_REQUIRED_GPU_FEATURES:
		return "GPU lacks required hardware features (Vulkan 1.3 / Ray Query required)";
	case REMIXAPI_ERROR_CODE_HRESULT_DRIVER_VERSION_BELOW_MINIMUM:
		return "GPU graphics driver version is below minimum requirement";
	case REMIXAPI_ERROR_CODE_HRESULT_DXVK_INSTANCE_EXTENSION_FAIL:
		return "Failed to query required Vulkan instance extensions";
	case REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_INSTANCE_FAIL:
		return "vkCreateInstance failed (Vulkan loader or driver unavailable)";
	case REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_DEVICE_FAIL:
		return "vkCreateDevice failed (GPU rejected device creation)";
	case REMIXAPI_ERROR_CODE_HRESULT_GRAPHICS_QUEUE_FAMILY_MISSING:
		return "Vulkan physical device has no graphics queue family";
	default:
		return "Unknown Remix error";
	}
}

static qboolean FileExistsW( const wchar_t *path )
{
	DWORD attrib = GetFileAttributesW( path );
	return (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY));
}

static qboolean DirectoryExistsW( const wchar_t *path )
{
	DWORD attrib = GetFileAttributesW( path );
	return (attrib != INVALID_FILE_ATTRIBUTES && (attrib & FILE_ATTRIBUTE_DIRECTORY));
}

static qboolean R_Remix_FindLibraryPath( const char *customPath, wchar_t *outPath, size_t maxLen, wchar_t *outDir, size_t maxDirLen )
{
	wchar_t tempPath[MAX_PATH];

	// Register cvar if not registered yet
	if( !r_remix_path && gEngfuncs.Cvar_Get )
	{
		r_remix_path = gEngfuncs.Cvar_Get( "r_remix_path", "", FCVAR_ARCHIVE, "Path to remix.dll or directory containing it" );
	}

	// Priority 0: Explicit custom path argument
	if( customPath && customPath[0] != '\0' )
	{
		MultiByteToWideChar( CP_UTF8, 0, customPath, -1, tempPath, MAX_PATH );
		if( FileExistsW( tempPath ) )
		{
			wcsncpy( outPath, tempPath, maxLen );
			goto Found;
		}
		if( DirectoryExistsW( tempPath ) )
		{
			_snwprintf( outPath, maxLen, L"%s\\remix.dll", tempPath );
			if( FileExistsW( outPath ) )
				goto Found;
		}
	}

	// Priority 1: User cvar r_remix_path or r_openremix_path
	if( r_remix_path && r_remix_path->string && r_remix_path->string[0] != '\0' )
	{
		MultiByteToWideChar( CP_UTF8, 0, r_remix_path->string, -1, tempPath, MAX_PATH );
		if( FileExistsW( tempPath ) )
		{
			wcsncpy( outPath, tempPath, maxLen );
			goto Found;
		}
		if( DirectoryExistsW( tempPath ) )
		{
			_snwprintf( outPath, maxLen, L"%s\\remix.dll", tempPath );
			if( FileExistsW( outPath ) )
				goto Found;
		}
		gEngfuncs.Con_Printf( S_WARN "ref_openremix: r_remix_path '%s' not found, falling back...\n", r_remix_path->string );
	}

	if( gEngfuncs.pfnGetCvarString )
	{
		const char *openremixCvar = gEngfuncs.pfnGetCvarString( "r_openremix_path" );
		if( openremixCvar && openremixCvar[0] != '\0' )
		{
			MultiByteToWideChar( CP_UTF8, 0, openremixCvar, -1, tempPath, MAX_PATH );
			if( FileExistsW( tempPath ) )
			{
				wcsncpy( outPath, tempPath, maxLen );
				goto Found;
			}
			if( DirectoryExistsW( tempPath ) )
			{
				_snwprintf( outPath, maxLen, L"%s\\remix.dll", tempPath );
				if( FileExistsW( outPath ) )
					goto Found;
			}
		}
	}

	// Priority 2: Environment variable REMIX_PATH
	const char *envRemix = getenv( "REMIX_PATH" );
	if( envRemix && envRemix[0] != '\0' )
	{
		MultiByteToWideChar( CP_UTF8, 0, envRemix, -1, tempPath, MAX_PATH );
		if( FileExistsW( tempPath ) )
		{
			wcsncpy( outPath, tempPath, maxLen );
			goto Found;
		}
		if( DirectoryExistsW( tempPath ) )
		{
			_snwprintf( outPath, maxLen, L"%s\\remix.dll", tempPath );
			if( FileExistsW( outPath ) )
				goto Found;
		}
		gEngfuncs.Con_Printf( S_WARN "ref_openremix: REMIX_PATH '%s' not found, falling back...\n", envRemix );
	}

	// Priority 3 & 4: Candidate relative and workspace paths
	for( int i = 0; g_candidatePaths[i] != NULL; i++ )
	{
		if( FileExistsW( g_candidatePaths[i] ) )
		{
			wcsncpy( outPath, g_candidatePaths[i], maxLen );
			goto Found;
		}
	}

	return false;

Found:
	// Normalize to absolute path
	{
		wchar_t absPath[MAX_PATH];
		DWORD ret = GetFullPathNameW( outPath, MAX_PATH, absPath, NULL );
		if( ret > 0 && ret < MAX_PATH )
		{
			wcsncpy( outPath, absPath, maxLen );
		}
	}

	// Extract parent directory
	wcsncpy( outDir, outPath, maxDirLen );
	for( int i = (int)wcslen( outDir ) - 1; i >= 0; i-- )
	{
		if( outDir[i] == L'\\' || outDir[i] == L'/' )
		{
			outDir[i] = L'\0';
			break;
		}
	}

	return true;
}

qboolean R_Remix_LoadLibrary( void )
{
	return Remix_LoadAndInitialize( NULL );
}

qboolean Remix_LoadAndInitialize( const char *custom_path )
{
	if( g_remixState.isLoaded )
		return true;

	wchar_t dllPath[MAX_PATH] = { 0 };
	wchar_t dllDir[MAX_PATH] = { 0 };

	if( !R_Remix_FindLibraryPath( custom_path, dllPath, MAX_PATH, dllDir, MAX_PATH ) )
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_openremix: Could not locate remix.dll!\n"
		                      "Please set 'r_remix_path' or REMIX_PATH environment variable to remix.dll location.\n" );
		return false;
	}

	gEngfuncs.Con_Printf( S_NOTE "ref_openremix: Found remix.dll at: %ls\n", dllPath );

	// Save existing DLL directory to restore later
	wchar_t savedDllDir[MAX_PATH] = { 0 };
	GetDllDirectoryW( MAX_PATH, savedDllDir );

	// Point DLL search to remix.dll directory so secondary dependencies (e.g. SDL3.dll) load
	SetDllDirectoryW( dllDir );

	HMODULE hDll = LoadLibraryExW( dllPath, NULL,
	                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
	                              LOAD_LIBRARY_SEARCH_DEFAULT_DIRS );
	if( !hDll )
	{
		hDll = LoadLibraryW( dllPath );
	}

	// Restore original DLL search directory
	SetDllDirectoryW( savedDllDir );

	if( !hDll )
	{
		DWORD err = GetLastError();
		gEngfuncs.Con_Printf( S_ERROR "ref_openremix: LoadLibrary failed for '%ls' (Win32 error: %lu)\n", dllPath, err );
		return false;
	}

	// Resolve initialize entry point
	PFN_remixapi_InitializeLibrary pfn_InitializeLibrary =
		(PFN_remixapi_InitializeLibrary)GetProcAddress( hDll, "remixapi_InitializeLibrary" );
	if( !pfn_InitializeLibrary )
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_openremix: 'remixapi_InitializeLibrary' export not found in '%ls'!\n", dllPath );
		FreeLibrary( hDll );
		return false;
	}

	// Initialize library info for Remix C API 0.1003.0
	remixapi_InitializeLibraryInfo info = { 0 };
	info.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
	info.version = REMIXAPI_VERSION_MAKE( 0, 1003, 0 );
	info.pNext = NULL;

	remixapi_Interface iface = { 0 };
	remixapi_ErrorCode status = pfn_InitializeLibrary( &info, &iface );
	if( status != REMIXAPI_ERROR_CODE_SUCCESS )
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_openremix: remixapi_InitializeLibrary failed: %s (code %u)\n",
		                      R_Remix_GetErrorString( status ), (unsigned)status );
		FreeLibrary( hDll );
		return false;
	}

	// Verify required entry points in dispatch table
	if( !iface.Startup || !iface.Shutdown || !iface.Present ||
	    !iface.CreateMesh || !iface.DestroyMesh ||
	    !iface.CreateMaterial || !iface.DestroyMaterial ||
	    !iface.SetupCamera || !iface.DrawInstance ||
	    !iface.CreateLight || !iface.DestroyLight ||
	    !iface.SubmitUIDrawList || !iface.SetConfigVariable )
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_openremix: remixapi_Interface missing mandatory function pointers!\n" );
		FreeLibrary( hDll );
		return false;
	}

	// Populate global dispatch table and loader state
	g_remix = iface;
	g_remixState.hRemixDll = hDll;
	g_remixState.isLoaded = true;
	g_remixState.isStarted = false;
	g_remixState.apiVersion = info.version;
	wcsncpy( g_remixState.dllPath, dllPath, MAX_PATH );
	wcsncpy( g_remixState.dllDir, dllDir, MAX_PATH );

	gEngfuncs.Con_Printf( "^2ref_openremix: Successfully initialized Remix C API (v0.1003.0)^7\n" );
	return true;
}

#if defined(_WIN32)
static HWND     g_gameHwnd = NULL;
static WNDPROC  g_hostWndProc = NULL;
static WNDPROC  g_remixWndProc = NULL;
static qboolean g_uiOpen = false;

void R_Remix_UpdateCursorClipping( void )
{
	static qboolean s_inClipping = false;
	if( s_inClipping )
		return;
	s_inClipping = true;

	if( !g_gameHwnd || !IsWindow( g_gameHwnd ) )
	{
		s_inClipping = false;
		return;
	}

	// Update cached UI state safely once per frame
	if( g_remix.GetUIState )
	{
		g_uiOpen = ( g_remix.GetUIState() != REMIXAPI_UI_STATE_NONE );
	}

	// 1. If OpenRemix UI is open, or window is not foreground or is minimized, release cursor
	if( g_uiOpen || GetForegroundWindow() != g_gameHwnd || IsIconic( g_gameHwnd ) )
	{
		ClipCursor( NULL );
		s_inClipping = false;
		return;
	}

	// 2. Check connection state (only clip during active gameplay, not in console/menus)
	int connState = gEngfuncs.EngineGetParm ? gEngfuncs.EngineGetParm( PARM_CONNSTATE, 0 ) : 0;
	if( connState != ca_active )
	{
		ClipCursor( NULL );
		s_inClipping = false;
		return;
	}

	// 3. Otherwise, in active gameplay: clip cursor to window client rectangle
	RECT clientRect;
	if( GetClientRect( g_gameHwnd, &clientRect ) )
	{
		POINT pt1 = { clientRect.left, clientRect.top };
		POINT pt2 = { clientRect.right, clientRect.bottom };
		ClientToScreen( g_gameHwnd, &pt1 );
		ClientToScreen( g_gameHwnd, &pt2 );

		RECT clip = { pt1.x, pt1.y, pt2.x, pt2.y };
		ClipCursor( &clip );
	}

	s_inClipping = false;
}

void R_Remix_ToggleUI( void )
{
	if( !g_remix.GetUIState || !g_remix.SetUIState )
		return;

	remixapi_UIState cur = g_remix.GetUIState();
	if( cur == REMIXAPI_UI_STATE_NONE )
	{
		g_remix.SetUIState( REMIXAPI_UI_STATE_ADVANCED );
		g_uiOpen = true;
		gEngfuncs.Con_Printf( "^2[OpenRemix]^7 Developer UI Opened (F12)\n" );
		ClipCursor( NULL );
	}
	else
	{
		g_remix.SetUIState( REMIXAPI_UI_STATE_NONE );
		g_uiOpen = false;
		gEngfuncs.Con_Printf( "^2[OpenRemix]^7 Developer UI Closed (F12)\n" );
	}

	if( g_remix.PumpEvents )
	{
		g_remix.PumpEvents();
	}
}

static void R_Remix_ToggleUI_Cmd( void )
{
	R_Remix_ToggleUI();
}

static LRESULT CALLBACK R_Remix_WindowProcHook( HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
	// 1. Intercept F12 key press immediately to toggle OpenRemix Developer UI
	if( uMsg == WM_KEYDOWN && wParam == VK_F12 )
	{
		R_Remix_ToggleUI();
		return 0; // Consume the key so game does not take screenshot
	}
	if( uMsg == WM_KEYUP && wParam == VK_F12 )
	{
		return 0;
	}

	// 2. Handle deactivation and focus loss
	if( uMsg == WM_ACTIVATE )
	{
		if( LOWORD( wParam ) == WA_INACTIVE )
		{
			ClipCursor( NULL );
		}
	}
	else if( uMsg == WM_KILLFOCUS )
	{
		ClipCursor( NULL );
	}

	// Route messages to host window procedure
	if( g_hostWndProc )
	{
		return CallWindowProcW( g_hostWndProc, hwnd, uMsg, wParam, lParam );
	}

	return DefWindowProcW( hwnd, uMsg, wParam, lParam );
}
#else
void R_Remix_UpdateCursorClipping( void ) {}
void R_Remix_ToggleUI( void )
{
	if( !g_remix.GetUIState || !g_remix.SetUIState )
		return;

	remixapi_UIState cur = g_remix.GetUIState();
	g_remix.SetUIState( cur == REMIXAPI_UI_STATE_NONE ? REMIXAPI_UI_STATE_ADVANCED : REMIXAPI_UI_STATE_NONE );
}
static void R_Remix_ToggleUI_Cmd( void )
{
	R_Remix_ToggleUI();
}
#endif

qboolean R_Remix_Startup( void *hwnd )
{
	if( !g_remixState.isLoaded )
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_openremix: Cannot startup Remix - library not loaded!\n" );
		return false;
	}

	if( g_remixState.isStarted )
		return true;

	if( !hwnd )
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_openremix: Cannot startup Remix - NULL HWND provided!\n" );
		return false;
	}

	remixapi_StartupInfo startupInfo = { 0 };
	startupInfo.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
	startupInfo.hwnd = (remixapi_HWND)hwnd;
	startupInfo.disableSrgbConversionForOutput = 0;
	startupInfo.forceNoVkSwapchain = 0;
	startupInfo.editorModeEnabled = 0;
	startupInfo.combineGuiInFinalColor = 1;

#if defined(_WIN32)
	g_gameHwnd = (HWND)hwnd;
	g_hostWndProc = (WNDPROC)GetWindowLongPtr( g_gameHwnd, GWLP_WNDPROC );
#endif

	remixapi_ErrorCode status = g_remix.Startup( &startupInfo );

#if defined(_WIN32)
	// Install our F12 toggle hook on the host window procedure
	SetWindowLongPtr( g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)R_Remix_WindowProcHook );
#endif

	if( status != REMIXAPI_ERROR_CODE_SUCCESS )
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_openremix: g_remix.Startup failed: %s (0x%08X)\n",
		                      R_Remix_GetErrorString( status ), (unsigned)status );
#if defined(_WIN32)
		if( g_hostWndProc )
			SetWindowLongPtr( g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)g_hostWndProc );
#endif
		return false;
	}

	// Register console command for toggling OpenRemix developer UI
	if( gEngfuncs.Cmd_AddCommand )
	{
		gEngfuncs.Cmd_AddCommand( "openremix_menu", R_Remix_ToggleUI_Cmd, "Toggle OpenRemix Developer UI (F12)" );
	}

	g_remixState.isStarted = true;
	gEngfuncs.Con_Printf( S_NOTE "ref_openremix: OpenRemix renderer started on HWND 0x%p\n", hwnd );
	return true;
}

void R_Remix_Shutdown( void )
{
	if( gEngfuncs.Cmd_RemoveCommand )
	{
		gEngfuncs.Cmd_RemoveCommand( "openremix_menu" );
	}

#if defined(_WIN32)
	ClipCursor( NULL );
	if( g_gameHwnd && g_hostWndProc )
	{
		SetWindowLongPtr( g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)g_hostWndProc );
		g_gameHwnd = NULL;
		g_hostWndProc = NULL;
		g_remixWndProc = NULL;
	}
#endif

	if( g_remixState.isStarted && g_remix.Shutdown )
	{
		gEngfuncs.Con_Printf( S_NOTE "ref_openremix: Shutting down OpenRemix renderer...\n" );
		remixapi_ErrorCode status = g_remix.Shutdown();
		if( status != REMIXAPI_ERROR_CODE_SUCCESS )
		{
			gEngfuncs.Con_Printf( S_WARN "ref_openremix: g_remix.Shutdown reported: %s\n", R_Remix_GetErrorString( status ) );
		}
		g_remixState.isStarted = false;
	}
}

void R_Remix_UnloadLibrary( void )
{
	if( g_remixState.isStarted )
	{
		R_Remix_Shutdown();
	}

	if( g_remixState.hRemixDll )
	{
		FreeLibrary( g_remixState.hRemixDll );
		g_remixState.hRemixDll = NULL;
	}

	memset( &g_remix, 0, sizeof( g_remix ) );
	memset( &g_remixState, 0, sizeof( g_remixState ) );
	gEngfuncs.Con_Printf( S_NOTE "ref_openremix: remix.dll unloaded.\n" );
}

void Remix_ShutdownAndUnload( void )
{
	R_Remix_UnloadLibrary();
}

qboolean R_Remix_IsLoaded( void )
{
	return g_remixState.isLoaded;
}

qboolean R_Remix_IsStarted( void )
{
	return g_remixState.isStarted;
}
