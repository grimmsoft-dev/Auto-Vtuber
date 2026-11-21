# Auto-Vtuber

A one-click launcher and plugin for [VTube Studio](https://store.steampowered.com/app/1325860/VTube_Studio/) on Windows, for streaming without a webcam. Double-click it and it:

1. starts VTube Studio through Steam (if it isn't already open), then
2. connects to VTube Studio as the plugin **"Lily Gaze Helper"** and makes your model:
   - **follow your mouse** with eyes, head and body,
   - **look around naturally** after the mouse has been still for 3 seconds, with small head tilts,
   - **look at your collab partner while they talk** (optional): in a VTube Studio collab where you both run Auto-Vtuber and share a Discord voice call, each model turns towards the other person while they speak, even if other people are in the call,
   - **watch your console game** (optional): when the [NitLink](https://store.steampowered.com/app/5346980/) capture card viewer is open in a window, the model looks at where the action is on screen and jolts slightly at loud game sounds.

It lives in the system tray and closes itself when VTube Studio closes.

## Requirements

- Windows 10 (version 2004 or later) or Windows 11, 64-bit.
- VTube Studio from Steam.

## Setup

1. Download `LilyVTubeLauncher.exe` from the [latest release](../../releases/latest) and put it anywhere (a desktop shortcut to it makes a nice one-click start).
2. In VTube Studio, open **Settings** and turn on **Start API** (port 8001).
3. Run `LilyVTubeLauncher.exe`. The first time, VTube Studio asks whether to allow **"Lily Gaze Helper"**: click **Allow**.
4. Map the helper's inputs to your model in VTube Studio (**model settings**, the person icon). For each parameter below, set its **Input** to the helper's parameter:

   | Model parameter | Input | Suggested input range | Suggested output |
   |---|---|---|---|
   | Face Left/Right Rotation | `CompanionLookX` | -1 to 1 | -30 to 30 |
   | Face Up/Down Rotation | `CompanionHeadY` | -1 to 1 | -30 to 30 |
   | Face Lean Rotation | `CompanionTilt` | -1 to 1 | -10 to 10 |
   | Eye X | `CompanionLookX` | -1 to 1 | -1 to 1 |
   | Eye Y | `CompanionLookY` | -1 to 1 | -1 to 1 |
   | Body Rotation X | `CompanionLookX` | -1 to 1 | -10 to 10 |
   | Body Rotation Y | `CompanionLookY` | -1 to 1 | -10 to 10 |

   Smoothing of about 10 for the eyes, 15 for the head and 25 for the body looks natural. For mouth movement from your microphone, set Mouth Open's input to `VoiceVolumePlusMouthOpen` and turn on the microphone in VTube Studio's settings.

## Looking at your collab partner (Discord)

The helper asks the Discord app on your PC who is talking in your voice call, so it can tell your partner apart from anyone else in the call. Each person sets this up once on their own PC:

1. Go to the [Discord Developer Portal](https://discord.com/developers/applications), sign in, and click **New Application** (any name, for example "Auto-Vtuber").
2. Open **OAuth2** in the app's menu:
   - copy the **Client ID**,
   - click **Reset Secret** and copy the **Client Secret**,
   - under **Redirects**, add `http://localhost` and save.
3. In Auto-Vtuber's tray menu choose **Set up Discord...**, paste the Client ID and Client Secret, and click **Save**.
4. Discord pops up an authorization request: click **Authorize**.
5. Join the Discord voice call. In the tray menu, pick your partner under **Collab partner**, and choose **Partner is on the left/right of the screen**.

While your partner talks and your mouse has been still for 3 seconds, your model looks their way. Moving the mouse always takes over, and your own talking doesn't change anything, so your model keeps its normal movement while you speak.

Notes:
- Making your own Discord application (as above) is simplest. If one person's application is shared instead, the other person must be added as an **App Tester** of it in the Developer Portal, and the secret must be sent privately. Never put the secret in a public place.
- The ID, secret and Discord login are stored encrypted for your Windows user in `%LOCALAPPDATA%\LilyVTubeLauncher\discord.dat`. To use a different application, choose **Set up Discord again...**.
- Discord keeps this local connection (RPC) in a limited-access state; it may change in a future Discord update. If it stops working, the rest of the helper is unaffected.

## Tray menu

Click the tray icon to see the connection status for VTube Studio, NitLink and Discord, turn **Watch the game in NitLink when the mouse is idle** on or off, choose your collab partner and their side, set up Discord, or stop the helper (VTube Studio keeps running).

## Notes

- `CompanionHeadY` is the same as `CompanionLookY` except looking up is halved, so the head doesn't tip back too far while the eyes keep their full range.
- The mouse position uses the same screen area as VTube Studio's own mouse inputs (from its config), or the main monitor.
- Game watching assumes the model sits bottom-left on stream: while it is on, the model looks mostly towards the right of the camera. The NitLink window can be covered or on another monitor, but not minimised. Only NitLink's own audio is listened to.
- Priority: mouse first, then your talking collab partner, then the game, then idle glances.
- Settings, the VTube Studio access token, the encrypted Discord login and a game-watch measurement log (`gamewatch.log`) are kept in `%LOCALAPPDATA%\LilyVTubeLauncher`.

## Building from source

Needs Visual Studio 2022 (Desktop development with C++) and CMake 3.25 or later.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The exe is written to `build\Release\LilyVTubeLauncher.exe` and needs no extra runtime files.
