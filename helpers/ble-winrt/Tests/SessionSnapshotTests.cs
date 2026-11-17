using Porthole.Ble.Core;
using Porthole.Ble.WinRT;

internal static class SessionSnapshotTests
{
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    public static Task Run()
    {
        SessionGate? gate = null;
        var observed = new List<(string Status, bool Ready)>();
        using (gate = new SessionGate(line => observed.Add((line, gate!.IsReady))))
        {
            PeerSession peer = gate.Connect("phone-A")!.Value;
            var lifetime = new PipeSessionLifetime();
            gate.SetSubscriptions(peer, keyboard: false, mouse: true, consumer: true);
            Check(!gate.IsReady && !lifetime.Observe(gate.IsReady), "Initial partial snapshot was ready.");
            long initialEpoch = gate.Epoch;

            // Sequential Keyboard=true would mix with old Consumer=true,
            // briefly announce subscribed, then revoke it on Consumer=false.
            gate.SetSubscriptions(peer, keyboard: true, mouse: true, consumer: false);
            Check(!gate.IsReady && observed.All(eventState => !eventState.Ready)
                && observed.All(eventState => eventState.Status != "subscribed")
                && gate.Epoch == initialEpoch && !lifetime.Observe(gate.IsReady),
                "Different partial snapshots exposed transient input readiness.");

            gate.SetSubscriptions(peer, keyboard: true, mouse: true, consumer: true);
            Check(gate.IsReady && observed.Count(eventState => eventState.Status == "subscribed") == 1
                && observed.Last() == ("subscribed", true) && !lifetime.Observe(gate.IsReady),
                "Pending-to-full snapshot did not announce readiness exactly once.");
            var token = new DispatchToken(peer, gate.Epoch);
            int eventsBeforeStable = observed.Count;
            for (int i = 0; i < 3; i++) gate.SetSubscriptions(peer, true, true, true);
            Check(gate.IsCurrent(token) && gate.Epoch == token.Epoch && observed.Count == eventsBeforeStable
                && !lifetime.Observe(gate.IsReady), "Stable full snapshot rotated or restarted a healthy session.");

            gate.SetSubscriptions(peer, keyboard: true, mouse: false, consumer: true);
            Check(!gate.IsReady && !gate.IsCurrent(token) && gate.Epoch > token.Epoch
                && observed.TakeLast(2).SequenceEqual(new[] { ("disconnected", false), ("connected", false) })
                && lifetime.Observe(gate.IsReady), "Genuine readiness loss retained an old dispatch epoch/pipe.");
            gate.SetSubscriptions(peer, true, true, true);
            Check(gate.IsReady && lifetime.Observe(gate.IsReady), "A resumed snapshot cleared the mandatory pipe restart.");

            gate.Disconnect(peer);
            PeerSession next = gate.Connect("phone-A")!.Value;
            gate.SetSubscriptions(peer, true, true, true);
            Check(!gate.IsReady, "An old peer's full snapshot enabled the replacement session.");
            gate.SetSubscribed(next, ReportKind.Keyboard, true);
            gate.SetSubscribed(next, ReportKind.Mouse, true);
            Check(!gate.IsReady, "Legacy single-report setter skipped a missing subscription.");
            gate.SetSubscribed(next, ReportKind.Consumer, true);
            Check(gate.IsReady, "Legacy single-report setter lost its readiness semantics.");
        }
        return Task.CompletedTask;
    }
}
