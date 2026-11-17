using System.Diagnostics;
using Porthole.Ble.Core;
using Porthole.Ble.WinRT;

internal static class RetiringCleanupTests
{
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
    private sealed class PhysicalPeer
    {
        public bool Active = true;
        public long Generation = 1;
        public readonly Dictionary<ReportKind, byte[]> State = new();
        public readonly HashSet<ReportKind> RawReports = Enum.GetValues<ReportKind>().ToHashSet();
        public readonly List<ReportKind> Queued = new();
        public bool Held => State.Values.Any(value => value.Any(b => b != 0));
        public void Hold()
        {
            State[ReportKind.Keyboard] = [0, 0, 4, 0, 0, 0, 0, 0];
            State[ReportKind.Mouse] = [1, 0, 0, 0, 0, 0, 0];
            State[ReportKind.Consumer] = [0x23, 2];
        }
    }
    private sealed class ClientWrapper(PhysicalPeer peer, object session, long ordinal)
    {
        public PhysicalPeer Peer { get; } = peer;
        public object Session { get; } = session;
        public long Ordinal { get; } = ordinal;
        public RetiringPeerRegistration<object> Registration => new(Session, Ordinal, "peer-A", Peer.Active);
    }
    private sealed record Captured(PhysicalPeer Peer, long Generation);
    private static RetiringReport<Captured>[] Capture(PhysicalPeer peer) => Enum.GetValues<ReportKind>()
        .Select(kind => new RetiringReport<Captured>(kind, new Captured(peer, peer.Generation))).ToArray();
    private static Task<bool>? Queue(ReportKind kind, Captured target, ReadOnlyMemory<byte> payload, CancellationToken ct)
    {
        Check(payload.Length == HidDescriptor.GetReportLength(kind) && payload.Span.IndexOfAnyExcept((byte)0) < 0,
            "Cleanup payload was not a correctly sized neutral report.");
        if (!target.Peer.Active || target.Peer.Generation != target.Generation || !target.Peer.RawReports.Contains(kind)) return null;
        target.Peer.Queued.Add(kind);
        target.Peer.State[kind] = payload.ToArray();
        return Task.FromResult(true);
    }

