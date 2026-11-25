using Windows.Devices.Enumeration;
using Windows.Media.Audio;

namespace Porthole.Ble.WinRT;

/// <summary>
/// Lets a paired phone play its sound through this PC, as it would through a
/// speaker (Windows 10 version 2004 and later). While enabled, the helper offers
/// itself to every paired phone that can send audio and asks each to connect,
/// retrying while the phone is out of range. Disabling closes those connections,
/// so the phone returns to its own speaker.
/// </summary>
internal sealed class PhoneAudioReceiver : IDisposable
{
    private static readonly TimeSpan RetryInterval = TimeSpan.FromSeconds(10);
    private readonly Action<string> diagnostic;
    private readonly object gate = new();
    private readonly Dictionary<string, AudioPlaybackConnection> connections = new();
    private DeviceWatcher? watcher;
    private bool enabled;

    public PhoneAudioReceiver(Action<string> diagnostic) => this.diagnostic = diagnostic;

    public void SetEnabled(bool on)
    {
        lock (gate)
        {
            if (enabled == on)
                return;
            enabled = on;
            if (on)
            {
                watcher = DeviceInformation.CreateWatcher(AudioPlaybackConnection.GetDeviceSelector());
                watcher.Added += OnAdded;
                watcher.Removed += OnRemoved;
                watcher.Start();
            }
            else
            {
                StopLocked();
            }
        }
        diagnostic(on ? "Phone audio: this PC is the phone's speaker." : "Phone audio: back on the phone.");
    }

    private void OnAdded(DeviceWatcher source, DeviceInformation device) => _ = KeepOpenAsync(source, device.Id);

    private void OnRemoved(DeviceWatcher source, DeviceInformationUpdate update)
    {
        lock (gate)
        {
            if (watcher == source && connections.Remove(update.Id, out var connection))
                connection.Dispose();
        }
    }

    private async Task KeepOpenAsync(DeviceWatcher source, string id)
    {
        AudioPlaybackConnection? connection;
        lock (gate)
        {
            if (!enabled || watcher != source || connections.ContainsKey(id))
                return;
            connection = AudioPlaybackConnection.TryCreateFromId(id);
            if (connection is null)
                return;
            connections[id] = connection;
        }
        try
        {
            await connection.StartAsync();
            while (true)
            {
                lock (gate)
                {
                    if (!enabled || !connections.TryGetValue(id, out var current) || current != connection)
                        return;
                }
                if (connection.State != AudioPlaybackConnectionState.Opened)
                {
                    var result = await connection.OpenAsync();
                    if (result.Status != AudioPlaybackConnectionOpenResultStatus.Success)
                        diagnostic($"Phone audio not connected yet ({result.Status}).");
                }
                await Task.Delay(RetryInterval);
            }
        }
        catch (Exception error) when (error is ObjectDisposedException or InvalidOperationException
                                      or System.Runtime.InteropServices.COMException)
        {
            // Disabled or removed while connecting.
            diagnostic($"Phone audio connection ended: {Program.SingleLine(error.Message)}");
        }
    }

    private void StopLocked()
    {
        if (watcher is not null)
        {
            watcher.Added -= OnAdded;
            watcher.Removed -= OnRemoved;
            if (watcher.Status is DeviceWatcherStatus.Started or DeviceWatcherStatus.EnumerationCompleted)
                watcher.Stop();
            watcher = null;
        }
        foreach (var connection in connections.Values)
            connection.Dispose();
        connections.Clear();
    }

    public void Dispose()
    {
        lock (gate)
        {
            enabled = false;
            StopLocked();
        }
    }
}
