# Stallion77 Skin Changer

Standalone macOS ARM64 live skin changer. The application code is C++ and
Objective-C++; the only Python file scans a new League binary for offsets.
There are no Tauri, Rust, TypeScript, or Zushi source dependencies.

## Run

From this folder, execute `./build/StallionSkinChanger`. It scans the installed
ARM64 League game, checks the official Riot skin list against the installed patch, and
opens a borderless ImGui overlay over a foreground match. Select the champion in your current match and
a skin. A game-function hook retains the selected ID when the actor's skin
setup runs again after a respawn. The hook remains in the game process until
the match ends, even if the UI closes. The ImGui window is external to the
game renderer; borderless or windowed game mode may be needed for macOS to
place an external window above the game.
Some skins have several forms under one skin ID (`mGearSkinUpgrades`), such as
Gun Goddess Miss Fortune's four weapons. After a switch, Stallion reads the
number of forms from the skin the game has loaded, so every skin with forms
gets one overlay button per form. Known lists are named: Miss Fortune 16
(Scarlet Fair, Zero Hour, Royal Arms, Starswarm), Battle Queen Katarina 29-36
(six daggers), PROJECT: Renekton 26-32, Pulsefire Ezreal 5, Elementalist Lux 7
(Light and nine elements) and Jax 14 (Wings); other skins, such as Spirit Guard
Udyr, get numbered buttons. For the named skins except Jax 14 the switch
selects the first form, because their meshes hide the weapons, or for Lux show
every element, until a form is equipped. A button stores the form in the
character stack and refreshes it on the game thread; the refresh calls the
actor's own gear setter, which shows that form's parts, hides the others and
installs its effects. Because the new mesh is still loading at the moment of the
switch, the overlay applies the form again about 1.5 seconds later; choosing the
current form again restores it after a respawn. The gear field and refresh
function must pass the installed binary's offset scan before live switching is
enabled.
Switching a skin does not make the game build a skin's own HUD, such as Gun
Goddess Miss Fortune's weapon picker above the portrait, or replace the
champion portrait. These HUDs (also for Elementalist Lux, Ahri 86, Viego 43,
Kai'Sa 71 and other skins) are only visual scenes in the client. A server script
that the champion gets only when the skin was chosen before the match opens
them, tracks their state and answers their clicks; for Miss Fortune that is the
`MissFortuneGunGoddessGearManager` passive, which the champion data limits to
skin 16. A live switch cannot add that script.
For Gun Goddess Miss Fortune, Stallion therefore draws the weapon picker itself,
from the game's own layout and textures: the frame with the four weapons above
the portrait, the equipped weapon highlighted, hover and pressed states, and
the round button on the portrait that opens and closes it. A click selects the
weapon like the overlay's buttons, and the click does not reach the game.
Stallion places the picker where the game places its own, anchored to the
bottom of the game area and scaled by the HUD scale setting (`[HUD] GlobalScale`
in `LoL/Config/game.cfg`, mapped to 0.66-1 as the game does). If it sits a
little off on your screen, hold Option and drag it; the offset is remembered in
`overlay.cfg` (`hud_dx`, `hud_dy`). The layout and atlas come from
[CommunityDragon](https://raw.communitydragon.org/) for the installed patch and
are cached in `build/skin_hud/`. Elementalist Lux's picker opens elements as the
match progresses, which only the server tracks, so Lux and other skins keep the
overlay's form buttons.
Some skins also play E or W through animation names of their own. The server
picks animation names for the skin it assigned at match start, so after a live
switch it still asks for the default skin's names, and the game falls back to
running or idling when the new skin lacks one. A survey of all 1650 champion
skin graphs on 16.21 found three such gaps with a clear replacement, which
Stallion adds to the switched skin's clip table: God-King Garen 13 and Fallen
God-King Garen 44 (`Spell3`, the two-sword E spin, plays `Spell3_Med`), Leona 8
(`Spell2` plays `Spell2_open`) and Jax 14 (`Spell3` plays the `Spell3_Idle`
spin). The entries are allocated with the game's own `operator new` on the game
thread, so the game frees them normally. Garen's spin always uses the
medium-speed clip, and the skins' own transition clips are not played. Other
gaps (DJ Sona, Sejuani 3, Akali 61 R, Anivia 5 Q, Kayn 20 E and a few more) have
no clip or no single equivalent clip and stay as they are.
Other transformation skins, such as DJ Sona, have additional gameplay behavior
that the current live appearance call does not initialize.
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
The generated `skin_ids.json` is tied to the installed patch. Use the overlay's
Refresh button to download the latest list for that patch; `data/skin_ids.json`
is an offline snapshot. Incomplete downloads leave the last usable catalog
intact and keep switching disabled for an outdated patch.
Refresh also rescans the installed game binary for offsets. For a terminal-only
refresh, run `./build/StallionSkinChanger --refresh-data`; it prints separate
offset and skin catalog results.

Typing into the overlay's search field uses a macOS event tap so gameplay
hotkeys do not fire while the field is active. macOS Accessibility/Input
Monitoring access is required for that input capture.

## Game updates

`tools/scan_offsets.py` scans the ARM64 Mach-O slice for the frame hook,
skin setup function, local actor global, character stack refresh, skin and
gear fields, the animation-player getter with its clip table, the path to the
skin's form list, and the game's `operator new`. It
verifies a unique setup call and related ARM64 instructions before replacing
`offsets.json`. Since patch 16.21 the actor's skin ID field is stored encoded;
the scanner reads the key and rotations from the decode in front of the setup
call. Schema 10 also captures the extra rotate and add used by newer builds;
`stallion-core` encodes the ID before writing it and refuses a field
that does not decode to a plausible ID. `stallion-core` checks the UUID of the running game against
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
