using System.Collections.Concurrent;
using System.Threading.Channels;
using Porthole.Ble.Core;

internal sealed class TestClock : IHelperClock
{
    private sealed record Wait(long At, int Duration, TaskCompletionSource Completion);
    private readonly object sync = new();
    private readonly List<Wait> waits = [];
    private long now;
    public long NowMilliseconds { get { lock (sync) return now; } }
    public ConcurrentQueue<int> Delays { get; } = new();
    public bool HasPending(int duration) { lock (sync) return waits.Any(w => w.Duration == duration); }

    public ValueTask DelayAsync(TimeSpan delay, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        int milliseconds = checked((int)delay.TotalMilliseconds);
        Delays.Enqueue(milliseconds);
        if (milliseconds < 100)
        {
            Advance(milliseconds);
            return ValueTask.CompletedTask;
        }
        var completion = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Wait wait;
        lock (sync)
        {
            wait = new Wait(now + milliseconds, milliseconds, completion);
            waits.Add(wait);
        }
        var registration = cancellationToken.Register(() =>
        {
            lock (sync) waits.Remove(wait);
            completion.TrySetCanceled(cancellationToken);
        });
        _ = completion.Task.ContinueWith(_ => registration.Dispose(), TaskScheduler.Default);
        return new ValueTask(completion.Task);
    }

    public void Advance(int milliseconds)
    {
        List<Wait> due;
        lock (sync)
        {
            now += milliseconds;
            due = waits.Where(w => w.At <= now).ToList();
            waits.RemoveAll(w => w.At <= now);
        }
        foreach (var wait in due)
            wait.Completion.TrySetResult();
    }
}

internal sealed class PushStream : Stream
{
    private readonly Channel<byte[]> input = Channel.CreateUnbounded<byte[]>();
    private byte[] current = [];
    private int offset;
    private long read;
    public long BytesRead => Interlocked.Read(ref read);
    public void Push(byte[] bytes) => input.Writer.TryWrite(bytes);
    public void Complete() => input.Writer.TryComplete();
    public override async ValueTask<int> ReadAsync(Memory<byte> destination, CancellationToken ct = default)
    {
        while (offset == current.Length)
        {
            try { current = await input.Reader.ReadAsync(ct); }
            catch (ChannelClosedException) { return 0; }
            offset = 0;
        }
        int count = Math.Min(destination.Length, current.Length - offset);
        current.AsMemory(offset, count).CopyTo(destination);
        offset += count;
        Interlocked.Add(ref read, count);
        return count;
    }
    public override bool CanRead => true;
    public override bool CanSeek => false;
    public override bool CanWrite => false;
    public override long Length => throw new NotSupportedException();
    public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
    public override int Read(byte[] b, int o, int n) => ReadAsync(b.AsMemory(o, n)).AsTask().GetAwaiter().GetResult();
    public override void Flush() { }
    public override long Seek(long o, SeekOrigin s) => throw new NotSupportedException();
    public override void SetLength(long n) => throw new NotSupportedException();
    public override void Write(byte[] b, int o, int n) => throw new NotSupportedException();
}

internal sealed class SplitStream(byte[] bytes) : MemoryStream(bytes)
{
    public override ValueTask<int> ReadAsync(Memory<byte> destination, CancellationToken ct = default)
        => base.ReadAsync(destination[..Math.Min(1, destination.Length)], ct);
}

internal sealed record Notification(DispatchToken Token, ReportKind Kind, byte[] Value, long At);

internal sealed class TraceNotifier(SessionGate gate, TestClock clock) : IReportNotifier
{
    public ConcurrentQueue<Notification> Sent { get; } = new();
    public Func<DispatchToken, ReportKind, byte[], CancellationToken, Task<bool>>? BeforeSend { get; set; }
    public async ValueTask<bool> NotifyAsync(DispatchToken token, ReportKind kind,
        ReadOnlyMemory<byte> payload, CancellationToken ct)
    {
        byte[] copy = payload.ToArray();
        if (BeforeSend is not null && !await BeforeSend(token, kind, copy, ct))
            return false;
        if (ct.IsCancellationRequested || !gate.IsCurrent(token))
            return false;
        Sent.Enqueue(new Notification(token, kind, copy, clock.NowMilliseconds));
        return true;
    }
}

internal sealed class Fixture : IDisposable
{
    public TestClock Clock { get; } = new();
    public ConcurrentQueue<string> Status { get; } = new();
    public SessionGate Gate { get; }
    public TraceNotifier Notifier { get; }
    public PeerSession Peer { get; }
    public Fixture(bool ready = true)
    {
        Gate = new SessionGate(Status.Enqueue);
        Peer = Gate.Connect("phone-A")!.Value;
        Notifier = new TraceNotifier(Gate, Clock);
        if (ready) Subscribe(Peer);
    }
    public void Subscribe(PeerSession peer)
    {
        foreach (var kind in Enum.GetValues<ReportKind>())
            Gate.SetSubscribed(peer, kind, true);
    }
    public ProtocolRunner Runner(RunnerOptions? options = null) => new(Gate, Notifier, Status.Enqueue, Clock, options);
    public void Dispose() => Gate.Dispose();
}
