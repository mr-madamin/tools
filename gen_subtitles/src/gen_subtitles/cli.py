import argparse


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="subtitle",
        description="Generate .srt subtitles from video files, in any language",
    )
    parser.add_argument("videos", nargs="*", help=".mkv / .avi / .mp4 files")
    parser.parse_args()
