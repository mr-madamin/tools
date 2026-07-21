# Wiggle

## Stroke

### Maintain stroke width when scaling

When scaling shape, stroke gets scaled as well. To stop that add expression to shape's stroke width:

```js
value / length(toComp([0, 0]), toComp([0.7071, 0.7071])) || 0.001;
```

## Scale

### Maintain scale while parented

```js
s = [];
ps = parent.transform.scale.value;
for (i = 0; i < ps.length; i++) {
  s[i] = value[i] * 100 / ps[i];
}
s
```
