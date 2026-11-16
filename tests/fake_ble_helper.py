"""A helper that exits once, then stays alive for the app's reconnect test."""
import os
import sys
import time
from pathlib import Path

counter = Path(os.environ["PORTHOLE_TEST_BLE_COUNTER"])
attempt = int(counter.read_text() or "0") + 1
counter.write_text(str(attempt))
print("ready\nsubscribed", flush=True)
if attempt == 1:
    time.sleep(0.1)
else:
    sys.stdin.buffer.read()
