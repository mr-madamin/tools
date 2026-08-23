# Scripting

ExtendScript (`.jsx`) automates the project itself — importing footage, adding layers, setting keyframes, building dockable panels — rather than driving a single property like an expression. Drop a script in AE's `ScriptUI Panels` folder and it shows up under `Window` as a dockable panel.

## Panel scaffolding

### Self-invoking panel that works docked or floating

Wrap everything in an IIFE that receives `this` as `thisObj`. When AE loads the file from the `ScriptUI Panels` folder, `thisObj` is a `Panel` (docked); when you run it from the Scripts menu, it isn't, so fall back to a floating `Window`. This one pattern makes the same file work both ways.

```js
(function myTool (thisObj) {
  function buildUi() {
    var panel = (thisObj instanceof Panel)
      ? thisObj
      : new Window("palette", "My Tool", undefined, { resizeable: true });
    // ...add controls...
    panel.layout.layout(true);
    return panel;
  }

  var uiPanel = buildUi(thisObj);
  if (uiPanel != null && uiPanel instanceof Window) {
    uiPanel.center();
    uiPanel.show();
  }
})(this);
```

### Lay out controls from a resource string

A single "resource" string describes the whole tree of groups and controls, which is far less verbose than adding each control by hand. Add it with `panel.add(res)`, then reach into the tree by the names you gave each node to set alignment, wire events, etc.

```js
res = "group{orientation:'column',\
  titleFading: StaticText{text:'Add fade animations'},\
  groupFading: Group{orientation:'row',\
    buttonFadeIn: Button{text:'Fade-in'},\
    buttonFadeOut: Button{text:'Fade-out'},\
  },\
}";

panel.grp = panel.add(res);
panel.grp.groupFading.alignment = 'left';
```

`orientation` is `'row'`, `'column'`, or `'stack'`; set `margins` / `alignment` per group to control spacing. Backslash-continue each line so the string stays readable.

### Add controls in code and handle clicks

Controls that need runtime data (a dropdown populated from an array) are easier to `add()` imperatively than to bake into the resource string. Assign `onClick` to wire a button to a function — pass an argument by using a closure instead of a bare function reference.

```js
var dropdown = group.add('dropdownlist', undefined, MY_OPTIONS);
dropdown.selection = 0;

var button = group.add('button', undefined, 'Add Intro');
button.onClick = function () {
  addIntro(dropdown.selection);   // closure captures the current selection
};

// no argument needed — reference the function directly
group.buttonFadeIn.onClick = addFadeIn;
```

### Show an image in the panel

