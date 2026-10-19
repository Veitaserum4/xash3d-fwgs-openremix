# Project: ref_openremix Native Renderer for Xash3D FWGS

## Architecture
`ref_openremix` is a dedicated native renderer shared library (`ref_openremix.dll`) for Xash3D FWGS implementing `ref_interface_t` (version 20) with entry point `GetRefAPI`. It directly loads and drives OpenRemix (`remix.dll` 0.1003) via the standard Remix C API (`remix_c.h` / `remix_native_ext.h`), embedding Vulkan swapchain presentation into the engine's Win32 window (`HWND`).

```
Engine (Xash3D FWGS 64-bit)
   │
   ├── GetRefAPI / ref_interface_t (v20)
   ▼
ref_openremix (wscript: ref/openremix)
   ├── r_context.c (GetRefAPI, lifecycle: Init, Shutdown, BeginFrame, EndFrame)
   ├── r_remix.c / h (remix.dll dynamic loader, remixapi_InitializeLibrary, startup, present)
   ├── r_bsp.c (world model & submodel surface extraction, batching, BLAS creation via CreateMeshBatched)
   ├── r_studio.c (studio model bone transforms, software skinning, viewmodel & NPC instancing)
   ├── r_sprite.c (translucent/additive sprites, beams, temp entities)
   ├── r_light.c (BSP entity light parsing: point/spot/sun; dynamic lights: flashlight, dlights, muzzleflashes, tram)
   ├── r_fog.c (volumetric fog parameter parsing, SetFogState, rtx.native.pathtrace.volumetrics mapping)
   ├── r_material.c (PBR suffix detection: _n, _r, _m, _e, GLTF swizzle, USD mod hash matching)
   └── r_ui.c (2D HUD & menu quad collection, texture registration, SubmitUIDrawList)
   │
   ▼ Remix C API (REMIXAPI_VERSION 0.1003.0)
remix.dll (Vulkan Ray-Tracing Path Tracer / Rasterizer)
```

## Feature Inventory
Every feature from user requirements and Phase 0 surveys is inventoried here with its assigned milestone.
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | Waf Build Integration | `ref/openremix/wscript`, root `wscript` `REFDLLS`/`SUBDIRS`, 64-bit AMD64 target configuration | M1 | Survey |
| 2 | Remix C API Loader & Handshake | Dynamic load of `remix.dll`, `remixapi_InitializeLibrary`, version check for 0.1003.0 | M1 | Survey / R1 |
| 3 | Renderer Lifecycle & Window Embedding | `GetRefAPI`, `R_Init`, `R_Shutdown`, `R_NewMap`, `BeginFrame`, `EndFrame`, HWND binding, `Present` | M1 | Survey / R1 |
| 4 | Camera Parameterization & Coordinate Setup | GoldSrc right-handed Z-up coordinates, `remixapi_CameraInfoParameterizedEXT`, `sceneScale = 0.025` | M1 | Survey / R1 |
| 5 | BSP World Geometry Extraction | Parse `model_t` / `msurface_t`, triangulate surfaces, vertex normals, UVs, batched mesh creation | M2 | Survey / R2 |
| 6 | Map Submodels & Movable Brushes | Instancing moving doors, platforms, pushables with dynamic transforms | M2 | Survey / R2 |
| 7 | Studio Model Skeletal Animation | Bone matrix computation, software skinning / bone transforms, Gordon viewmodel weapon tag, NPCs | M2 | Survey / R2 |
| 8 | Sprites, Beams & Temp Entities | Translucent/additive sprite quads, animated glowing beams, explosion particles | M2 | Survey / R2 |
| 9 | 2D HUD & Menu Compositing | Collect 2D screen quads, register UI textures via `RegisterUITexture`, submit via `SubmitUIDrawList` | M2 | Survey / R2 |
| 10 | BSP Static Light Entity Parsing | Parse `world->entities` dictionary for `light`, `light_spot`, `light_environment` into Remix sphere/distant lights | M3 | Survey / R3 |
| 11 | Player Flashlight & Orientations | Viewmodel-attached flashlight with position offset, cone angle, focus exponent, real-time ray-traced shadows | M3 | Survey / R3 |
| 12 | Dynamic Lights (Dlights & Elights) | Explosions, projectiles, muzzleflashes with line-of-sight trace checks, frame updates via `UpdateLightDefinition` | M3 | Survey / R3 |
| 13 | Special Map Headlights & Illuminators | Intro tram headlights (`c0a0`-`c0a0e`) and map-scripted dynamic illuminators | M3 | Survey / R3 |
| 14 | Volumetric Fog Mapping | Parse fog entities and cvars, map to `remixapi_SetFogState` (extinction, scattering, ambient, asymmetry) | M4 | Survey / R4 |
| 15 | Volumetric Light Shafts & Up-Axis | Configure `rtx.native.pathtrace.volumetrics` and set `volumetricUpAxis = 2` (Z-up) for Half-Life | M4 | Survey / R4 |
| 16 | PBR Texture Suffix Auto-Detection | Detect `_n` (normal), `_r` (roughness), `_m` (metallic), `_e` (emission), build `remixapi_MaterialInfoPbrEXT` | M5 | Survey / R5 |
| 17 | USD Modding & Material Hash Compatibility | Compute 64-bit FNV-1a hashes for meshes and materials compatible with `rtx-remix/mods` replacements | M5 | Survey / R5 |
| 18 | Resource Cleanup & Stability | Clean release of meshes, materials, lights, UI textures on map change/shutdown; zero Vulkan validation errors | M6 | Survey / R1-R5 |
| 19 | E2E Testing Suite (Tiers 1-4) | Comprehensive automated test suite for headless and interactive validation across all 5 tiers | E2E | Requirement |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Backend Core & Lifecycle | Waf build integration, Remix loader, `GetRefAPI`, HWND binding, frame presentation, camera basis | none | DONE |
| M2 | Geometry & UI Translation | BSP world triangulation, submodels, studio models (viewmodels/NPCs), sprites, beams, 2D HUD via `SubmitUIDrawList` | M1 | PLANNED |
| M3 | Lighting & Dynamic Lights | BSP static lights (`light`, `light_spot`, `light_env`), flashlight, dlights/elights, muzzleflashes, tram lights | M1 | PLANNED |
| M4 | Volumetric Fog & Atmosphere | Engine fog parsing, `remixapi_SetFogState`, `rtx.native.pathtrace.volumetrics`, Z-up alignment | M1 | PLANNED |
| M5 | PBR Materials & Textures | PBR suffix auto-detection (`_n`, `_r`, `_m`, `_e`), material creation, USD mod replacement hashes | M1 | PLANNED |
| M6 | Integration & Stability | End-to-end map loading (`c1a0`), runtime stability, map transitions, zero leaks, zero Vulkan validation errors | M2, M3, M4, M5 | PLANNED |
| E2E | E2E Testing Track | Independent requirement-driven opaque-box test harness & test suite (Tiers 1-4) | Parallel with M1-M6 | PLANNED |

