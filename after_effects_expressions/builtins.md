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

### Wiggling only one axis

Runs `wiggle` on the full property, then swaps back in the original value for the axis you don't want jittered — here only Y moves, X stays put.

```js
w = wiggle(2, 50);
[value[0], w[1]]
```

### Layering wiggle on top of existing keyframes

Because `wiggle` returns a value relative to the property's current value, adding it to an offset (rather than replacing the property outright) preserves any keyframed animation underneath while adding jitter on top.

```js
value + wiggle(3, 15) - value
```

### Extra octaves for more detailed/organic noise

Third argument (`octaves`) layers additional, higher-frequency wiggles on top of the base one for a more complex, less mechanical result. Fourth argument (`amp_mult`) controls how much each extra octave's amplitude falls off (default `0.5`).

```js
wiggle(2, 30, 4, 0.5)
```

### Fading wiggle in/out over time

Multiplies the wiggle offset by a 0–1 ramp from `linear`/`ease`, so jitter can build up or settle down instead of being constant for the whole layer duration.

```js
amount = ease(time, 0, 2, 0, 1); // ramps up over the first 2 seconds
value + (wiggle(2, 50) - value) * amount
```

## Math

### Math.round()

Rounds a value to the nearest whole number — handy for snapping a slider-driven value to whole units (e.g. frame counts, integer counters).

```js
Math.round(effect("Slider Control")("Slider"))
```

### clamp()

Restricts a value to a minimum/maximum range, useful for keeping opacity, scale, or effect values from overshooting.

```js
clamp(value, 0, 100)
```

### linear() / ease()

Remaps a value from one range to another. `linear` is a constant rate of change; `ease` applies smooth acceleration/deceleration at the ends. Great for driving one property off another (e.g. opacity fading in/out based on position, or a slider controlling multiple properties).

```js
// as time goes from 0s to 1s, opacity goes from 0 to 100
linear(time, 0, 1, 0, 100)

// same, but with easing at the start/end
ease(time, 0, 1, 0, 100)
```

### random()

Returns a random number (or array, for multi-dimensional properties) each frame. Without arguments it returns a float between 0 and 1; with arguments it returns a value within that range.

```js
random(0, 100)
```

### Math.sin() / Math.cos() for smooth oscillation

Drives a continuous back-and-forth wave without keyframes or `wiggle`'s randomness — good for breathing/pulsing/bobbing loops where you want a perfectly smooth, repeatable cycle.

```js
amp = 20;   // pixels of swing
freq = 2;   // cycles per second
value + amp * Math.sin(time * freq * Math.PI * 2)
```

### Vector math: length(), normalize(), dot(), cross()

`length()` gives the distance between two points (or the magnitude of one vector) — useful for scale/opacity falloff based on proximity to a control layer. `normalize()`, `dot()`, and `cross()` are for direction math (e.g. angle between two layers, or building custom orientation logic).

```js
dist = length(thisComp.layer("Control").transform.position, transform.position);
falloff = linear(dist, 0, 300, 100, 0); // closer to Control = bigger
[falloff, falloff]
```

## Error handling

### Guard against a missing layer or effect

Wrapping a fragile reference in `try/catch` keeps the expression from turning red (and breaking the whole comp's render) when a referenced layer/effect is renamed, deleted, or just doesn't exist yet while you're building the rig.

```js
try {
  thisComp.layer("Control").transform.position
} catch (err) {
  value
}
```

## Physics

### Spring / overshoot on existing keyframes

The classic velocity-based spring: reads the velocity going into the nearest keyframe and lets it decay in a damped sine wave afterward, so a hand-keyed motion overshoots and settles instead of stopping dead. Apply to the same property that has the keyframes (Position, Scale, Rotation, etc.).

```js
freq = 3;    // oscillations per second
decay = 5;   // how fast it settles
n = 0;
if (numKeys > 0) n = nearestKey(time).index;
if (n > 0 && key(n).time > time) n--;
if (n == 0) {
  value;
} else {
  t = time - key(n).time;
  v = velocityAtTime(key(n).time - thisComp.frameDuration / 10);
  value + v * (freq == 0 ? 0 : 1/freq) * 0.15 * Math.sin(freq * t * 2 * Math.PI) / Math.exp(decay * t);
}
```