Load a PNG into an `Image{}` control (or any control's `.image`) via a `File` — handy for a logo or icon header.

```js
group.logo.image = File('~/assets/logo.png');
```

## Guarding & selection

### Require a composition to be active

Most operations need a comp. `app.project.activeItem` is whatever's focused in the Project/Timeline; check it's actually a `CompItem` before touching layers, and bail with an `alert` otherwise.

```js
function isCompSelected() {
  return app.project.activeItem instanceof CompItem;
}

if (!isCompSelected()) {
  return alert('You have to select a composition.');
}
```

### Get the time range of the selected layers

Walk `comp.selectedLayers` and collapse their in/out points into a single start/end span — useful for sizing an adjustment or effect layer to cover exactly what the user selected.

```js
function getSelectedLayersDuration() {
  var comp = app.project.activeItem;
  var startTime = null, endTime = null;

  if (comp.selectedLayers.length === 0) {
    return alert('You have to select at least one layer');
  }
  for (var i = 0; i < comp.selectedLayers.length; i++) {
    var layer = comp.selectedLayers[i];
    if (startTime === null) {
      startTime = layer.inPoint;
      endTime = layer.outPoint;
    } else {
      if (layer.inPoint < startTime) startTime = layer.inPoint;
      if (layer.outPoint > endTime) endTime = layer.outPoint;
    }
  }
  return { start: startTime, end: endTime };
}
```

## Undo

### Group everything into one undo step

Wrap each action between `beginUndoGroup` / `endUndoGroup` so the whole operation collapses to a single Cmd/Ctrl+Z, instead of the user having to undo each layer, keyframe, and effect separately.

```js
app.beginUndoGroup('Add Watermark');
// ...import, add layers, set keyframes...
app.endUndoGroup();
```

## Importing & adding layers

### Import a file and add it to the comp

`importFile` brings a footage item into the Project; `comp.layers.add()` places it on the active comp. Check `File.exists` first so a missing asset fails loudly instead of throwing.

```js
var video = new File('~/assets/intro.mov');
if (!video.exists) return alert('Failed to find video file');

var opts = new ImportOptions();
opts.file = video;
opts.video = true;                       // or set .sequence / .forceAlphabetical
var item = app.project.importFile(opts);

var layer = comp.layers.add(item);
layer.name = 'intro_video';
layer.startTime = 1;                     // shift when the footage begins
```

`new ImportOptions(file)` is a shorthand when you don't need to set flags.

### Stack layers in a specific order

New layers land at the top. `moveAfter` / `moveBefore` reorder relative to another layer — e.g. put a vignette solid just under the first selected layer.

```js
logoLayer.moveAfter(videoLayer);
solid.moveBefore(comp.selectedLayers[0]);
```

### Add a solid, adjustment layer, or vignette

`layers.addSolid([r,g,b], name, w, h, pixelAspect, duration)` creates a color solid. Set `adjustmentLayer = true` to turn it into an adjustment layer so its effects apply to everything beneath it.

```js
var solid = comp.layers.addSolid([0, 0, 0], 'color_correction', 3840, 2160, 1, 10);
solid.adjustmentLayer = true;
solid.inPoint = duration.start;
solid.outPoint = duration.end;
```

## Keyframes

### Set keyframed values on a property

Address a property by its match name (e.g. `ADBE Transform Group` → `ADBE Opacity`) and call `setValueAtTime(time, value)` to create a keyframe. Two keyframes at different times make an animation — here a 3-second fade-in on opacity and audio together.

```js
var start = layer.inPoint;
var end = layer.inPoint + 3;

var opacity = layer.property('ADBE Transform Group').property('ADBE Opacity');
opacity.setValueAtTime(start, 0);
opacity.setValueAtTime(end, 100);

var audio = layer.property('ADBE Audio Group').property('ADBE Audio Levels');
audio.setValueAtTime(start, [-48, -48]);   // dB, stereo
audio.setValueAtTime(end, [0, 0]);
```

`addKey(time)` sets a keyframe at the property's current value without changing it — useful as the "hold" anchor before an animated change.

### Scale and position are 2-value arrays

Transform properties like Scale and Position take `[x, y]` (or `[x, y, z]` in 3D). Scale is a percentage.

```js
var t = layer.property('ADBE Transform Group');
t.property('ADBE Scale').setValue([80, 80]);
t.property('ADBE Position').setValue([3500, 210]);
```

## Markers

### Add a comp marker

`MarkerValue` plus `comp.markerProperty.setValueAtTime` drops a named marker on the comp's timeline — handy as a "Start" cue that expressions or editors can sync to.

```js
var marker = new MarkerValue('Start');
comp.markerProperty.setValueAtTime(11, marker);

comp.duration = comp.duration + 11;   // extend the comp to make room
```

## Effects

### Add and configure an effect

`layer.Effects.addProperty(matchName)` applies an effect; the returned object lets you set its parameters by name. Match names come from AE's effect list (e.g. `ADBE CurvesCustom`, `ADBE HUE SATURATION`).

```js
solid.Effects.addProperty('ADBE CurvesCustom');
var hue = solid.Effects.addProperty('ADBE HUE SATURATION');
hue.property('Master Saturation').setValue(30);
```

## Masks

### Build a mask shape from scratch

Add a mask with `layer.Masks.addProperty("ADBE Mask Atom")`, then read the current `Shape` value, mutate its `vertices` / tangent arrays, and write it back. The `0.5523` "magic number" (kappa) approximates a circle with Bézier tangents — used here for a soft elliptical vignette in SUBTRACT mode.

```js
var mask = solid.Masks.addProperty("ADBE Mask Atom");
mask.maskMode = MaskMode.SUBTRACT;
mask.name = 'Vignette';
mask.feather = [500, 500];

var maskProperty = mask.property("ADBE Mask Shape");
var shape = maskProperty.value;

var ratio = 0.5523;
var h = solid.width / 2, v = solid.height / 2;
var th = h * ratio, tv = v * ratio;

shape.vertices    = [[h, 0], [0, v], [h, 2 * v], [2 * h, v]];
shape.inTangents  = [[th, 0], [0, -tv], [-th, 0], [0, tv]];
shape.outTangents = [[-th, 0], [0, tv], [th, 0], [0, -tv]];
shape.closed = true;

maskProperty.setValue(shape);
mask.property("Mask Feather").setValue([600, 600]);
```
