# Built-in functions

## Time

### To alter any properties over time add to this to any property

```js
time*50
```

### Posterizing time

Posterizing time gives animations or footage a choppy, stop-motion look by reducing the frame rate.

```js
posterizeTime(12)
```

## Repeat

### Repeating animations without keyframes

```js
loopOut(type = 'cycle', numKeyframes = 0)
```

## Wiggle

```js
wiggle(1, 50)
```

## Math

### Math.round()

```js
Math.round(effect("Slider Control")("Slider"))
```
