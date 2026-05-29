package thresholds

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"sort"
	"strings"
)

type Threshold struct {
	MaxTimeNS int64 `json:"max_time_ns"`
}

type Set map[string]Threshold

func Parse(reader io.Reader) (Set, error) {
	var raw Set
	decoder := json.NewDecoder(reader)
	if err := decoder.Decode(&raw); err != nil {
		return nil, fmt.Errorf("parse thresholds JSON: %w", err)
	}
	if len(raw) == 0 {
		return nil, fmt.Errorf("threshold config does not contain any benchmarks")
	}

	set := make(Set, len(raw))
	for name, threshold := range raw {
		if name == "" {
			return nil, fmt.Errorf("threshold config contains an empty benchmark name")
		}
		if threshold.MaxTimeNS <= 0 {
			return nil, fmt.Errorf("threshold %q must have positive max_time_ns", name)
		}
		expandedName, err := expandBenchmarkName(name)
		if err != nil {
			return nil, err
		}
		if expandedName == "" {
			return nil, fmt.Errorf("threshold %q expands to an empty benchmark name", name)
		}
		if _, ok := set[expandedName]; ok {
			return nil, fmt.Errorf("threshold config contains duplicate benchmark name after expansion: %q", expandedName)
		}
		set[expandedName] = threshold
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

func expandBenchmarkName(name string) (string, error) {
	missing := make([]string, 0)
	expanded := os.Expand(name, func(key string) string {
		value, ok := os.LookupEnv(key)
		if !ok {
			missing = append(missing, key)
			return ""
		}
		return value
	})
	if len(missing) > 0 {
		sort.Strings(missing)
		return "", fmt.Errorf("threshold %q references unset environment variable(s): %s", name, strings.Join(missing, ", "))
	}
	return expanded, nil
}
