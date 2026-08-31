# General

## Value

### Control keyframes when expression is set

You can still hand-animate keyframes even with an expression applied, by adding `+ value` — the expression's result is added on top of whatever the keyframes produce instead of replacing it.

```js
time + value
```

### Switch from positive to negative value in a quick way

Flips the sign of a value — e.g. to mirror a rotation or offset on alternating layers.

```js
value * -1
```

## Linking properties

### Link one property to another on the same layer

Pulls a value from a different property on the same layer, so they stay in sync automatically.

```js
transform.position
```

### Link to an effect control

References a value from an effect applied to the layer, letting one control (a Slider, Checkbox, Color Control, etc.) drive other properties.

```js
effect("Slider Control")("Slider")
```

### Link to another layer's property

References a property on a different layer in the same comp — the basis for parenting behavior via expressions instead of AE's Parent column.

```js
thisComp.layer("Control").transform.position
```

## Conditionals

### Toggle behavior with a Checkbox Control

Common pattern for turning a behavior on/off from an effect control instead of editing the expression.

```js
if (effect("Checkbox Control")("Checkbox").value == 1) {
  value * 2
} else {
  value
}
```

### Switch between more than two states with a dropdown

Cleaner than stacking multiple Checkbox Controls once there are 3+ options — add a Dropdown Menu Control, name its items, and switch on its numeric index.

```js
switch (effect("Direction")("Menu").value) {
  case 1: value + [100, 0]; break;
  case 2: value + [-100, 0]; break;
  case 3: value + [0, 100]; break;
  default: value;
}
```

### Trigger once when the playhead passes a marker

Finds the latest comp marker at or before the current time and flips a value once the playhead reaches it — useful for one-shot reveals synced to an edit point rather than a hardcoded time.

```js
m = thisComp.marker;
t = -1;
for (i = 1; i <= m.numKeys; i++) {
  if (m.key(i).time <= time) t = m.key(i).time;
}
t >= 0 ? 100 : 0
```

## Text

### Drive text content from a Slider Control

Displays a rounded slider value as text — handy for animated counters.

```js
Math.round(effect("Slider Control")("Slider")).toString()
```

## Index

### Layer-index-driven variation

`index` is the layer's position in the stack (1-based). Useful for giving duplicated layers slightly different behavior without editing each expression by hand.

```js
value + index * 10
```
