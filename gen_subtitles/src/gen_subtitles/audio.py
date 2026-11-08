import json
import subprocess
from dataclasses import dataclass
from pathlib import Path


class ProbeError(Exception):
    """ffprobe failed to read a file."""


@dataclass
class AudioTrack:
    index: int
    codec_name: str
    channels: int
    sample_rate: int
    language: str | None  # "und"/missing -> None
    title: str | None = None


def _parse_streams(data: dict) -> list[AudioTrack]:
    streams = []
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


def probe_tracks(path: str | Path) -> list[AudioTrack]:
    try:
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
    except FileNotFoundError:
        raise ProbeError("ffprobe not found - is ffmpeg installed and on PATH?")
    except subprocess.CalledProcessError as e:
        raise ProbeError(f"ffprobe failed for {path}:\n{e.stderr.strip()}")

    data = json.loads(result.stdout)

    return _parse_streams(data)
