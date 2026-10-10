package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"math"
	"os"
	"time"

	"github.com/MisterVVP/a2a-cpp/tools/bench_runner/internal/results"
	"github.com/MisterVVP/a2a-cpp/tools/bench_runner/internal/trend"
)

const exitInvalidInput = 2

func main() { os.Exit(run()) }

func readTimings(path, field string) ([]trend.Timing, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	return trend.Parse(f, field)
}

func run() int {
	resultsPath := flag.String("results", "", "Current Google Benchmark JSON")
	baselinePath := flag.String("baseline-results", "", "Previous successful main Google Benchmark JSON (optional)")
	outputPath := flag.String("output", "", "Compact trend JSON output")
	summaryPath := flag.String("summary", "", "Markdown comparison output")
	field := flag.String("time-field", results.RealTimeField, "real_time or cpu_time")
	commit := flag.String("commit", "", "Measured commit SHA")
	runURL := flag.String("run-url", "", "Current CI run URL")
	baselineURL := flag.String("baseline-run-url", "", "Previous successful main CI run URL")
	environment := flag.String("environment", "", "Runner and benchmark build configuration")
	noise := flag.Float64("noise-percent", trend.DefaultNoisePercent, "Advisory noise band in percent")
	flag.Parse()
	if *resultsPath == "" || *outputPath == "" || *summaryPath == "" {
		fmt.Fprintln(os.Stderr, "--results, --output, and --summary are required")
		return exitInvalidInput
	}
	if *noise < 0 || math.IsNaN(*noise) || math.IsInf(*noise, 0) {
		fmt.Fprintln(os.Stderr, "--noise-percent must be finite and nonnegative")
		return exitInvalidInput
	}
	current, err := readTimings(*resultsPath, *field)
	if err != nil {
		fmt.Fprintf(os.Stderr, "current results: %v\n", err)
		return exitInvalidInput
	}
	var baseline []trend.Timing
	if *baselinePath != "" {
		baseline, err = readTimings(*baselinePath, *field)
		if err != nil {
			fmt.Fprintf(os.Stderr, "baseline results: %v\n", err)
			return exitInvalidInput
		}
	}
	snapshot := trend.Snapshot{SchemaVersion: trend.SchemaVersion, Commit: *commit, RunURL: *runURL,
		RecordedAt: time.Now().UTC().Format(time.RFC3339), TimeField: *field, Environment: *environment, Benchmarks: current}
	payload, err := json.Marshal(snapshot)
	if err != nil {
		fmt.Fprintf(os.Stderr, "encode trend: %v\n", err)
		return exitInvalidInput
	}
	if err := os.WriteFile(*outputPath, append(payload, '\n'), 0o644); err != nil {
		fmt.Fprintf(os.Stderr, "write trend: %v\n", err)
		return exitInvalidInput
	}
	summary := trend.Markdown(snapshot, baseline, *baselineURL, *noise)
	if err := os.WriteFile(*summaryPath, []byte(summary), 0o644); err != nil {
		fmt.Fprintf(os.Stderr, "write summary: %v\n", err)
		return exitInvalidInput
	}
	fmt.Print(summary)
	return 0
}
