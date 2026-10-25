# Xash3D FWGS + OpenRemix

This fork adds `ref_openremix`, a path-traced renderer for Half-Life built on [OpenRemix](https://github.com/Veitaserum4/OpenRemix-J). The engine is unchanged from upstream Xash3D FWGS; the fork adds the renderer (`ref/openremix`) and a **Ray tracing** options page in the game menu.

> This project uses AI-generated code.

## Features

- Path-traced world, brush entities and models, with the map's lights, the sun, the flashlight and dynamic lights (muzzle flashes, explosions).
- Glowing light fixtures and screens light the room. Beams, glow sprites and sparks emit light too.
- Refractive glass, and water with depth absorption, waves and an underwater view (Snell's window).
- Mirrors and chrome with perfect reflections (primary surface replacement).
- Optional [XashRT](https://github.com/sultim-t/xash-rt) content: material settings, HD textures, normal and roughness/metal maps, and extra map geometry and lights such as mirrors.
- The original HUD and menus, plus upscaling (DLSS, FSR 3.1), DLSS frame generation and volumetric fog.

## Requirements

- Windows 10/11, x64.
- A GPU with hardware ray tracing. DLSS and frame generation need a compatible NVIDIA GPU.
- Half-Life from Steam (the `valve` folder).
- 64-bit game libraries: Steam's `hl.dll`/`client.dll` are 32-bit, so build [hlsdk-portable](https://github.com/FWGS/hlsdk-portable) for x64.

## Building

**Build OpenRemix first.** Follow its README; on Windows that's `scripts/windows/build-remix.ps1` from a Visual Studio x64 Developer PowerShell with Clang and the Vulkan SDK. The engine build needs its headers (`public/include`), and the game needs its runtime (`_work/packages/windows/bin`).

**Get the SDL3 Visual C++ development package** (`SDL3-devel-<version>-VC.zip`) from the [SDL releases](https://github.com/libsdl-org/SDL/releases). Extract it anywhere; waf expects its `include` and `lib/x64` layout.

**Build the engine with the renderer** from a Visual Studio x64 Developer command prompt:

```
git clone --recursive <this repository>
cd xash3d-fwgs-openremix
waf configure -8 --enable-ref-openremix --use-sdl3 -s <SDL3 folder> --openremix-include=<OpenRemix>/public/include
waf build
waf install --destdir=<game folder>
```

`-8` builds 64-bit, which OpenRemix requires. Instead of `--openremix-include`, you can set the `OPENREMIX_INCLUDE` environment variable, or keep the OpenRemix checkout next to this repository as `OpenRemix`. Never use GitHub's ZIP downloads: they leave out the submodules.

## Setting up the game folder

1. Run `waf install`, or copy `xash3d.exe`, `xash.dll`, `filesystem_stdio.dll`, `ref_openremix.dll`, `menu.dll` and `SDL3.dll` into one folder.
2. Copy the OpenRemix runtime next to them: everything in OpenRemix's `_work/packages/windows/bin` (`remix.dll`, `SDL3.dll`, the `dlss` folder).
3. Put your 64-bit game libraries in `valve/cl_dlls` and `valve/dlls`.
   - The engine loads the menu from `valve/cl_dlls/menu.dll` before the copy next to `xash3d.exe`. If you keep a `menu.dll` there, replace it with this build's.
4. Run:

   ```
   xash3d.exe -ref openremix -basedir "<Steam>\steamapps\common\Half-Life" -game valve
   ```

   `-basedir` reads the game's content from your Steam install. Leave it out if the `valve` folder sits next to `xash3d.exe`.

If `remix.dll` lives somewhere else, point the renderer at it with the `r_remix_path` cvar or the `REMIX_PATH` environment variable.

### Optional: XashRT content

Download `hl1-rt-resources.zip` from the [xash-rt 1.0.5 release](https://github.com/sultim-t/xash-rt/releases/tag/1.0.5) and extract its `rt` folder next to `xash3d.exe`. Only `rt/data`, `rt/scenes`, `rt/mat` (`gfx`, `resource` and `#HUD` can be skipped) and the files directly in `rt` are used. Leave out `rt/valve`: its HD crowbar model replaces the stock one. Turn the content off with `rt_xrt 0`.

## Settings

Most settings are in the game menu under **Options → Video → Ray tracing**: upscaler and DLSS quality, vsync, frame generation and its multiplier, volumetric fog, and glass. **F12** opens the full OpenRemix overlay. Console variables:

| Cvar | Default | Effect |
|---|---|---|
| `rt_glass_texture` | 0.9 | Glass texture opacity; 0 = clear refractive glass |
| `rt_water`, `rt_water_dist`, `rt_water_ior` | 1, 300, 1.333 | Ray-traced water, its clarity (units) and refraction |
| `rt_water_fog` | 1 | Underwater fog density relative to Half-Life's (0 = clear) |
| `rt_chrome`, `rt_chrome_reflectance`, `rt_chrome_roughness` | 1, 0.6, 0.3 | Model chrome without XashRT materials as real metal, its reflectance and roughness (`rt_chrome 0` = classic fake chrome) |
| `rt_fx_emis` | 4 | Glow of beams and sprites |
| `rt_fx_lights` | 0 | Beams, sprites and sparks light their surroundings as emitters, on texture load (off: they only glow; on is very slow with many effects, as in the Resonance Cascade) |
| `rt_debug` | 0 | Print what the renderer draws and changes (sprites, light style changes) to the console |
| `rt_glow_sprites` | 0 | Draw the glare sprites that fake bloom on lights (they grow with distance) |
| `rt_tracer_emis` | 6 | Glow of sparks and tracers, relative to `rt_fx_emis` |
| `rt_liquid_emis` | 1 | Glow of glowing liquids such as toxic waste |
| `rt_light_e` | 5000 | Light from entity lights (alien controller orbs, Nihilanth's spheres) |
| `rt_emis` | 1 | Glow of light fixtures and screens |
| `rt_xrt` | 1 | Use XashRT content from `rt/` |
| `rt_xrt_scene_lights` | 8 | Brightness of XashRT's extra map lights |
| `rt_light_volumetric` | 0.02 | How much the map's own lights scatter in volumetric fog (the lights XashRT marks as volumetric use `rt_xrt_volumetric`), on map load |
| `rt_sun_volumetric` | 80 | How much more the sun scatters in volumetric fog (light shafts), on map load |
| `rt_xrt_volumetric` | 80 | How much more XashRT's fog lights (`isVolumetric`) scatter in volumetrics, on map load |
| `rt_autopbr` | 0 | Experimental: infer metal, roughness and normal maps for textures without XashRT or USD mod materials (`sound/materials.txt`, then texture names) |
| `rt_autopbr_normals` | 1 | Strength of the normal maps auto PBR generates from textures (0 = none) |
| `rt_texture_nearest` | 0 | Nearest-neighbour texture filtering (crisp texels) |
| `rt_caustics` | 1 | Caustics: light through water and bumpy glass is focused by their normal maps (strength and contrast in the OpenRemix overlay) |
| `rt_flsh`, `rt_flsh_cookie` | 8000, 1 | Flashlight brightness, and its light cookie (a texture projected along the beam; `rt/cookies/flashlight.png` replaces the built-in one) |
| `rt_ies` | 1 | IES light profiles for the flashlight and map spotlights; `rt/ies/flashlight.ies` and `rt/ies/spotlight.ies` replace the built-in ones |
| `rt_sun` | 50 | Strength of the map's sun (`light_environment`) |
| `rt_sky_brightness` | 8 | Sky brightness (skybox or scrolling clouds) |
| `rt_sky_light` | 1 | Light the sky casts on the scene (0 = none) |
| `rt_menu_warmup` | 1 | Prepare the path tracer in the main menu instead of at the first map load |

Material settings (glass, glow, XashRT) take effect on the next map load.

Until you save a profile in the overlay, OpenRemix starts with Xash3D's look: DLSS Balanced, 8 bounces, exposure 0.53, bloom 0.11 (spread 0.43), ORCA on, and contrast before tonemapping at 1.41. A saved profile (`openremix/profiles/`) replaces these.

---

> [!CAUTION]
> **Only download Xash3D FWGS from official sources.** Third-party builds, "modded launchers", "optimized" repacks and random mirrors are frequently bundled with malware, miners, spyware and credential stealers. We cannot vouch for anything we did not build. Get official binaries only from the [releases page](https://github.com/FWGS/xash3d-fwgs/releases/tag/continuous).

# Xash3D FWGS Engine <img align="right" width="128" height="128" src="https://github.com/FWGS/xash3d-fwgs/raw/master/game_launch/icon-xash-material.png" alt="Xash3D FWGS icon" />
[![GitHub Actions Status](https://github.com/FWGS/xash3d-fwgs/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/FWGS/xash3d-fwgs/actions/workflows/c-cpp.yml) [![FreeBSD Build Status](https://img.shields.io/cirrus/github/FWGS/xash3d-fwgs?label=freebsd%20build)](https://cirrus-ci.com/github/FWGS/xash3d-fwgs) \
[![Discord Server](https://img.shields.io/discord/355697768582610945?logo=Discord&label=International%20Discord%20chat)](http://xash.su/discord/) [![Russian speakers Telegram Chat](https://img.shields.io/badge/Russian_speakers_Telegram_chat-gray?logo=Telegram)](https://t.me/flyingwithgauss) \
[![Download Daily Build](https://img.shields.io/badge/downloads-testing-orange)](https://github.com/FWGS/xash3d-fwgs/releases/tag/continuous)

Xash3D ([pronounced](https://ipa-reader.com/?text=ks%C9%91%CA%82) `[ksɑʂ]`) FWGS is a game engine, aimed to provide compatibility with Half-Life Engine and extend it, as well as to give game developers well known workflow.

Xash3D FWGS is a heavily modified fork of an original [Xash3D Engine](https://www.moddb.com/engines/xash3d-engine) by Unkle Mike.

## Donate
[![Donate to FWGS button](https://img.shields.io/badge/Donate_to_FWGS-%3C3-magenta)](Documentation/donate.md) \
If you like Xash3D FWGS, consider supporting individual engine maintainers. By supporting us, you help to continue developing this game engine further. The sponsorship links are available in [documentation](Documentation/donate.md).

## Fork features
* Steam Half-Life (HLSDK 2.5) support.
* Crossplatform and modern compilers support: supports Windows, Linux, BSD & Android on x86 & ARM and [many more](Documentation/ports.md).
* Better multiplayer: multiple master servers, headless dedicated server, voice chat, [GoldSrc protocol support](Documentation/goldsrc-protocol-support.md) and IPv6 support.
* Multiple renderers support: OpenGL, GLESv1, GLESv2 and Software.
* Advanced virtual filesystem: `.pk3` and `.pk3dir` support, compatibility with GoldSrc FS module, fast case-insensitivity emulation for crossplatform.
* Mobility API: better game integration on mobile devices (vibration, touch controls).
* Different input methods: touch and gamepad in addition to mouse & keyboard.
* TrueType font rendering, as a part of mainui_cpp.
* External VGUI support module.
* PNG & KTX2 image format support.
* Ogg Vorbis (`.ogg`) & Ogg Opus (`.opus`) audio formats support.
* [A set of small improvements](Documentation/), without broken compatibility.

## Installation & Running
0) Get Xash3D FWGS binaries: you can use [testing](https://github.com/FWGS/xash3d-fwgs/releases/tag/continuous) build or you can compile engine from source code.
1) Copy engine binaries to some directory.
2) Copy `valve` directory from [Half-Life](https://store.steampowered.com/app/70/HalfLife/) to directory with engine binaries.
If your CPU is NOT x86 compatible or you're running 64-bit version of the engine, you may want to compile [Half-Life SDK](https://github.com/FWGS/hlsdk-portable).
This repository contains our fork of HLSDK and restored source code for Half-Life expansions and some mods.
You still needed to copy `valve` directory as all game resources located there.
3) Run the main executable (`xash3d.exe` or AppImage).

For additional info, run Xash3D with `-help` command line key.

### Android
0) Install the APK file.
1) Copy `valve` directory to a folder named `xash` in the Internal storage.
2) Run games from within the app.

## Reporting issues
* Issues are accepted in both English and Russian.
* They are only accepted if you run legally acquired product (e.g. Half-Life in Steam).

## Contributing code
* Check the CONTRIBUTING.md file.

## Build instructions
We are using Waf build system. If you have some Waf-related questions, I recommend you to read [Waf Book](https://waf.io/book/).

**NOTE: NEVER USE GitHub's ZIP ARCHIVES. GitHub doesn't include external dependencies we're using!**

### Prerequisites
If your CPU is x86 compatible and you're on Windows or Linux, we are building 32-bit code by default. This was done to maintain compatibility with Steam releases of Half-Life and based on it's engine games.
Even if Xash3D FWGS does support targetting 64-bit, you can't load games without recompiling them from source code!

If your CPU is NOT x86 compatible or you decided build 64-bit version of engine, you may want to compile [Half-Life SDK](https://github.com/FWGS/hlsdk-portable).
This repository contains our fork of HLSDK and restored source code for Half-Life expansions and some mods.

#### Windows (Visual Studio)
* Install Visual Studio.
* Install latest [Python](https://python.org) **OR** run `cinst python.install` if you have Chocolatey.
* Install latest [Git](https://git-scm.com/download/win) **OR** run `cinst git.install` if you have Chocolatey.
* Download [SDL2](https://libsdl.org/download-2.0.php) development package for Visual Studio.
* Clone this repository: `git clone --recursive https://github.com/FWGS/xash3d-fwgs`.
* Make sure you have at least 12GB of free space to store all build-time dependencies: ~10GB for Visual Studio, 300 MB for Git, 100 MB for Python and other.

#### GNU/Linux
##### Debian/Ubuntu
* Only for 32-bit engine on 64-bit x86 operating system:
  * Enable i386 on your system: `$ sudo dpkg --add-architecture i386`.
  * Install `aptitude` ([why?](https://github.com/FWGS/xash3d-fwgs/issues/1828#issuecomment-2415131759)):  `$ sudo apt update && sudo apt upgrade && sudo apt install aptitude`
  * Install development tools: `$ sudo aptitude --without-recommends install git build-essential gcc-multilib g++-multilib libsdl2-dev:i386 libfreetype-dev:i386 libopus-dev:i386 libbz2-dev:i386 libvorbis-dev:i386 libopusfile-dev:i386 libogg-dev:i386`.
  * Set PKG_CONFIG_PATH environment variable to point at 32-bit libraries: `$ export PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig`.

* For 64-bit engine on 64-bit x86 and other non-x86 systems:
  * Install development tools: `$ sudo apt install git build-essential python libsdl2-dev libfreetype6-dev libopus-dev libbz2-dev libvorbis-dev libopusfile-dev libogg-dev`.

* Clone this repostory: `$ git clone --recursive https://github.com/FWGS/xash3d-fwgs`.

##### RedHat/Fedora
* Only for 32-bit engine on 64-bit x86 operating system:
  * Install development tools: `$ sudo dnf install git gcc gcc-c++ glibc-devel.i686 SDL3-devel.i686 sdl2-compat-devel.i686 opus-devel.i686 freetype-devel.i686 bzip2-devel.i686 libvorbis-devel.i686 opusfile-devel.i686 libogg-devel.i686 libatomic.i686`.
  * Set PKG_CONFIG_PATH environment variable to point at 32-bit libraries: `$ export PKG_CONFIG_PATH=/usr/lib/pkgconfig`.

* For 64-bit engine on 64-bit x86 and other non-x86 systems:
  * Install development tools: `$ sudo dnf install git gcc gcc-c++ SDL3-devel sdl2-compat-devel opus-devel freetype-devel bzip2-devel libvorbis-devel opusfile-devel libogg-devel`.

* Clone this repostory: `$ git clone --recursive https://github.com/FWGS/xash3d-fwgs`.

#### Android (Windows/Linux/macOS)
* Install [Android Studio](https://developer.android.com/studio) (or the command line tools).
* Install [Python](https://python.org) (at least 2.7, latest is better).
* Install [Git](https://git-scm.com/download/win).
* Install [Ninja](https://ninja-build.org/).
* Install [CMake](https://cmake.org/) (for some dependencies).

* Clone this repostory: `$ git clone --recursive https://github.com/FWGS/xash3d-fwgs`.

#### iOS/iPadOS
* Install Xcode from the appstore.
* Install [Homebrew package manager](https://brew.sh).

* Install build dependencies by running: `brew install python`.

* Clone the SDL2 repo `$ git clone --recursive https://github.com/libsdl-org/SDL.git -b SDL2` and compile the iOS framework by navigating to SDL/Xcode/SDL and opening the Xcode project.

* Clone this repository: `$ git clone --recursive https://github.com/FWGS/xash3d-fwgs`.

### Building
#### Windows (Visual Studio)
0) Open command line.
1) Navigate to `xash3d-fwgs` directory.
2) (optional) Examine which build options are available: `waf --help`.
3) Configure build: `waf configure --sdl2=c:/path/to/SDL2`.
4) Compile: `waf build`.
5) Install: `waf install --destdir=c:/path/to/any/output/directory`.

#### Linux
If compiling 32-bit on amd64, make sure `PKG_CONFIG_PATH` from the previous step is set correctly, prior to running configure.

0) (optional) Examine which build options are available: `./waf --help`.
1) Configure build: `./waf configure` (you need to pass `-8` to compile 64-bit engine on 64-bit x86 processor).
2) Compile: `./waf build`.
3) Install: `./waf install --destdir=/path/to/any/output/directory`.

#### Android (Windows/Linux/macOS)
To build you should clone [SDL](https://github.com/libsdl-org/SDL) from `SDL2` branch and [HLSDK-portable](https://github.com/FWGS/hlsdk-portable) `mobile-hacks` branch repositories to 3rdparty folder, after that you should be able to open the project in Android Studio from `android` directory or manually call Gradle to build the APK.

#### iOS/iPadOS (MacOS only)
0) (optional) Examine which build options are available: `./waf --help`.
1) Configure build: `./waf configure --ios --enable-bundled-deps --sdl2 (path/to/SDL2.framework)`, set `--ios-simulator` instead of `--ios` if you want to build for simulator.
2) Compile `./waf build`.
3) Navigate to `build` and copy your compiled SDL2.framework there, then add your game dylibs to `build/ios/libs/(gamedir)/(dlls/cl_dlls)`(You can also run `scripts/ios/buildhlsdk.sh` instead to automatically create an ipa with hlsdk dylibs)
4) Run `scripts/ios/createipa.sh` to create an installable ipa

### Running tests

Tests are enabled with `--enable-tests` passed to `./waf configure` and can be run with `./waf --alltests`.

This builds both standalone unit tests and a separate engine test binary (`xash3d_tests`) that embeds engine-level tests. The engine test binary requires no game assets.
