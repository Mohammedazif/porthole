"""Exercise EOF/TERM escalation without Bluetooth, D-Bus or a GLib loop."""
import os
import signal
import sys
import time

mode = sys.argv[1] if len(sys.argv) > 1 else "hung"


def ignore_term(_signal, _frame):
    print("term-ignored", flush=True)


signal.signal(signal.SIGTERM, signal.SIG_IGN if mode == "closed-output" else ignore_term)
print(f"pid {os.getpid()}\nready", flush=True)
if mode == "exit":
    sys.exit(0)
if mode == "closed-output":
    sys.stdout.flush()
    os.close(sys.stdout.fileno())
sys.stdin.buffer.read()
if mode != "closed-output":
    print("eof-ignored", flush=True)
while True:
    time.sleep(1)
