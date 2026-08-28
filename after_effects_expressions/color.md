# Color

AE expressions represent color as an `[r, g, b, a]` array with each channel 0–1, not 0–255 — these convert to/from more familiar formats.

## Conversion

### Hex to RGB

Converts a hex string (e.g. from a brand style guide) into the array format a Color property expects.

```js
hexToRgb = function(hex) {
  hex = hex.replace("#", "");
  r = parseInt(hex.substring(0, 2), 16) / 255;
  g = parseInt(hex.substring(2, 4), 16) / 255;
  b = parseInt(hex.substring(4, 6), 16) / 255;
  return [r, g, b, 1];
};
hexToRgb("FF6B35")
```

### RGB to HSL and back

AE doesn't expose hue/saturation/lightness natively, but HSL math makes effects like hue-cycling or "same hue, different lightness" palettes much simpler than working in raw RGB.

```js
function rgbToHsl(rgb) {
  r = rgb[0]; g = rgb[1]; b = rgb[2];
  max = Math.max(r, g, b), min = Math.min(r, g, b);
  l = (max + min) / 2;
  if (max == min) { h = 0; s = 0; }
  else {
    d = max - min;
    s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
    if (max == r) h = (g - b) / d + (g < b ? 6 : 0);
    else if (max == g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h /= 6;
  }
  return [h, s, l];
}

function hslToRgb(h, s, l) {
  function hue2rgb(p, q, t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1/6) return p + (q - p) * 6 * t;
    if (t < 1/2) return q;
    if (t < 2/3) return p + (q - p) * (2/3 - t) * 6;
    return p;
  }
  if (s == 0) return [l, l, l, 1];
  q = l < 0.5 ? l * (1 + s) : l + s - l * s;
  p = 2 * l - q;
  return [hue2rgb(p, q, h + 1/3), hue2rgb(p, q, h), hue2rgb(p, q, h - 1/3), 1];
}
```

## Tinting

### Pull a fill color from a shared palette source

Rather than linking every shape's fill to the same Color Control one at a time, reference one "Palette" layer's effects so a single source of truth drives the whole comp's colors.

```js
thisComp.layer("Palette").effect("Primary")("Color")
```

### Cycling hue over time

Rotates a fill through the color wheel continuously — apply to a shape's Fill Color, using `hslToRgb` defined above (or inlined via a shared "Color Utils" expression referenced with `.expression` from another layer).

```js
hue = (time * 30 % 360) / 360; // degrees per second, normalized to 0-1
hslToRgb(hue, 0.8, 0.5)
```

## Sampling

### Pick a color out of another layer

`sampleImage(point, radius, postEffect, t)` reads the average color of a rectangular region of a layer and returns it as `[r, g, b, a]`. It samples in the sampled layer's own space, so a position from elsewhere in the comp has to be converted first. Common uses: a text layer that tints itself to whatever it's sitting on, or a UI element picking up the color of the footage behind it.

```js
src = thisComp.layer("Backdrop");
p = src.fromComp(toComp(anchorPoint)); // this layer's anchor, in Backdrop's space
src.sampleImage(p, [8, 8], true, time)
```

`postEffect = true` samples after the layer's effects and masks; `false` samples the raw source. The radius is a half-width/half-height in pixels, so `[8, 8]` averages a 16×16 patch — larger radii are steadier but slower.

`sampleImage` is one of the most expensive calls in the expression language. Call it once per expression, never inside a loop, and consider pairing it with `posterizeTime()` when the sampled area changes slowly.
