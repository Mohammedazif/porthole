namespace Porthole.Ble.Core;

// PeerSession is stable for a physical connection. Dispatch epochs additionally
// rotate after readiness loss, invalidating queued reports and pending fences.
public sealed class SessionGate : IDisposable
{
    private readonly object stateLock = new();
    private readonly Action<string> status;
    private CancellationTokenSource lifetime = new();
    private PeerSession? peer;
    private long nextPeer, epoch;
    private byte subscriptions;
    private bool disposed;
    private const byte AllReports = (1 << 1) | (1 << 2) | (1 << 3);

    public SessionGate(Action<string> status) => this.status = status;
    public PeerSession? CurrentPeer { get { lock (stateLock) return peer; } }
    public long Epoch { get { lock (stateLock) return epoch; } }
    public bool IsReady { get { lock (stateLock) return ReadyLocked; } }
    private bool ReadyLocked => !disposed && peer.HasValue && subscriptions == AllReports;

    public PeerSession? Connect(string clientId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(clientId);
        lock (stateLock)
        {
            ObjectDisposedException.ThrowIf(disposed, this);
            if (peer.HasValue)
                return peer.Value.ClientId == clientId ? peer : null;
            RotateLocked();
            peer = new PeerSession(++nextPeer, clientId);
            subscriptions = 0;
            status("connected");
            return peer;
        }
    }

    public void SetSubscribed(PeerSession session, ReportKind kind, bool enabled)
    {
        _ = HidDescriptor.GetReportLength(kind);
        lock (stateLock)
        {
            if (disposed || peer != session)
                return;
            bool wasReady = ReadyLocked;
            byte bit = (byte)(1 << (int)kind);
            subscriptions = enabled ? (byte)(subscriptions | bit) : (byte)(subscriptions & ~bit);
            if (wasReady && !ReadyLocked)
            {
                RotateLocked();
                // The main app must stop input; "connected" alone deliberately
                // preserves its previous ready state for late BlueZ events.
                status("disconnected");
                status("connected");
            }
            else if (!wasReady && ReadyLocked)
                status("subscribed");
        }
    }

    public void Disconnect(PeerSession session)
    {
        lock (stateLock)
        {
            if (disposed || peer != session)
                return;
            ClearLocked();
        }
    }

    public void Reset()
    {
        lock (stateLock)
        {
            if (!disposed)
                ClearLocked();
        }
    }

    private void ClearLocked()
    {
        bool connected = peer.HasValue;
        RotateLocked();
        peer = null;
        subscriptions = 0;
        if (connected)
            status("disconnected");
    }

    private void RotateLocked()
    {
        epoch++;
        var old = lifetime;
        lifetime = new CancellationTokenSource();
        old.Cancel();
        old.Dispose();
    }

    public bool IsCurrent(DispatchToken token)
    {
        lock (stateLock)
            return CurrentLocked(token);
    }

    private bool CurrentLocked(DispatchToken token)
        => ReadyLocked && peer == token.Peer && epoch == token.Epoch;

    internal bool Capture(out DispatchTicket ticket)
    {
        lock (stateLock)
        {
            if (!ReadyLocked)
            {
                ticket = default;
                return false;
            }
            ticket = new DispatchTicket(new DispatchToken(peer!.Value, epoch), lifetime.Token);
            return true;
        }
    }

    internal bool Publish(DispatchToken token, Action action)
    {
        lock (stateLock)
        {
            if (!CurrentLocked(token))
                return false;
            action();
            return true;
        }
    }

    public void Dispose()
    {
        lock (stateLock)
        {
            if (disposed)
                return;
            ClearLocked();
            disposed = true;
            lifetime.Cancel();
            lifetime.Dispose();
        }
    }
}
