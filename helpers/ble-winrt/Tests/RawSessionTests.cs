using Porthole.Ble.WinRT;

internal static class RawSessionTests
{
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private sealed class FakeSession
    {
        public bool Active { get; private set; }
        public event Action? Changed;
        public int Observers => Changed?.GetInvocationList().Length ?? 0;
        public Action SnapshotCallback() => Changed ?? (() => { });
        public void SetActive(bool active) { Active = active; Changed?.Invoke(); }
    }

    // Matches the adapter's ordering: snapshot raw CCCDs, watch their sessions,
    // then check Active. A zero-Active snapshot drops input, retaining observers.
    private sealed class PeripheralModel : IDisposable
    {
        private readonly RawSessionRegistry<FakeSession> registry;
        private readonly Dictionary<FakeSession, (long Ordinal, Action Handler)> handlers = [];
        private FakeSession[] raw = [];
        public bool Ready { get; private set; }
        public int Reconciliations { get; private set; }
        public PeripheralModel()
        {
            registry = new RawSessionRegistry<FakeSession>((session, ordinal) =>
            {
                Action handler = () =>
                {
                    if (registry!.IsCurrent(session, ordinal)) Reconcile();
                };
                handlers.Add(session, (ordinal, handler));
                session.Changed += handler;
            }, (session, ordinal) =>
            {
                if (handlers.TryGetValue(session, out var observed) && observed.Ordinal == ordinal)
                {
                    session.Changed -= observed.Handler;
                    handlers.Remove(session);
                }
            });
        }
        public void Subscriptions(params FakeSession[] sessions) { raw = sessions; Reconcile(); }
        public void DropPeer() => Ready = false;
        public long Ordinal(FakeSession session) => registry.TryGetOrdinal(session, out long ordinal) ? ordinal : 0;
        private void Reconcile()
        {
            Reconciliations++;
            registry.Synchronize(raw);
            Ready = raw.Length == 3 && raw.All(session => session.Active);
            if (!Ready) DropPeer();
        }
        public void Dispose() => registry.Dispose();
    }

    public static Task Run()
    {
        var sessions = new[] { new FakeSession(), new FakeSession(), new FakeSession() };
        using var peripheral = new PeripheralModel();
        peripheral.Subscriptions(sessions); // All CCCDs arrived before Session.Active.
        Check(!peripheral.Ready && sessions.All(session => session.Observers == 1),
            "Inactive raw sessions were filtered before observation.");
        peripheral.DropPeer();
        Check(sessions.All(session => session.Observers == 1), "Dropping a pending peer detached its raw sessions.");
        foreach (FakeSession session in sessions) session.SetActive(true);
        Check(peripheral.Ready, "Active events could not restore readiness without another CCCD event.");

        FakeSession removed = sessions[0];
        long oldOrdinal = peripheral.Ordinal(removed);
        Action queuedOldCallback = removed.SnapshotCallback();
        peripheral.Subscriptions(sessions[1..]);
        Check(removed.Observers == 0 && peripheral.Ordinal(removed) == 0, "Absent raw session remained observed.");
        int beforeStale = peripheral.Reconciliations;
        queuedOldCallback();
        Check(peripheral.Reconciliations == beforeStale, "Removed session callback triggered reconciliation.");
        peripheral.Subscriptions(sessions);
        Check(peripheral.Ordinal(removed) > oldOrdinal && removed.Observers == 1 && peripheral.Ready,
            "Re-added session reused its old registration or duplicated its handler.");
        beforeStale = peripheral.Reconciliations;
        queuedOldCallback();
        Check(peripheral.Reconciliations == beforeStale, "Queued callback from a prior registration affected the same re-added object.");
        peripheral.Dispose();
        Check(sessions.All(session => session.Observers == 0), "Disposal left native session observers attached.");
        queuedOldCallback();
        Check(peripheral.Reconciliations == beforeStale, "Disposed callback reconciled the adapter.");

        var first = new FakeSession(); var second = new FakeSession();
        var attached = new HashSet<FakeSession>();
        RawSessionRegistry<FakeSession>? registry = null;
        registry = new RawSessionRegistry<FakeSession>((session, ordinal) =>
        {
            Check(registry!.IsCurrent(session, ordinal), "Registration was not current during a synchronous event.");
            attached.Add(session);
            if (session == first) registry.Dispose();
        }, (session, ordinal) =>
        {
            Check(!registry!.IsCurrent(session, ordinal), "Removed registration was current during unsubscribe.");
            attached.Remove(session);
        });
        registry.Synchronize([first, second]);
        Check(attached.Count == 0 && !registry.TryGetOrdinal(first, out _) && !registry.TryGetOrdinal(second, out _),
            "Reentrant disposal attached stale remaining sessions.");
        registry.Synchronize([second]);
        Check(attached.Count == 0, "A late synchronization revived a disposed registry.");

        registry = new RawSessionRegistry<FakeSession>((session, _) =>
        {
            attached.Add(session);
            if (session == first) registry!.Synchronize([second]);
        }, (session, _) => attached.Remove(session));
        registry.Synchronize([first]);
        Check(attached.SetEquals([second]), "Reentrant synchronization did not supersede the old snapshot.");
        registry.Clear();
        Check(attached.Count == 0, "Explicit clearing did not detach all observations.");
        registry.Synchronize([first, first]);
        Check(attached.SetEquals([second]), "Duplicate/reentrant snapshots produced duplicate observers.");
        registry.Dispose();
        Check(attached.Count == 0, "Final disposal leaked an observer.");
        return Task.CompletedTask;
    }
}
