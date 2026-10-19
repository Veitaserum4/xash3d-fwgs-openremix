# Test Infrastructure Specification: ref_openremix (Tiers 1–4)

**Document Version**: 1.0.0  
**Target Architecture**: AMD64 (x86_64) Native Shared Library (`ref_openremix.dll`)  
**Host Engine**: Xash3D FWGS (Ref API Version 20)  
**Ray Tracing Backend**: OpenRemix 0.1003.0 (`remix.dll` via standard Remix C API)  
**Root Working Directory**: `c:\Users\Jacob\Desktop\XASH3D-OPENREMIX\xash3d-fwgs-openremix`

---

## 1. Test Philosophy & Methodology

### 1.1 Opaque-Box & Requirement-Driven Testing
The testing suite for `ref_openremix` follows an **opaque-box, requirement-driven methodology**:
- Tests evaluate the system strictly via observable contracts: exported PE symbols, C ABI structures, binary signatures, function return codes, coordinate transformations, mathematical properties, and engine runtime behavior.
- Tests do **not** depend on private internal helper implementations or unexposed variables.
- Every test case has an explicit **Authoritative Expected Output** derived from:
  1. OpenRemix C API Specifications (`public/include/remix/remix_c.h`, `remix_native_ext.h`).
  2. Verified OpenRemix Binary (`remix.dll` in `_work/packages/windows/bin`).
  3. Xash3D Engine Renderer API (`engine/ref_api.h`, `REF_API_VERSION 20`).
  4. Reference Ray Tracing Implementations (`xash-rt` BSP light parser, PBR naming conventions, and volumetrics mapping).
  5. User Requirements and Milestones (`PROJECT.md`, `ORIGINAL_REQUEST.md`).

### 1.2 Progressive Testability
During milestone progression (M1 through M6):
- Tests must be verifiable progressively using current artifacts and specifications without failing prematurely due to unbuilt future milestones.
- When compiled binaries are present, binary verification (PE parsing, symbol table checks, DLL loading, dynamic export calls) executes.
- When components are under development, contract validation, ABI alignment, format parsers, and mock harness verification validate the specification invariants.
- Self-contained, isolated test cases ensure zero test order dependence and automatic cleanup.

### 1.3 4-Tier Test Taxonomy
The test suite is structured into four distinct tiers:
- **Tier 1: Feature Coverage (Primary Paths)**: Verifies the primary functional behavior and contract compliance for each of the 18 features in the `PROJECT.md` Feature Inventory (>= 5 test cases per feature; 90 test cases total).
- **Tier 2: Boundary & Corner Cases**: Verifies edge cases, extreme values, boundary conditions, malformed inputs, NULL pointer resilience, and graceful failure modes for each feature (>= 5 test cases per feature; 90 test cases total).
- **Tier 3: Cross-Feature Combinations**: Verifies pairwise subsystem interactions (e.g. lighting + studio models, fog + point lights, PBR textures + BSP geometry, 2D HUD + 3D scene, dynamic lights + moving brushes).
- **Tier 4: Real-World Scenarios**: Validates realistic game workloads, stress benchmarks, and end-to-end engine executions (Half-Life `c1a0` hazard course, intro tram ride `c0a0`, multi-entity firefight stress, level transitions, rapid flashlight toggles, and fallback paths).

---

## 2. Authoritative Source of Truth Matrix

| Domain | Authoritative Reference File | Key Contracts & Invariants |
|---|---|---|
| **Remix C API Version & ABI** | `public/include/remix/remix_c.h` | `REMIXAPI_VERSION_MAJOR=0`, `REMIXAPI_VERSION_MINOR=1003`, `REMIXAPI_VERSION_PATCH=0`. Minor mismatch triggers `REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION` (8). |
| **OpenRemix Binary Package** | `_work/packages/windows/bin/remix.dll` | PE machine `0x8664` (AMD64), 128 exported C functions including `remixapi_InitializeLibrary`, `remixapi_Startup`, `remixapi_SubmitUIDrawList`, `remixapi_SetFogState`. |
| **Engine Ref Interface** | `engine/ref_api.h` | `REF_API_VERSION = 20`. Single DLL export `int GetRefAPI(int version, ref_interface_t *funcs, ref_api_t *engfuncs, ref_globals_t *globals)`. |
| **Coordinate System & Scale** | `PROJECT.md` § Interface Contracts | GoldSrc right-handed Z-up (`hostUpAxis = 2`, `volumetricUpAxis = 2`). Scale: `1 unit = 0.025 m` (`sceneScale = 0.025`, `hostMetersPerUnit = 0.0254`). |
| **Light & Entity Parsing** | `xash-rt/ref/gl/gl_rlight.c` | Token-based parsing of `WORLDMODEL->entities`, classnames `light`, `light_spot`, `light_environment`. Color normalization `/ 255.0f`, inverted Quake pitch. |
| **PBR Material Pipeline** | `PROJECT.md` § Feature 16 & `xash-rt` | Texture suffix conventions: `_n` (normal), `_r` (roughness), `_m` (metallic), `_e` (emission). Base texture without suffix. |
| **USD Mod Hash Compatibility** | OpenRemix Modding Spec & `PROJECT.md` | 64-bit FNV-1a hash calculation for mesh names, texture paths, and material identifiers. |

