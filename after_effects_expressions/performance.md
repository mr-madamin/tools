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
