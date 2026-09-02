#!/usr/bin/env python3
import subprocess
import sys
from pathlib import Path

for library_argument in sys.argv[1:]:
    library = Path(library_argument)
    result = subprocess.run(["nm", "-C", str(library)], check=True, capture_output=True, text=True)
    for forbidden in ("subscription_diagnostics", "A2A_SUBSCRIPTION_DIAGNOSTICS"):
        if forbidden in result.stdout or forbidden.encode() in library.read_bytes():
            raise SystemExit(f"normal SDK library contains disabled diagnostic token: {forbidden}")
