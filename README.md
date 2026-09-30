# Stallion77 Skin Changer

Standalone macOS ARM64 live skin changer. The application code is C++ and
Objective-C++; the only Python file scans a new League binary for offsets.
There are no Tauri, Rust, TypeScript, or Zushi source dependencies.

## Run

From this folder, execute `./StallionSkinChanger`. It scans the installed
ARM64 League game, refreshes the official Riot skin list on every launch, and
opens a borderless ImGui overlay over a foreground match. Select the champion in your current match and
a skin. A game-function hook retains the selected ID when the actor's skin
setup runs again after a respawn. The hook remains in the game process until
the match ends, even if the UI closes. The ImGui window is external to the
game renderer; borderless or windowed game mode may be needed for macOS to
place an external window above the game.
The header's `_` button reduces the overlay to a title-only bar. Drag either
header to move the overlay; click the minimized bar without dragging to restore
it. Drag the grip in the bottom-right corner to resize it: the lists follow the
window height and the text and spacing scale with its width, so nothing is cut
off. The mouse wheel scrolls the lists, and a small cursor is drawn where a
click will land. Size and position stay inside the game window and are
remembered in `overlay.cfg` next to the program (`cursor=0` turns the drawn
cursor off; delete the file to reset).
Only one Stallion instance can run at a time, and the overlay hides when the
League match is no longer the foreground app. Its input tap also releases
keyboard and mouse input as soon as another app becomes active.

League switches the display to its own resolution when a match starts. The
overlay waits until the game window and display mode have stopped changing,
appears in the top-right corner of the game window, and follows the window if
it changes. It confirms its real position with WindowServer and corrects it if
macOS placed it somewhere else. Clicks are mapped with the cursor position of
the mouse event itself and the window position WindowServer reports for the
overlay. Process and window queries run on a helper thread, and the overlay
only redraws when something changed.

For a game installed elsewhere, pass the selected outer `League of Legends.app`
or inner `LeagueofLegends.app` as the first argument. The program appends the
fixed `Contents/LoL/Game/LeagueofLegends.app/Contents/MacOS/LeagueofLegends`
suffix for the outer app. If game task access is denied, launch
the program with `sudo` from Terminal.

## Skin IDs

The IDs and names come from [Riot Data Dragon](https://developer.riotgames.com/docs/lol#data-dragon),
using the installed game's patch version and each champion's `skins` array.
The generated `skin_ids.json` is checked against the fresh Riot list at each
launch; `data/skin_ids.json` is an offline snapshot. `Skins: OK` appears only
after that full download and local readback succeed. Incomplete downloads
leave the last usable catalog intact and keep switching disabled.

Typing into the overlay's search field uses a macOS event tap so gameplay
hotkeys do not fire while the field is active. macOS Accessibility/Input
Monitoring access is required for that input capture.

## Game updates

`tools/scan_offsets.py` scans the ARM64 Mach-O slice for the frame hook,
skin setup function, local actor global, and two actor field offsets. It
verifies a unique setup call and related ARM64 instructions before replacing
`offsets.json`. `stallion-core` checks the UUID of the running game against
the profile before reading or patching memory. If the patterns or function
layout change, scanning fails and live switching remains disabled. Such a
patch requires a new verified signature; no scanner can safely infer every
future semantic code change from old bytes alone.

Manually check without writing:

```sh
python3 tools/scan_offsets.py --check
```

## Build

Requires Xcode Command Line Tools, GNU Make, Homebrew GLFW, and RapidJSON.
Make downloads pinned Dear ImGui v1.91.9b.

```sh
make
make clean
```

The included binaries were built for this Mac. They are signed ad hoc; the
core carries the macOS debugger entitlement used by the live game call.
