# Composition

## Referencing compositions

### comp()

Reference another composition by name (e.g. one nested/precomposed elsewhere in the project), then chain into its layers and properties.

```js
comp("COMP_NAME").layer("LAYER NAME")
```

### thisComp

Reference the composition the current layer lives in — the most common way to reach other layers, since it doesn't hardcode a comp name.

```js
thisComp.layer("LAYER NAME")
```

## Referencing layers

### By name vs. by index

Layers can be referenced by their name (a string) or by their stacking index (a number, 1 = topmost). Index references are handy in expressions meant to be copied across many layers.

```js
thisComp.layer("Ball")
thisComp.layer(1)
```

### Relative to the current layer

`thisLayer` refers to the layer the expression is on. Combine with `index` to reach neighboring layers without hardcoding names — useful when duplicating a layer.

```js
thisLayer
thisComp.layer(index - 1) // the layer directly above this one in the stack
```

## Composition properties

Useful read-only properties available on any comp reference (`thisComp`, `comp("Name")`, etc.).

```js
thisComp.width
thisComp.height
thisComp.duration       // seconds
thisComp.frameDuration  // seconds per frame
thisComp.numLayers
```

## Markers

### Reading composition markers

Comp markers are handy for triggering behavior at specific points in time, e.g. cues synced to audio or edit points.

```js
// time (in seconds) of the first comp marker
thisComp.marker.key(1).time
```

### Layer markers

A layer's own markers travel with it when it's duplicated or moved in time, which makes them the better choice for a rig you intend to copy — comp markers stay put. `marker` on its own refers to this layer's markers.

```js
marker.numKeys
marker.key(1).time
marker.key(1).comment
```

### Referencing a marker by its comment

Naming a marker (double-click it and type a comment) means the expression keeps working when markers get added, removed, or reordered — unlike an index, which silently shifts.

```js
thisComp.marker.key("intro").time
```

### The marker at or before the playhead

`nearestKey()` returns whichever marker is closest in either direction, so it needs a step back to find the one already passed. This is the base for most marker-driven logic.

```js
n = 0;
if (marker.numKeys > 0) {
  n = marker.nearestKey(time).index;
  if (marker.key(n).time > time) n--;
}
n > 0 ? marker.key(n).comment : ""
```

### Retrigger an animation at every marker

Restarts a decaying pulse each time the playhead passes a marker, so one expression handles any number of hits without keyframes — drop markers on the beat and the layer bumps on each one.

```js
n = 0;
if (marker.numKeys > 0) {
  n = marker.nearestKey(time).index;
  if (marker.key(n).time > time) n--;
}
if (n == 0) {
  value
} else {
  t = time - marker.key(n).time;
  value + 40 * Math.sin(t * 12) / Math.exp(7 * t);
}
```

### Marker duration

Dragging a marker's edge gives it a duration, which turns it into a region rather than a point — handy for "hold this state while the marker is under the playhead."

```js
n = 0;
if (marker.numKeys > 0) {
  n = marker.nearestKey(time).index;
  if (marker.key(n).time > time) n--;
}
inRegion = n > 0 && time < marker.key(n).time + marker.key(n).duration;
inRegion ? 100 : 0
```

## Camera and lights

### Referencing the active camera

Returns whichever camera layer is currently active at the given time — useful for 3D setups where the camera being viewed through may change.

```js
thisComp.activeCamera
```

## Layer state

### Get a layer's actual bounding box

`sourceRectAtTime()` returns `{top, left, width, height}` for a layer in its own space, recalculated at the given time — unlike a static value, it updates when the source changes size (e.g. a text layer whose content grows). Use it to auto-size a background box behind text or fit a shape to a source layer.

```js
r = thisComp.layer("Title").sourceRectAtTime();
[r.width + 40, r.height + 40] // add padding; drive a rectangle's Size
```

### Skip logic on a hidden or disabled layer

`layer.active` is true only when the layer's video switch is on, it's not shy/muted-out, and the playhead is inside its work area — a broader check than just visibility.

```js
if (thisComp.layer("Flash").active) {
  value + 20
} else {
  value
}
```

### Reacting to a layer's own trim (in/out points)

`inPoint`/`outPoint` are the layer's trimmed start/end times in the comp, so a fade can be relative to where the clip actually starts and ends instead of the comp's timeline — handy when the same expression is copied onto layers with different trims.

```js
fadeIn = linear(time, inPoint, inPoint + 0.5, 0, 100);
fadeOut = linear(time, outPoint - 0.5, outPoint, 100, 0);
Math.min(fadeIn, fadeOut)
```
