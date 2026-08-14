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
