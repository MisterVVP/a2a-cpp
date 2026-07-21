package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/MisterVVP/a2a-cpp/tools/bench_runner/internal/report"
	"github.com/MisterVVP/a2a-cpp/tools/bench_runner/internal/results"
	"github.com/MisterVVP/a2a-cpp/tools/bench_runner/internal/thresholds"
)

const (
	exitPass             = 0
	exitThresholdFailure = 1
	exitInvalidInput     = 2
	markdownFormat       = "markdown"
)

func main() {
	os.Exit(run())
}

func run() int {
	resultsPath := flag.String("results", "", "Path to Google Benchmark JSON results")
	thresholdsPath := flag.String("thresholds", "", "Path to benchmark thresholds JSON")
	summaryPath := flag.String("summary", "", "Path to write Markdown summary")
	tolerance := flag.Float64("tolerance", 1.0, "Multiplier applied to max_time_ns thresholds")
	timeField := flag.String("time-field", results.RealTimeField, "Google Benchmark time field: real_time or cpu_time")
	format := flag.String("format", markdownFormat, "Summary format")
	failOnUntracked := flag.Bool("fail-on-untracked", false, "Fail when results contain benchmarks without thresholds")
	flag.Parse()

	if *resultsPath == "" || *thresholdsPath == "" || *summaryPath == "" {
		fmt.Fprintln(os.Stderr, "--results, --thresholds, and --summary are required")
		return exitInvalidInput
	}
	if *tolerance <= 0 {
		fmt.Fprintln(os.Stderr, "--tolerance must be positive")
		return exitInvalidInput
	}
	if *format != markdownFormat {
		fmt.Fprintf(os.Stderr, "unsupported --format %q\n", *format)
		return exitInvalidInput
	}

	resultFile, err := os.Open(*resultsPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "open results: %v\n", err)
		return exitInvalidInput
	}
	defer resultFile.Close()

	measurements, err := results.Parse(resultFile, *timeField)
	if err != nil {
		fmt.Fprintf(os.Stderr, "invalid benchmark results: %v\n", err)
		return exitInvalidInput
	}

	thresholdFile, err := os.Open(*thresholdsPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "open thresholds: %v\n", err)
		return exitInvalidInput
	}
	defer thresholdFile.Close()

	thresholdSet, err := thresholds.Parse(thresholdFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "invalid thresholds: %v\n", err)
		return exitInvalidInput
	}

	evaluation := report.Evaluate(measurements, thresholdSet, *tolerance, *failOnUntracked)
	summary := report.Markdown(evaluation, *timeField)
	fmt.Print(summary)
	if err := os.WriteFile(*summaryPath, []byte(summary), 0o644); err != nil {
		fmt.Fprintf(os.Stderr, "write summary: %v\n", err)
		return exitInvalidInput
	}
	if evaluation.Failures > 0 {
		return exitThresholdFailure
	}
	return exitPass
}
