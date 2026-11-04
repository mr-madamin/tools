from gen_subtitles.srt import Cue, format_timestamp, write_srt


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


def test_write_srt(tmp_path):
    path = tmp_path / "test.srt"
    cues = [
        Cue(0.0, 1.0, "Hello there!"),
        Cue(2.0, 3.5, "Hi! How are you?"),
        Cue(4.1, 5.2, "Fine"),
    ]
    expected = (
        "1\n00:00:00,000 --> 00:00:01,000\nHello there!\n\n"
        "2\n00:00:02,000 --> 00:00:03,500\nHi! How are you?\n\n"
        "3\n00:00:04,100 --> 00:00:05,200\nFine\n\n"
    )

    write_srt(cues, path)
    data = path.read_text(encoding="utf-8")
    assert data == expected


def test_write_srt_handles_non_ascii(tmp_path):
    path = tmp_path / "es.srt"
    write_srt([Cue(0.0, 1.0, "¿Dónde estás, señor?")], path)
    assert "¿Dónde estás, señor?" in path.read_text(encoding="utf-8")
