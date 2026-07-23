# Built-in functions

## Time

### Animate a property continuously without keyframes

Adds a value that keeps changing every frame, driven by AE's internal clock (`time`, in seconds). Useful for constant rotation, spinning loaders, scrolling backgrounds, etc.

```js
time*50
```

### Posterizing time

Posterizing time gives animations or footage a choppy, stop-motion look by reducing the effective frame rate the expression is evaluated at, regardless of the comp's frame rate.

```js
posterizeTime(12)
```

### Time remapping / offsetting a property's value

Reads a property's value at a different point in time than the current frame — useful for delays, echoes, or trailing effects.

```js
// value 10 frames ago
thisComp.layer("Ball").transform.position.valueAtTime(time - 10/frameRate)
```

## Repeat

### Repeating animations without keyframes

Loops the keyframes on a property. `type` controls the loop style: `'cycle'` (jump back to start), `'pingpong'` (play forward then backward), `'offset'` (continue from where keyframes left off, repeating the delta), or `'continue'` (keep the velocity going past the last keyframe). `numKeyframes` limits the loop to the last N keyframes (`0` = all).

```js
loopOut(type = 'cycle', numKeyframes = 0)
```

`loopIn()` is the same but applies before the first keyframe instead of after the last.

### Staggering identical animations across duplicated layers

Offsets each layer's timing based on its layer index, so copies of the same animated layer play in a staggered sequence instead of in sync.

```js
delay = (index - 1) * 0.1; // seconds between each layer
thisComp.layer(index).transform.position.valueAtTime(time - delay)
```

## Wiggle

### Random, organic motion

Adds jittery movement/variation to a property. First argument is frequency (wiggles per second), second is amplitude (in the property's units, e.g. pixels for position).

```js
wiggle(1, 50)
```

### Consistent (repeatable) randomness

`seedRandom` locks the random sequence so every layer doesn't wiggle identically, and so re-rendering doesn't change the result. `timeless = true` keeps it fixed regardless of playhead position.

```js
seedRandom(index, true);
wiggle(2, 30)
```

## Math

### Math.round()

Rounds a value to the nearest whole number — handy for snapping a slider-driven value to whole units (e.g. frame counts, integer counters).

```js
Math.round(effect("Slider Control")("Slider"))
```
