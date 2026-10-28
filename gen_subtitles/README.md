# gen_subtitles

Generate accurate `.srt` subtitles for any video, in **any language**, 100% locally.

Pipeline: **FFmpeg** (pick + extract the audio track) → **`faster-whisper`**
(transcribe) → **wav2vec2 forced alignment** (exact word timings) →
subtitle-friendly `.srt`.

A step-by-step learning build — see [PLAN.md](PLAN.md) for the milestones, the
AI/Python concepts behind each, and the working style.

> **Status:** 🚧 Early build. Milestone 1 (skeleton) first. The commands below are
> the planned interface and will light up as the milestones land.

## Setup (once)

Requires [`uv`](https://docs.astral.sh/uv/) and `ffmpeg` on your PATH.

```
uv sync
```

## Use

```
uv run subtitle movie.mkv                         # writes movie.<lang>.srt next to the video
uv run subtitle *.mkv *.avi                       # batch; models load once
uv run subtitle movie.mkv --lang es               # force the audio language (default: auto-detect)
uv run subtitle movie.mkv --track 2               # force audio track (0 = first audio track)
uv run subtitle movie.mkv --prompt "Almodóvar, Pepa, Lucía"   # name/slang hints
uv run subtitle movie.mkv --model large-v3        # bigger model, slower, more accurate
```

Inspecting a file's audio tracks:

```
uv run subtitle tracks movie.mkv                  # list audio tracks (index, language, title)
```

Translating finished subtitles (later milestone):

```
uv run subtitle translate movie.es.srt --to en    # LLM translation, cue-for-cue
```

The audio track is chosen by its language tag (or `--track`); the language is
auto-detected unless you pass `--lang`. Lines follow subtitle conventions:
≤42 chars/line, ≤2 lines, ≤6 s, split on pauses and sentence ends. Finish with a
manual pass in a subtitle editor (Subtitle Edit, Aegisub) to fix words/timings.

## Notes

- Everything in the core pipeline runs on-device — no API, no per-file cost.
  (The optional `translate` step is the one exception; it uses the Claude API.)
- Keep video clips and movies **out of git** — they're large and not code.
