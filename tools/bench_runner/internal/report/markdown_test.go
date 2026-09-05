package report

import (
	"strings"
	"testing"

	"github.com/MisterVVP/a2a-cpp/tools/bench_runner/internal/results"
	"github.com/MisterVVP/a2a-cpp/tools/bench_runner/internal/thresholds"
)

func TestEvaluateDetectsFailuresMissingAndUntracked(t *testing.T) {
	measurements := map[string]results.Measurement{
		"BM_Fast":      {Name: "BM_Fast", ActualNS: 50},
		"BM_Slow":      {Name: "BM_Slow", ActualNS: 150},
		"BM_Untracked": {Name: "BM_Untracked", ActualNS: 10},
	}
	thresholdSet := thresholds.Set{
		"BM_Fast":    {MaxTimeNS: 100},
		"BM_Slow":    {MaxTimeNS: 100},
		"BM_Missing": {MaxTimeNS: 100},
	}
	evaluation := Evaluate(measurements, thresholdSet, 1.0, false)
	if evaluation.Failures != 2 {
		t.Fatalf("Failures = %d, want 2", evaluation.Failures)
	}
	if len(evaluation.Missing) != 1 {
		t.Fatalf("Missing count = %d, want 1", len(evaluation.Missing))
	}
	if len(evaluation.Untracked) != 1 {
		t.Fatalf("Untracked count = %d, want 1", len(evaluation.Untracked))
	}
}

func TestEvaluateUsesMedianForThresholdWhenAvailable(t *testing.T) {
	medianNS := int64(90)
	measurements := map[string]results.Measurement{
		"BM_Noisy": {Name: "BM_Noisy", ActualNS: 150, MedianNS: &medianNS},
	}
	thresholdSet := thresholds.Set{"BM_Noisy": {MaxTimeNS: 100}}

	evaluation := Evaluate(measurements, thresholdSet, 1.0, false)

	if evaluation.Failures != 0 {
		t.Fatalf("Failures = %d, want 0 when median is below threshold", evaluation.Failures)
	}
	if evaluation.Rows[0].Status != "PASS" {
		t.Fatalf("Status = %q, want PASS", evaluation.Rows[0].Status)
	}
	if evaluation.Rows[0].Ratio != 0.9 {
		t.Fatalf("Ratio = %f, want 0.9", evaluation.Rows[0].Ratio)
	}
}

func TestMarkdownFormatsThresholdColumnAndThousandsSeparators(t *testing.T) {
	evaluation := Evaluation{Rows: []Row{{Benchmark: "BM_Test", ActualNS: 14200, ThresholdNS: 20000, Ratio: 0.71, Status: "PASS"}}}
	summary := Markdown(evaluation, results.RealTimeField)
	if !strings.Contains(summary, "| Benchmark | Actual real_time, ns | Median real_time, ns | Threshold, ns | Ratio | Status |") {
		t.Fatalf("summary does not contain explicit threshold header: %s", summary)
	}
	if !strings.Contains(summary, "Measured field: `real_time`.") {
		t.Fatalf("summary does not describe measured field: %s", summary)
	}
	if !strings.Contains(summary, "Threshold status and ratio use the median") {
		t.Fatalf("summary does not describe median threshold evaluation: %s", summary)
	}
	if !strings.Contains(summary, "14,200") || !strings.Contains(summary, "20,000") {
		t.Fatalf("summary does not contain formatted values: %s", summary)
	}
	if !strings.Contains(summary, "## Test\n") {
		t.Fatalf("summary does not group benchmarks by component: %s", summary)
	}
}

func TestMarkdownStartsATableForEachComponent(t *testing.T) {
	evaluation := Evaluation{Rows: []Row{
		{Benchmark: "BM_JsonRpcEnvelope_ParseOnly", ThresholdNS: 100, Status: "PASS"},
		{Benchmark: "BM_RestQueryParser_ParseOnly", ThresholdNS: 100, Status: "PASS"},
	}}
	summary := Markdown(evaluation, results.RealTimeField)
	if strings.Count(summary, "| Benchmark |") != 2 {
		t.Fatalf("summary does not contain one table per component: %s", summary)
	}
	if !strings.Contains(summary, "## JsonRpcEnvelope\n") || !strings.Contains(summary, "## RestQueryParser\n") {
		t.Fatalf("summary does not contain component headings: %s", summary)
	}
}

func TestEvaluateCanFailOnUntracked(t *testing.T) {
	measurements := map[string]results.Measurement{"BM_Untracked": {Name: "BM_Untracked", ActualNS: 10}}
	thresholdSet := thresholds.Set{"BM_Tracked": {MaxTimeNS: 100}}
	evaluation := Evaluate(measurements, thresholdSet, 1.0, true)
	if evaluation.Failures != 2 {
		t.Fatalf("Failures = %d, want missing plus untracked failures", evaluation.Failures)
	}
}
