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

## Counters

### Countdown timer

Counts down from a fixed duration starting at the layer's in point, formatted `M:SS`. Clamped at zero so it holds rather than going negative once the layer outlasts the countdown.

```js
total = 90; // seconds
remaining = Math.max(0, total - (time - inPoint));
mins = Math.floor(remaining / 60);
secs = Math.floor(remaining % 60);
mins + ":" + (secs < 10 ? "0" + secs : secs)
```

### Countdown down to hundredths

Same idea at higher resolution — the sub-second part is what sells a stopwatch, so it needs its own zero padding.

```js
remaining = Math.max(0, effect("Duration")("Slider") - (time - inPoint));
secs = Math.floor(remaining);
cents = Math.floor((remaining - secs) * 100);
secs + "." + (cents < 10 ? "0" + cents : cents)
```

## Time remapping

These go on a layer's Time Remap property (`Layer > Time > Enable Time Remapping`), whose value is a time in seconds into the source.

### Play forward, then hold the last frame

Plays at normal speed from the layer's in point and freezes on the final frame instead of looping or going black. Backing off by one frame duration avoids landing past the end of the source.

```js
Math.min(time - inPoint, thisLayer.source.duration - thisComp.frameDuration)
```

### Constant speed change from a slider

Scales playback rate off a control instead of keyframing Time Remap by hand — `1` is normal speed, `0.5` half, `2` double, negative plays backwards.

```js
speed = effect("Speed")("Slider");
(time - inPoint) * speed
```

### Loop a section of the source

Wraps playback within a start/end window using the modulo operator, so a clip cycles a chosen range regardless of the layer's length in the comp.

```js
start = 1.0;  // seconds into the source
end = 3.5;
start + ((time - inPoint) % (end - start))
```