---

## 3. Tier 1: Feature Coverage (>=5 Tests Per Feature)

All 18 features from `PROJECT.md` are mapped with at least 5 primary behavior test cases.

### Feature 1: Waf Build Integration
- `T1_F01_01`: Root `wscript` includes `openremix` in `REFDLLS` list with appropriate build flags.
- `T1_F01_02`: Root `wscript` contains subproject entry for `ref/openremix` activated when `OPENREMIX` is selected.
- `T1_F01_03`: Command-line option `-8` / `--64bits` configures `conf.env.MSVC_TARGETS = ['x64']`.
- `T1_F01_04`: `ref/openremix/wscript` defines target `'ref_openremix'` with shared library feature (`bld.shlib`).
- `T1_F01_05`: `ref/openremix/exports.txt` explicitly exports `GetRefAPI` for Windows DLL symbol linkage.

### Feature 2: Remix C API Loader & Handshake
- `T1_F02_01`: `remix.dll` binary verified as PE32+ (AMD64 64-bit) with signature `PE\0\0` and machine `0x8664`.
- `T1_F02_02`: `remix.dll` exports `remixapi_InitializeLibrary` at valid function RVA.
- `T1_F02_03`: `remix_c.h` defines `REMIXAPI_VERSION_MAKE(0, 1003, 0)` matching OpenRemix ABI specification.
- `T1_F02_04`: Loader info structure `remixapi_InitializeLibraryInfo` initializes with `sType = 1` and compiled version.
- `T1_F02_05`: Dispatch table `remixapi_Interface` populates function pointers on successful library initialization.

### Feature 3: Renderer Lifecycle & Window Embedding
- `T1_F03_01`: `GetRefAPI` returns `REF_API_VERSION` (20) when valid parameters are passed.
- `T1_F03_02`: `ref_interface_t` function table populated with non-null lifecycle callbacks (`R_Init`, `R_Shutdown`, `R_NewMap`, `R_BeginFrame`, `R_EndFrame`).
- `T1_F03_03`: `R_Init` queries platform window handle via `gEngfuncs.R_GetWindowHandle(..., REF_WINDOW_TYPE_WIN32)`.
- `T1_F03_04`: `remixapi_StartupInfo` binds to the retrieved Win32 `HWND` without creating duplicate SDL windows.
- `T1_F03_05`: `R_EndFrame` triggers `remixapi_Interface.Present(&presentInfo)` to present path-traced frame to swapchain.

### Feature 4: Camera Parameterization & Coordinate Setup
- `T1_F04_01`: GoldSrc right-handed coordinate system converted to OpenRemix camera orientation (`hostUpAxis = 2`).
- `T1_F04_02`: Scene scale configured to `0.025` meters per unit in startup info (`sceneScale = 0.025`).
- `T1_F04_03`: Camera view vectors (origin, forward, right, up) map correctly into `remixapi_CameraInfoParameterizedEXT`.
- `T1_F04_04`: Field of view (FOV) calculation accounts for aspect ratio and matches GoldSrc `r_refdef.fov_x` / `fov_y`.
- `T1_F04_05`: Near and far clipping planes configure valid depth ranges for path tracing without z-fighting.

### Feature 5: BSP World Geometry Extraction
- `T1_F05_01`: Extraction of `model_t` world surfaces iterates `msurface_t` array across all BSP nodes and leaves.
- `T1_F05_02`: Surface vertex reconstruction via `mod->surfedges` and `mod->vertexes` produces valid triangle fan/strip topology.
- `T1_F05_03`: Planar normals extracted from `surf->plane->normal` or computed via cross product for each triangle.
- `T1_F05_04`: Texture coordinates `(u, v)` derived from `surf->texinfo->vecs` and texture dimension normalization.
- `T1_F05_05`: Mesh surfaces batched into OpenRemix Bottom-Level Acceleration Structure (BLAS) via `CreateMeshBatched`.

### Feature 6: Map Submodels & Movable Brushes
- `T1_F06_01`: Submodel entities (`*1`, `*2`, func_door, func_plat) identified and separated from static world BLAS.
- `T1_F06_02`: Dynamic origin and rotation angles applied to submodel transformation matrix each frame.
- `T1_F06_03`: Instanced submodels submitted with unique instance handles via `CreateInstance` / `DrawInstance`.
- `T1_F06_04`: Submodel visibility culling respects engine PVS and frustum tests before submission.
- `T1_F06_05`: Movable brushes retain correct material assignments and surface UVs under arbitrary rotation.

