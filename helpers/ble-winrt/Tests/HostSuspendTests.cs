using System.Buffers.Binary;
using System.Collections.Concurrent;
using Porthole.Ble.Core;
using Porthole.Ble.WinRT;

internal static class HostSuspendTests
{
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private static async Task Until(Func<bool> ready)
    {
        for (int i = 0; i < 2000; i++)
        {
            if (ready()) return;
            await Task.Delay(1);
        }
        throw new TimeoutException("Expected HID power-state test condition did not arrive.");
    }

    private static byte[] Frame(byte type, params byte[] payload) => [type, (byte)payload.Length, .. payload];
    private static byte[] Key(byte key) => Frame(1, 0, 0, key, 0, 0, 0, 0, 0);
    private static byte[] Button(byte button) => Frame(2, button, 0, 0, 0, 0, 0, 0);
    private static byte[] Sync(uint id)
    {
        byte[] payload = new byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(payload, id);
        return Frame(16, payload);
    }
    private static byte[] Join(params byte[][] frames) => frames.SelectMany(frame => frame).ToArray();

    public static async Task Run()
    {
        using var f = new Fixture(); using var input = new PushStream();
        var lifetime = new PipeSessionLifetime();
        Check(!lifetime.Observe(f.Gate.IsReady), "Initial ready session required restart.");
        var token = new DispatchToken(f.Peer, f.Gate.Epoch);
        Task run = f.Runner().RunAsync(input);
        input.Push(Join(Key(4), Button(1), Sync(601)));
        await Until(() => f.Status.Contains("sync 601")
            && f.Notifier.Sent.Any(report => report.Kind == ReportKind.Keyboard && report.Value[2] == 4)
            && f.Notifier.Sent.Any(report => report.Kind == ReportKind.Mouse && report.Value[0] == 1));
        int reportsBeforeHint = f.Notifier.Sent.Count, statusBeforeHint = f.Status.Count;
        f.Gate.SetHostSuspended(true);
        Check(f.Gate.HostSuspended && f.Gate.IsCurrent(token) && f.Gate.IsReady
            && !lifetime.Observe(f.Gate.IsReady) && f.Notifier.Sent.Count == reportsBeforeHint
            && f.Status.Count == statusBeforeHint,
            "Suspend revoked the session, cleared held input, emitted status, or restarted its pipe.");

        // Releases are still valid for the same live peer after the power hint.
        input.Push(Join(Key(0), Button(0), Sync(602)));
        await Until(() => f.Status.Contains("sync 602")
            && f.Notifier.Sent.Last(report => report.Kind == ReportKind.Keyboard).Value.All(value => value == 0));
        Check(f.Gate.IsCurrent(token) && f.Gate.HostSuspended
            && f.Notifier.Sent.Last(report => report.Kind == ReportKind.Mouse).Value.All(value => value == 0),
            "Power-state hint lost a held keyboard/button release or changed epoch.");
        reportsBeforeHint = f.Notifier.Sent.Count; statusBeforeHint = f.Status.Count;
        for (int i = 0; i < 4; i++)
        {
            f.Gate.SetHostSuspended(true);
            f.Gate.SetHostSuspended(false);
        }
        Check(!f.Gate.HostSuspended && f.Gate.IsCurrent(token) && !lifetime.Observe(f.Gate.IsReady)
            && f.Status.Count == statusBeforeHint && f.Notifier.Sent.Count == reportsBeforeHint,
            "Repeated Suspend/Exit hints emitted fake readiness events, zeros, or epoch changes.");
        input.Complete(); await run;

        foreach (bool disconnect in new[] { false, true })
        {
            using var lost = new Fixture(); var pipe = new PipeSessionLifetime();
            pipe.Observe(lost.Gate.IsReady);
            var old = new DispatchToken(lost.Peer, lost.Gate.Epoch);
            lost.Gate.SetHostSuspended(true);
            if (disconnect) lost.Gate.Disconnect(lost.Peer);
            else lost.Gate.SetSubscriptions(lost.Peer, keyboard: true, mouse: true, consumer: false);
            Check(!lost.Gate.IsReady && !lost.Gate.IsCurrent(old) && pipe.Observe(lost.Gate.IsReady),
                "Real session/subscription loss while suspended did not invalidate the old pipe.");
            if (disconnect)
            {
                Check(!lost.Gate.HostSuspended, "Disconnect retained the old host power state.");
                var next = lost.Gate.Connect("phone-B")!.Value; lost.Subscribe(next);
                Check(!lost.Gate.HostSuspended && pipe.Observe(lost.Gate.IsReady),
                    "A new peer inherited Suspend or bypassed the fresh-pipe requirement.");
            }
        }

        var status = new ConcurrentQueue<string>();
        using var unconnected = new SessionGate(status.Enqueue);
        unconnected.SetHostSuspended(true);
        Check(unconnected.HostSuspended && unconnected.CurrentPeer is null && !unconnected.IsReady && status.IsEmpty,
            "An early power hint created a false connection.");
        PeerSession peer = unconnected.Connect("phone-A")!.Value;
        Check(!unconnected.HostSuspended, "A new peer inherited an unassigned early power hint.");
        unconnected.SetHostSuspended(true);
        Check(unconnected.Connect("phone-A") == peer && unconnected.HostSuspended,
            "Duplicate same-peer connection forgot its power hint.");
        unconnected.Reset();
        Check(!unconnected.HostSuspended, "Transport reset retained the old host power state.");
    }
}
