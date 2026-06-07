# obs-game-detector

OBS Studio plugin that auto-detects running Steam / Epic / GOG games and creates
`wasapi_process_output_capture` audio sources inside a configured group with a
colour label — no WebSocket, no external process, no Python.

## How it works

- Polls running processes every 5 seconds (configurable)
- Matches process paths against Steam / Epic / GOG install directories
- Creates a `wasapi_process_output_capture` source for each detected game
- Places the source inside the **"Gaming Audio"** group in the **"Gaming"** scene
- Applies an orange colour label (configurable)
- Removes sources when the game process exits
- Handles BattlEye launchers (`_BE.exe` → actual game exe)

## Build (Windows)

### Prerequisites

| Tool | Version |
|------|---------|
| Visual Studio | 2022 (Desktop C++ workload) |
| CMake | >= 3.28 (already at `C:\Program Files\CMake`) |
| Git | any |

### Steps

```powershell
cd "C:\Users\CIsaa\Desktop\Development\obs-game-detector"

# Configure — downloads OBS source + pre-built deps automatically
cmake --preset windows-x64

# Build
cmake --build build_x64 --config RelWithDebInfo

# Install into OBS plugins folder
cmake --install build_x64 --config RelWithDebInfo
```

The `windows-x64` preset downloads the exact OBS source version listed in
`buildspec.json` and matching pre-built deps. First configure takes a few
minutes (~500 MB download).

The `--install` step copies `obs-game-detector.dll` to:

    C:\Program Files\obs-studio\obs-plugins\64bit\

Restart OBS after installing.

## Configuration

OBS -> Tools -> Game Detector Settings

| Setting | Default |
|---------|---------|
| Scene Name | Gaming |
| Group Name | Gaming Audio |
| Poll interval (sec) | 5 |
| Source Colour | Orange |
