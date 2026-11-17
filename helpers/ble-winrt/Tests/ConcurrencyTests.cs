using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Diagnostics;
using Porthole.Ble.Core;
using Porthole.Ble.WinRT;

internal static class ConcurrencyTests
{
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
    private static async Task Until(Func<bool> condition)
    {
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(2));
        while (!condition()) await Task.Delay(1, deadline.Token);
    }
    private static byte[] Frame(byte type, params byte[] payload) => [type, (byte)payload.Length, .. payload];
    private static byte[] Key(byte key) => Frame(1, 0, 0, key, 0, 0, 0, 0, 0);
    private static byte[] Mouse(byte button, byte dx = 0) => Frame(2, button, dx, 0, 0, 0, 0, 0);
    private static byte[] Sync(uint id)
    {
        byte[] value = new byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(value, id);
        return Frame(16, value);
    }
    private static byte[] Join(params byte[][] frames) => frames.SelectMany(frame => frame).ToArray();

    // Physical peer state deliberately outlives logical SessionGate readiness.
    // Unregistering a Windows provider does not forcibly disconnect a shared link.
    private sealed class PhysicalNotifier(SessionGate gate) : IReportNotifier
    {
        private readonly object stateLock = new();
        private readonly Dictionary<ReportKind, byte[]> values = new();
        private PeerSession? physicalPeer;
        public ConcurrentQueue<Notification> Sent { get; } = new();
        public Func<DispatchToken, ReportKind, byte[], CancellationToken, Task<bool>>? BeforeSend { get; set; }
        public bool Active { get; set; } = true;
        public bool Held
        {
            get { lock (stateLock) return values.Values.Any(value => value.Any(b => b != 0)); }
        }
        public bool KeyboardHeld
        {
            get { lock (stateLock) return values.TryGetValue(ReportKind.Keyboard, out var value) && value.Any(b => b != 0); }
        }
        public bool ButtonHeld
        {
            get { lock (stateLock) return values.TryGetValue(ReportKind.Mouse, out var value) && value[0] != 0; }
        }
        public async ValueTask<bool> NotifyAsync(DispatchToken token, ReportKind kind, ReadOnlyMemory<byte> payload, CancellationToken ct)
        {
            byte[] bytes = payload.ToArray();
            if (BeforeSend is not null && !await BeforeSend(token, kind, bytes, ct)) return false;
            ct.ThrowIfCancellationRequested();
            if (!Active || !gate.IsCurrent(token)) return false;
            await Task.Delay(1, ct); // Real asynchronous completion, not a virtual clock.
            if (!Active || !gate.IsCurrent(token)) return false;
            lock (stateLock)
            {
                physicalPeer = token.Peer;
                values[kind] = bytes;
                Sent.Enqueue(new Notification(token, kind, bytes, Environment.TickCount64));
            }
            return true;
        }
        public Task<bool>? QueueCapturedNeutral(ReportKind kind, DispatchToken captured,
            ReadOnlyMemory<byte> payload, CancellationToken ct)
        {
            lock (stateLock)
            {
                if (!Active || physicalPeer != captured.Peer) return null;
                Check(payload.Span.IndexOfAnyExcept((byte)0) < 0, "Retirement tried to send user input.");
                values[kind] = payload.ToArray();
                Sent.Enqueue(new Notification(captured, kind, payload.ToArray(), Environment.TickCount64));
                return Task.FromResult(true);
            }
        }
        public void PhysicalDisconnect()
        {
            Active = false;
            lock (stateLock) values.Clear();
        }
    }

    public static async Task AtomicCallbackStorm()
    {
        var status = new ConcurrentQueue<string>();
        using var gate = new SessionGate(status.Enqueue);
        var peer = gate.Connect("physical-peer-A")!.Value;
        long epoch = gate.Epoch;
        Task[] partials = Enumerable.Range(0, 4).Select(worker => Task.Run(() =>
        {
            for (int i = 0; i < 3000; i++)
            {
                int missing = (worker + i) % 3;
                gate.SetSubscriptions(peer, missing != 0, missing != 1, missing != 2);
                gate.SetHostSuspended((i & 1) == 0);
                Check(!gate.IsCurrent(new DispatchToken(peer, epoch)), "Incomplete concurrent snapshots exposed a ready token.");
            }
        })).ToArray();
        await Task.WhenAll(partials);
        Check(!gate.IsReady && gate.Epoch == epoch && !status.Contains("subscribed"),
            "Partial snapshot reorder manufactured readiness or rotated the physical peer.");
        gate.SetSubscriptions(peer, true, true, true);
        var current = new DispatchToken(peer, gate.Epoch);
        await Task.WhenAll(Enumerable.Range(0, 4).Select(_ => Task.Run(() =>
        {
            for (int i = 0; i < 3000; i++)
            {
                gate.SetSubscriptions(peer, true, true, true);
                gate.SetHostSuspended((i & 1) == 0);
                Check(gate.IsCurrent(current), "Stable subscriptions or a power callback revoked the input epoch.");
            }
        })));
        Check(status.Count(line => line == "subscribed") == 1 && gate.Epoch == epoch,
            "Duplicate full snapshots emitted additional readiness transitions.");
        gate.Disconnect(peer);
        var replacement = gate.Connect("physical-peer-B")!.Value;
        gate.SetSubscriptions(replacement, true, true, true);
        await Task.WhenAll(Enumerable.Range(0, 4).Select(_ => Task.Run(() =>
        {
            for (int i = 0; i < 1000; i++)
            {
                gate.SetSubscriptions(peer, false, false, false);
                gate.Disconnect(peer);
                Check(gate.CurrentPeer == replacement && gate.IsReady,
                    "An old physical peer's callback changed its replacement.");
            }
        })));
    }

    public static async Task StatefulEofAndCancellation()
    {
        foreach (bool cancel in new[] { false, true })
        {
            var status = new ConcurrentQueue<string>();
            using var gate = new SessionGate(status.Enqueue);
            var peer = gate.Connect("physical-peer-A")!.Value;
            gate.SetSubscriptions(peer, true, true, true);
            var token = new DispatchToken(peer, gate.Epoch);
            var physical = new PhysicalNotifier(gate);
            using var input = new PushStream(); using var stop = new CancellationTokenSource();
            var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            physical.BeforeSend = async (_, kind, value, ct) =>
            {
                if (kind == ReportKind.Mouse && value[1] == 7)
                {
                    entered.TrySetResult();
                    await Task.Delay(Timeout.Infinite, ct);
                }
                return true;
            };
            Task run = new ProtocolRunner(gate, physical, status.Enqueue).RunAsync(input, stop.Token);
            input.Push(Join(Key(4), Mouse(1), Sync(701)));
            await Until(() => physical.KeyboardHeld && physical.ButtonHeld && status.Contains("sync 701"));
            for (int i = 0; i < 30; i++)
            {
                gate.SetHostSuspended(true);
                gate.SetHostSuspended(false);
                Check(gate.IsCurrent(token) && physical.KeyboardHeld && physical.ButtonHeld,
                    "A host power hint silently cleared physical held state or replaced the epoch.");
                await Task.Yield();
            }
            if (cancel)
            {
                input.Push(Join(Mouse(1, 7), Sync(702)));
                await entered.Task.WaitAsync(TimeSpan.FromSeconds(2));
                stop.Cancel();
            }
            else input.Complete();
            var elapsed = Stopwatch.StartNew();
            await run.WaitAsync(TimeSpan.FromSeconds(2));
            Check(elapsed.Elapsed < TimeSpan.FromSeconds(2) && !physical.Held && physical.Active,
                "EOF/cancellation left a physically connected phone holding a key/button.");
            Check(physical.Sent.TakeLast(3).All(report => report.Value.All(value => value == 0)),
                "Independent EOF releases were missing or overtaken by an old report.");
            Check(!status.Contains("sync 702") && !gate.IsReady, "Cancelled work was acknowledged or its epoch remained ready.");
        }
    }

    public static async Task StatefulBackpressureAndDrain()
    {
        foreach (bool keyLane in new[] { false, true })
        {
            var status = new ConcurrentQueue<string>();
            using var gate = new SessionGate(status.Enqueue);
            var peer = gate.Connect("physical-peer-A")!.Value;
            gate.SetSubscriptions(peer, true, true, true);
            var physical = new PhysicalNotifier(gate);
            using var input = new PushStream();
            var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            physical.BeforeSend = async (_, kind, value, ct) =>
            {
                if ((keyLane && kind == ReportKind.Keyboard && value[2] == 5) ||
                    (!keyLane && kind == ReportKind.Mouse && value[1] == 7))
                {
                    entered.TrySetResult();
                    await Task.Delay(Timeout.Infinite, ct);
                }
                return true;
            };
            var options = new RunnerOptions { KeyCapacity = 4, MotionCapacity = 4 };
            Task run = new ProtocolRunner(gate, physical, status.Enqueue, options: options).RunAsync(input);
            input.Push(Join(Key(4), Mouse(1), Sync(721)));
            await Until(() => physical.KeyboardHeld && physical.ButtonHeld && status.Contains("sync 721"));
            input.Push(keyLane ? Key(5) : Mouse(1, 7));
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(2));
            input.Push(Join(Enumerable.Range(0, 20).Select(_ => keyLane ? Key(6) : Mouse(1, 8)).Append(Sync(722)).ToArray()));
            await run.WaitAsync(TimeSpan.FromSeconds(2));
            Check(status.Contains(keyLane ? "error key queue full" : "error motion queue full"),
                "Backpressure was not reported for the blocked physical input lane.");
            Check(!physical.Held && !status.Contains("sync 722") && !gate.IsReady,
                "Overflow left a physical key/button held, acknowledged discarded work, or kept the old epoch ready.");
        }

        // A real 65-second mouse delay must not hold EOF shutdown for 65 seconds.
        var eofStatus = new ConcurrentQueue<string>();
        using var eofGate = new SessionGate(eofStatus.Enqueue);
        var eofPeer = eofGate.Connect("physical-peer-A")!.Value;
        eofGate.SetSubscriptions(eofPeer, true, true, true);
        var eofPhysical = new PhysicalNotifier(eofGate);
        using var eofInput = new PushStream();
        Task eof = new ProtocolRunner(eofGate, eofPhysical, eofStatus.Enqueue).RunAsync(eofInput);
        eofInput.Push(Join(Key(4), Mouse(1), Sync(723)));
        await Until(() => eofPhysical.KeyboardHeld && eofPhysical.ButtonHeld && eofStatus.Contains("sync 723"));
        eofInput.Push(Join(Frame(18, 255, 255), Mouse(0, 2), Sync(724)));
        eofInput.Complete();
        var clock = Stopwatch.StartNew();
        await eof.WaitAsync(TimeSpan.FromSeconds(2));
        Check(eofStatus.Contains("error input drain timed out") && !eofStatus.Contains("sync 724")
            && !eofPhysical.Held && clock.Elapsed < TimeSpan.FromSeconds(2),
            "Real-time EOF failed to cancel a long queued delay and release the physically held input.");
    }

    public static async Task SerializedCallbackRaces()
    {
        object callbackLock = new();
        var physicalSessions = new[] { new object(), new object(), new object() };
        var handlers = new Dictionary<object, (long Ordinal, Action Callback)>();
        var backlog = new ConcurrentQueue<Action>();
        int current = 0, stale = 0, mismatches = 0;
        RawSessionRegistry<object>? registry = null;
        registry = new RawSessionRegistry<object>((session, ordinal) =>
        {
            Action callback = () =>
            {
                lock (callbackLock)
                {
                    if (!registry!.IsCurrent(session, ordinal)) { stale++; return; }
                    current++;
                    if (!handlers.TryGetValue(session, out var active) || active.Ordinal != ordinal) mismatches++;
                }
            };
            handlers.Add(session, (ordinal, callback));
        }, (session, ordinal) =>
        {
            if (handlers.TryGetValue(session, out var active) && active.Ordinal == ordinal) handlers.Remove(session);
        });
        Task changes = Task.Run(() =>
        {
            for (int i = 0; i < 3000; i++)
            {
                lock (callbackLock)
                {
                    object session = physicalSessions[i % physicalSessions.Length];
                    registry.Synchronize([session]);
                    backlog.Enqueue(handlers[session].Callback);
                }
                if ((i & 15) == 0) Thread.Yield();
            }
        });
        Task delivery = Task.Run(async () =>
        {
            while (!changes.IsCompleted || !backlog.IsEmpty)
            {
                if (backlog.TryDequeue(out Action? callback)) callback();
                else await Task.Yield();
            }
        });
        await Task.WhenAll(changes, delivery);
        lock (callbackLock)
        {
            Action last = handlers.Values.Single().Callback;
            registry.Dispose();
            last();
            Check(current + stale == 3001 && stale > 0 && mismatches == 0 && handlers.Count == 0,
                "Delayed native callbacks reused an old registration or survived disposal.");
        }

        // The adapter serializes these callbacks. Either lock ordering must
        // make a concrete advertising error win over a concurrent Started.
        for (int i = 0; i < 250; i++)
        {
            var startup = new AdvertisingStartupState(); startup.RequestStart();
            object advertisingLock = new();
            await Task.WhenAll(Task.Run(() => { lock (advertisingLock) startup.Observe(AdvertisingState.Started, false); }),
                Task.Run(() => { lock (advertisingLock) startup.Observe(AdvertisingState.Aborted, true); }));
            Check(startup.Failed && startup.Observe(AdvertisingState.Started, false) == AdvertisingAction.Failed,
                "A Started/error race cleared the advertising failure.");
            startup.RequestStop();
            Check(startup.Observe(AdvertisingState.Aborted, true) == AdvertisingAction.Ignored,
                "A delayed callback after expected shutdown changed startup state.");
        }
    }

    public static async Task CachedGenerationCannotBleed()
    {
        object observerLock = new(), nativeSession = new();
        using var registry = new RawSessionRegistry<object>((_, _) => { }, (_, _) => { });
        registry.Synchronize([nativeSession]);
        Check(registry.TryGetOrdinal(nativeSession, out long selectedOrdinal), "Initial target was not observed.");
        var selected = new RetiringPeerRegistration<object>(nativeSession, selectedOrdinal, "same-bond", true);
        var status = new ConcurrentQueue<string>();
        using var gate = new SessionGate(status.Enqueue);
        var peer = gate.Connect("same-bond")!.Value;
        gate.SetSubscriptions(peer, true, true, true);
        var old = new DispatchToken(peer, gate.Epoch);
        var physical = new PhysicalNotifier(gate);
        using var input = new PushStream(); using var stop = new CancellationTokenSource();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var continueDelivery = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        physical.BeforeSend = async (_, kind, value, _) =>
        {
            if (kind != ReportKind.Mouse || value[1] != 13) return true;
            entered.TrySetResult();
            await continueDelivery.Task;
            lock (observerLock)
            {
                registry.TryGetOrdinal(nativeSession, out long currentOrdinal);
                return RetiringPeerCleanup.IsStillRegistered(selected,
                    [new RetiringPeerRegistration<object>(nativeSession, currentOrdinal, "same-bond", true)]);
            }
        };
        Task run = new ProtocolRunner(gate, physical, status.Enqueue).RunAsync(input, stop.Token);
        input.Push(Join(Key(4), Key(0), Mouse(0, 13), Mouse(0, 14), Sync(730)));
        await entered.Task.WaitAsync(TimeSpan.FromSeconds(2));
        long replacementOrdinal;
        lock (observerLock)
        {
            // This is the critical ordering: raw synchronization reuses the COM
            // object BEFORE the adapter reaches its logical peer-drop branch.
            registry.Synchronize([]);
            registry.Synchronize([nativeSession]);
            Check(registry.TryGetOrdinal(nativeSession, out replacementOrdinal) && replacementOrdinal != selectedOrdinal,
                "A re-added native object reused its former observation generation.");
            Check(!registry.IsCurrent(selected.Session, selected.Ordinal),
                "The cached normal target survived a raw registration replacement.");
            gate.Disconnect(peer);
        }
        int queuedCleanup = 0;
        var captured = Enum.GetValues<ReportKind>()
            .Select(kind => new RetiringReport<RetiringPeerRegistration<object>>(kind, selected)).ToArray();
        var cleanup = await RetiringPeerCleanup.RunAsync(captured, (kind, target, value, _) =>
        {
            if (!RetiringPeerCleanup.IsStillRegistered(target,
                [new RetiringPeerRegistration<object>(nativeSession, replacementOrdinal, "same-bond", true)])) return null;
            queuedCleanup++;
            return Task.FromResult(true);
        }, TimeSpan.FromMilliseconds(200));
        stop.Cancel(); continueDelivery.SetResult();
        await run.WaitAsync(TimeSpan.FromSeconds(2));
        Check(cleanup.Skipped == 3 && queuedCleanup == 0 && !gate.IsCurrent(old)
            && !status.Contains("sync 730")
            && !physical.Sent.Any(report => report.Kind == ReportKind.Mouse && report.Value[1] is 13 or 14),
            "Old normal input, its fence, or neutral cleanup crossed into the replacement raw generation.");
        var lifetime = new PipeSessionLifetime(); lifetime.Observe(true);
        Check(lifetime.Observe(gate.IsReady) && lifetime.Observe(true),
            "A replacement generation bypassed the fresh-pipe requirement.");
    }

    public static async Task SameBondIdleAndRestoredLifetimes()
    {
        // Advance the actual runner's injected monotonic clock beyond 32-bit
        // milliseconds, retaining one ready peer through many idle/power hints.
        using (var live = new Fixture())
        using (var input = new PushStream())
        {
            var token = new DispatchToken(live.Peer, live.Gate.Epoch);
            int statusBefore = live.Status.Count;
            Task run = live.Runner().RunAsync(input);
            for (uint cycle = 0; cycle < 24; cycle++)
            {
                live.Clock.Advance(300_000_000);
                live.Gate.SetHostSuspended(true);
                live.Gate.SetSubscriptions(live.Peer, true, true, true);
                live.Gate.SetHostSuspended(false);
                uint fence = 800 + cycle;
                input.Push(Join(Key(4), Key(0), Mouse(1), Mouse(0), Sync(fence)));
                await Until(() => live.Status.Contains($"sync {fence}")
                    && live.Notifier.Sent.Count(report => report.Kind == ReportKind.Keyboard && report.Value[2] == 4) == cycle + 1
                    && live.Notifier.Sent.Last(report => report.Kind == ReportKind.Keyboard).Value.All(value => value == 0));
                Check(live.Gate.IsCurrent(token) && live.Gate.Epoch == token.Epoch,
                    "Same-bond idle/power callbacks replaced the ready session.");
            }
            Check(live.Clock.NowMilliseconds > uint.MaxValue
                && live.Status.Skip(statusBefore).All(line => line.StartsWith("sync ")),
                "Long monotonic uptime emitted spurious reconnection or readiness events.");
            input.Complete(); await run;
            Check(live.Notifier.Sent.Count(report => report.Kind == ReportKind.Keyboard && report.Value[2] == 4) == 24
                && live.Notifier.Sent.Count(report => report.Kind == ReportKind.Mouse && report.Value[0] == 1) == 24
                && live.Notifier.Sent.TakeLast(3).All(report => report.Value.All(value => value == 0)),
                "Long-lived session lost physical input or ended with held input.");
        }

        // This models the OS supplying an initial restored-CCCD snapshot, not
        // a claim that Windows/iOS actually persist those CCCDs across providers.
        var oldCallbacks = new List<Action>();
        for (uint generation = 0; generation < 12; generation++)
        {
            var next = new Fixture(false);
            foreach (Action stale in oldCallbacks) stale();
            Check(!next.Gate.IsReady, "An older helper lifetime activated the current transport.");
            next.Gate.SetSubscriptions(next.Peer, true, true, true);
            await next.Runner().RunAsync(new MemoryStream(Join(Key(4), Key(0), Mouse(1), Mouse(0), Sync(900 + generation))));
            var notifications = next.Notifier.Sent.ToArray();
            Check(next.Status.Contains($"sync {900 + generation}")
                && notifications.Take(3).All(report => report.Value.All(value => value == 0))
                && notifications.TakeLast(3).All(report => report.Value.All(value => value == 0)),
                "A restored transport skipped fresh neutral priming, a fence, or final releases.");
            oldCallbacks.Add(() =>
            {
                next.Gate.SetSubscriptions(next.Peer, true, true, true);
                next.Gate.Disconnect(next.Peer);
            });
            next.Dispose();
        }
    }

    // Desired invariant: a transient failed operation is not proof that the
    // physical phone disconnected. Cleanup must release that captured peer.
    public static async Task NativeFailureMustReleasePhysicalPeer()
    {
        var status = new ConcurrentQueue<string>();
        using var gate = new SessionGate(status.Enqueue);
        var peer = gate.Connect("physical-peer-A")!.Value;
        gate.SetSubscriptions(peer, true, true, true);
        var physical = new PhysicalNotifier(gate);
        using var input = new PushStream(); using var stop = new CancellationTokenSource();
        var failed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Task<NeutralCleanupResult>? cleanup = null;
        physical.BeforeSend = (token, kind, value, _) =>
        {
            if (kind == ReportKind.Mouse && value[1] == 9)
            {
                // Mirrors WinRtHidPeripheral.NotifyAsync's current failure path:
                // logical DropPeer happens while the real GATT client stays active.
                var captured = Enum.GetValues<ReportKind>()
                    .Select(report => new RetiringReport<DispatchToken>(report, token)).ToArray();
                gate.Disconnect(token.Peer);
                cleanup = RetiringPeerCleanup.RunAsync(captured,
                    physical.QueueCapturedNeutral, TimeSpan.FromMilliseconds(200));
                failed.TrySetResult();
                return Task.FromResult(false);
            }
            return Task.FromResult(true);
        };
        Task run = new ProtocolRunner(gate, physical, status.Enqueue).RunAsync(input, stop.Token);
        input.Push(Join(Key(4), Mouse(1), Sync(711)));
        await Until(() => physical.KeyboardHeld && physical.ButtonHeld && status.Contains("sync 711"));
        input.Push(Join(Mouse(1, 9), Sync(712)));
        await failed.Task.WaitAsync(TimeSpan.FromSeconds(2));
        stop.Cancel(); // Program's native Failure branch cancels the reader.
        await run.WaitAsync(TimeSpan.FromSeconds(2));
        Check(cleanup is not null && (await cleanup).Succeeded == 3,
            "Captured retiring peer did not receive its independent neutral cleanup.");
        Check(!status.Contains("sync 712"), "Failed input was falsely acknowledged.");
        Check(!physical.Held,
            "Native failure logically disconnected the gate, so EOF skipped neutral releases while the real phone still holds a key/button.");
    }
}
