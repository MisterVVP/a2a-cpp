package thresholds

import (
	"encoding/json"
	"fmt"
	"io"
	"sort"
)

type Threshold struct {
	MaxTimeNS int64 `json:"max_time_ns"`
}

type Set map[string]Threshold

func Parse(reader io.Reader) (Set, error) {
	var set Set
	decoder := json.NewDecoder(reader)
	if err := decoder.Decode(&set); err != nil {
		return nil, fmt.Errorf("parse thresholds JSON: %w", err)
	}
	if len(set) == 0 {
		return nil, fmt.Errorf("threshold config does not contain any benchmarks")
	}
	for name, threshold := range set {
		if name == "" {
			return nil, fmt.Errorf("threshold config contains an empty benchmark name")
		}
		if threshold.MaxTimeNS <= 0 {
			return nil, fmt.Errorf("threshold %q must have positive max_time_ns", name)
		}
	}
	return set, nil
}

func Names(set Set) []string {
	names := make([]string, 0, len(set))
	for name := range set {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}
