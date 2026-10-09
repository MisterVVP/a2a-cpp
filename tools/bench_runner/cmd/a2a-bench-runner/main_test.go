package main

import (
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

const (
	smokeResultsPath   = "testdata/results.json"
	smokeThresholdPath = "testdata/thresholds.json"
	smokePassRow       = "| BM_Smoke | 120 | 100 | 110 | 0.91 | PASS |"
	smokeFailRow       = "| BM_Smoke | 120 | 100 | 110 | 0.91 | FAIL |"
	smokeCPUFailRow    = "| BM_Smoke | 220 | 200 | 110 | 1.82 | FAIL |"
	smokeReportHeading = "# Benchmark threshold check"
)

// Exercise the compiled command so flag parsing and os.Exit are tested together
// with the parser, threshold evaluator, and report writer.
func TestCLISmoke(t *testing.T) {
	binary := filepath.Join(t.TempDir(), "a2a-bench-runner")
	if output, err := exec.Command("go", "build", "-race", "-o", binary, ".").CombinedOutput(); err != nil {
		t.Fatalf("build CLI: %v\n%s", err, output)
	}

	cases := []struct {
		name       string
		args       []string
		wantExit   int
		wantReport string
		wantError  string
	}{
		{name: "median passes despite mean above threshold", wantExit: exitPass, wantReport: smokePassRow},
		{name: "tolerance fails", args: []string{"--tolerance", "0.5"}, wantExit: exitThresholdFailure, wantReport: smokeFailRow},
		{name: "CPU time fails", args: []string{"--time-field", "cpu_time"}, wantExit: exitThresholdFailure, wantReport: smokeCPUFailRow},
		{name: "untracked fails when requested", args: []string{"--fail-on-untracked"}, wantExit: exitThresholdFailure, wantReport: smokePassRow},
		{name: "missing required argument", args: []string{"--results", ""}, wantExit: exitInvalidInput, wantError: "--results, --thresholds, and --summary are required"},
		{name: "unsupported format", args: []string{"--format", "json"}, wantExit: exitInvalidInput, wantError: "unsupported --format"},
		{name: "invalid tolerance", args: []string{"--tolerance", "0"}, wantExit: exitInvalidInput, wantError: "--tolerance must be positive"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			summaryPath := filepath.Join(t.TempDir(), "summary.md")
			args := []string{"--results", smokeResultsPath, "--thresholds", smokeThresholdPath, "--summary", summaryPath}
			args = append(args, tc.args...)
			output, err := exec.Command(binary, args...).CombinedOutput()
			exitCode := exitPass
			if err != nil {
				var exitError *exec.ExitError
				if !errors.As(err, &exitError) {
					t.Fatalf("run CLI: %v", err)
				}
				exitCode = exitError.ExitCode()
			}
			if exitCode != tc.wantExit {
				t.Fatalf("exit = %d, want %d\n%s", exitCode, tc.wantExit, output)
			}
			if tc.wantError != "" {
				if !strings.Contains(string(output), tc.wantError) {
					t.Fatalf("output = %s, want error %q", output, tc.wantError)
				}
				if _, err := os.Stat(summaryPath); !errors.Is(err, os.ErrNotExist) {
					t.Fatalf("invalid input produced a summary: %v", err)
				}
				return
			}
			summary, err := os.ReadFile(summaryPath)
			if err != nil {
				t.Fatalf("read summary: %v", err)
			}
			if string(output) != string(summary) {
				t.Fatal("stdout and saved Markdown differ")
			}
			for _, fragment := range []string{smokeReportHeading, tc.wantReport, "## Untracked benchmarks", "| BM_Untracked |"} {
				if !strings.Contains(string(summary), fragment) {
					t.Errorf("summary missing %q:\n%s", fragment, summary)
				}
			}
		})
	}
}