### Feature 7: Studio Model Skeletal Animation
- `T1_F07_01`: Model header `studiohdr_t` parsed for bones, sequences, and submodels.
- `T1_F07_02`: Bone matrix transformations computed per frame via `R_StudioSetupBones` using quaternion slerp.
- `T1_F07_03`: Software vertex skinning transforms mesh vertices into world-space or bone-local coordinate space.
- `T1_F07_04`: First-person viewmodel weapon tagged with `remixapi_InstanceInfoObjectPickingEXT` / viewmodel categories.
- `T1_F07_05`: Third-person player and NPC studio models instanced with correct entity origin and yaw orientation.

### Feature 8: Sprites, Beams & Temp Entities
- `T1_F08_01`: Sprite models (`.spr`) parsed for frame dimensions, type (VP_PARALLEL, ORIENTED), and texture format.
- `T1_F08_02`: Camera-facing billboarding calculated dynamically for oriented and parallel sprites.
- `T1_F08_03`: Render modes (`kRenderTransAdd`, `kRenderTransAlpha`, `kRenderGlow`) mapped to Remix blend states.
- `T1_F08_04`: Segmented beams and lightning bolts generated as quad strips connecting endpoint coordinates.
- `T1_F08_05`: Temporary particles and explosion debris submitted as instanced quads with decaying alpha.

### Feature 9: 2D HUD & Menu Compositing
- `T1_F09_01`: Engine switch to 2D mode via `R_Set2DMode(true)` redirects draw operations to UI draw list buffer.
- `T1_F09_02`: UI textures (HUD digits, crosshair, icons, font atlases) registered via `remixapi_RegisterUITexture`.
- `T1_F09_03`: Screen quad coordinates mapped to `remixapi_UIVertex` with normalized UVs and vertex colors.
- `T1_F09_04`: 2D draw list submitted each frame prior to presentation via `remixapi_SubmitUIDrawList`.
- `T1_F09_05`: UI textures freed cleanly upon level transition or engine shutdown via `remixapi_FreeUITexture`.

### Feature 10: BSP Static Light Entity Parsing
- `T1_F10_01`: BSP entity lump (`WORLDMODEL->entities`) parsed sequentially for light classnames.
- `T1_F10_02`: Classname `light` parsed into omnidirectional sphere light with origin, RGB color, and intensity.
- `T1_F10_03`: Classname `light_spot` parsed into shaped spotlight with cone angles (`_cone`, `_cone2`) and direction vector.
- `T1_F10_04`: Classname `light_environment` parsed into distant directional sun light with pitch and yaw angles.
- `T1_F10_05`: Light colors normalized from 0–255 range to 0.0–1.0 float vectors matching Remix light requirements.

### Feature 11: Player Flashlight & Orientations
- `T1_F11_01`: Flashlight entity linked to player view origin with forward offset along view direction.
- `T1_F11_02`: Flashlight spotlight configured with realistic inner/outer cone angles (e.g. 20° inner, 35° outer).
- `T1_F11_03`: Toggle state (`cl_flashlight` / imp 100) dynamically enables or destroys the Remix light instance.
- `T1_F11_04`: Real-time ray-traced shadows cast by flashlight light rays intersecting world and entity geometry.
- `T1_F11_05`: Flashlight orientation updates synchronously with player mouse pitch/yaw changes without 1-frame latency.

### Feature 12: Dynamic Lights (Dlights & Elights)
- `T1_F12_01`: Engine dynamic light table (`cl_dlights`) traversed each frame for active light records.
- `T1_F12_02`: Dlight radius and color converted to Remix spherical light parameters (`radiance`, `radius`).
- `T1_F12_03`: Light decay and expiration remove inactive dlights from Remix scene.
- `T1_F12_04`: Entity lights (`elights` for muzzle flashes and item glows) tracked and positioned relative to parent entity.
- `T1_F12_05`: Light handles updated in-place via `UpdateLightDefinition` to avoid recreating light resources every frame.

### Feature 13: Special Map Headlights & Illuminators
- `T1_F13_01`: Intro tram (`c0a0` - `c0a0e`) headlight entity detected and linked to moving tram submodel origin.
- `T1_F13_02`: Tram headlight orientation tracks curved track path and rotation matrix of the carriage.
- `T1_F13_03`: Scripted map illuminators (e.g. alarm beacons, pulsing reactor cores) respond to lightstyle updates.
- `T1_F13_04`: Flicker styles (style 1–11) parsed from `cl_lightstyle_lerping` and applied as intensity modulators.
- `T1_F13_05`: Moving lights properly invalidate cached irradiance / SHARC caches to avoid ghosting artifacts.

