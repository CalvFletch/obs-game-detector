# OBS Game Detector

Automatically adds per game audio capture to OBS when you launch a game based on library locations.

## Requirements

- Windows
- OBS Studio 31 or later

## Install

1. Download the latest **windows-x64** `.zip` from [Releases](https://github.com/CIsaa/obs-game-detector/releases).
2. Extract the archive.
3. Copy the `obs-game-detector` folder into:

       %ProgramData%\obs-studio\plugins\
4. Restart OBS.

## Setup

Open **Tools -> Game Detector Settings** in OBS.

### Known Games

Every game the plugin has seen is listed here.

- Uncheck a game to stop OBS from creating an audio source for it.
- **Remove** drops a game from the list. It will come back if detected again.
- Set default audio tracks, or override tracks per game. Track options match your OBS **Settings -> Output -> Recording** track setup.

### Lookup Directories

The plugin finds games by matching running processes to install folders. Steam, Epic, GOG, and Ubisoft libraries are discovered automatically.

Use **Add Directory** only for games installed outside those libraries (for example, a standalone folder on another drive). **Refresh Game Libraries** rescans your launchers if you installed a game recently.

### Target Scenes

Choose which OBS scenes receive the **Game Audio** group. Only checked scenes are updated.

## Troubleshooting

**A game was not detected**

- Make sure the game is running and was launched from a known library path.
- Open settings and click **Refresh Game Libraries**.
- If the game lives outside Steam/Epic/GOG/Ubisoft, add its install folder under **Lookup Directories**.

**Audio source was created for the wrong process**

- Some launchers spawn helper processes before the real game. The plugin filters common launcher and anti-cheat processes; if something still slips through, disable that entry under **Known Games**.

## Building from source

See [BUILD.md](BUILD.md).
