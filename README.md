# Auto-Vtuber

A one-click launcher and plugin for [VTube Studio](https://store.steampowered.com/app/1325860/VTube_Studio/) on Windows, for streaming without a webcam. Double-click it and it:

1. starts VTube Studio through Steam (if it isn't already open), then
2. connects to VTube Studio as the plugin **"Lily Gaze Helper"** and makes your model:
   - **follow your mouse** with eyes, head and body,
   - **look around naturally** after the mouse has been still for 3 seconds, with small head tilts,
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

## Tray menu

Click the tray icon to see the connection status, turn **Watch the game in NitLink when the mouse is idle** on or off, or stop the helper (VTube Studio keeps running).

## Notes

- `CompanionHeadY` is the same as `CompanionLookY` except looking up is halved, so the head doesn't tip back too far while the eyes keep their full range.
- The mouse position uses the same screen area as VTube Studio's own mouse inputs (from its config), or the main monitor.
- Game watching assumes the model sits bottom-left on stream: while it is on, the model looks mostly towards the right of the camera. The NitLink window can be covered or on another monitor, but not minimised. Only NitLink's own audio is listened to.
- Settings, the VTube Studio access token and a game-watch measurement log (`gamewatch.log`) are kept in `%LOCALAPPDATA%\LilyVTubeLauncher`.

## Building from source

Needs Visual Studio 2022 (Desktop development with C++) and CMake 3.25 or later.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The exe is written to `build\Release\LilyVTubeLauncher.exe` and needs no extra runtime files.
