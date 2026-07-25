# Shapes

## Stroke

### Maintain stroke width when scaling

When scaling a shape, its stroke gets scaled as well, which can make strokes look too thick or thin. To keep the stroke a consistent width regardless of the shape's scale, add this expression to the stroke's Width property:

```js
value / length(toComp([0, 0]), toComp([0.7071, 0.7071])) || 0.001;
```

## Scale

### Maintain scale while parented

Parenting multiplies scale down the hierarchy, so a parented layer visually shrinks/grows with its parent. This counteracts the parent's scale so the layer keeps its own intended scale.

```js
s = [];
ps = parent.transform.scale.value;
for (i = 0; i < ps.length; i++) {
  s[i] = value[i] * 100 / ps[i];
}
s
```

## Trim Paths

### Animate a shape "drawing on"

Drives Trim Paths' End (or Start) property from time, growing the visible stroke from 0% to 100% — a common "line draw" reveal.

```js
linear(time, 0, 1, 0, 100) // End property, draws over 1 second
```

## Path

### Get a point along a path

Returns the [x, y] position at a given percentage (0–1) along a shape's path — useful for placing/animating other layers along a curve, or driving position with `random()` for scatter effects.

```js
// midpoint of the path, in the shape layer's own space
content("Shape 1").content("Path 1").path.pointOnPath(0.5)
```

`tangentOnPath(t)` and `normalOnPath(t)` return the direction/perpendicular vectors at that same point — handy for orienting a layer to follow the path.

## Repeater

### Offset color or scale across repeated copies

Inside a Repeater's Transform group, `index` refers to the copy number (0-based) and `content("Repeater 1").transform.copies` is the total count — combine them to vary a property per copy.

```js
// hue/scale shift across copies, e.g. on a repeater's Transform > Scale
mix = index / (content("Repeater 1").transform.copies - 1);
linear(mix, 0, 1, 100, 50) // scale shrinks from 100% to 50% across copies
```

## Fill

### Tie fill color to a Color Control effect

Lets one Color Control effect drive the fill of multiple shapes, so a single control updates them all.

```js
effect("Color Control")("Color")
```