### Feature 14: Volumetric Fog Mapping
- `T1_F14_01`: Engine fog entities (`env_fog`) and console variables parsed for fog color, start, and end distances.
- `T1_F14_02`: Extinction distance calculated from fog parameters and mapped to `remixapi_SetFogState`.
- `T1_F14_03`: Volumetric scattering color and ambient medium density configured in Remix fog state.
- `T1_F14_04`: Phase function asymmetry parameter `g` configured (e.g. 0.0 for isotropic, 0.4 for forward scattering).
- `T1_F14_05`: Volumetric state enabled and toggled dynamically across map boundaries.

### Feature 15: Volumetric Light Shafts & Up-Axis
- `T1_F15_01`: Configuration variable `rtx.native.pathtrace.volumetrics` activated when fog is present.
- `T1_F15_02`: Parameter `volumetricUpAxis` explicitly set to `2` (Z-up) to align with Half-Life coordinate system.
- `T1_F15_03`: Directional sun light interacts with participating medium to produce path-traced god rays.
- `T1_F15_04`: High-intensity spotlights generate visible light shafts through volumetric fog volumes.
- `T1_F15_05`: Volumetric scattering integrates cleanly with scene depth buffer and alpha surfaces.

### Feature 16: PBR Texture Suffix Auto-Detection
- `T1_F16_01`: Texture loader checks filesystem for normal map suffix `_n` and binds to tangent normal slot.
- `T1_F16_02`: Loader checks for roughness suffix `_r` and metallic suffix `_m` and generates ORM/metallic-roughness textures.
- `T1_F16_03`: Loader checks for emission suffix `_e` and sets emission factor / emissive texture in material info.
- `T1_F16_04`: Material descriptor built using `remixapi_MaterialInfo` and extended with `remixapi_MaterialInfoPbrEXT`.
- `T1_F16_05`: Fallback to default metallic (0.0) and roughness (0.8) when PBR texture maps are absent.

### Feature 17: USD Modding & Material Hash Compatibility
- `T1_F17_01`: 64-bit FNV-1a hash algorithm computed for mesh surface names and texture paths.
- `T1_F17_02`: Hash calculation matches OpenRemix USD modding convention (`rtx-remix/mods`).
- `T1_F17_03`: Material info populated with `remixapi_MaterialInfoReplacementHashEXT` using computed 64-bit hash.
- `T1_F17_04`: Mesh instance populated with `remixapi_InstanceInfoReplacementHashEXT` to allow mod mesh replacement.
- `T1_F17_05`: Mod folder scan (`rtx-remix/mods/*.usda`) integrates with runtime without crash when mods are present or absent.

### Feature 18: Resource Cleanup & Stability
- `T1_F18_01`: Map unload (`R_NewMap` with empty model) triggers release of all active BLAS meshes via `DestroyMesh`.
- `T1_F18_02`: Static and dynamic light handles destroyed via `DestroyLight` upon level change.
- `T1_F18_03`: Material handles destroyed via `DestroyMaterial` without leaving dangling pointers.
- `T1_F18_04`: `R_Shutdown` calls `remixapi_Interface.Shutdown()` and unloads `remix.dll` cleanly.
- `T1_F18_05`: Zero Vulkan validation errors reported by validation layers during a complete map load-unload cycle.

---

## 4. Tier 2: Boundary, Extreme & Corner Cases (>=5 Tests Per Feature)

All 18 features are tested under adversarial, boundary, and stress conditions (>= 5 test cases per feature; 90 test cases total).

### Feature 1: Waf Build Integration
- `T2_F01_01`: Configure with invalid compiler flag handles error gracefully without corrupting `_work/` or Waf cache.
- `T2_F01_02`: Building with missing 32-bit option when `-8` is omitted fails with informative target architecture warning.
- `T2_F01_03`: `ref/openremix` wscript handles empty source list or non-existent header include directories gracefully.
- `T2_F01_04`: Concurrent multi-threaded build (`waf -j16`) compiles without intermediate file collision or lock contention.
- `T2_F01_05`: Re-running configure without clean maintains idempotency in generated build environment.

### Feature 2: Remix C API Loader & Handshake
- `T2_F02_01`: Attempting to load non-existent DLL path returns `REMIXAPI_ERROR_CODE_INVALID_PATH` or null module.
- `T2_F02_02`: Calling `remixapi_InitializeLibrary` with `version = REMIXAPI_VERSION_MAKE(0, 6, 0)` (stock NVIDIA) returns `REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION` (8).
- `T2_F02_03`: Passing `NULL` pointer as `out_remixInterface` returns `REMIXAPI_ERROR_CODE_INVALID_ARGUMENT`.
- `T2_F02_04`: Passing invalid `sType` in `remixapi_InitializeLibraryInfo` returns `REMIXAPI_ERROR_CODE_INVALID_ARGUMENT`.
- `T2_F02_05`: Loading corrupt or truncated PE binary triggers graceful error without memory violation or SEH abort.

