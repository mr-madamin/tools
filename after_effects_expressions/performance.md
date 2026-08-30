# Engine, performance, and debugging

## Expression engine

After Effects ships two engines, chosen per project in `File > Project Settings > Expressions`: **JavaScript** (modern, and much faster) and **Legacy ExtendScript** (pre-CC 2019). Switching an existing project to the JavaScript engine can break expressions that worked before, usually for one of these reasons.

### Ask for `.value` explicitly

A property reference and that property's value are not the same object. Legacy ExtendScript was loose about converting between them; the JavaScript engine is stricter, and comparisons in particular can behave differently. Writing `.value` costs nothing and works in both engines.

```js
if (effect("Checkbox Control")("Checkbox").value == 1) {
  value * 2
} else {
  value
}
```

### Reserved words

The JavaScript engine reserves words that ExtendScript allowed as variable names — `class`, `const`, `let`, `export`, `import`, `static`, `enum`, `super`, among others. An expression that fails immediately after an engine switch is often a variable named one of these.

### Legacy-only syntax

`$.` (the ExtendScript global), `Number.prototype` patches, and other ExtendScript-specific constructs do not exist in the JavaScript engine. Rewrite them in plain JavaScript rather than switching the project back.

## Cost

Expressions are evaluated per property, per frame, on every layer. A rig that scrubs smoothly on one layer can stall a comp once duplicated forty times.

### Cache repeated references in a variable

Each reference to another layer's property is a fresh lookup. Reading once into a variable is both faster and easier to read, especially inside loops.

```js
// slow: three separate lookups
// thisComp.layer("Ctrl").effect("X")("Slider") ... twice more

ctrl = thisComp.layer("Ctrl").effect("X")("Slider");
[ctrl, ctrl * 2, ctrl * 3]
```

### Know which calls are expensive

`sourceRectAtTime()`, `sampleImage()`, and `valueAtTime()` on a property that itself carries an expression are the usual culprits — each forces AE to evaluate something extra. Call them once and reuse the result; never call them inside a loop that runs per character or per copy.

```js
r = thisComp.layer("Title").sourceRectAtTime(); // once
[r.width + 40, r.height + 40]
```

### Throttle evaluation with posterizeTime

Dropping an expensive expression to a lower evaluation rate often looks identical and costs a fraction as much — particularly for things that only need to be roughly right, like a background that reacts to a moving layer.

```js
posterizeTime(12);
thisComp.layer("Source").sampleImage(transform.position)
```

### Bake the rig once it's final

`Animation > Keyframe Assistant > Convert Expression to Keyframes` replaces an expression with baked per-frame keyframes. Useful for handing a comp to someone else, or for taking the evaluation cost out of a long render. Duplicate the layer first — the conversion is one-way.

### Turn expressions off while working

The `Enable Expressions` toggle at the bottom of the Timeline panel disables every expression in the comp at once, which is the quickest way to tell whether a slow preview is the expressions' fault.
