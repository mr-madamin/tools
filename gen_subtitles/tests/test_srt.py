from gen_subtitles.srt import format_timestamp


def test_format_timestamp_zero():
    assert format_timestamp(0) == "00:00:00,000"


def test_format_timestamp_under_a_minute():
    assert format_timestamp(65.0) == "00:01:05,000"


def test_format_timestamp_rounding_rollover():
    assert format_timestamp(59.9995) == "00:01:00,000"
