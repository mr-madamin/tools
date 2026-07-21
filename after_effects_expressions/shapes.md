# Wiggle

## Stroke

When scaling shape, stroke gets scaled as well. To stop that add expression to shape's stroke width:

```js
value / length(toComp([0, 0]), toComp([0.7071, 0.7071])) || 0.001;
```
