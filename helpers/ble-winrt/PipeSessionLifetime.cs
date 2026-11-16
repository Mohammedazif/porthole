namespace Porthole.Ble.WinRT;

/// <summary>
/// The wire format has no generation field. Once a ready connection is lost,
/// bytes already buffered in stdin cannot be assigned safely to another one.
/// A new connection therefore requires a new helper process and a fresh pipe.
/// Callers serialize this state with the adapter's lock.
/// </summary>
public sealed class PipeSessionLifetime
{
    private bool wasReady;
    public bool RestartRequired { get; private set; }

    public bool Observe(bool ready)
    {
        if (wasReady && !ready) RestartRequired = true;
        if (ready) wasReady = true;
        return RestartRequired;
    }
}
