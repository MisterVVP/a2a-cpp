package thresholds

import (
	"strings"
	"testing"
)

func TestParseRejectsNonPositiveThreshold(t *testing.T) {
	if _, err := Parse(strings.NewReader(`{"BM_Test":{"max_time_ns":0}}`)); err == nil {
		t.Fatal("Parse returned nil error, want non-positive threshold error")
	}
}

func TestParseAcceptsThreshold(t *testing.T) {
	set, err := Parse(strings.NewReader(`{"BM_Test":{"max_time_ns":100}}`))
	if err != nil {
		t.Fatalf("Parse returned error: %v", err)
	}
	if set["BM_Test"].MaxTimeNS != 100 {
		t.Fatalf("MaxTimeNS = %d, want 100", set["BM_Test"].MaxTimeNS)
	}
}
