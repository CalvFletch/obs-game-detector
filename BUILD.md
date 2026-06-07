# Building from source

This plugin targets **Windows only**. The CMake presets for macOS and Linux come from the OBS plugin template; game detection (`gd_watch`) depends on WMI and is not portable.

OBS version pinned in `buildspec.json` (currently **31.1.1**).

## Prerequisites (Windows)

| Tool | Version |
|------|---------|
| Visual Studio | 2022 with **Desktop development with C++** |
| CMake | 3.28 or later |
| Git | any |

## Configure and build

```powershell
git clone https://github.com/CIsaa/obs-game-detector.git
cd obs-game-detector
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

The `windows-x64` preset downloads the OBS sources and matching prebuilt dependencies listed in `buildspec.json`. First configure is a large one-time download (~500 MB).

Build output (without installing) lands in:

    build_x64/rundir/RelWithDebInfo/

## Install into OBS

Close OBS first. A running OBS instance locks the plugin DLL.

```powershell
cmake --install build_x64 --config RelWithDebInfo
```

Default install layout:

    %ProgramData%\obs-studio\plugins\obs-game-detector\bin\64bit\obs-game-detector.dll
    %ProgramData%\obs-studio\plugins\obs-game-detector\data\

Restart OBS after installing.

To install elsewhere, pass `--prefix` to `cmake --install`.

Worker threads enqueue events; all OBS API calls run on the UI thread via `processor_tick`.

## IDE / clangd

`.clangd` points at `build_x64` for `compile_commands.json`. Configure the project once so that database exists before relying on editor diagnostics.