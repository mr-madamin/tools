import json
import subprocess
from dataclasses import dataclass
from pathlib import Path


@dataclass
class AudioTrack:
    index: int
    codec_name: str
    channels: int
    sample_rate: int
    language: str | None  # "und"/missing -> None
    title: str | None = None


def probe_tracks(path: str | Path) -> list[AudioTrack]:
    streams = []

    result = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-print_format",
            "json",
            "-show_streams",
            "-select_streams",
            "a",
            str(path),
        ],
        capture_output=True,
        text=True,
        check=True,
    )

    data = json.loads(result.stdout)

    for stream in data["streams"]:
        tags = stream.get("tags", {})
        language = tags.get("language")
        title = tags.get("title")
        parsed = {
            "index": stream["index"],
            "codec_name": stream["codec_name"],
            "channels": stream["channels"],
            "sample_rate": int(stream["sample_rate"]),
            "language": language if language != "und" else None,
            "title": title,
        }
        streams.append(AudioTrack(**parsed))

    return streams
