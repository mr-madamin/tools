# Audio

## Amplitude

### Driving a property from audio levels

`Animation > Keyframe Assistant > Convert Audio to Keyframes` creates an "Audio Amplitude" layer with Left/Right/Both Channels sliders already keyframed from the track. Reference those sliders to make any property audio-reactive.

```js
amp = thisComp.layer("Audio Amplitude").effect("Both Channels")("Slider");
100 + amp * 0.5
```

### Smoothing jittery audio-driven motion

Raw amplitude keyframes are noisy frame to frame; averaging a few nearby samples smooths the reaction out without killing responsiveness.

```js
amp = thisComp.layer("Audio Amplitude").effect("Both Channels")("Slider");
n = 5; // samples to average
sum = 0;
for (i = 0; i < n; i++) {
  sum += amp.valueAtTime(time - i / frameRate);
}
avg = sum / n;
100 + avg * 0.5
```

### Threshold trigger from audio

Fires a behavior only once amplitude crosses a threshold — e.g. flashing a layer on a beat instead of continuously scaling with volume.

```js
amp = thisComp.layer("Audio Amplitude").effect("Both Channels")("Slider");
amp.value > 20 ? 100 : 0
```
