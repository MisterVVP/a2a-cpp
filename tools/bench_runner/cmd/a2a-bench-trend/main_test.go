package main

import (
	"encoding/json"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/MisterVVP/a2a-cpp/tools/bench_runner/internal/results"
	"github.com/MisterVVP/a2a-cpp/tools/bench_runner/internal/trend"
)

const (
	smokeResults     = "../a2a-bench-runner/testdata/results.json"
	smokeCommit      = "test-commit"
	smokeRunURL      = "https://github.com/example/repo/actions/runs/2"
	smokeEnvironment = "test-runner"
)

func buildCLI(t *testing.T) string {
	t.Helper()
	binary := filepath.Join(t.TempDir(), "a2a-bench-trend")
	if output, err := exec.Command("go", "build", "-race", "-o", binary, ".").CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, output)
	}
	return binary
}

func TestCLISmoke(t *testing.T) {
	binary := buildCLI(t)
	changedResults := filepath.Join(t.TempDir(), "changed-results.json")
	const slowerResults = `{"benchmarks":[
		{"name":"BM_Smoke_mean","real_time":240,"time_unit":"ns"},
		{"name":"BM_Smoke_median","real_time":200,"time_unit":"ns"},
		{"name":"BM_Untracked","real_time":50,"time_unit":"ns"}]}`
	if err := os.WriteFile(changedResults, []byte(slowerResults), 0o644); err != nil {
		t.Fatal(err)
	}
	cases := []struct {
		name string
		args []string
		exit int
		want string
	}{
		{"first run", nil, 0, "No previous successful main"},
		{"comparison", []string{"--baseline-results", smokeResults}, 0, "+0.00%"},
		{"large slowdown remains advisory", []string{"--results", changedResults, "--baseline-results", smokeResults}, 0, "+100.00% | +100.00% | slower"},
		{"CPU comparison", []string{"--baseline-results", smokeResults, "--time-field", results.CPUTimeField}, 0, "+0.00%"},
		{"missing baseline", []string{"--baseline-results", "nonexistent"}, exitInvalidInput, "baseline results:"},
		{"missing required flag", []string{"--output", ""}, exitInvalidInput, "are required"},
		{"invalid field", []string{"--time-field", "unknown"}, exitInvalidInput, "unsupported time field"},
		{"negative noise", []string{"--noise-percent", "-1"}, exitInvalidInput, "must be finite"},
		{"NaN noise", []string{"--noise-percent", "NaN"}, exitInvalidInput, "must be finite"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			outputPath, summaryPath := filepath.Join(dir, "trend.json"), filepath.Join(dir, "summary.md")
			args := []string{"--results", smokeResults, "--output", outputPath, "--summary", summaryPath,
				"--commit", smokeCommit, "--run-url", smokeRunURL, "--environment", smokeEnvironment}
			args = append(args, tc.args...)
			output, err := exec.Command(binary, args...).CombinedOutput()
			exit := 0
			if err != nil {
				var exitError *exec.ExitError
				if !errors.As(err, &exitError) {
					t.Fatal(err)
				}
				exit = exitError.ExitCode()
			}
			if exit != tc.exit || !strings.Contains(string(output), tc.want) {
				t.Fatalf("exit %d, output %s", exit, output)
			}
			if tc.exit != 0 {
				if _, err := os.Stat(outputPath); !errors.Is(err, os.ErrNotExist) {
					t.Fatalf("invalid input produced snapshot: %v", err)
				}
				return
			}
			payload, err := os.ReadFile(outputPath)
			if err != nil {
				t.Fatal(err)
			}
			var snapshot trend.Snapshot
			if err := json.Unmarshal(payload, &snapshot); err != nil {
				t.Fatal(err)
			}
			if snapshot.SchemaVersion != trend.SchemaVersion || snapshot.Commit != smokeCommit || snapshot.RunURL != smokeRunURL || snapshot.Environment != smokeEnvironment || snapshot.RecordedAt == "" || len(snapshot.Benchmarks) != 2 {
				t.Fatalf("snapshot = %+v", snapshot)
			}
			summary, err := os.ReadFile(summaryPath)
			if err != nil {
				t.Fatal(err)
			}
			if string(summary) != string(output) {
				t.Fatal("stdout differs from saved summary")
			}
		})
	}
}

