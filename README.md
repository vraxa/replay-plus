# replay plus

a filming mod for the skate. replay editor. proper fisheye lenses, camcorder colour, real 4:3, focus you can
keyframe, a light on the camera and handheld shake. and it all shows up in your exported clips too, not just on
screen.

it's a ReSkate.dll with the animator studio (2.1.5, on reskate 20261116) built in, so you keep all the studio stuff
and online still works like normal.

**[download it here](https://github.com/vraxa/replay-plus/releases/latest)**

## install

grab the updater for your system from the [latest release](https://github.com/vraxa/replay-plus/releases/latest),
close skate. and the reskate launcher, and run it:

- windows: ReplayPlusUpdater.exe
- mac (apple silicon): ReplayPlusUpdater-macOS.zip
- linux: ReplayPlusUpdater-x86_64.AppImage

point it at the folder with ReSkateLauncher.exe and hit Install (on mac and linux it looks inside your crossover,
whisky, proton or wine bottles for it first). it gets the newest replay plus and the reskate launcher that goes with
it (from reskate's own releases), checks the files, keeps a copy of what you had and sets everything up. run it
again to update. "Remove Replay Plus" swaps it for the plain animator studio (your presets stay), and "Put back what
I had before" undoes the lot. if you don't have replay plus, "Update Animator Studio" keeps the plain studio (and its
reskate launcher) up to date.

none of them are signed, so windows or macOS might complain the first time. on windows hit More info > Run anyway,
on mac go to System Settings > Privacy & Security > Open Anyway, and on linux make the AppImage executable first.
if you'd rather do it by hand, the zip in the release has a readme that says how.

## what's in it

- lens: make your own fisheye (distortion, image circle, zoom, how much the edges get squished etc). there's a 16:9
  and a 4:3 fisheye preset to start from
- frame: 4:3 or 2.39 bars, and you can drop in your own hood / overlay png
- colour: camcorder style colour plus exposure, contrast, grain, vignette and all that
- focus: depth of field you can keyframe through the clip, or just leave on auto
- light: a light on the camera, good for night clips
- shake: handheld shake that you can turn on and off along the clip
- game: the game's own effects like motion blur, bloom, lens flares and fog
- video: a real 4:3 picture that isn't stretched, and a sharper middle when you're using a fisheye
- presets: save your look with a name, export it as a file and send it to people. presets people send you go in
  the import folder and just show up

## using it

open a replay, hold R3 and press RB (or hit F10) to open the menu. LB/RB switches tabs, the dpad changes stuff, A
selects, X resets and B closes it.

holding R3 in the editor also gives you shortcuts without opening the menu: dpad up/down and the triggers set focus
keys, dpad left turns the shake on or off, LB adds a shake key and View deletes the keys where you are. tapping R3
still takes a photo like it normally does.

the [guide](GUIDE.md) has the rest.

## stuff to know

- the studio's own auto updater is off in this dll, because it would swap it for a studio without replay plus. use
  the replay plus updater instead
- if you want sharper exports, turn up Export quality in Insert > SETTINGS > REPLAYS
- the 4:3 picture needs the game in borderless fullscreen (that's the default)
- the R3 shortcuts need an xinput controller, so an xbox pad, or a ps pad through ds4windows or steam input

## source

[src](src) has the code for replay shake and replay plus, and [patches](patches) has the same changes as patches
for the [animator studio](https://github.com/FourthStudio/ReSkate-Animator-Studio)'s source, which the release dll
is built from ([patches/SOURCE.txt](patches/SOURCE.txt) has the steps). [updater](updater) is the updater's source:
the windows one in c#, the mac and linux one in python, built by the workflow in [.github](.github/workflows).

## credits

built on [ReSkate](https://github.com/Dingo-Shenanigans/ReSkate) by Dingo Shenanigans and the
[Animator Studio](https://github.com/FourthStudio/ReSkate-Animator-Studio) by Fourth Studio. big thanks to both,
none of this would exist without them.

GPL-3.0, same as reskate. not affiliated with EA or Full Circle.
