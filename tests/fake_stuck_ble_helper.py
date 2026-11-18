"""Exercise EOF/TERM escalation without Bluetooth, D-Bus or a GLib loop."""
import fcntl
import os
import select
import signal
import sys
import time

mode = sys.argv[1] if len(sys.argv) > 1 else "hung"


def ignore_term(_signal, _frame):
    print("term-ignored", flush=True)


signal.signal(signal.SIGTERM, signal.SIG_IGN if mode == "closed-output" else ignore_term)
if mode == "never-reads" and hasattr(fcntl, "F_SETPIPE_SZ"):
    # Linux lets either endpoint fix the capacity before announcing readiness.
    fcntl.fcntl(sys.stdin.fileno(), fcntl.F_SETPIPE_SZ, 4096)
print(f"pid {os.getpid()}\nready", flush=True)
if mode == "exit":
    sys.exit(0)
if mode == "closed-output":
    sys.stdout.flush()
    os.close(sys.stdout.fileno())
if mode == "never-reads":
    # HUP remains observable with unread bytes: prove stdin was closed without
    # draining it and accidentally relieving the application's backpressure.
    poller = select.poll()
    poller.register(sys.stdin.fileno(), select.POLLHUP)
    while not poller.poll(100):
        pass
    print("eof-ignored", flush=True)
    while True:
        time.sleep(1)
sys.stdin.buffer.read()
if mode != "closed-output":
    print("eof-ignored", flush=True)
while True:
    time.sleep(1)
