using System.Buffers.Binary;
using Porthole.Ble.Core;

internal static class PrimingTests
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
        throw new TimeoutException("Expected priming state did not arrive.");
    }

    private static byte[] Input(uint sync)
    {
        byte[] fence = new byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(fence, sync);
        return [1, 8, 0, 0, 4, 0, 0, 0, 0, 0, 2, 7, 0, 1, 0, 0, 0, 0, 0, 16, 4, .. fence];
    }

    public static async Task Run()
    {
        foreach (ReportKind failedKind in Enum.GetValues<ReportKind>())
        {
            using var failed = new Fixture();
            failed.Notifier.BeforeSend = (_, kind, bytes, _) =>
                Task.FromResult(kind != failedKind || bytes.Any(value => value != 0));
            await failed.Runner().RunAsync(new MemoryStream(Input(501)));
            Check(failed.Status.Contains($"error initial neutral {failedKind.ToString().ToLowerInvariant()} report could not be delivered"),
                "Failed priming did not identify the report kind.");
            Check(!failed.Status.Contains("sync 501") && failed.Notifier.Sent.All(report => report.Value.All(value => value == 0)),
                "Failed neutral priming allowed user input or a Sync acknowledgment.");
            Check(!failed.Gate.IsReady, "Failed priming did not finish shutdown.");
        }

        using var blocked = new Fixture(); using var stream = new PushStream();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        int firstNeutral = 0;
        blocked.Notifier.BeforeSend = async (_, kind, bytes, ct) =>
        {
            if (kind == ReportKind.Keyboard && bytes.All(value => value == 0)
                && Interlocked.Exchange(ref firstNeutral, 1) == 0)
            {
                entered.TrySetResult();
                await release.Task.WaitAsync(ct);
            }
            return true;
        };
        Task run = blocked.Runner().RunAsync(stream);
        byte[] input = Input(502); stream.Push(input);
        await entered.Task; await Until(() => stream.BytesRead == input.Length);
        Check(blocked.Notifier.Sent.IsEmpty && !blocked.Status.Contains("sync 502"),
            "A second input lane bypassed blocked first-neutral priming.");
        release.SetResult();
        await Until(() => blocked.Status.Contains("sync 502"));
        var first = blocked.Notifier.Sent.Take(3).ToArray();
        Check(first.Select(report => report.Kind).SequenceEqual(Enum.GetValues<ReportKind>())
            && first.All(report => report.Value.All(value => value == 0)),
            "User reports were delivered before all neutral priming reports succeeded.");
        stream.Complete(); await run;

        using var stalled = new Fixture(); using var pending = new PushStream();
        firstNeutral = 0;
        stalled.Notifier.BeforeSend = async (_, kind, bytes, ct) =>
        {
            if (kind == ReportKind.Keyboard && bytes.All(value => value == 0)
                && Interlocked.Exchange(ref firstNeutral, 1) == 0)
                await Task.Delay(Timeout.Infinite, ct);
            return true;
        };
        run = stalled.Runner().RunAsync(pending);
        pending.Push(Input(503));
        await Until(() => stalled.Clock.HasPending(500));
        stalled.Clock.Advance(500); await run;
        Check(stalled.Status.Contains("error initial neutral keyboard report could not be delivered")
            && !stalled.Status.Contains("sync 503") && !stalled.Gate.IsReady
            && stalled.Notifier.Sent.All(report => report.Value.All(value => value == 0)),
            "A timed-out first-neutral notification acknowledged input or hung shutdown.");
    }
}
