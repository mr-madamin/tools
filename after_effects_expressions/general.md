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
if (effect("Checkbox Control")("Checkbox") == 1) {
  value * 2
} else {
  value
}
```

## Text

### Drive text content from a Slider Control

Displays a rounded slider value as text — handy for animated counters.

```js
Math.round(effect("Slider Control")("Slider")).toString()
```
