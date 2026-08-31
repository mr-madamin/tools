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