## Interface Contracts
### Engine ↔ ref_openremix
- Export: `int GetRefAPI(int version, ref_interface_t *pFunctionTable, ref_api_t *engfuncs, ref_globals_t *pGlobals)`
- `version`: Expected `REF_API_VERSION` (20)
- `engfuncs`: Core engine services (`R_GetWindowHandle`, `GL_GetProcAddress`, `ENGINE_GET_PARM`, `host_client`, `cl_dlights`, file I/O)
- `pGlobals`: Screen width, height, time, frame duration
- `pFunctionTable`: Renderer callbacks implemented by `ref_openremix`:
  - `R_Init`, `R_Shutdown`, `R_NewMap`, `R_BeginFrame`, `R_RenderScene`, `R_Set2DMode`, `R_EndFrame`, `R_DrawStretchPic`, `FillRGBA`, etc.

### ref_openremix ↔ remix.dll (Remix C API)
- ABI Version: `REMIXAPI_VERSION_MAKE(0, 1003, 0)`
- Entry Point: `remixapi_InitializeLibrary(&remixapi_InitializeLibraryInfo, &remixapi_Interface)`
- Startup: `remixapi_StartupInfo` with `hwnd = engine_hwnd` (disables SDL message pump automatically)
- Units & Scale: 1 unit = 0.025 meters (`sceneScale = 0.025`, `hostMetersPerUnit = 0.0254`)
- Coordinate System: Right-handed Z-up (`hostUpAxis = 2`, `volumetricUpAxis = 2`)
- Frame Presentation: `remixapi_Interface.Present(&presentInfo)` called in `R_EndFrame`

## Code Layout
- `ref/openremix/`: Dedicated renderer backend source files
  - `wscript`: Waf build script compiling `ref_openremix.dll`
  - `exports.txt`: Win32 exports (`GetRefAPI`)
  - `r_local.h`: Master internal renderer header, engine includes, remix headers
  - `r_context.c`: `GetRefAPI`, engine callbacks registration
  - `r_remix.c` / `r_remix.h`: Dynamic loading of `remix.dll`, API dispatch table, initialization and teardown
  - `r_main.c`: Frame orchestration, `R_BeginFrame`, `R_RenderScene`, `R_EndFrame`, camera setup
  - `r_bsp.c`: BSP tree and surface extraction, triangulation, mesh batching
  - `r_studio.c`: Studio model bone transform evaluation, software skinning, instance submission
  - `r_sprite.c`: Sprites, beams, temp entities, particle quads
  - `r_light.c`: Static BSP entity lights, dynamic flashlight, muzzleflash, dlights, tram lights
  - `r_fog.c`: Fog entity parsing, `remixapi_SetFogState`, volumetrics parameters
  - `r_material.c`: Texture cache, PBR suffix scanning, material creation, FNV-1a mod hashes
  - `r_ui.c`: 2D HUD and menu quad accumulation, UI texture registration, `remixapi_SubmitUIDrawList`
- `tests/e2e/`: E2E test suite runners and test cases
