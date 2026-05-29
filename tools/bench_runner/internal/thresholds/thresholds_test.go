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

func TestParseExpandsEnvironmentVariablesInBenchmarkNames(t *testing.T) {
	t.Setenv("A2A_BENCHMARK_THREADS", "8")
	set, err := Parse(strings.NewReader(`{"BM_Test/threads:${A2A_BENCHMARK_THREADS}":{"max_time_ns":100}}`))
	if err != nil {
		t.Fatalf("Parse returned error: %v", err)
	}
	if set["BM_Test/threads:8"].MaxTimeNS != 100 {
		t.Fatalf("expanded threshold missing or wrong: %#v", set)
	}
}

func TestParseRejectsUnsetEnvironmentVariablesInBenchmarkNames(t *testing.T) {
	if _, err := Parse(strings.NewReader(`{"BM_Test/threads:${A2A_BENCHMARK_THREADS}":{"max_time_ns":100}}`)); err == nil {
		t.Fatal("Parse returned nil error, want unset environment variable error")
	}
}
