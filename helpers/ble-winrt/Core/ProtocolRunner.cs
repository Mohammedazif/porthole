using System.Buffers.Binary;
using System.Threading.Channels;

namespace Porthole.Ble.Core;

public sealed record RunnerOptions
{
    public int MotionCapacity { get; init; } = 128;
    public int KeyCapacity { get; init; } = 128;
    public int IntervalMilliseconds { get; init; } = 15;
    public TimeSpan NotificationTimeout { get; init; } = TimeSpan.FromMilliseconds(500);
    public TimeSpan DrainTimeout { get; init; } = TimeSpan.FromMilliseconds(750);
    public TimeSpan ReleaseTimeout { get; init; } = TimeSpan.FromMilliseconds(200);
}

// Input is bounded independently from the two output lanes. Overflow is fatal:
// silently dropping a movement or release would make a later fence dishonest.
public sealed class ProtocolRunner
{
    private sealed record Envelope(ProtocolFrame Frame, DispatchTicket Ticket);
    private readonly SessionGate gate;
    private readonly IReportNotifier notifier;
    private readonly Action<string> status;
    private readonly IHelperClock clock;
    private readonly RunnerOptions options;
    private readonly object outputLock = new(), primeLock = new();
    private Task<bool>? primeTask;
    private long primeEpoch = -1, protocolFaultEpoch = -1;
    private int started;
    private CancellationToken runCancellation;
    private CancellationTokenSource? runSource;

    public ProtocolRunner(SessionGate gate, IReportNotifier notifier, Action<string> status,
        IHelperClock? clock = null, RunnerOptions? options = null)
    {
        this.gate = gate;
        this.notifier = notifier;
        this.status = status;
        this.clock = clock ?? new SystemHelperClock();
        this.options = options ?? new RunnerOptions();
        if (this.options.MotionCapacity <= 0 || this.options.KeyCapacity <= 0
            || this.options.IntervalMilliseconds < 5 || this.options.NotificationTimeout <= TimeSpan.Zero
            || this.options.DrainTimeout <= TimeSpan.Zero || this.options.ReleaseTimeout <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(options));
    }

    // Phone audio frames are settings, not reports: they apply at once, even
    // before the reports are ready, and never wait in a lane.
    public Action<bool>? PhoneAudio { get; set; }

    private void Emit(string line) { lock (outputLock) status(line); }
    private void ProtocolError(string line)
    {
        Interlocked.Exchange(ref protocolFaultEpoch, gate.Epoch);
        Emit(line);
    }

    private void Fail(string line)
    {
        ProtocolError(line);
        runSource?.Cancel();
    }

    public async Task RunAsync(Stream input, CancellationToken cancellationToken = default)
    {
        if (Interlocked.Exchange(ref started, 1) != 0)
            throw new InvalidOperationException("A protocol runner can only run once.");
        using var run = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        runCancellation = run.Token;
        runSource = run;
        var motion = Channel.CreateBounded<Envelope>(new BoundedChannelOptions(options.MotionCapacity)
        { SingleReader = true, SingleWriter = true, FullMode = BoundedChannelFullMode.Wait });
        var keys = Channel.CreateBounded<Envelope>(new BoundedChannelOptions(options.KeyCapacity)
        { SingleReader = true, SingleWriter = true, FullMode = BoundedChannelFullMode.Wait });
        Task sendMotion = SendLaneAsync(motion.Reader, true);
        Task sendKeys = SendLaneAsync(keys.Reader, false);
        try
        {
            await foreach (var frame in FrameReader.ReadAsync(input, Fail, run.Token).ConfigureAwait(false))
            {
                if (frame.Type == FrameType.PhoneAudio)
                {
                    PhoneAudio?.Invoke(frame.Payload[0] == 1);
                    continue;
                }
                if (!gate.Capture(out var ticket))
                {
                    Fail("error input received before reports are ready");
                    break;
                }
                var destination = frame.Type is FrameType.Keyboard or FrameType.Consumer ? keys : motion;
                if (!destination.Writer.TryWrite(new Envelope(frame, ticket)))
                {
                    ProtocolError(destination == keys ? "error key queue full" : "error motion queue full");
                    run.Cancel();
                    break;
                }
            }
        }
        catch (OperationCanceledException) when (run.IsCancellationRequested) { }
        catch (IOException)
        {
            ProtocolError("error reading helper input");
            run.Cancel();
        }
        finally
        {
            motion.Writer.TryComplete();
            keys.Writer.TryComplete();
            Task drained = Task.WhenAll(sendMotion, sendKeys);
            if (!await WaitBoundedAsync(drained, options.DrainTimeout).ConfigureAwait(false))
            {
                Emit("error input drain timed out");
                run.Cancel();
            }
            // Release independently of the cancelled sender token. Even malformed
            // input/overflow must not leave a held key or mouse button behind.
            await ReleaseAsync().ConfigureAwait(false);
            run.Cancel();
            gate.Reset(); // invalidates any platform operation that ignored cancellation
            runSource = null;
        }
    }

    private Task<bool> EnsurePrimedAsync(DispatchTicket ticket)
    {
        lock (primeLock)
        {
            if (primeEpoch != ticket.Token.Epoch)
            {
                primeEpoch = ticket.Token.Epoch;
                primeTask = PrimeAsync(ticket);
            }
            return primeTask!;
        }
    }