### Feature 3: Renderer Lifecycle & Window Embedding
- `T2_F03_01`: Calling `GetRefAPI` with mismatched `version` (e.g. 19 or 21) returns 0 and does not populate funcs table.
- `T2_F03_02`: Passing `NULL` for `pFunctionTable`, `engfuncs`, or `pGlobals` to `GetRefAPI` returns 0 safely.
- `T2_F03_03`: Calling `R_Init` with invalid Win32 `HWND` (`(HWND)0` or `(HWND)-1`) returns error code without crash.
- `T2_F03_04`: Rapid consecutive calls to `R_Init` and `R_Shutdown` execute cleanly without double-free errors.
- `T2_F03_05`: Calling `R_RenderScene` when window is minimized (0x0 client rect) skips draw submission safely.

### Feature 4: Camera Parameterization & Coordinate Setup
- `T2_F04_01`: Extreme FOV values (e.g. 1.0° telescopic sniper, 179.0° ultra-wide fisheye) calculate valid perspective matrices.
- `T2_F04_02`: Camera origin at extreme world coordinates (`±131072.0`) retains floating point precision without jitter.
- `T2_F04_03`: Pitch angles at exact vertical limits (`±89.99°`) do not produce gimbal lock or NaN vectors.
- `T2_F04_04`: Zero or negative viewport dimensions (`width <= 0`, `height <= 0`) handle gracefully without division-by-zero.
- `T2_F04_05`: Near plane distance equal to or greater than far plane distance caught and clamped to safe defaults.

### Feature 5: BSP World Geometry Extraction
- `T2_F05_01`: Degenerate BSP polygons (zero surface area, collinear vertices) culled without generating invalid triangle indices.
- `T2_F05_02`: Polygon with maximum vertices (Quake max 64 vertices per poly) triangulates into valid fan without stack overflow.
- `T2_F05_03`: Corrupted texture coordinates (NaN or infinite UV values) clamped to finite floating point range.
- `T2_F05_04`: Malformed BSP tree with unlinked leaves or invalid plane normals skips invalid surfaces safely.
- `T2_F05_05`: Huge BSP map exceeding 65,536 world vertices handles index buffering using 32-bit integer indices.

### Feature 6: Map Submodels & Movable Brushes
- `T2_F06_01`: Submodel with zero brush surfaces handles gracefully without allocating empty mesh BLAS.
- `T2_F06_02`: Extreme rotation velocities (e.g. 10,000 deg/sec rotating fan blade) compute stable transformation matrices.
- `T2_F06_03`: Submodel crossing level boundaries or disappearing instantly handles instance destruction cleanly.
- `T2_F06_04`: Up to 512 simultaneous moving brush entities processed without buffer overflow.
- `T2_F06_05`: Submodel embedded entirely within solid void geometry culls from draw list.

### Feature 7: Studio Model Skeletal Animation
- `T2_F07_01`: Studio model with 0 bones or corrupted `studiohdr_t` reports error and skips rendering without engine crash.
- `T2_F07_02`: Model with maximum supported bones (128 bones) evaluates transforms within single-frame budget.
- `T2_F07_03`: Extreme bone scaling or NaN animation frame interpolations clamped to identity transform.
- `T2_F07_04`: Studio mesh with missing skin texture defaults to magenta check pattern or fallback neutral material.
- `T2_F07_05`: Rapid switching of player viewmodel weapons (30 weapon switches per second) avoids instance handle leaks.

### Feature 8: Sprites, Beams & Temp Entities
- `T2_F08_01`: Zero-dimension sprite (`width=0` or `height=0`) skipped without submitting degenerate quads.
- `T2_F08_02`: Additive sprite with alpha value `0.0` or `< 0.0` culled from rendering.
- `T2_F08_03`: Laser beam with identical start and end coordinates (`start == end`) discarded safely.
- `T2_F08_04`: Burst of 2,048 simultaneous particle temp entities handles buffer capacity gracefully via ring allocation.
- `T2_F08_05`: Animated sprite with corrupted frame index clamped to `numframes - 1`.

### Feature 9: 2D HUD & Menu Compositing
- `T2_F09_01`: Empty UI frame (zero HUD elements drawn, e.g. clean cinematic screenshot mode) submits empty draw list safely.
- `T2_F09_02`: Screen quad with negative screen coordinates (off-screen HUD element) clipped to viewport bounds.
- `T2_F09_03`: Attempting to register 0x0 or invalid dimension UI texture returns error without allocating Vulkan image.
- `T2_F09_04`: Rapid UI texture re-registration (animated HUD numbers updating every frame) releases old textures cleanly.
- `T2_F09_05`: Registering up to 1,024 unique UI glyphs and icons handles UI texture table without leak.

