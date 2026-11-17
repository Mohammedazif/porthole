using Porthole.Ble.Core;

namespace Porthole.Ble.WinRT;

public readonly record struct RetiringReport<TTarget>(ReportKind Kind, TTarget Target);
public readonly record struct RetiringPeerRegistration<TSession>(TSession Session, long Ordinal, string PeerId, bool Active);
public readonly record struct NeutralCleanupResult(int Queued, int Succeeded, int Skipped, bool TimedOut);

// The caller captures immutable old targets and validates their live registration
// immediately before queueing. Never find a new target here after logical loss.
public static class RetiringPeerCleanup
{
    public static bool IsStillRegistered<TSession>(RetiringPeerRegistration<TSession> captured,
        IEnumerable<RetiringPeerRegistration<TSession>> current)
        => captured.Ordinal > 0 && current.Any(candidate => candidate.Active
            && candidate.Ordinal == captured.Ordinal
            && EqualityComparer<TSession>.Default.Equals(candidate.Session, captured.Session)
            && StringComparer.Ordinal.Equals(candidate.PeerId, captured.PeerId));

    public static async Task<NeutralCleanupResult> RunAsync<TTarget>(
        IReadOnlyList<RetiringReport<TTarget>> captured,
        Func<ReportKind, TTarget, ReadOnlyMemory<byte>, CancellationToken, Task<bool>?> queueNeutral,
        TimeSpan deadline)
    {
        ArgumentOutOfRangeException.ThrowIfLessThanOrEqual(deadline, TimeSpan.Zero);
        using var stop = new CancellationTokenSource();
        var pending = new List<Task<bool>>();
        int skipped = 0;
        // Start every eligible neutral report before awaiting any completion.
        // One hung keyboard notification must not suppress the mouse release.
        foreach (var report in captured)
        {
            Task<bool>? queued;
            try
            {
                queued = queueNeutral(report.Kind, report.Target,
                    new byte[HidDescriptor.GetReportLength(report.Kind)], stop.Token);
            }
            catch { queued = Task.FromResult(false); }
            if (queued is null) skipped++;
            else pending.Add(ObserveAsync(queued));
        }
        bool timedOut = false;
        try { await Task.WhenAll(pending).WaitAsync(deadline).ConfigureAwait(false); }
        catch (TimeoutException) { timedOut = true; }
        finally { stop.Cancel(); }
        return new NeutralCleanupResult(pending.Count,
            pending.Count(task => task.IsCompletedSuccessfully && task.Result), skipped, timedOut);
    }

    private static async Task<bool> ObserveAsync(Task<bool> notification)
    {
        try { return await notification.ConfigureAwait(false); }
        catch { return false; }
    }
}
