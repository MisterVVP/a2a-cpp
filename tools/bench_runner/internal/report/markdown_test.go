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

func TestMarkdownFormatsThresholdColumnAndThousandsSeparators(t *testing.T) {
	evaluation := Evaluation{Rows: []Row{{Benchmark: "BM_Test", ActualNS: 14200, ThresholdNS: 20000, Ratio: 0.71, Status: "PASS"}}}
	summary := Markdown(evaluation, results.RealTimeField)
	if !strings.Contains(summary, "| Benchmark | Actual real_time, ns | Median real_time, ns | Threshold, ns | Ratio | Status |") {
		t.Fatalf("summary does not contain explicit threshold header: %s", summary)
	}
	if !strings.Contains(summary, "Measured field: `real_time`.") {
		t.Fatalf("summary does not describe measured field: %s", summary)
	}
	if !strings.Contains(summary, "14,200") || !strings.Contains(summary, "20,000") {
		t.Fatalf("summary does not contain formatted values: %s", summary)
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
