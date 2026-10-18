package config_test

import (
	"strings"
	"testing"

	"lanfoldersync/internal/config"
)

func TestParseAcceptsAValidConfig(t *testing.T) {
	text := `{
	  "shared_dir": "/Users/you/SyncMe",
	  "peer": { "host": "192.168.1.42", "port": 8765 },
	  "token": "a-long-random-string"
	}`

	cfg, err := config.Parse([]byte(text), "config.json")
	if err != nil {
		t.Fatalf("a valid config was rejected: %v", err)
	}
	if cfg.SharedDir != "/Users/you/SyncMe" {
		t.Errorf("shared_dir = %q", cfg.SharedDir)
	}
	if cfg.PeerHost != "192.168.1.42" {
		t.Errorf("peer.host = %q", cfg.PeerHost)
	}
	if cfg.PeerPort != 8765 {
		t.Errorf("peer.port = %d", cfg.PeerPort)
	}
	if cfg.Token != "a-long-random-string" {
		t.Errorf("token = %q", cfg.Token)
	}
}

// Absent keys are fine: the defaults are the point. Only the token is required.
func TestParseAllowsAbsentOptionalKeys(t *testing.T) {
	cfg, err := config.Parse([]byte(`{"token": "t"}`), "config.json")
	if err != nil {
		t.Fatalf("a minimal config was rejected: %v", err)
	}
	if cfg.SharedDir != "" || cfg.PeerHost != "" || cfg.PeerPort != 0 {
		t.Fatalf("absent keys should stay empty, got %+v", cfg)
	}
}

// The whole reason this package doesn't just unmarshal into a struct: a value
// of the wrong type is a typo the user made in their editor, and the message
// has to name THEIR key. encoding/json would report
// "cannot unmarshal string into Go struct field ... of type int".
func TestParseNamesEveryMistypedValue(t *testing.T) {
	cases := []struct {
		label string
		text  string
		want  string
	}{
		{
			"quoted port",
			`{"token": "t", "peer": {"port": "9999"}}`,
			"peer.port must be a number, not in quotes",
		},
		{
			"port out of range",
			`{"token": "t", "peer": {"port": 70000}}`,
			"peer.port must be 1-65535",
		},
		{
			"port zero",
			`{"token": "t", "peer": {"port": 0}}`,
			"peer.port must be 1-65535",
		},
		{
			"port is infinite",
			`{"token": "t", "peer": {"port": 1e999}}`,
			"peer.port must be 1-65535",
		},
		{
			"shared_dir not a string",
			`{"token": "t", "shared_dir": 5}`,
			`"shared_dir" must be a string, in double quotes`,
		},
		{
			"token not a string",
			`{"token": 12345}`,
			`"token" must be a string, in double quotes`,
		},
		{
			"peer host not a string",
			`{"token": "t", "peer": {"host": ["192.168.1.42"]}}`,
			`"peer.host" must be a string, in double quotes`,
		},
		{
			"peer not an object",
			`{"token": "t", "peer": "192.168.1.42:8765"}`,
			`"peer" must be an object`,
		},
		{
			"missing token",
			`{"shared_dir": "/tmp"}`,
			`missing "token"`,
		},
		{
			"not JSON",
			`{this is not json}`,
			"is not valid JSON",
		},
		{
			"not an object",
			`[1, 2, 3]`,
			"is not valid JSON",
		},
	}

	for _, c := range cases {
		t.Run(c.label, func(t *testing.T) {
			_, err := config.Parse([]byte(c.text), "myconfig.json")
			if err == nil {
				t.Fatalf("%s was accepted", c.label)
			}
			if !strings.Contains(err.Error(), c.want) {
				t.Fatalf("message was %q, expected it to contain %q", err, c.want)
			}
			// Every message names the file, so the user knows which one to fix.
			if !strings.Contains(err.Error(), "myconfig.json") {
				t.Fatalf("message does not name the file: %q", err)
			}
		})
	}
}

// A null is the same as absent, not the same as a wrong type: a user who
// commented a key out by nulling it gets the default, not an error.
func TestParseTreatsNullAsAbsent(t *testing.T) {
	cfg, err := config.Parse([]byte(`{"token": "t", "shared_dir": null, "peer": null}`),
		"config.json")
	if err != nil {
		t.Fatalf("nulls were rejected: %v", err)
	}
	if cfg.SharedDir != "" || cfg.PeerPort != 0 {
		t.Fatalf("expected defaults, got %+v", cfg)
	}
}
