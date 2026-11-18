# replay plus guide

## installing

easiest way is the updater. grab `ReplayPlusUpdater.exe` from the
[releases page](https://github.com/vraxa/replay-plus/releases/latest), close skate. and the reskate launcher, and
run it. the first time it asks where your ReSkateLauncher.exe is, then just hit Install.

it downloads the newest replay plus and the reskate launcher that goes with it (from reskate's own releases),
checks every file before it touches anything, keeps a copy of what you had, and turns off the launcher's auto
updates so it doesn't swap the dll back. run it again whenever you want to update. "Remove Replay Plus" swaps it
back to the plain animator studio (the same version, from fourth studio's own release) and keeps your presets, and
"Put back what I had before" undoes the lot.

on mac and linux the game runs through crossover, whisky, proton or wine, so reskate lives inside that bottle or
prefix (its drive_c). there's an updater for those too, in the same release:

- mac (apple silicon): `ReplayPlusUpdater-macOS.zip`. unzip it and open Replay Plus Updater. macOS blocks it the
  first time since it isn't from the app store, so go to System Settings > Privacy & Security and hit Open Anyway
- linux: `ReplayPlusUpdater-x86_64.AppImage`. make it executable (`chmod +x`, or right click > properties > allow
  running it as a program) and open it

they look for your reskate folder in the usual bottles and prefixes on their own, or you can point them at it (on
mac, Cmd+Shift+. shows hidden folders like Library).

doing it by hand: rename your ReSkate.dll to ReSkate.dll.studio, put the new one in, and set `"updates"` to
`false` in ReSkateLauncher.settings.json.

## the menu

in the replay editor hold R3 and press RB, or press F10.

- LB / RB: tabs
- dpad: up and down picks a row, left and right changes it (hold to go faster)
- A selects, X resets to default, holding Y hides the menu so you can see the picture, B closes it

on keyboard it's the arrows, Enter, R, Q / E and Backspace. you can still scrub the replay with the triggers
while the menu's open.

## R3 shortcuts

hold R3 in the replay editor and:

- RB: open the menu
- dpad up / down: focus further / nearer (puts a focus key where you are)
- LT / RT: less / more blur
- dpad right: a focus key here
- dpad left: shake on / off
- LB: a shake key (turns the shake on or off from that point)
- View: delete the keys where you are

F10 opens the menu and F11 turns the shake on or off. focus keys show up blue and shake keys orange on the
timeline. a quick tap of R3 is still the game's photo button.

## the tabs

- shake: handheld shake, its style, strength and speed
- lens: the fisheye. distortion (0% is no lens at all), image circle, edge squish, zoom and squeeze, plus fringe,
  shade, glare and softness if you want it to look like cheap glass. the field of view can be the editor's own,
  fixed, or auto
- frame: 4:3 or 2.39 bars, and an overlay, either your own png (a lens hood or whatever) or a round edge
- colour: a camcorder curve, exposure, contrast, saturation, warmth, lift / gamma / gain, split toning, grain
  and vignette
- focus: depth of field, fixed, keyframed along the replay, or auto. it blurs what's behind the focus too
- light: a light on the camera
- game: the game's own effects held where you put them (motion blur, bloom, lens flares, fog, exposure and so on)
- video: sharp middle (renders finer so a wide fisheye doesn't go soft in the middle), the editor's picture shape
  (a real 4:3 that isn't stretched), render resolutions, and a bigger texture pool for big maps
- presets: your saved looks

## presets

Save as new saves everything as it is and asks for a name. on a preset, left / right picks what A does: load,
rename, export or delete (delete asks twice and it goes to the recycle bin).

export puts a copy in the export folder, that's the file you send people. presets people send you go in the import
folder and get added on their own after a few seconds.

there are two to start from, Fisheye 16-9 and Fisheye 4-3. if you delete them they stay deleted.

## where stuff is

| what | where |
|---|---|
| presets | `%LOCALAPPDATA%\ReSkate\Replay Plus\Presets` |
| presets people send you | `%LOCALAPPDATA%\ReSkate\Replay Plus\Import` |
| presets you export | `%LOCALAPPDATA%\ReSkate\Replay Plus\Export` |
| overlay pngs | `%LOCALAPPDATA%\ReSkate\Replay Plus\Overlays` |
| settings | `%LOCALAPPDATA%\ReSkate\profiles\offline\replay_plus.json` |

## good to know

- everything shows up in exported videos too, not just on screen
- for sharper exports turn up Export quality in Insert > SETTINGS > REPLAYS
- the 4:3 picture shape needs the game in borderless fullscreen (that's the default)
- the R3 shortcuts need an xinput controller, so an xbox pad, or a ps pad through ds4windows or steam input
- the studio's own updater is off in this dll since it would put back a studio without replay plus. use the
  replay plus updater instead
- `replayplus help` in the console (~) lists every command
