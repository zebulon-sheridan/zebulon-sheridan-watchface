# Zebulon Sheridan

A Pebble watchface. Three panels: the time and date up top, the Zebulon
Sheridan wordmark in the center, and a chiron (ticker) at the bottom that
scrolls Heavy Comforter lyrics.

The chiron advances on a wrist flick (accelerometer tap) and also auto-advances
after each line finishes scrolling, so it stays alive on the wrist.

## The three panels

| Panel | What it is |
|---|---|
| Top | Time (42pt bold) with the date beneath it |
| Center | The Zebulon Sheridan wordmark (bundled bitmap, transparent on black) |
| Bottom | A chiron that scrolls Heavy Comforter lyric fragments |

## Building

Requires the Pebble SDK. See <https://developer.repebble.com>.

```sh
pebble build                          # build for all target platforms
pebble install --emulator basalt      # run in the basalt emulator
```

## Target platforms

aplite, basalt, chalk, diorite, emery. Defined in `targetPlatforms` in
`package.json`.

## Project layout

```
src/c/           C source for the watchface
resources/       Images and other bundled resources
package.json     Project metadata (UUID, platforms, resources)
wscript          Build rules
```

## Links

- Heavy Comforter: <https://heavycomforter.com>
- Zebulon Sheridan: <https://zebulonsheridan.com>
