using System.Runtime.CompilerServices;

namespace Porthole.Ble.Core;

public enum ReportKind : byte { Keyboard = 1, Mouse = 2, Consumer = 3 }
public enum FrameType : byte { Keyboard = 1, Mouse = 2, Consumer = 3, Sync = 16, Interval = 17, Delay = 18 }
public sealed record ProtocolFrame(FrameType Type, byte[] Payload);

public static class FrameReader
{
    public static int ExpectedLength(byte type) => type switch
    {
        1 => 8, 2 => 7, 3 => 2, 16 => 4, 17 or 18 => 2, _ => -1
    };

    public static async IAsyncEnumerable<ProtocolFrame> ReadAsync(Stream input, Action<string> error,
        [EnumeratorCancellation] CancellationToken cancellationToken = default)
    {
        var header = new byte[2];
        while (true)
        {
            int count = await ReadExactAsync(input, header, cancellationToken).ConfigureAwait(false);
            if (count == 0)
                yield break;
            if (count != header.Length)
            {
                error("error truncated frame header");
                yield break;
            }
            var payload = new byte[header[1]];
            if (await ReadExactAsync(input, payload, cancellationToken).ConfigureAwait(false) != payload.Length)
            {
                error("error truncated frame payload");
                yield break;
            }
            if (ExpectedLength(header[0]) != payload.Length)
            {
                error($"error bad frame type {header[0]} length {payload.Length}");
                continue; // consume the declared payload before looking for the next header
            }
            yield return new ProtocolFrame((FrameType)header[0], payload);
        }
    }

    private static async ValueTask<int> ReadExactAsync(Stream input, Memory<byte> destination,
        CancellationToken cancellationToken)
    {
        int count = 0;
        while (count < destination.Length)
        {
            int read = await input.ReadAsync(destination[count..], cancellationToken).AsTask()
                                  .WaitAsync(cancellationToken).ConfigureAwait(false);
            if (read == 0)
                break;
            count += read;
        }
        return count;
    }
}

public readonly record struct PeerSession(long Id, string ClientId);
public readonly record struct DispatchToken(PeerSession Peer, long Epoch);
internal readonly record struct DispatchTicket(DispatchToken Token, CancellationToken Lifetime);

public interface IReportNotifier
{
    // Target only token.Peer's subscribed client. Recheck epoch/readiness before
    // the platform call and honor cancellation; never use broadcast notification.
    ValueTask<bool> NotifyAsync(DispatchToken token, ReportKind kind,
        ReadOnlyMemory<byte> payload, CancellationToken cancellationToken);
}

public interface IHelperClock
{
    long NowMilliseconds { get; }
    ValueTask DelayAsync(TimeSpan delay, CancellationToken cancellationToken);
}

public sealed class SystemHelperClock : IHelperClock
{
    public long NowMilliseconds => Environment.TickCount64;
    public ValueTask DelayAsync(TimeSpan delay, CancellationToken cancellationToken)
        => new(Task.Delay(delay, cancellationToken));
}
