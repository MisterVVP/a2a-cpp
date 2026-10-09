package trend

import (
	"encoding/json"
	"math"
	"strings"
	"testing"

	"github.com/MisterVVP/a2a-cpp/tools/bench_runner/internal/results"
)

const benchmarkName = "BM_Test"

func number(value float64) *float64 { return &value }

func TestParseAggregatesAndRepetitions(t *testing.T) {
	cases := []struct {
		name, input, field string
		mean, median       float64
	}{
		{"aggregates win over raw samples", `{"benchmarks":[
			{"name":"BM_Test_mean","real_time":12.5,"time_unit":"ns"},
			{"name":"BM_Test","real_time":999,"time_unit":"ns"},
			{"name":"BM_Test_median","real_time":10.5,"time_unit":"ns"},
			{"name":"BM_Test_cv","real_time":0.5,"time_unit":"%"}]}`, results.RealTimeField, 12.5, 10.5},
		{"explicit aggregate metadata", `{"benchmarks":[
			{"name":"display-mean","run_name":"BM_Test","aggregate_name":"mean","cpu_time":12.5,"time_unit":"ns"},
			{"name":"display-median","run_name":"BM_Test","aggregate_name":"median","cpu_time":10.5,"time_unit":"ns"}]}`, results.CPUTimeField, 12.5, 10.5},
		{"odd repetitions", `{"benchmarks":[
			{"name":"BM_Test","real_time":3,"time_unit":"ns"},
			{"name":"BM_Test","real_time":1,"time_unit":"ns"},
			{"name":"BM_Test","real_time":8,"time_unit":"ns"}]}`, results.RealTimeField, 4, 3},
		{"even repetitions", `{"benchmarks":[
			{"name":"BM_Test","real_time":3,"time_unit":"ns"},
			{"name":"BM_Test","real_time":2,"time_unit":"ns"}]}`, results.RealTimeField, 2.5, 2.5},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			rows, err := Parse(strings.NewReader(tc.input), tc.field)
			if err != nil {
				t.Fatal(err)
			}
			if len(rows) != 1 || rows[0].Name != benchmarkName {
				t.Fatalf("rows = %+v", rows)
			}
			if rows[0].MeanNS == nil || *rows[0].MeanNS != tc.mean || rows[0].MedianNS == nil || *rows[0].MedianNS != tc.median {
				t.Fatalf("timings = %+v, want mean %v median %v", rows[0], tc.mean, tc.median)
			}
		})
	}
}

func TestParseRejectsInvalidInput(t *testing.T) {
	for _, input := range []string{
		`not json`, `{"benchmarks":[]}`, `{"benchmarks":[]} {}`,
		`{"benchmarks":[{"name":"BM_Test","real_time":-1,"time_unit":"ns"}]}`,
		`{"benchmarks":[{"name":"BM_Test","time_unit":"ns"}]}`,
		`{"benchmarks":[{"name":"BM_Test","real_time":1,"time_unit":"ms"}]}`,
		`{"benchmarks":[{"name":"","real_time":1,"time_unit":"ns"}]}`,
		`{"benchmarks":[{"name":"BM_Test_mean","real_time":1,"time_unit":"ns"},{"name":"BM_Test_mean","real_time":2,"time_unit":"ns"}]}`,
	} {
		if _, err := Parse(strings.NewReader(input), results.RealTimeField); err == nil {
			t.Errorf("accepted %s", input)
		}
	}
	if _, err := Parse(strings.NewReader(`{}`), "unknown"); err == nil {
		t.Fatal("accepted unsupported field")
	}
}

