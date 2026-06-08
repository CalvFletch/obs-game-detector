# OBS Game Detector

Automatically creates a **Game Audio** capture source in OBS when you launch a game, placed in a locked group inside the scenes you choose.

## Requirements

- Windows
- OBS Studio 31 or later

## Install

1. Download the latest **windows-x64** `.zip` from [Releases](https://github.com/CalvFletch/obs-game-detector/releases).
2. Extract the archive.
3. Copy the `obs-game-detector` folder into:

       %ProgramData%\obs-studio\plugins\
4. Restart OBS.

## Setup

Open **Tools → Game Detector Settings** in OBS.

### Known Games

Every game the plugin has detected is listed here. All changes apply immediately — no Apply button needed.

- **Capture checkbox** — uncheck to stop OBS creating an audio source for that game.
- **Audio track columns** — override which recording tracks carry a game's audio. Track columns match your OBS **Settings → Output → Recording** setup. The **Default** row sets the baseline for all games.
- **Right-click a game row:**
  - **Disable** — grays out the game and stops audio capture. The row stays in the list.
  - **Enable** — re-enables a disabled game, restoring its previous capture state.
  - **Show Disabled / Hide Disabled** — toggles visibility of grayed-out rows.
  - **Enable All** — re-enables every disabled game at once.

### Lookup Directories

Steam, Epic, GOG, and Ubisoft libraries are discovered automatically. Use **Add Directory** only for games installed outside those libraries. **Refresh Game Libraries** rescans your launchers after a new install. **Remove** deletes a custom directory entry; **Restore Defaults** resets to auto-discovered paths only.

### Target Scenes

Check the OBS scenes that should receive the **Game Audio** group. Changes apply immediately.

## How it works

When a game process is detected the plugin:
1. Creates a **Game Capture** audio source named after the game.
2. Places it inside a locked **Game Audio** group in every checked scene.
3. Removes the source when the game exits.

## Troubleshooting

**A game was not detected**

- Make sure the game is running and was launched from a known library path.
- Open settings and click **Refresh Game Libraries**.
- If the game lives outside Steam/Epic/GOG/Ubisoft, add its install folder under **Lookup Directories**.

**Audio source was created for the wrong process**

- Some launchers spawn helper processes before the real game. The plugin filters common launcher and anti-cheat processes. If something still slips through, right-click it under **Known Games** and choose **Disable**.

## Building from source

See [BUILD.md](BUILD.md).