    public static async Task Run()
    {
        object nativeSession = new();
        var capturedIdentity = new RetiringPeerRegistration<object>(nativeSession, 7, "peer-A", true);
        // A collection getter can return a different client wrapper for the same
        // registered native session. Wrapper identity must not suppress releases.
        var wrappedPeer = new PhysicalPeer(); wrappedPeer.Hold();
        var oldClientWrapper = new ClientWrapper(wrappedPeer, nativeSession, 7);
        var newClientWrapper = new ClientWrapper(wrappedPeer, nativeSession, 7);
        var deliveredTo = new List<ClientWrapper>();
        var wrappedTargets = Enum.GetValues<ReportKind>()
            .Select(kind => new RetiringReport<ClientWrapper>(kind, oldClientWrapper)).ToArray();
        var wrappedResult = await RetiringPeerCleanup.RunAsync(wrappedTargets, (kind, target, bytes, _) =>
        {
            if (!RetiringPeerCleanup.IsStillRegistered(target.Registration, [newClientWrapper.Registration])) return null;
            deliveredTo.Add(target);
            target.Peer.State[kind] = bytes.ToArray();
            return Task.FromResult(true);
        }, TimeSpan.FromMilliseconds(200));
        Check(wrappedResult.Succeeded == 3 && !wrappedPeer.Held
            && deliveredTo.All(target => ReferenceEquals(target, oldClientWrapper))
            && !ReferenceEquals(oldClientWrapper, newClientWrapper),
            "A fresh equivalent wrapper suppressed cleanup or became the notification target.");
        foreach (var replacementIdentity in new[]
        {
            new RetiringPeerRegistration<object>(nativeSession, 8, "peer-A", true),
            new RetiringPeerRegistration<object>(new object(), 7, "peer-A", true),
            new RetiringPeerRegistration<object>(nativeSession, 7, "peer-B", true),
            new RetiringPeerRegistration<object>(nativeSession, 7, "peer-A", false)
        })
            Check(!RetiringPeerCleanup.IsStillRegistered(capturedIdentity, [replacementIdentity]),
                "A different generation, native session, peer or closed registration was accepted.");

        var original = new PhysicalPeer(); original.Hold();
        var captured = Capture(original);
        var result = await RetiringPeerCleanup.RunAsync(captured, Queue, TimeSpan.FromMilliseconds(200));
        Check(!original.Held && result.Queued == 3 && result.Succeeded == 3 && !result.TimedOut,
            "A still-active captured peer retained a held keyboard, mouse or consumer report.");

        foreach (bool closed in new[] { true, false })
        {
            original = new PhysicalPeer(); original.Hold(); captured = Capture(original);
            if (closed) original.Active = false;
            else original.Generation++; // Same session object has another observation generation.
            result = await RetiringPeerCleanup.RunAsync(captured, Queue, TimeSpan.FromMilliseconds(200));
            Check(result.Queued == 0 && result.Skipped == 3 && original.Queued.Count == 0,
                "A closed/reused session received cleanup from its earlier generation.");
        }

        original = new PhysicalPeer(); original.Hold(); captured = Capture(original);
        var replacement = new PhysicalPeer(); replacement.Hold();
        original.Active = false;
        result = await RetiringPeerCleanup.RunAsync(captured, Queue, TimeSpan.FromMilliseconds(200));
        Check(replacement.Held && replacement.Queued.Count == 0 && result.Skipped == 3,
            "Cleanup retargeted a replacement physical phone.");

        original = new PhysicalPeer(); original.Hold(); captured = Capture(original);
        original.RawReports.Remove(ReportKind.Consumer);
        result = await RetiringPeerCleanup.RunAsync(captured, Queue, TimeSpan.FromMilliseconds(200));
        Check(result.Succeeded == 2 && result.Skipped == 1 && original.Queued.Count == 2
            && original.State[ReportKind.Keyboard].All(b => b == 0) && original.State[ReportKind.Mouse].All(b => b == 0),
            "A missing CCCD either received unauthorized cleanup or suppressed the available keyboard/mouse releases.");

        original = new PhysicalPeer(); original.Hold(); captured = Capture(original);
        result = await RetiringPeerCleanup.RunAsync(captured, (kind, target, bytes, ct) =>
        {
            if (kind == ReportKind.Keyboard) throw new IOException("Native enqueue failed.");
            return Queue(kind, target, bytes, ct);
        }, TimeSpan.FromMilliseconds(200));
        Check(result.Queued == 3 && result.Succeeded == 2
            && original.State[ReportKind.Keyboard].Any(b => b != 0)
            && original.State[ReportKind.Mouse].All(b => b == 0),
            "One rejected native operation prevented another report's release or was misreported as success.");

        original = new PhysicalPeer(); original.Hold(); captured = Capture(original);
        result = await RetiringPeerCleanup.RunAsync(captured, (kind, target, bytes, ct) =>
        {
            Task<bool>? queued = Queue(kind, target, bytes, ct);
            if (kind == ReportKind.Keyboard) target.Peer.Generation++;
            return queued;
        }, TimeSpan.FromMilliseconds(200));
        Check(result.Queued == 1 && result.Skipped == 2 && result.Succeeded == 1,
            "A registration replaced during cleanup still received later reports from the captured generation.");

        // An asynchronous completion can stall even after its zero is queued.
        // Queue every report first, so a stuck keyboard cannot leave a button down.
        original = new PhysicalPeer(); original.Hold(); captured = Capture(original);
        var never = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        var elapsed = Stopwatch.StartNew();
        result = await RetiringPeerCleanup.RunAsync(captured, (kind, target, bytes, ct) =>
        {
            Task<bool>? queued = Queue(kind, target, bytes, ct);
            return kind == ReportKind.Keyboard ? never.Task : queued;
        }, TimeSpan.FromMilliseconds(40));
        Check(result.TimedOut && result.Queued == 3 && result.Succeeded == 2 && !original.Held
            && elapsed.Elapsed < TimeSpan.FromSeconds(1),
            "A hung completion blocked the other neutral reports or made cleanup unbounded.");
        never.SetResult(true); // No dangling test operation.

        // Model the per-client ordering assumed by Microsoft's sample: reports already queued for
        // one client run before subsequently queued zeros, even when completion
        // callbacks arrive late. This is an explicit OS assumption, not a claim
        // that these portable tests executed the real GATT server.
        original = new PhysicalPeer(); captured = Capture(original);
        var oldDelivery = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Task tail = Task.Run(async () => { await oldDelivery.Task; original.Hold(); });
        var old = tail;
        Task<bool>? OrderedZero(ReportKind kind, Captured target, ReadOnlyMemory<byte> bytes, CancellationToken ct)
        {
            Task preceding = tail;
            Task<bool> queued = Task.Run(async () =>
            {
                await preceding;
                return await (Queue(kind, target, bytes, ct) ?? Task.FromResult(false));
            });
            tail = queued;
            return queued;
        }
        Task<NeutralCleanupResult> cleanup = RetiringPeerCleanup.RunAsync(captured, OrderedZero, TimeSpan.FromMilliseconds(200));
        Check(!cleanup.IsCompleted, "Old queued input was not in front of the neutral cleanup.");
        oldDelivery.SetResult();
        result = await cleanup;
        await old;
        Check(result.Succeeded == 3 && !original.Held,
            "Old queued input overwrote the retiring peer's final neutral state.");
    }
}
