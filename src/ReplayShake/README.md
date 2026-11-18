# replay shake

camera shake for the skate. replay editor. handheld, walking, running, vehicle, earthquake, drift or your own, and
each keyframe can have its own shake that blends into the next one.

it shakes the actual view the game renders from, so shadows, fog and motion blur all move with it like a real
camera would, and exports come out the same as what you saw in the editor.

## where it is

- Insert > SETTINGS > REPLAY SHAKE (after REPLAYS)
- replay plus's SHAKE tab, or hold R3 and press dpad left (F11 on keyboard) to turn it on and off
- the console (~), with `replayshake ...`

your settings and every keyframe's shake are saved in `%LOCALAPPDATA%\ReSkate\profiles\offline\replay_shake.json`.

## setup

there's nothing to set up on skate. build 25414733 (the one reskate's launcher keeps), it already knows the replay
editor's camera. if a new game build ever comes out: `replayshake probe 1`, open the replay editor, and add the
camera it shows with `replayshake camera add 0x...`.

## exports

the shake is timed by the export's frames. if you export at something other than 60 fps, set the same rate with
`replayshake fps 30` (or 24) so the shake runs at the right speed.

## commands

| command | |
|---|---|
| `replayshake on` / `off` | shake on or off |
| `replayshake default preset handheld` | the default shake's style: handheld, walking, running, vehicle, earthquake or drift |
| `replayshake default intensity 130` | how strong it is, in percent (up to 300) |
| `replayshake default speed 1.1` | how fast it moves |
| `replayshake fps 60` | your export's frame rate |
| `replayshake gameplay 1` | shake while skating too |
| `replayshake status` | what it's doing right now |