### Feature 10: BSP Static Light Entity Parsing
- `T2_F10_01`: Empty or missing `world->entities` lump parses without error and leaves light count at zero.
- `T2_F10_02`: Entity with malformed attributes (missing values, unterminated quotes, unclosed `{}`) recovers and continues parsing.
- `T2_F10_03`: Light entity with negative intensity or zero color `(0 0 0)` culled to prevent black hole artifacts.
- `T2_F10_04`: Spotlight with `_cone` greater than or equal to `_cone2` clamped to valid nested angular ranges.
- `T2_F10_05`: BSP map with over 2,048 static light entities prioritizes brightest lights and handles capacity limit gracefully.

### Feature 11: Player Flashlight & Orientations
- `T2_F11_01`: Flashlight active while player is submerged in water applies correct underwater attenuation/tint.
- `T2_F11_02`: Player flashlight orientation during rapid 360° mouse flick updates smoothly without numerical instability.
- `T2_F11_03`: Flashlight toggled on and off 100 times in 1 second handles light instance allocation without leak.
- `T2_F11_04`: Flashlight position when player is crouched inside narrow air vent (tight geometry bounding) avoids shadow self-intersection.
- `T2_F11_05`: Flashlight color or brightness cvar set to 0.0 disables light emission immediately.

### Feature 12: Dynamic Lights (Dlights & Elights)
- `T2_F12_01`: Dynamic light radius set to 0.0 or negative value skipped from scene submission.
- `T2_F12_02`: Dynamic light positioned outside valid world bounds (`origin > 65535`) culled immediately.
- `T2_F12_03`: 64 simultaneous dlights (maximum engine capacity) submitted without exceeding Remix light limits.
- `T2_F12_04`: Dlight expiring at fractional frame time interpolates smoothly to zero intensity without popping.
- `T2_F12_05`: Muzzleflash elight persisting for exactly 1 frame cleans up on the very next frame.

### Feature 13: Special Map Headlights & Illuminators
- `T2_F13_01`: Tram moving at high speed across map teleport trigger retains continuous headlight tracking.
- `T2_F13_02`: Broken lightstyle string (empty or corrupted characters outside `'a'-'z'`) defaults to steady style `'m'`.
- `T2_F13_03`: Lightstyle lerp factor at boundary `0.0` and `1.0` evaluates smoothly without floating point jitter.
- `T2_F13_04`: Map with multiple moving trams (multiplayer or custom mod) handles multiple tracked headlights cleanly.
- `T2_F13_05`: Scripted trigger disabling tram headlight instantly destroys Remix light instance.

### Feature 14: Volumetric Fog Mapping
- `T2_F14_01`: Fog start distance greater than fog end distance caught and clamped to prevent inverse extinction.
- `T2_F14_02`: Negative extinction distance or density clamped to 0.0 (disabling fog).
- `T2_F14_03`: Asymmetry parameter `g` outside valid range `[-0.99, 0.99]` clamped safely.
- `T2_F14_04`: Pure black fog color `(0, 0, 0)` does not divide by zero in scattering calculations.
- `T2_F14_05`: Rapid transition between dense fog zone and zero fog room transitions smoothly over multiple frames.

### Feature 15: Volumetric Light Shafts & Up-Axis
- `T2_F15_01`: Directional sun light pointing straight down `(0, 0, -1)` or straight up `(0, 0, 1)` evaluates stable ray marching.
- `T2_F15_02`: High-intensity spotlight (intensity > 1,000,000) inside fog does not produce floating point overflow (INF/NaN).
- `T2_F15_03`: Camera fully inside dense fog volume maintains near-plane volumetric integration without clipping holes.
- `T2_F15_04`: Disabling volumetric path tracing via cvar restores standard path tracing without restart.
- `T2_F15_05`: Volumetric light shafts intersecting transparent alpha water surfaces composite with correct transmission order.

### Feature 16: PBR Texture Suffix Auto-Detection
- `T2_F16_01`: Texture with mixed case suffixes (e.g. `wall_N.png`, `metal_R.tga`) detected case-insensitively.
- `T2_F16_02`: Texture file with multiple suffix patterns (e.g. `sign_main_n_r.png`) parses correctly according to standard separator.
- `T2_F16_03`: Corrupted PBR image file (invalid PNG/TGA header) falls back to default flat normal/roughness without crash.
- `T2_F16_04`: Texture name with special characters (spaces, hyphens, non-ASCII) resolves without path corruption.
- `T2_F16_05`: Model with 512 individual PBR materials caches texture handles without duplicate disk reads.

### Feature 17: USD Modding & Material Hash Compatibility
- `T2_F17_01`: Empty mesh name or texture path string returns standard baseline FNV-1a hash offset.
- `T2_F17_02`: Extremely long texture path (> 1,024 characters) computes hash without buffer overrun.
- `T2_F17_03`: Case sensitivity normalization applied to paths before hashing to prevent Windows path mismatch.
- `T2_F17_04`: USD mod directory containing unreadable or locked files skips locked asset with warning log.
- `T2_F17_05`: Dynamically unloading USD mod scene restores original BSP/studio geometry without restart.

