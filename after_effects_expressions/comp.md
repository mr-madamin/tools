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

## Camera and lights

### Referencing the active camera

Returns whichever camera layer is currently active at the given time — useful for 3D setups where the camera being viewed through may change.

```js
thisComp.activeCamera
```
