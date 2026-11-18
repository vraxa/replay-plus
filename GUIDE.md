# replay plus guide

## installing

easiest way is the updater. grab `ReplayPlusUpdater.exe` from the
[releases page](https://github.com/vraxa/replay-plus/releases/latest), close skate. and the reskate launcher, and
run it. the first time it asks where your ReSkateLauncher.exe is. then pick one:

- Install Replay Plus + Animator Studio: grabs the newest ready-made animator studio with replay plus on it, and
  the reskate launcher that goes with it. it's a download, never a build, so it's quick. if a brand new studio
  hasn't got a build yet you get the one before it
- Update Replay Plus: the newest replay plus for the studio you have now. the studio stays exactly as it is. on the
  plain studio this button says Add Replay Plus
- Update Animator Studio: the newest studio, and replay plus stays on it if you have it
- Remove Replay Plus: swaps it back to the plain animator studio (the same version, from fourth studio's own
  release) and keeps your presets
- Vanilla ReSkate: plain reskate from reskate's own release, no studio and no replay plus. it turns the launcher's
  own updates back on, so reskate keeps itself up to date like normal
- Put back what I had before: undoes the lot

everything comes from the original releases (replay plus from here, the studio from fourth studio, reskate and its
launcher from reskate), every file gets checked before it touches anything, it keeps a copy of what you had, and
while the studio or replay plus is in it turns off the launcher's auto updates so it doesn't swap the dll back. run
it again whenever you want to update.

replay plus is built into the studio's dll, so there's a build for each studio version (`ReSkate-studio-2.1.5.dll`
and so on in the release), and Install always uses one of those. if you update replay plus or the studio onto a
version that hasn't got one yet, the windows updater can make it itself: it grabs
that studio's source, puts replay plus on it, builds it and runs the tests before it installs anything. for that you
need git, cmake and the visual studio 2022 build tools with c++ (all free), about 4 GB of space and 10 to 30
minutes. if replay plus doesn't go onto that studio by itself (the studio changed too much), it says so and leaves
everything alone.

on mac and linux the game runs through crossover, whisky, proton or wine, so reskate lives inside that bottle or
prefix (its drive_c). there's an updater for those too, in the same release:

- mac (apple silicon): `ReplayPlusUpdater-macOS.zip`. unzip it and open Replay Plus Updater. macOS blocks it the
  first time since it isn't from the app store, so go to System Settings > Privacy & Security and hit Open Anyway
- linux: `ReplayPlusUpdater-x86_64.AppImage`. make it executable (`chmod +x`, or right click > properties > allow
  running it as a program) and open it

they look for your reskate folder in the usual bottles and prefixes on their own, or you can point them at it (on
mac, Cmd+Shift+. shows hidden folders like Library). they do the same things as the windows one, they just can't
build replay plus themselves, so they stick to the ready-made builds.

doing it by hand: rename your ReSkate.dll to ReSkate.dll.studio, put the new one in (the `ReSkate-studio-...dll`
for your studio version, renamed to ReSkate.dll), and set `"updates"` to `false` in ReSkateLauncher.settings.json.

## the menu

in the replay editor hold R3 and press RB, or press F10.

- LB / RB: tabs
- dpad: up and down picks a row, left and right changes it (hold to go faster)
- A selects, X resets to default, holding Y hides the menu so you can see the picture, B closes it

on keyboard it's the arrows or WASD, Enter or Space, R to reset, Q / E or Tab for tabs, and Backspace or Esc to
close. Home / End jump to the first / last row, and Shift with left / right does bigger steps. with an xbox pad
you can still scrub the replay with the triggers while the menu's open.

the mouse works too. open the menu with F10 (or just move the mouse while it's open) and you get a pointer: click
the tabs and rows, drag the sliders, click either side of a value to step it (hold to keep going), scroll the list
with the wheel. the button prompts at the bottom are clickable as well.

## R3 shortcuts

hold R3 in the replay editor and:

- RB: open the menu
- dpad up / down: focus further / nearer (puts a focus key where you are)
- LT / RT: less / more blur
- dpad right: camera light on / off
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
- focus: depth of field, fixed, keyframed along the replay, or auto (whatever's in the middle). it blurs what's
  behind the focus too
- light: a light on the camera
- game: the game's own effects held where you put them (motion blur, bloom, lens flares, fog, exposure and so on)
- video: sharp middle (renders finer so a wide fisheye doesn't go soft in the middle), the editor's picture shape
  (a real 4:3 that isn't stretched), render resolutions, a bigger texture pool for big maps, and where your exported
  videos go
- presets: your saved looks
- experimental: newer stuff that reads the game's own replay, so a game update might break it for a bit. Skater
  autofocus (auto follows your skater wherever they are in the shot instead of whatever's in the middle) and Keep
  keys (below)

## keeping your keys

the game wipes your camera keyframes when you leave the replay editor. with Keep keys on (experimental tab, it's on
by default) they're saved when you leave and put back when you open the editor again, along with your focus and shake
keys, as long as that bit of footage is still in the replay. turn it off and every visit starts clean. keys don't
carry over a game restart, since the replay's gone by then.

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
| exported videos | the game's own folder (`%LOCALAPPDATA%\ReSkate\Game\Skate\data\Videos`), or wherever you set Videos go to (video tab) |
| settings | `%LOCALAPPDATA%\ReSkate\profiles\offline\replay_plus.json` |

## good to know

- everything shows up in exported videos too, not just on screen
- for sharper exports turn up Export quality in Insert > SETTINGS > REPLAYS
- with your own video folder set, each export gets moved there a second or two after it's done. the game's own
  "exported" popup still says your AppData folder, just ignore that
- the 4:3 picture shape needs the game in borderless fullscreen (that's the default)
- the R3 shortcuts need an xinput controller, so an xbox pad, or a ps pad through ds4windows or steam input
- the studio's own updater is off in this dll since it would put back a studio without replay plus. use the
  replay plus updater instead
- `replayplus help` in the console (~) lists every command