// Exercise artifact selection without GitHub access. The fake CLI verifies that
// lookup is restricted to successful main push runs of the CI workflow.
func TestCompareMainScript(t *testing.T) {
	binary := buildCLI(t)
	root, err := filepath.Abs("../../../..")
	if err != nil {
		t.Fatal(err)
	}
	fixture, err := filepath.Abs(smokeResults)
	if err != nil {
		t.Fatal(err)
	}
	const fakeGH = `#!/usr/bin/env bash
set -euo pipefail
case "$1 $2" in
  'run view')
    [[ "$3" == 99 ]]
    [[ "$TEST_SCENARIO" != metadata-error ]] || exit 1
    [[ "$TEST_SCENARIO" != metadata-auth-error ]] || exit 4
    [[ "$TEST_SCENARIO" != empty-metadata ]] || exit 0
    printf '2026-01-03T00:00:00Z\n'
    ;;
  'run list')
    touch "$TEST_LOOKUP_MARKER"
    [[ " $* " == *' --workflow ci.yml '* ]]
    [[ " $* " == *' --branch main '* ]]
    [[ " $* " == *' --event push '* ]]
    [[ " $* " == *' --status success '* ]]
    [[ "$TEST_SCENARIO" != api-error ]] || exit 1
    [[ "$TEST_SCENARIO" != first-run ]] || exit 0
    if [[ "$TEST_SCENARIO" == older-rerun ]]; then
      # Model server-side filtering before the fetch limit: 101 newer runs
      # otherwise hide the preceding successful run, whose artifact exists.
      if [[ " $* " == *' --created <2026-01-03T00:00:00Z '* ]]; then
        printf '42\t2026-01-02T00:00:00Z\n'
      else
        for ((run = 200; run > 100; run--)); do
          printf '%s\t2026-01-04T00:00:00Z\n' "$run"
        done
      fi
      exit 0
    fi
    if [[ "$TEST_SCENARIO" == metadata-error || "$TEST_SCENARIO" == metadata-auth-error || "$TEST_SCENARIO" == empty-metadata ]]; then
      printf '42\t2026-01-04T00:00:00Z\n'
      exit 0
    fi
    printf '99\t2026-01-01T00:00:00Z\n100\t2026-01-04T00:00:00Z\n42\t2026-01-02T00:00:00Z\n41\t2026-01-01T00:00:00Z\n'
    ;;
  'run download')
    [[ "$3" == 42 ]]
    [[ " $* " == *' --name benchmark-results '* ]]
    [[ "$TEST_SCENARIO" != expired ]] || exit 1
    dest="${@: -1}"
    if [[ "$TEST_SCENARIO" == malformed ]]; then
      printf 'invalid JSON' > "$dest/benchmark-results.json"
    elif [[ "$TEST_SCENARIO" != missing-results ]]; then
      cp "$TEST_FIXTURE" "$dest/benchmark-results.json"
    fi
    ;;
  *) exit 1 ;;
esac
`
	const fakeGo = `#!/usr/bin/env bash
set -euo pipefail
[[ "$1" == run && "$2" == ./tools/bench_runner/cmd/a2a-bench-trend ]]
shift 2
exec "$TEST_BINARY" "$@"
`
	const fakeDate = `#!/usr/bin/env bash
printf '2026-01-05T00:00:00Z\n'
`
	cases := []struct {
		scenario, want string
		exit           int
	}{
		{"success", "actions/runs/42", 0},
		{"older-rerun", "actions/runs/42", 0},
		{"first-run", "No previous successful main", 0},
		{"api-error", "No previous successful main", 0},
		{"metadata-error", "No previous successful main", 0},
		{"metadata-auth-error", "No previous successful main", 0},
		{"empty-metadata", "No previous successful main", 0},
		{"expired", "No previous successful main", 0},
		{"missing-results", "No previous successful main", 0},
		{"malformed", "baseline results:", exitInvalidInput},
	}
	for _, tc := range cases {
		t.Run(tc.scenario, func(t *testing.T) {
			dir := t.TempDir()
			lookupMarker := filepath.Join(dir, "baseline-lookup")
			for name, content := range map[string]string{"gh": fakeGH, "go": fakeGo, "date": fakeDate} {
				if err := os.WriteFile(filepath.Join(dir, name), []byte(content), 0o755); err != nil {
					t.Fatal(err)
				}
			}
			args := []string{"tools/bench_runner/scripts/compare-main.sh", "--results", fixture,
				"--output", filepath.Join(dir, "trend.json"), "--summary", filepath.Join(dir, "summary.md")}
			cmd := exec.Command("bash", args...)
			cmd.Dir = root
			cmd.Env = append(os.Environ(), "PATH="+dir+":"+os.Getenv("PATH"), "RUNNER_TEMP="+dir,
				"GITHUB_REPOSITORY=example/repo", "GITHUB_RUN_ID=99", "GITHUB_SERVER_URL=https://github.com",
				"TEST_BINARY="+binary, "TEST_FIXTURE="+fixture, "TEST_SCENARIO="+tc.scenario,
				"TEST_LOOKUP_MARKER="+lookupMarker)
			output, err := cmd.CombinedOutput()
			exit := 0
			if err != nil {
				var exitError *exec.ExitError
				if !errors.As(err, &exitError) {
					t.Fatal(err)
				}
				exit = exitError.ExitCode()
			}
			if exit != tc.exit || !strings.Contains(string(output), tc.want) {
				t.Fatalf("exit %d, output %s", exit, output)
			}
			if tc.scenario == "metadata-error" || tc.scenario == "metadata-auth-error" || tc.scenario == "empty-metadata" {
				if _, err := os.Stat(lookupMarker); !errors.Is(err, os.ErrNotExist) {
					t.Fatalf("baseline lookup continued without current run metadata: %v", err)
				}
			}
			if paths, err := filepath.Glob(filepath.Join(dir, "benchmark-baseline.*")); err != nil || len(paths) != 0 {
				t.Fatalf("temporary baseline was not cleaned up: %v %v", paths, err)
			}
		})
	}
}
