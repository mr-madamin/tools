package jsonval_test

import (
	"math"
	"strings"
	"testing"

	"lanfoldersync/internal/jsonval"
)

func TestParseAcceptsWhatTheProtocolSends(t *testing.T) {
	documents := []string{
		`{"op": "HELLO", "token": "x"}`,
		`{"op": "PUT", "path": "a/b.txt", "size": 5, "mtime": 1600000000.123456}`,
		`{"op": "MANIFEST", "files": {}}`,
		`{"nested": {"deep": {"deeper": [1, 2, {"ok": true}]}}}`,
		`{"escapes": "quote\" backslash\\ newline\n unicodeé"}`,
		`{"negative": -1.5e-3}`,
	}
	for _, document := range documents {
		if _, err := jsonval.Parse([]byte(document)); err != nil {
			t.Errorf("Parse(%s) failed: %v", document, err)
		}
	}
}

func TestParseRefusesTrailingGarbage(t *testing.T) {
	// A frame is ONE value. Accepting "{} extra" as "{}" would let two peers
	// disagree about what they just exchanged.
	for _, document := range []string{`{} extra`, `{"op": "BYE"}{"op": "BYE"}`, `1 2`} {
		if _, err := jsonval.Parse([]byte(document)); err == nil {
			t.Errorf("Parse(%s) should have failed", document)
		}
	}
}

// encoding/json would replace invalid bytes inside a string with U+FFFD and
// carry on, where Python's .decode("utf-8") raises. Without this check a frame
// the other ports refuse would be accepted here with a mangled path.
func TestParseRefusesInvalidUTF8(t *testing.T) {
	cases := [][]byte{
		[]byte(`{"path": "a` + "\xff" + `b"}`),
		[]byte(`{"path": "` + "\xc3" + `"}`), // truncated two-byte sequence
		[]byte("\xff\xfe"),
	}
	for _, payload := range cases {
		if _, err := jsonval.Parse(payload); err == nil {
			t.Errorf("Parse(%q) should have failed on UTF-8", payload)
		}
	}
}

// 200 KB of '[' is a frame nobody has authenticated. The cap is checked on a
// byte scan, before the parser is handed anything.
func TestParseRefusesDeepNesting(t *testing.T) {
	deep := strings.Repeat("[", 200_000)
	if _, err := jsonval.Parse([]byte(deep)); err == nil {
		t.Fatal("200 KB of '[' was accepted")
	}

	// Just over the cap is refused...
	over := strings.Repeat("[", jsonval.MaxDepth+1) + strings.Repeat("]", jsonval.MaxDepth+1)
	if _, err := jsonval.Parse([]byte(over)); err == nil {
		t.Fatalf("nesting of %d was accepted", jsonval.MaxDepth+1)
	}
	// ...and a legal depth is not. Real payloads nest three deep.
	under := strings.Repeat("[", jsonval.MaxDepth) + strings.Repeat("]", jsonval.MaxDepth)
	if _, err := jsonval.Parse([]byte(under)); err != nil {
		t.Fatalf("nesting of %d should be allowed: %v", jsonval.MaxDepth, err)
	}

	// Brackets inside a string are not nesting.
	if _, err := jsonval.Parse([]byte(`{"path": "[[[[[[[[[["}`)); err != nil {
		t.Fatalf("brackets inside a string were counted as depth: %v", err)
	}
	if _, err := jsonval.Parse([]byte(`{"path": "\"[[[["}`)); err != nil {
		t.Fatalf("an escaped quote confused the depth scan: %v", err)
	}
}

// The number grammar is the scanner's job, and it gets it right -- which the C
// port did not, because it handed the token to strtod and accepted whatever it
// consumed.
func TestParseRefusesNonJSONNumbers(t *testing.T) {
	for _, document := range []string{
		`{"size": +1}`,
		`{"size": 01}`,
		`{"size": .5}`,
		`{"size": nan}`,
		`{"size": NaN}`,
		`{"size": Infinity}`,
		`{"size": 1.}`,
		`{"size": 0x10}`,
	} {
		if _, err := jsonval.Parse([]byte(document)); err == nil {
			t.Errorf("Parse(%s) should have failed", document)
		}
	}
}

