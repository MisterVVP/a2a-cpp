#!/usr/bin/env bash
set -euo pipefail

# Run from the repository root. GH_TOKEN needs only Actions read access.
# A missing/expired artifact or unavailable API starts a new history point;
# malformed downloaded benchmark data is an input error, not a silent fallback.
: "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY must be set}"
baseline_dir="$(mktemp -d "${RUNNER_TEMP:-/tmp}/benchmark-baseline.XXXXXX")"
trap 'rm -rf "$baseline_dir"' EXIT
baseline_args=()
current_run="${GITHUB_RUN_ID:-}"
cutoff="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
if [[ -n "$current_run" ]]; then
  # A rerun must use its original creation time, never the wall-clock fallback.
  cutoff=""
  if started="$(gh run view "$current_run" --repo "$GITHUB_REPOSITORY" --json createdAt --jq .createdAt)"; then
    cutoff="$started"
  fi
fi

if [[ -n "$cutoff" ]] && runs="$(gh run list --repo "$GITHUB_REPOSITORY" --workflow ci.yml \
  --branch main --event push --status success --created "<$cutoff" --limit 100 \
  --json databaseId,createdAt --jq '.[] | [.databaseId, .createdAt] | @tsv')"; then
  while IFS=$'\t' read -r run_id created_at; do
    if [[ -z "$run_id" || "$run_id" == "$current_run" || ! "$created_at" < "$cutoff" ]]; then
      continue
    fi
    if gh run download "$run_id" --repo "$GITHUB_REPOSITORY" \
      --name benchmark-results --dir "$baseline_dir"; then
      if [[ -s "$baseline_dir/benchmark-results.json" ]]; then
        baseline_args=(--baseline-results "$baseline_dir/benchmark-results.json"
          --baseline-run-url "${GITHUB_SERVER_URL:-https://github.com}/$GITHUB_REPOSITORY/actions/runs/$run_id")
      fi
    else
      echo "Previous successful main benchmark artifact is unavailable; recording without a baseline." >&2
    fi
    # Do not substitute an older run for the previous successful main run.
    break
  done <<< "$runs"
else
  echo "Could not determine previous successful main baseline; recording without a baseline." >&2
fi

go run ./tools/bench_runner/cmd/a2a-bench-trend "${baseline_args[@]}" "$@"