### Feature 18: Resource Cleanup & Stability
- `T2_F18_01`: Stress test: 50 consecutive map loads and reloads (`c1a0` $\rightarrow$ `c1a0a` $\rightarrow$ `c1a0`) maintain stable memory footprint.
- `T2_F18_02`: Engine forced kill (`SIGTERM` / `Host_Error`) releases Win32 window and OS resources cleanly.
- `T2_F18_03`: Unloading renderer DLL while background threads or async shader compilations are pending waits for idle.
- `T2_F18_04`: Vulkan device lost event caught and logged gracefully without hanging the host operating system.
- `T2_F18_05`: Running with full Vulkan sync and validation layers reports zero memory leaks, handle leaks, or barrier hazards.

---

## 5. Tier 3: Cross-Feature Combinations (Pairwise Integration)

Tier 3 tests verify the correct integration across distinct subsystems:

| Test ID | Subsystem Interaction | Verification Invariant |
|---|---|---|
| `T3_COMB_01` | **BSP Geometry (F05) + Static Lights (F10)** | Static BSP light entities illuminate world geometry with ray-traced direct and diffuse indirect bounces. |
| `T3_COMB_02` | **Studio Models (F07) + Player Flashlight (F11)** | Animated Gordon viewmodel weapon and world NPCs cast dynamic ray-traced shadows from flashlight spotlight. |
| `T3_COMB_03` | **Volumetric Fog (F14) + Point/Spot Lights (F10, F11)** | High-intensity spot lights and flashlight produce visible light shafts in participating volumetric medium. |
| `T3_COMB_04` | **PBR Materials (F16) + World BSP Geometry (F05)** | Auto-detected normal, roughness, metallic, and emissive textures render with realistic specular reflections on BSP walls. |
| `T3_COMB_05` | **2D HUD (F09) + 3D Path-Traced Scene (F03, F04)** | 2D HUD (health, ammo, crosshair) overlays over 3D rendered frame via `SubmitUIDrawList` with zero depth fighting or color corruption. |
| `T3_COMB_06` | **Movable Brushes (F06) + Dynamic Lights (F12)** | Moving platforms and doors receive real-time dynamic light from explosions and cast moving shadows. |
| `T3_COMB_07` | **Sprites & Beams (F08) + Volumetric Atmosphere (F14)** | Glowing energy beams and translucent sprites blend correctly with volumetric fog without clipping artifacts. |
| `T3_COMB_08` | **Special Map Headlights (F13) + Movable Submodels (F06)** | Tram headlight follows tram submodel position and orientation dynamically through curved tunnel path. |
| `T3_COMB_09` | **USD Modding Hashes (F17) + PBR Textures (F16)** | Modded asset replacement hashes override default PBR material bindings while preserving fallback mesh parameters. |
| `T3_COMB_10` | **Rapid Level Transition (F18) + Active Dynamic Lights (F12) + UI Textures (F09)** | Changing level while dlights, muzzle flashes, and UI textures are active cleanly purges all handles without memory leaks. |
| `T3_COMB_11` | **Z-Up Camera Setup (F04) + Volumetric Up-Axis (F15)** | Half-Life Z-up orientation (`hostUpAxis = 2`) perfectly matches volumetric up-axis (`volumetricUpAxis = 2`). |
| `T3_COMB_12` | **Studio Model Skeletal Animation (F07) + Movable Brush Attachment (F06)** | NPC or player standing on moving elevator moves smoothly with parent transform under ray-traced shadows. |
| `T3_COMB_13` | **Lightstyle Flickering (F10, F13) + PBR Emissive Surfaces (F16)** | Fluorescent light flickering style synchronizes light intensity with emissive texture surface radiance. |
| `T3_COMB_14` | **Window Resize / Minimization (F03) + Active Path Tracing Presentation (F02, F04)** | Resizing the game window triggers swapchain recreation without Vulkan validation error or crash. |
| `T3_COMB_15` | **Missing Remix DLL Graceful Fallback (F02) + Ref API Initialization (F03)** | If `remix.dll` is missing, `ref_openremix` reports clean error to `engfuncs.Host_Error` without fatal crash. |

---

## 6. Tier 4: Real-World Scenarios & Workloads

Tier 4 tests validate end-to-end execution of realistic gameplay scenarios and stress workloads:

### Scenario 1: Half-Life Map `c1a0` (Hazard Course / Inbound Transition)
- **Workload**: Load Half-Life 1 map `c1a0` with `-ref openremix`.
- **Validation**:
  1. Engine loads all world BSP geometry (15,000+ triangles).
  2. Extracts over 80 static `light` and `light_spot` entities.
  3. Spawns Barney and scientist studio models with animated idle sequences.
  4. Renders 2D HUD (100 HP, HEV suit battery, crosshair) legibly over 3D scene.
  5. Sustains stable 60+ FPS frame presentation to Win32 window.

