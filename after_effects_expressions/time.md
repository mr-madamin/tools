# Time and timecode

## Display

### Show the current time as timecode

`timeToTimecode()` formats a time value (in seconds) as a `HH:MM:SS:FF` string. With no arguments it uses the current time, including the comp's display start time. Apply to Source Text.

```js
timeToTimecode()
```

Related converters, all taking seconds and returning a formatted value:

```js
timeToCurrentFormat()  // matches the project's display format (timecode or frames)
timeToFrames()         // frame number, as a number
framesToTime(120)      // seconds, from a frame count
```

### Frame counter

`timeToFrames()` counts from the comp's display start time, so subtracting `inPoint` gives frames elapsed since the layer began instead of since the comp did.

```js
f = timeToFrames(time - inPoint);
"Frame " + f
```

### Elapsed time on a layer, as timecode

Pass `isDuration = true` so the result is treated as a length rather than an absolute position on the timeline — otherwise the comp's display start time gets folded in.

```js
timeToTimecode(time - inPoint, 1 / thisComp.frameDuration, true)
```

