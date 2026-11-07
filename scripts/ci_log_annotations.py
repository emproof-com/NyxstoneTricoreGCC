#!/usr/bin/env python3
# Prints the tail of a log file as GitHub Actions error annotations, so that a
# failed hosted run can be read through the public check-runs API without a
# token (the step logs need one).  A step may emit 10 error annotations; each
# carries one chunk of the tail.
#
#   python3 scripts/ci_log_annotations.py <log file> [bytes of tail, default 12000]
import sys

path = sys.argv[1]
tail = int(sys.argv[2]) if len(sys.argv) > 2 else 12000
try:
    with open(path, "rb") as f:
        data = f.read()[-tail:].decode("utf-8", "replace")
except OSError as e:
    print(f"::error title=ci log::cannot read {path}: {e}")
    sys.exit(0)

parts = 10
size = max(1, -(-len(data) // parts))
chunks = [data[i:i + size] for i in range(0, len(data), size)]
for k, chunk in enumerate(chunks):
    # Workflow command encoding of the message text.
    chunk = chunk.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    print(f"::error title=ci log {k + 1}/{len(chunks)}::{chunk}")