    private async Task<bool> PrimeAsync(DispatchTicket ticket)
    {
        foreach (var kind in new[] { ReportKind.Keyboard, ReportKind.Mouse, ReportKind.Consumer })
        {
            if (!await NotifyAsync(ticket, kind, new byte[HidDescriptor.GetReportLength(kind)],
                                    runCancellation).ConfigureAwait(false))
            {
                if (gate.IsCurrent(ticket.Token) && !runCancellation.IsCancellationRequested)
                    Fail($"error initial neutral {kind.ToString().ToLowerInvariant()} report could not be delivered");
                return false;
            }
            await DelayAsync(ticket, options.IntervalMilliseconds, runCancellation).ConfigureAwait(false);
        }
        return gate.IsCurrent(ticket.Token) && !runCancellation.IsCancellationRequested;
    }

    private async Task SendLaneAsync(ChannelReader<Envelope> reader, bool motion)
    {
        long epoch = -1;
        bool healthy = true;
        int interval = options.IntervalMilliseconds;
        try
        {
            await foreach (var envelope in reader.ReadAllAsync(runCancellation).ConfigureAwait(false))
            {
                var ticket = envelope.Ticket;
                if (ticket.Lifetime.IsCancellationRequested || !gate.IsCurrent(ticket.Token))
                    continue;
                if (epoch != ticket.Token.Epoch)
                {
                    epoch = ticket.Token.Epoch;
                    healthy = true;
                    interval = options.IntervalMilliseconds;
                }
                try
                {
                    if (!await EnsurePrimedAsync(ticket).ConfigureAwait(false))
                    {
                        healthy = false;
                        continue;
                    }
                    var frame = envelope.Frame;
                    if (frame.Type is FrameType.Mouse or FrameType.AbsMouse or FrameType.Keyboard
                        or FrameType.Consumer)
                    {
                        var kind = frame.Type == FrameType.AbsMouse ? ReportKind.Mouse : (ReportKind)frame.Type;
                        // A failed press must not prevent its queued zero release.
                        if ((!healthy || Volatile.Read(ref protocolFaultEpoch) == epoch)
                            && !IsZero(frame.Payload))
                            continue;
                        bool sent = await NotifyAsync(ticket, kind, frame.Payload,
                                                       runCancellation).ConfigureAwait(false);
                        healthy &= sent;
                        if (!sent && gate.IsCurrent(ticket.Token))
                            Fail($"error notifying {kind.ToString().ToLowerInvariant()} report");
                        await DelayAsync(ticket, interval, runCancellation).ConfigureAwait(false);
                    }
                    else if (frame.Type == FrameType.Interval)
                        interval = Math.Max(5, (int)BinaryPrimitives.ReadUInt16LittleEndian(frame.Payload));
                    else if (frame.Type == FrameType.Delay)
                        await DelayAsync(ticket, BinaryPrimitives.ReadUInt16LittleEndian(frame.Payload),
                                          runCancellation).ConfigureAwait(false);
                    else if (frame.Type == FrameType.Sync && healthy
                             && Volatile.Read(ref protocolFaultEpoch) != epoch
                             && !runCancellation.IsCancellationRequested && !ticket.Lifetime.IsCancellationRequested)
                    {
                        uint id = BinaryPrimitives.ReadUInt32LittleEndian(frame.Payload);
                        gate.Publish(ticket.Token, () => Emit($"sync {id}"));
                    }
                }
                catch (OperationCanceledException) when (ticket.Lifetime.IsCancellationRequested) { }
            }
        }
        catch (OperationCanceledException) when (runCancellation.IsCancellationRequested) { }
    }

    private static bool IsZero(byte[] payload)
    {
        foreach (byte value in payload)
            if (value != 0)
                return false;
        return true;
    }

    private async ValueTask DelayAsync(DispatchTicket ticket, int milliseconds, CancellationToken cancellationToken)
    {
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, ticket.Lifetime);
        await clock.DelayAsync(TimeSpan.FromMilliseconds(milliseconds), linked.Token).ConfigureAwait(false);
    }

    private async Task<bool> NotifyAsync(DispatchTicket ticket, ReportKind kind, ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        if (!gate.IsCurrent(ticket.Token) || ticket.Lifetime.IsCancellationRequested)
            return false;
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, ticket.Lifetime);
        Task<bool> notify;
        try
        {
            notify = notifier.NotifyAsync(ticket.Token, kind, payload, linked.Token).AsTask();
        }
        catch (OperationCanceledException) when (linked.IsCancellationRequested) { return false; }
        catch (Exception error) when (error is not OutOfMemoryException) { return false; }
        if (!await WaitBoundedAsync(notify, options.NotificationTimeout, linked.Token).ConfigureAwait(false))
        {
            linked.Cancel();
            return false;
        }
        try
        {
            return await notify.ConfigureAwait(false) && !linked.IsCancellationRequested
                   && gate.IsCurrent(ticket.Token);
        }
        catch (OperationCanceledException) when (linked.IsCancellationRequested) { return false; }
        catch (Exception error) when (error is not OutOfMemoryException) { return false; }
    }

    private async Task<bool> WaitBoundedAsync(Task task, TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        Task timer = clock.DelayAsync(timeout, deadline.Token).AsTask();
        Task completed = await Task.WhenAny(task, timer).ConfigureAwait(false);
        deadline.Cancel();
        return completed == task;
    }

    private async Task ReleaseAsync()
    {
        if (!gate.Capture(out var ticket))
            return;
        using var release = new CancellationTokenSource();
        async Task SendZerosAsync()
        {
            foreach (var kind in new[] { ReportKind.Keyboard, ReportKind.Mouse, ReportKind.Consumer })
            {
                await NotifyAsync(ticket, kind, new byte[HidDescriptor.GetReportLength(kind)],
                                  release.Token).ConfigureAwait(false);
            }
        }
        if (!await WaitBoundedAsync(SendZerosAsync(), options.ReleaseTimeout).ConfigureAwait(false))
            release.Cancel();
    }
}
