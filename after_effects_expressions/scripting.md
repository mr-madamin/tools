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
