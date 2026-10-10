// Package trend records benchmark timings and reports advisory changes.
package trend

import (
	"encoding/json"
	"fmt"
	"io"
	"math"
	"sort"
	"strings"

	"github.com/MisterVVP/a2a-cpp/tools/bench_runner/internal/results"
)

const (
	SchemaVersion       = 1
	DefaultNoisePercent = 5.0
	meanAggregate       = "mean"
	medianAggregate     = "median"
	comparisonEpsilon   = 1e-9
)

type Timing struct {
	Name     string   `json:"name"`
	MeanNS   *float64 `json:"mean_ns,omitempty"`
	MedianNS *float64 `json:"median_ns,omitempty"`
}

type Snapshot struct {
	SchemaVersion int      `json:"schema_version"`
	Commit        string   `json:"commit"`
	RunURL        string   `json:"run_url"`
	RecordedAt    string   `json:"recorded_at"`
	TimeField     string   `json:"time_field"`
	Environment   string   `json:"environment"`
	Benchmarks    []Timing `json:"benchmarks"`
}

type samples struct {
	timing Timing
	values []float64
}

// Parse retains aggregate precision. Raw repetitions are used only when an
// explicit aggregate is absent, regardless of the order of benchmark entries.
func Parse(reader io.Reader, timeField string) ([]Timing, error) {
	if timeField != results.RealTimeField && timeField != results.CPUTimeField {
		return nil, fmt.Errorf("unsupported time field %q", timeField)
	}
	var file struct {
		Benchmarks []struct {
			Name      string   `json:"name"`
			RunName   string   `json:"run_name"`
			Aggregate string   `json:"aggregate_name"`
			RealTime  *float64 `json:"real_time"`
			CPUTime   *float64 `json:"cpu_time"`
			TimeUnit  string   `json:"time_unit"`
		} `json:"benchmarks"`
	}
	decoder := json.NewDecoder(reader)
	if err := decoder.Decode(&file); err != nil {
		return nil, fmt.Errorf("parse benchmark results: %w", err)
	}
	if err := decoder.Decode(new(any)); err != io.EOF {
		return nil, fmt.Errorf("expected a single benchmark JSON document")
	}
	byName := make(map[string]*samples)
	for _, entry := range file.Benchmarks {
		name, aggregate := results.NormalizeName(entry.Name)
		if entry.Aggregate != "" {
			aggregate = entry.Aggregate
			if entry.RunName != "" {
				name = entry.RunName
			}
		}
		if aggregate != "" && aggregate != meanAggregate && aggregate != medianAggregate {
			continue
		}
		value := entry.RealTime
		if timeField == results.CPUTimeField {
			value = entry.CPUTime
		}
		if name == "" || value == nil || *value < 0 || math.IsNaN(*value) || math.IsInf(*value, 0) {
			return nil, fmt.Errorf("benchmark %q has missing or invalid timing", name)
		}
		if entry.TimeUnit != results.Nanoseconds {
			return nil, fmt.Errorf("benchmark %q must use ns", name)
		}
		group := byName[name]
		if group == nil {
			group = &samples{timing: Timing{Name: name}}
			byName[name] = group
		}
		switch aggregate {
		case meanAggregate:
			if group.timing.MeanNS != nil {
				return nil, fmt.Errorf("duplicate mean for %q", name)
			}
			group.timing.MeanNS = value
		case medianAggregate:
			if group.timing.MedianNS != nil {
				return nil, fmt.Errorf("duplicate median for %q", name)
			}
			group.timing.MedianNS = value
		default:
			group.values = append(group.values, *value)
		}
	}
	if len(byName) == 0 {
		return nil, fmt.Errorf("no usable benchmark timings")
	}
	timings := make([]Timing, 0, len(byName))
	for _, group := range byName {
		if len(group.values) > 0 {
			fillAggregates(group)
		}
		timings = append(timings, group.timing)
	}
	sort.Slice(timings, func(i, j int) bool { return timings[i].Name < timings[j].Name })
	return timings, nil
}

func fillAggregates(group *samples) {
	if group.timing.MeanNS == nil {
		// Divide before adding to avoid overflow for large finite inputs.
		mean := 0.0
		for _, value := range group.values {
			mean += value / float64(len(group.values))
		}
		group.timing.MeanNS = &mean
	}
	if group.timing.MedianNS == nil {
		sort.Float64s(group.values)
		middle := len(group.values) / 2
		median := group.values[middle]
		if len(group.values)%2 == 0 {
			median = group.values[middle-1]/2 + median/2
		}
		group.timing.MedianNS = &median
	}
}

func delta(current, baseline *float64) *float64 {
	if current == nil || baseline == nil || *baseline == 0 {
		return nil
	}
	change := (*current - *baseline) / *baseline * 100
	if math.IsInf(change, 0) || math.IsNaN(change) {
		return nil
	}
	return &change
}

func percentage(value *float64) string {
	if value == nil {
		return "n/a"
	}
	return fmt.Sprintf("%+.2f%%", *value)
}

func cell(value string) string {
	return strings.NewReplacer("|", "\\|", "\n", " ", "\r", " ", "`", "'").Replace(value)
}

// Markdown uses medians for the signal when both runs have them. Changes never
// fail a run: investigate repeated changes before making roadmap decisions.
func Markdown(current Snapshot, baseline []Timing, baselineURL string, noisePercent float64) string {
	var out strings.Builder
	out.WriteString("# Benchmark trends\n\n")
	fmt.Fprintf(&out, "Measured field: `%s`; timings in ns. Positive changes are slower.\n\n", cell(current.TimeField))
	fmt.Fprintf(&out, "Advisory only: changes within ±%.2f%% are marked as noise. Prefer medians across repetitions; confirm changes over multiple CI runs before acting. Hard thresholds remain independent.\n\n", noisePercent)
	if baseline == nil {
		out.WriteString("No previous successful main benchmark artifact is available. This run starts the trend history.\n")
		return out.String()
	}
	fmt.Fprintf(&out, "Baseline: previous successful main run %s\n\n", cell(baselineURL))
	out.WriteString("| Benchmark | Mean change | Median change | Signal |\n|---|---:|---:|---|\n")
	previous := make(map[string]Timing, len(baseline))
	for _, row := range baseline {
		previous[row.Name] = row
	}
	for _, row := range current.Benchmarks {
		old, ok := previous[row.Name]
		if !ok {
			fmt.Fprintf(&out, "| %s | n/a | n/a | new |\n", cell(row.Name))
			continue
		}
		mean, median := delta(row.MeanNS, old.MeanNS), delta(row.MedianNS, old.MedianNS)
		preferred := median
		if row.MedianNS == nil || old.MedianNS == nil {
			preferred = mean
		}
		signal := "not comparable"
		if preferred != nil {
			signal = "within noise band"
			// Fractional nanoseconds can put an exact boundary a few floating
			// point units above the noise band.
			boundary := noisePercent + comparisonEpsilon*math.Max(1, noisePercent)
			if *preferred > boundary {
				signal = "slower (confirm over multiple runs)"
			}
			if *preferred < -boundary {
				signal = "faster (confirm over multiple runs)"
			}
		}
		fmt.Fprintf(&out, "| %s | %s | %s | %s |\n", cell(row.Name), percentage(mean), percentage(median), signal)
		delete(previous, row.Name)
	}
	for _, old := range baseline {
		if _, ok := previous[old.Name]; ok {
			fmt.Fprintf(&out, "| %s | n/a | n/a | missing from current run |\n", cell(old.Name))
		}
	}
	return out.String()
}
