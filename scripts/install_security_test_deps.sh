#!/usr/bin/env bash
set -euo pipefail

# Linux tools for optional signing builds and live TLS security scenarios.
# The general SDK dependency installer does not require these tools.
sudo_command=()
if [[ "${EUID}" -ne 0 ]]; then
  sudo_command=(sudo)
fi
"${sudo_command[@]}" apt-get install -y --no-install-recommends libssl-dev nginx openssl
