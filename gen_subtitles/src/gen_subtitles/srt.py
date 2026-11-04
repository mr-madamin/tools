from dataclasses import dataclass


@dataclass
class Cue:
    start: float  # seconds
    end: float  # seconds
    text: str


def format_timestamp(seconds: float) -> str:
    total_ms = round(seconds * 1000)
    s, ms = divmod(total_ms, 1000)
    m, s = divmod(s, 60)
    h, m = divmod(m, 60)
    return f"{h:02d}:{m:02d}:{s:02d},{ms:03d}"


def write_srt(cues, path):
    if path is None:
        raise ValueError(f"path wasn't provided: {path!r}")

    blocks = []
    for i, cue in enumerate(cues, start=1):
        blocks.append(
            f"{i}\n"
            f"{format_timestamp(cue.start)} --> {format_timestamp(cue.end)}\n"
            f"{cue.text}\n\n"
        )
    with open(path, "w", encoding="utf-8") as f:
        f.write("".join(blocks))
