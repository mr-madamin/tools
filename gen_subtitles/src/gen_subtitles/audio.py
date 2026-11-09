import json
import subprocess
from dataclasses import dataclass
from pathlib import Path


class ProbeError(Exception):
    """ffprobe failed to read a file."""


class ExtractError(Exception):
    """ffmpeg failed to read a file"""


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


def extract_audio(path: str | Path, out_path: str | Path, track_index: int) -> Path:
    try:
        subprocess.run(
            [
                "ffmpeg",
                "-v",
                "error",
                "-y",
                "-i",
                str(path),
                "-map",
                f"0:{track_index}",
                "-ac",
                "1",
                "-ar",
                "16000",
                "-c:a",
                "pcm_s16le",
                str(out_path),
            ],
            capture_output=True,
            text=True,
            check=True,
        )
    except FileNotFoundError:
        raise ExtractError("ffmpeg not found - is ffmpeg installed and on PATH?")
    except subprocess.CalledProcessError as e:
        raise ExtractError(f"ffmpeg failed for {path}:\n{e.stderr.strip()}")

    return Path(out_path)
