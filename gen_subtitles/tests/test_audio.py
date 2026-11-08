from gen_subtitles.audio import AudioTrack, _parse_streams


def test_parse_undefined_language_becomes_none():
    # The real water.mp4 shape: language is tagged "und" (undefined).
    data = {
        "streams": [
            {
                "index": 1,
                "codec_name": "aac",
                "channels": 2,
                "sample_rate": "44100",
                "tags": {"language": "und"},
            }
        ]
    }
    (track,) = _parse_streams(data)
    assert track == AudioTrack(
        index=1,
        codec_name="aac",
        channels=2,
        sample_rate=44100,
        language=None,
        title=None,
    )


def test_parse_keeps_real_language_and_title():
    data = {
        "streams": [
            {
                "index": 2,
                "codec_name": "ac3",
                "channels": 6,
                "sample_rate": "48000",
                "tags": {"language": "spa", "title": "Español 5.1"},
            }
        ]
    }
    (track,) = _parse_streams(data)
    assert track.language == "spa"
    assert track.title == "Español 5.1"


def test_parse_missing_tags_is_none_not_crash():
    # Many files carry no tags dict at all.
    data = {
        "streams": [
            {
                "index": 1,
                "codec_name": "aac",
                "channels": 2,
                "sample_rate": "44100",
            }
        ]
    }
    (track,) = _parse_streams(data)
    assert track.language is None
    assert track.title is None


def test_parse_sample_rate_is_cast_to_int():
    data = {
        "streams": [
            {
                "index": 1,
                "codec_name": "aac",
                "channels": 2,
                "sample_rate": "44100",
                "tags": {},
            }
        ]
    }
    (track,) = _parse_streams(data)
    assert track.sample_rate == 44100
    assert isinstance(track.sample_rate, int)
