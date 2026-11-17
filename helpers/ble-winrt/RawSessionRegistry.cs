namespace Porthole.Ble.WinRT;

// Observe the raw subscription sessions before checking their Active state.
// The adapter serializes this registry with its state lock. Registration IDs
// distinguish queued callbacks from an earlier observation of the same object.
public sealed class RawSessionRegistry<TSession> : IDisposable where TSession : class
{
    private readonly Dictionary<TSession, long> sessions;
    private readonly Action<TSession, long> added;
    private readonly Action<TSession, long> removed;
    private long nextOrdinal, revision;
    private bool disposed;

    public RawSessionRegistry(Action<TSession, long> added, Action<TSession, long> removed,
        IEqualityComparer<TSession>? comparer = null)
    {
        this.added = added;
        this.removed = removed;
        sessions = new Dictionary<TSession, long>(comparer);
    }

    public void Synchronize(IEnumerable<TSession> rawSessions)
    {
        if (disposed) return;
        var desired = new HashSet<TSession>(rawSessions, sessions.Comparer);
        long operation = ++revision;
        foreach (var existing in sessions.ToArray())
        {
            if (disposed || revision != operation) return;
            if (desired.Contains(existing.Key)) continue;
            sessions.Remove(existing.Key);
            removed(existing.Key, existing.Value);
        }
        foreach (TSession session in desired)
        {
            if (disposed || revision != operation) return;
            if (sessions.ContainsKey(session)) continue;
            long ordinal = ++nextOrdinal;
            sessions.Add(session, ordinal); // A synchronous status event is current.
            try { added(session, ordinal); }
            catch
            {
                if (IsCurrent(session, ordinal))
                {
                    sessions.Remove(session);
                    removed(session, ordinal);
                }
                throw;
            }
        }
    }

    public bool TryGetOrdinal(TSession session, out long ordinal)
        => sessions.TryGetValue(session, out ordinal);

    public bool IsCurrent(TSession session, long ordinal)
        => !disposed && sessions.TryGetValue(session, out long current) && current == ordinal;

    public void Clear()
    {
        revision++;
        var previous = sessions.ToArray();
        sessions.Clear(); // Every queued old callback is stale before any detach.
        foreach (var session in previous)
            removed(session.Key, session.Value);
    }

    public void Dispose()
    {
        if (disposed) return;
        disposed = true;
        Clear();
    }
}