// A bool is not a number. Python needed an explicit isinstance(bool) exclusion
// here, because its bool is a subclass of int and `true` compared equal to 1;
// C needed the type tag checked by hand at every call site.
func TestNumberAndStrDistinguishTypes(t *testing.T) {
	value, err := jsonval.Parse([]byte(
		`{"number": 5, "float": 1.5, "boolean": true, "string": "5", "null": null, "object": {}}`))
	if err != nil {
		t.Fatal(err)
	}
	object, ok := jsonval.AsObject(value)
	if !ok {
		t.Fatal("not an object")
	}

	if n, ok := object.Number("number"); !ok || n != 5 {
		t.Errorf("Number(number) = %v, %v", n, ok)
	}
	if n, ok := object.Number("float"); !ok || n != 1.5 {
		t.Errorf("Number(float) = %v, %v", n, ok)
	}
	for _, key := range []string{"boolean", "string", "null", "object", "absent"} {
		if _, ok := object.Number(key); ok {
			t.Errorf("Number(%s) should not be a number", key)
		}
	}

	if s, ok := object.Str("string"); !ok || s != "5" {
		t.Errorf("Str(string) = %v, %v", s, ok)
	}
	for _, key := range []string{"number", "boolean", "null", "object", "absent"} {
		if _, ok := object.Str(key); ok {
			t.Errorf("Str(%s) should not be a string", key)
		}
	}

	// Present-but-unusable and absent are different bugs, so Find reports
	// presence separately -- the PUT refusals depend on telling them apart.
	if _, present := object.Find("null"); !present {
		t.Error("an explicit null should count as present")
	}
	if _, present := object.Find("absent"); present {
		t.Error("a missing key should not count as present")
	}
}

// "1e999" parses -- to infinity, deliberately -- so the refusal comes from the
// caller's range check and names the field, instead of a parse error that
// names nothing.
func TestHugeExponentBecomesInfinityRatherThanAParseError(t *testing.T) {
	value, err := jsonval.Parse([]byte(`{"size": 1e999}`))
	if err != nil {
		t.Fatalf("1e999 should parse: %v", err)
	}
	object, _ := jsonval.AsObject(value)
	size, ok := object.Number("size")
	if !ok {
		t.Fatal("1e999 should read as a number")
	}
	if !math.IsInf(size, 1) {
		t.Fatalf("expected +Inf, got %v", size)
	}
	if jsonval.Finite(size) {
		t.Fatal("Finite() must reject it -- this is the check that stops it being used as a length")
	}
}

func TestEscapeMatchesTheOtherPorts(t *testing.T) {
	cases := []struct{ in, want string }{
		{`plain.txt`, `"plain.txt"`},
		{`with "quotes"`, `"with \"quotes\""`},
		{`back\slash`, `"back\\slash"`},
		{"tab\there", `"tab\there"`},
		{"line\nbreak", `"line\nbreak"`},
		{"\x00\x1f", `"\u0000\u001f"`},
		// Raw UTF-8 passes through: json.Marshal would escape < > & as
		// < and non-ASCII would become \uXXXX under Python's default
		// ensure_ascii, and then two ports' output would stop matching.
		{"café", `"café"`},
		{"a<b>&c", `"a<b>&c"`},
	}
	for _, c := range cases {
		if got := jsonval.Escape(c.in); got != c.want {
			t.Errorf("Escape(%q) = %s, want %s", c.in, got, c.want)
		}
	}

	// Whatever we escape must parse back to the same string.
	for _, c := range cases {
		value, err := jsonval.Parse([]byte(`{"k": ` + jsonval.Escape(c.in) + `}`))
		if err != nil {
			t.Fatalf("Escape(%q) produced unparseable JSON: %v", c.in, err)
		}
		object, _ := jsonval.AsObject(value)
		if got, _ := object.Str("k"); got != c.in {
			t.Errorf("round trip of %q gave %q", c.in, got)
		}
	}
}

// Every port's `unknown op:` line reads the same, because they all print
// Python's repr.
func TestPyRepr(t *testing.T) {
	parse := func(document string) (any, bool) {
		value, err := jsonval.Parse([]byte(document))
		if err != nil {
			t.Fatalf("Parse(%s): %v", document, err)
		}
		object, _ := jsonval.AsObject(value)
		return object.Find("op")
	}

	cases := []struct{ document, want string }{
		{`{"op": "MANFEST"}`, `'MANFEST'`},
		{`{"op": null}`, `None`},
		{`{}`, `None`},
		{`{"op": 5}`, `5`},
		{`{"op": 5.5}`, `5.5`},
		{`{"op": -2}`, `-2`},
		{`{"op": true}`, `True`},
		{`{"op": false}`, `False`},
		{`{"op": [1]}`, `[...]`},
		{`{"op": {"a": 1}}`, `{...}`},
	}
	for _, c := range cases {
		value, present := parse(c.document)
		if got := jsonval.PyRepr(value, present); got != c.want {
			t.Errorf("PyRepr of %s = %s, want %s", c.document, got, c.want)
		}
	}
}