### Scenario 2: Intro Tram Sequence (`c0a0` through `c0a0e`)
- **Workload**: Execute the iconic Black Mesa Inbound monorail sequence.
- **Validation**:
  1. Tram carriage submodel moves smoothly along path tracks.
  2. Tram headlight projects dynamic ray-traced cone forward into dark tunnels.
  3. Exterior environment sky and sun illuminate massive canyon vistas.
  4. Intermittent scripted alarms and flashing warning lights evaluate lightstyles without stutter.
  5. Audio-visual synchrony maintained during automated tram door opening.

### Scenario 3: Intense Combat Firefight Stress Workload
- **Workload**: Simulate heavy firefight with 10 HECU marines, 5 alien slaves, player firing MP5 grenades.
- **Validation**:
  1. Up to 15 animated studio models instanced simultaneously with software bone skinning.
  2. Multiple explosion dlights, muzzle flashes (elights), and rocket trails active in single frame.
  3. Particle explosion sprites and smoke beams composite with scene geometry.
  4. Zero Vulkan descriptor set exhaustion or buffer overrun.
  5. Peak memory usage remains strictly bounded.

### Scenario 4: Seamless Level Transition (`c1a0` $\rightarrow$ `c1a0a`)
- **Workload**: Trigger `changelevel` transition across map boundary.
- **Validation**:
  1. Previous map world BLAS, entity lights, and model instances destroyed cleanly.
  2. Player state, viewmodel weapon, and flashlight orientation transfer to new level.
  3. New map geometry and static lights parsed and uploaded without reloading `remix.dll`.
  4. Zero Vulkan validation layer error messages during transition.

### Scenario 5: Rapid Flashlight Dark Sector Exploration
- **Workload**: Navigate dark sector (Sector 7 power outage) with rapid flashlight cycling.
- **Validation**:
  1. Flashlight instantly illuminates pitch-black rooms with crisp ray-traced contact shadows.
  2. Toggling flashlight 20 times within 5 seconds causes zero handle leak or micro-stutter.
  3. Light cone angle and attenuation falloff match human flashlight physics.

### Scenario 6: Resolution Change & Fullscreen Toggle Stress
- **Workload**: Dynamically cycle video modes: 1280x720 windowed $\rightarrow$ 1920x1080 borderless $\rightarrow$ 2560x1440 fullscreen.
- **Validation**:
  1. Win32 `HWND` resized and Vulkan swapchain recreated cleanly.
  2. 2D HUD adapts coordinates and redraws at new resolution without stretching.
  3. Path tracer internal buffers reallocated without memory leaks.

### Scenario 7: Headless & Offline Fallback Validation
- **Workload**: Run test suite in headless continuous integration environment without physical display.
- **Validation**:
  1. Validates loader handshake, exports, ABI offsets, parsing algorithms, and contract tables.
  2. Gracefully detects absence of Vulkan device and skips GPU presentation without reporting false failures.
  3. Returns standard exit code `0` on test pass.

---

## 7. Automated Test Runner & Execution Specification

### 7.1 Architecture
The automated test runner is implemented in `tests/e2e/runner.py` using Python 3 (standard library only: `unittest`, `ctypes`, `struct`, `os`, `sys`, `subprocess`, `hashlib`). It requires **zero external third-party dependencies**, ensuring execution across all Windows developer machines and CI runners.

### 7.2 Directory Layout
```
tests/e2e/
├── runner.py                   # Central CLI test runner with exit code 0/1
├── common.py                   # Shared utilities: PE parser, struct validators, mock engine
├── test_tier1_features.py      # Tier 1 Feature Coverage (Features 1-18, >=5 tests each, 90 tests)
├── test_tier2_boundaries.py    # Tier 2 Boundary & Corner Cases (Features 1-18, >=5 tests each, 90 tests)
├── test_tier3_combinations.py  # Tier 3 Pairwise Subsystem Interactions (15+ combination tests)
└── test_tier4_scenarios.py     # Tier 4 Real-World Map Loads & Workloads (7 scenario tests)
```

### 7.3 CLI Invocation
```powershell
# Run entire test suite (Tiers 1-4)
python tests/e2e/runner.py

# Run specific tier
python tests/e2e/runner.py --tier 1
python tests/e2e/runner.py --tier 2
python tests/e2e/runner.py --tier 3
python tests/e2e/runner.py --tier 4

# Run with verbose output
python tests/e2e/runner.py --verbose

# Run specific feature tests
python tests/e2e/runner.py --feature 2
```

### 7.4 Exit Code Contract
- `0`: All executed test cases passed successfully.
- `1`: One or more test cases failed, or fatal runner execution error.
