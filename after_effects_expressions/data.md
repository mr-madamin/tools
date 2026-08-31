# External data

After Effects can import `.json`, `.csv`, and `.tsv` files as footage items. Once imported, `footage("name")` reaches them from any expression, so a chart, lower third, or data-driven title updates when the file changes rather than when someone re-keys it.

## JSON, CSV, TSV

### Reading a JSON file

`sourceData` returns the parsed file — objects become objects, arrays become arrays. The file must be imported into the project panel; the string is its name there, not a path.

```js
data = footage("data.json").sourceData;
data.title
```

### Reading a row from a CSV or TSV

AE parses spreadsheet files into an array of rows, with the header line supplying each row's keys. Index by row number, then by column name.

```js
rows = footage("stats.csv").sourceData;
rows[0]["Revenue"]
```

### Driving a layer from its own index

Combined with `index`, one expression copied across duplicated layers pulls a different record onto each — the usual way to build a data-driven list or bar chart.

```js
rows = footage("stats.csv").sourceData;
row = rows[index - 1];
row["Label"] + ": " + row["Value"]
```

### Guarding against missing rows

`sourceData` returns whatever is in the file, so a layer that outnumbers the rows will throw and disable the expression. Check the length before indexing.

```js
rows = footage("stats.csv").sourceData;
i = index - 1;
i < rows.length ? rows[i]["Label"] : ""
```

### Bar height from a data value

Normalizing against the maximum in the set keeps bars in frame no matter what the numbers are, so the same rig works on new data without re-tuning.

```js
rows = footage("stats.csv").sourceData;
vals = [];
for (i = 0; i < rows.length; i++) vals.push(parseFloat(rows[i]["Value"]));
max = Math.max.apply(null, vals);
h = linear(vals[index - 1], 0, max, 0, 600);
[value[0], h] // drive a rectangle's Size
```

## MGJSON

Motion Graphics JSON (`.mgjson`) carries values that change over time, so unlike plain JSON it is sampled at the current frame rather than read once. `dataValue()` takes a path of indices into the file's data streams.

```js
footage("telemetry.mgjson").dataValue([0])
```

Companion methods for working with the stream's own keyframes:

```js
footage("telemetry.mgjson").dataKeyCount([0])
footage("telemetry.mgjson").dataKeyTimes([0], 0, 10)   // key times within a range
footage("telemetry.mgjson").dataKeyValues([0], 0, 10)  // values within a range
```

## Footage properties

Any footage item — not just data files — exposes its own dimensions and timing, useful for fitting a layer to a source whose size may change.

```js
footage("logo.png").width
footage("logo.png").height
footage("clip.mov").duration
footage("clip.mov").frameDuration
```
