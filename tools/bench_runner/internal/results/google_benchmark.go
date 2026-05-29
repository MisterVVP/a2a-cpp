package results

import (
	"encoding/json"
	"fmt"
	"io"
	"math"
	"sort"
	"strings"
)

const (
	RealTimeField = "real_time"
	CPUTimeField  = "cpu_time"
	Nanoseconds   = "ns"
)

type GoogleBenchmarkFile struct {
	Benchmarks []BenchmarkEntry `json:"benchmarks"`
}

type BenchmarkEntry struct {
	Name     string  `json:"name"`
	RealTime float64 `json:"real_time"`
	CPUTime  float64 `json:"cpu_time"`
	TimeUnit string  `json:"time_unit"`
}

type Measurement struct {
	Name     string
	ActualNS int64
	MedianNS *int64
	TimeUnit string
}

func Parse(reader io.Reader, timeField string) (map[string]Measurement, error) {
	var file GoogleBenchmarkFile
	decoder := json.NewDecoder(reader)
	if err := decoder.Decode(&file); err != nil {
		return nil, fmt.Errorf("parse Google Benchmark JSON: %w", err)
	}
	if len(file.Benchmarks) == 0 {
		return nil, fmt.Errorf("Google Benchmark JSON does not contain benchmark entries")
	}
	if timeField != RealTimeField && timeField != CPUTimeField {
		return nil, fmt.Errorf("unsupported time field %q", timeField)
	}

	measurements := make(map[string]Measurement)
	medianValues := make(map[string]int64)
	for _, entry := range file.Benchmarks {
		if entry.Name == "" {
			return nil, fmt.Errorf("benchmark entry has empty name")
		}
		if entry.TimeUnit != "" && entry.TimeUnit != Nanoseconds {
			return nil, fmt.Errorf("benchmark %q uses unsupported time unit %q", entry.Name, entry.TimeUnit)
		}
		baseName, aggregate := NormalizeName(entry.Name)
		if aggregate == "stddev" || aggregate == "cv" {
			continue
		}
		value, err := selectedTime(entry, timeField)
		if err != nil {
			return nil, err
		}
		actual := int64(math.Round(value))
		if aggregate == "median" {
			medianValues[baseName] = actual
			continue
		}
		if aggregate == "" || aggregate == "mean" {
			if existing, ok := measurements[baseName]; ok && aggregate == "" && existing.Name == baseName {
				continue
			}
			measurements[baseName] = Measurement{Name: baseName, ActualNS: actual, TimeUnit: Nanoseconds}
		}
	}
	for name, median := range medianValues {
		measurement, ok := measurements[name]
		if !ok {
			continue
		}
		measurement.MedianNS = &median
		measurements[name] = measurement
	}
	if len(measurements) == 0 {
		return nil, fmt.Errorf("Google Benchmark JSON does not contain usable timing entries")
	}
	return measurements, nil
}

func Names(measurements map[string]Measurement) []string {
	names := make([]string, 0, len(measurements))
	for name := range measurements {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

func NormalizeName(name string) (string, string) {
	for _, suffix := range []string{"_mean", "_median", "_stddev", "_cv"} {
		if strings.HasSuffix(name, suffix) {
			return strings.TrimSuffix(name, suffix), strings.TrimPrefix(suffix, "_")
		}
	}
	return name, ""
}

func selectedTime(entry BenchmarkEntry, timeField string) (float64, error) {
	var value float64
	switch timeField {
	case RealTimeField:
		value = entry.RealTime
	case CPUTimeField:
		value = entry.CPUTime
	default:
		return 0, fmt.Errorf("unsupported time field %q", timeField)
	}
	if value < 0 || math.IsNaN(value) || math.IsInf(value, 0) {
		return 0, fmt.Errorf("benchmark %q has invalid %s value", entry.Name, timeField)
	}
	return value, nil
}