func TestParseSortsAndPreservesAbsentAggregates(t *testing.T) {
	input := `{"benchmarks":[{"name":"Z_mean","real_time":0,"time_unit":"ns"},{"name":"A_median","real_time":1.25,"time_unit":"ns"}]}`
	rows, err := Parse(strings.NewReader(input), results.RealTimeField)
	if err != nil {
		t.Fatal(err)
	}
	if rows[0].Name != "A" || rows[0].MeanNS != nil || rows[1].MedianNS != nil || *rows[1].MeanNS != 0 {
		t.Fatalf("rows = %+v", rows)
	}
	payload, err := json.Marshal(Snapshot{SchemaVersion: SchemaVersion, Benchmarks: rows})
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(payload), `"mean_ns":0`) || strings.Contains(string(payload), `"mean_ns":null`) {
		t.Fatalf("payload = %s", payload)
	}
}

func TestMarkdownDeltasAndNoise(t *testing.T) {
	const baselineURL = "https://github.com/example/repo/actions/runs/1"
	baseline := []Timing{{Name: benchmarkName, MeanNS: number(100), MedianNS: number(100)}}
	cases := []struct {
		name         string
		mean, median float64
		want         []string
	}{
		{"outlier mean stays advisory", 200, 104, []string{"+100.00%", "+4.00%", "within noise band"}},
		{"positive noise boundary", 200, 105, []string{"+100.00%", "+5.00%", "within noise band"}},
		{"negative noise boundary", 80, 95, []string{"-20.00%", "-5.00%", "within noise band"}},
		{"slower", 120, 110, []string{"+20.00%", "+10.00%", "slower (confirm over multiple runs)"}},
		{"faster", 80, 90, []string{"-20.00%", "-10.00%", "faster (confirm over multiple runs)"}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			snapshot := Snapshot{TimeField: results.RealTimeField, Benchmarks: []Timing{{Name: benchmarkName, MeanNS: number(tc.mean), MedianNS: number(tc.median)}}}
			markdown := Markdown(snapshot, baseline, baselineURL, DefaultNoisePercent)
			for _, want := range append(tc.want, baselineURL, "Advisory only") {
				if !strings.Contains(markdown, want) {
					t.Errorf("missing %q in %s", want, markdown)
				}
			}
		})
	}
}

func TestMarkdownMissingZeroAndFallback(t *testing.T) {
	snapshot := Snapshot{Benchmarks: []Timing{
		{Name: "new|row\n", MeanNS: number(100)},
		{Name: "zero", MeanNS: number(100), MedianNS: number(0)},
		{Name: "mean-only", MeanNS: number(120)},
	}}
	baseline := []Timing{
		{Name: "zero", MeanNS: number(0), MedianNS: number(0)},
		{Name: "mean-only", MeanNS: number(100), MedianNS: number(100)},
		{Name: "removed", MeanNS: number(100)},
	}
	markdown := Markdown(snapshot, baseline, "", DefaultNoisePercent)
	for _, fragment := range []string{
		"new\\|row  | n/a | n/a | new", "zero | n/a | n/a | not comparable",
		"mean-only | +20.00% | n/a | slower", "removed | n/a | n/a | missing from current run",
	} {
		if !strings.Contains(markdown, fragment) {
			t.Errorf("missing %q in %s", fragment, markdown)
		}
	}
	if noBaseline := Markdown(snapshot, nil, "", DefaultNoisePercent); !strings.Contains(noBaseline, "No previous successful main") {
		t.Fatal(noBaseline)
	}
}

func TestDeltaHandlesOverflow(t *testing.T) {
	if delta(number(math.MaxFloat64), number(math.SmallestNonzeroFloat64)) != nil {
		t.Fatal("overflow should be unavailable")
	}
}

func TestNoiseBoundaryWithFractionalTimings(t *testing.T) {
	baseline := []Timing{{Name: benchmarkName, MedianNS: number(0.3)}}
	snapshot := Snapshot{Benchmarks: []Timing{{Name: benchmarkName, MedianNS: number(0.315)}}}
	markdown := Markdown(snapshot, baseline, "", DefaultNoisePercent)
	if !strings.Contains(markdown, "+5.00% | within noise band") {
		t.Fatal(markdown)
	}
}
