# Text

## Animators

### Per-character selector info

Inside a text animator's Range Selector, adding an Expression Selector exposes `textIndex` (this character's 1-based position) and `textTotal` (character count) — use them to stagger or vary a value per character instead of applying it uniformly.

```js
// stagger each character's reveal, ~50ms apart
n = textIndex - 1;
delay = n * 0.05;
linear(time, delay, delay + 0.3, 0, 100)
```

### Typewriter reveal

Reveals a string one character at a time, driven by a slider or time — no preset or per-character keyframing needed. Apply to Source Text.

```js
full = "Hello World";
t = effect("Progress")("Slider"); // 0-100
n = Math.floor(full.length * t / 100);
full.substr(0, n)
```

### Typewriter with a blinking cursor

Extends the typewriter reveal with a trailing `|` that blinks. Two effect controls drive it: a **Slider** ("Text") giving the number of visible characters, and a **Checkbox** ("On/Off") to toggle the cursor. The cursor stays solid while characters are still being revealed (detected by comparing the slider's value one frame ahead against now), then blinks on a fixed frame interval once typing stops. Apply to Source Text.

```js
var sign = '|';
var blinkInterval = 25;                       // frames per on/off half-cycle
var i = effect('Text')('ADBE Slider Control-0001');
var on = effect('On/Off')('ADBE Checkbox Control-0001');

var check = timeToFrames(time) / blinkInterval;
var end;

if (on == 1) {
  if (i.valueAtTime(time + thisComp.frameDuration) > i) {
    end = sign;                               // still typing -> solid cursor
  } else {
    end = (Math.floor(check) % 2 === 0) ? sign : ' ';   // done -> blink
  }
} else {
  end = ' ';
}

text.sourceText.substr(0, parseInt(i)) + end;
```

To couple a text **animator's** Range Selector to the same character count (so effects like a per-character glow track the revealed text), drive its Start/End from the source text instead of a second slider:

```js
// Animator range Start
text.animator("ADBE Text Animator")("ADBE Text Selectors")("ADBE Text Selector")("ADBE Text Index End") - 1

// Animator range End
text("ADBE Text Document").length
```

### Counting number display

Formats a driving value as text with thousands separators — extends the plain `Math.round().toString()` pattern for score counters, stat reveals, or currency displays.

```js
n = Math.round(effect("Slider Control")("Slider"));
s = n.toString();
for (i = s.length - 3; i > 0; i -= 3) {
  s = s.substring(0, i) + "," + s.substring(i);
}
s
```

### Random character scramble-in

Cycles each character through random glyphs before locking in the real text, left to right — a common "hacker/decode" reveal. Apply to Source Text.

```js
chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
full = "REVEAL";
progress = effect("Progress")("Slider"); // 0-100
seedRandom(index, true);
out = "";
for (i = 0; i < full.length; i++) {
  charProgress = clamp(linear(progress, i / full.length * 100, (i + 1) / full.length * 100, 0, 1), 0, 1);
  out += charProgress >= 1 ? full.charAt(i) : chars.charAt(Math.floor(random(chars.length)));
}
out
```

## Source text

### Splitting / joining source text

Breaks a text layer's string into parts for per-word or per-line logic — e.g. capitalizing only the first word, or picking out a specific line.

```js
words = text.sourceText.split(" ");
words[0] = words[0].toUpperCase();
words.join(" ")
```

## Styling

Text style expressions (After Effects 2022 and later) let a Source Text expression change how the text looks, not just what it says. The chain starts from `text.sourceText.style` and every setter returns the style, so calls can be chained.

### Drive font size and color from controls

Puts type styling under the same slider/color controls as the rest of a rig, instead of buried in the Character panel — so a template's look can be changed without selecting the text.

```js
c = effect("Tint")("Color");
text.sourceText.style
  .setFontSize(effect("Size")("Slider"))
  .setFillColor([c[0], c[1], c[2]])
```

`setFillColor` takes a three-element array, so the alpha channel from a Color Control has to be dropped.

### Other style setters

```js
text.sourceText.style
  .setFont("Helvetica-Bold")
  .setTracking(80)
  .setLeading(72)
  .setApplyStroke(true)
  .setStrokeColor([1, 1, 1])
  .setStrokeWidth(2)
```

Read the current values back with the matching getters — `style.fontSize`, `style.font`, `style.tracking`, and so on — which is how you modify a style relative to whatever the Character panel is set to rather than hardcoding it.

```js
text.sourceText.style.setFontSize(style.fontSize * 1.5)
```

### Style a range of characters

Passing a start index and character count applies a setter to only part of the string — for highlighting a word without splitting it onto its own layer.

```js
s = "Total: 1,240";
text.sourceText.style.setText(s).setFillColor([1, 0.42, 0.21], 7, 5)
```
