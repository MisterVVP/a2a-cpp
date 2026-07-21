package report

import (
	"fmt"
	"sort"
	"strings"

	"github.com/MisterVVP/a2a-cpp/tools/bench_runner/internal/results"
	"github.com/MisterVVP/a2a-cpp/tools/bench_runner/internal/thresholds"
)

type Row struct {
	Benchmark   string
	ActualNS    int64
	MedianNS    *int64
	ThresholdNS int64
	Ratio       float64
	Status      string
}

type MissingRow struct {
	Benchmark   string
	ThresholdNS int64
}

type UntrackedRow struct {
	Benchmark string
	ActualNS  int64
}

type Evaluation struct {
	Rows      []Row
	Missing   []MissingRow
	Untracked []UntrackedRow
	Failures  int
}

func Evaluate(measurements map[string]results.Measurement, thresholdSet thresholds.Set, tolerance float64, failOnUntracked bool) Evaluation {
	evaluation := Evaluation{}
	for _, name := range thresholds.Names(thresholdSet) {
		threshold := thresholdSet[name]
		measurement, ok := measurements[name]
		if !ok {
			evaluation.Missing = append(evaluation.Missing, MissingRow{Benchmark: name, ThresholdNS: threshold.MaxTimeNS})
			evaluation.Failures++
			continue
		}
		allowed := float64(threshold.MaxTimeNS) * tolerance
		status := "PASS"
		if float64(measurement.ActualNS) > allowed {
			status = "FAIL"
			evaluation.Failures++
		}
		evaluation.Rows = append(evaluation.Rows, Row{Benchmark: name, ActualNS: measurement.ActualNS, MedianNS: measurement.MedianNS, ThresholdNS: threshold.MaxTimeNS, Ratio: float64(measurement.ActualNS) / float64(threshold.MaxTimeNS), Status: status})
	}
	for _, name := range results.Names(measurements) {
		if _, ok := thresholdSet[name]; ok {
			continue
		}
		evaluation.Untracked = append(evaluation.Untracked, UntrackedRow{Benchmark: name, ActualNS: measurements[name].ActualNS})
		if failOnUntracked {
			evaluation.Failures++
		}
	}
	return evaluation
}

func Markdown(evaluation Evaluation, timeField string) string {
	var builder strings.Builder
	builder.WriteString("# Benchmark threshold check\n\n")
	builder.WriteString(fmt.Sprintf("Measured field: `%s`. Thresholds are in nanoseconds.\n\n", timeField))
	builder.WriteString(fmt.Sprintf("| Benchmark | Actual %s, ns | Median %s, ns | Threshold, ns | Ratio | Status |\n", timeField, timeField))
	builder.WriteString("|---|---:|---:|---:|---:|---|\n")
	for _, row := range evaluation.Rows {
		median := ""
		if row.MedianNS != nil {
			median = FormatInt(*row.MedianNS)
		}
		builder.WriteString(fmt.Sprintf("| %s | %s | %s | %s | %.2f | %s |\n", row.Benchmark, FormatInt(row.ActualNS), median, FormatInt(row.ThresholdNS), row.Ratio, row.Status))
	}
	if len(evaluation.Missing) > 0 {
		builder.WriteString("\n## Missing benchmark results\n\n")
		builder.WriteString("| Benchmark | Threshold, ns |\n")
		builder.WriteString("|---|---:|\n")
		for _, row := range evaluation.Missing {
			builder.WriteString(fmt.Sprintf("| %s | %s |\n", row.Benchmark, FormatInt(row.ThresholdNS)))
		}
	}
	if len(evaluation.Untracked) > 0 {
		builder.WriteString("\n## Untracked benchmarks\n\n")
		builder.WriteString(fmt.Sprintf("| Benchmark | Actual %s, ns |\n", timeField))
		builder.WriteString("|---|---:|\n")
		sort.Slice(evaluation.Untracked, func(left int, right int) bool {
			return evaluation.Untracked[left].Benchmark < evaluation.Untracked[right].Benchmark
		})
		for _, row := range evaluation.Untracked {
			builder.WriteString(fmt.Sprintf("| %s | %s |\n", row.Benchmark, FormatInt(row.ActualNS)))
		}
	}
	return builder.String()
}

func FormatInt(value int64) string {
	negative := value < 0
	if negative {
		value = -value
	}
	digits := fmt.Sprintf("%d", value)
	parts := make([]string, 0, (len(digits)+2)/3)
	for len(digits) > 3 {
		parts = append(parts, digits[len(digits)-3:])
		digits = digits[:len(digits)-3]
	}
	parts = append(parts, digits)
	for left, right := 0, len(parts)-1; left < right; left, right = left+1, right-1 {
		parts[left], parts[right] = parts[right], parts[left]
	}
	formatted := strings.Join(parts, ",")
	if negative {
		return "-" + formatted
	}
	return formatted
}
