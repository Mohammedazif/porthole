namespace Porthole.Ble.WinRT;

public enum AdvertisingState { Created, Stopped, Started, Aborted, Partial }
public enum AdvertisingAction { Ignored, Pending, Started, Failed }

// One advertising attempt per provider. The adapter serializes calls with its
// state lock and rejects callbacks from other providers before reaching here.
public sealed class AdvertisingStartupState
{
    private bool requested, stopping;
    public bool Established { get; private set; }
    public bool Failed { get; private set; }

    public void RequestStart()
    {
        if (requested || stopping)
            throw new InvalidOperationException("An advertising attempt cannot be restarted.");
        requested = true;
    }

    public void RequestStop() => stopping = true;

    public AdvertisingAction Observe(AdvertisingState state, bool hasError)
    {
        if (!requested || stopping) return AdvertisingAction.Ignored;
        if (Failed) return AdvertisingAction.Failed;
        if (hasError) return Fail();
        switch (state)
        {
            case AdvertisingState.Created:
                return AdvertisingAction.Pending;
            case AdvertisingState.Aborted:
                // Windows can emit Aborted(Success) before its first Started.
                // It does not establish advertising or extend the caller's deadline.
                return Established ? Fail() : AdvertisingAction.Pending;
            case AdvertisingState.Stopped:
                return Fail();
            case AdvertisingState.Started:
            case AdvertisingState.Partial:
                if (Established) return AdvertisingAction.Ignored;
                Established = true;
                return AdvertisingAction.Started;
            default:
                return Fail();
        }
    }

    public AdvertisingAction Timeout()
    {
        if (!requested || stopping) return AdvertisingAction.Ignored;
        if (Failed) return AdvertisingAction.Failed;
        return Established ? AdvertisingAction.Started : Fail();
    }

    private AdvertisingAction Fail()
    {
        Failed = true;
        return AdvertisingAction.Failed;
    }
}
