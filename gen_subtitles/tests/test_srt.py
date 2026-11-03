from gen_subtitles.srt import Cue, format_timestamp


def test_format_timestamp_zero():
    assert format_timestamp(0) == "00:00:00,000"


def test_format_timestamp_under_a_minute():
    assert format_timestamp(65.0) == "00:01:05,000"


def test_format_timestamp_rounding_rollover():
    assert format_timestamp(59.9995) == "00:01:00,000"


def test_cue_holds_its_fields():
    c = Cue(4.5, 7.25, "¿Dónde estás?")
    assert c.start == 4.5
    assert c.end == 7.25
    assert c.text == "¿Dónde estás?"


def test_cues_compare_by_value():
    assert Cue(0.0, 1.0, "hi") == Cue(0.0, 1.0, "hi")
