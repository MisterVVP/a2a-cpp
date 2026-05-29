package results

import (
	"strings"
	"testing"
)

func TestParsePrefersMeanAggregate(t *testing.T) {
	json := `{"benchmarks":[{"name":"BM_Test","real_time":10,"cpu_time":11,"time_unit":"ns"},{"name":"BM_Test_mean","real_time":20,"cpu_time":21,"time_unit":"ns"},{"name":"BM_Test_median","real_time":18,"cpu_time":19,"time_unit":"ns"},{"name":"BM_Test_stddev","real_time":1,"cpu_time":1,"time_unit":"ns"}]}`
	measurements, err := Parse(strings.NewReader(json), RealTimeField)
	if err != nil {
		t.Fatalf("Parse returned error: %v", err)
	}
	measurement := measurements["BM_Test"]
	if measurement.ActualNS != 20 {
		t.Fatalf("ActualNS = %d, want 20", measurement.ActualNS)
	}
	if measurement.MedianNS == nil || *measurement.MedianNS != 18 {
		t.Fatalf("MedianNS = %v, want 18", measurement.MedianNS)
	}
}

func TestParseSupportsCPUTime(t *testing.T) {
	json := `{"benchmarks":[{"name":"BM_Test_mean","real_time":10,"cpu_time":25,"time_unit":"ns"}]}`
	measurements, err := Parse(strings.NewReader(json), CPUTimeField)
	if err != nil {
		t.Fatalf("Parse returned error: %v", err)
	}
	if measurements["BM_Test"].ActualNS != 25 {
		t.Fatalf("ActualNS = %d, want 25", measurements["BM_Test"].ActualNS)
	}
}

func TestParseRejectsUnsupportedTimeUnit(t *testing.T) {
	json := `{"benchmarks":[{"name":"BM_Test","real_time":10,"cpu_time":10,"time_unit":"ms"}]}`
	if _, err := Parse(strings.NewReader(json), RealTimeField); err == nil {
		t.Fatal("Parse returned nil error, want unsupported unit error")
	}
}
