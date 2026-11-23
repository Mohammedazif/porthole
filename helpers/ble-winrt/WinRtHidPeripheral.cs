using System.Diagnostics;
using Porthole.Ble.Core;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Devices.Enumeration;
using Windows.Devices.Radios;
using Windows.Storage.Streams;

namespace Porthole.Ble.WinRT;

/// <summary>One native Windows HOGP service. Windows owns pairing, CCCDs and radio timing.</summary>
internal sealed class WinRtHidPeripheral : IReportNotifier, IAsyncDisposable
{
    private readonly object stateLock = new();
    private readonly PipeSessionLifetime pipeLifetime = new();
    private readonly AdvertisingStartupState advertising = new();
    private readonly Stopwatch advertisingClock = new();
    private int advertisingAttempt;
    private readonly SessionGate gate;
    private readonly Action<string> status;
    private readonly Action<string> log;
    private readonly Dictionary<ReportKind, GattLocalCharacteristic> reports = new();
    private readonly Dictionary<ReportKind, byte[]> lastReports = new();
    private readonly Dictionary<ReportKind, GattSubscribedClient> targets = new();
    private readonly Dictionary<ReportKind, CapturedTarget> selectedTargets = new();
    // Raw subscriptions can arrive before SessionStatus becomes Active. Keep
    // their observations for the provider lifetime, independently of input readiness.
    private readonly RawSessionRegistry<GattSession> rawSessions;
    private readonly Dictionary<GattSession, PeerSession> sessionPeers = new();
    private readonly Dictionary<GattSession, Windows.Foundation.TypedEventHandler<GattSession, GattSessionStatusChangedEventArgs>> sessionCallbacks = new();
    private readonly List<GattLocalCharacteristic> readable = new();
    private readonly List<GattLocalCharacteristic> writable = new();
    private readonly TaskCompletionSource<Exception> failure = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly TaskCompletionSource started = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private sealed record CapturedTarget(PeerSession Peer, GattLocalCharacteristic Characteristic,
        GattSubscribedClient Client, GattSession Session, long Ordinal);
    private bool closing, retiring, everReady;
    private readonly HashSet<(GattSession Session, long Ordinal)> closedObservations = new();
    private Task? retirement;
    private Task? disposal;
    private GattServiceProvider? provider;
    private PeerSession? activePeer;
    private byte[] keyboardLeds = [0];
    private bool disposed;
    private bool advertisingReported;

    public WinRtHidPeripheral(SessionGate gate, Action<string> status, Action<string> log)
    {
        this.gate = gate;
        this.status = status;
        this.log = log;
        rawSessions = new RawSessionRegistry<GattSession>(ObserveSession, ForgetSession);
        foreach (ReportKind kind in Enum.GetValues<ReportKind>())
            lastReports[kind] = new byte[HidDescriptor.GetReportLength(kind)];
    }

    public Task<Exception> Failure => failure.Task;
    private static Guid Uuid(ushort id) => new($"0000{id:x4}-0000-1000-8000-00805f9b34fb");

    public async Task InitializeAsync(CancellationToken ct)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(ct);
        deadline.CancelAfter(TimeSpan.FromSeconds(15));
        BluetoothAdapter? adapter = await BluetoothAdapter.GetDefaultAsync().AsTask(deadline.Token);
        if (adapter is null)
            throw new InvalidOperationException("No Windows Bluetooth adapter was found. Enable or attach a Bluetooth adapter, then run --probe.");
        if (!adapter.IsLowEnergySupported)
            throw new InvalidOperationException("This Bluetooth adapter does not support Bluetooth Low Energy. Use a BLE adapter with Windows peripheral-role support.");
        if (!adapter.IsPeripheralRoleSupported)
            throw new InvalidOperationException("The Windows Bluetooth driver does not support the BLE peripheral role. Update its driver or use a peripheral-capable adapter; Wi-Fi viewing still works.");
        Radio? radio = await adapter.GetRadioAsync().AsTask(deadline.Token);
        if (radio is null || radio.State != RadioState.On)
            throw new InvalidOperationException("The Windows Bluetooth radio is off or unavailable. Turn Bluetooth on in Windows Settings, then run --probe.");
        DeviceAccessStatus access = DeviceAccessInformation.CreateFromId(adapter.DeviceId).CurrentStatus;
        if (access is DeviceAccessStatus.DeniedBySystem or DeviceAccessStatus.DeniedByUser)
            throw new UnauthorizedAccessException("Windows denied this desktop app access to the Bluetooth adapter.");
        log($"BLE peripheral role supported; adapter '{Program.SingleLine(radio.Name)}'.");
        log($"Pair this PC's Windows Bluetooth name (normally '{Program.SingleLine(Environment.MachineName)}') from the iPhone's Bluetooth settings. Windows controls the advertised name and pairing UI.");
        GattServiceProviderResult result = await GattServiceProvider.CreateAsync(Uuid(0x1812)).AsTask(deadline.Token);
        if (result.Error != BluetoothError.Success || result.ServiceProvider is null)
            throw BluetoothFailure("create HID service 1812", result.Error);
        provider = result.ServiceProvider;
        provider.AdvertisementStatusChanged += AdvertisementChanged;

        await CreateStaticAsync(0x2a4a, [0x11, 0x01, 0, 0x02], deadline.Token); // HID 1.11; normally connectable.
        await CreateStaticAsync(0x2a4b, HidDescriptor.ReportMap.ToArray(), deadline.Token);
        foreach (ReportKind kind in Enum.GetValues<ReportKind>())
        {
            GattLocalCharacteristic report = await CreateCharacteristicAsync(0x2a4d,
                GattCharacteristicProperties.Read | GattCharacteristicProperties.Notify, deadline.Token);
            reports.Add(kind, report);
            report.ReadRequested += ReadReport;
            readable.Add(report);
            // Windows automatically supplies the CCCD for a Notify characteristic.
            await CreateReportReferenceAsync(report, (byte)kind, 1, deadline.Token);
            report.SubscribedClientsChanged += SubscribersChanged;
        }
        GattLocalCharacteristic output = await CreateCharacteristicAsync(0x2a4d,
            GattCharacteristicProperties.Read | GattCharacteristicProperties.Write |
            GattCharacteristicProperties.WriteWithoutResponse, deadline.Token);
        output.ReadRequested += ReadKeyboardOutput;
        output.WriteRequested += WriteKeyboardOutput;
        readable.Add(output);
        writable.Add(output);
        await CreateReportReferenceAsync(output, (byte)ReportKind.Keyboard, 2, deadline.Token);

        GattLocalCharacteristic protocol = await CreateCharacteristicAsync(0x2a4e,
            GattCharacteristicProperties.Read | GattCharacteristicProperties.WriteWithoutResponse, deadline.Token);
        protocol.ReadRequested += ReadProtocol;
        protocol.WriteRequested += WriteProtocol;
        readable.Add(protocol);
        writable.Add(protocol);
        GattLocalCharacteristic control = await CreateCharacteristicAsync(0x2a4c,
            GattCharacteristicProperties.WriteWithoutResponse, deadline.Token);
        control.WriteRequested += WriteControl;
        writable.Add(control);
        log("HID1812 created: encrypted keyboard/mouse/consumer reports, report map, report references, LED output, protocol mode and control point. No reserved GAP/GATT/Device Information services are registered.");
    }

    private async Task<GattLocalCharacteristic> CreateCharacteristicAsync(ushort id, GattCharacteristicProperties properties, CancellationToken ct)
    {
        var parameters = new GattLocalCharacteristicParameters
        {
            CharacteristicProperties = properties,
            ReadProtectionLevel = GattProtectionLevel.EncryptionRequired,
            WriteProtectionLevel = GattProtectionLevel.EncryptionRequired
        };
        GattLocalCharacteristicResult result = await provider!.Service.CreateCharacteristicAsync(Uuid(id), parameters).AsTask(ct);
        if (result.Error != BluetoothError.Success || result.Characteristic is null)
            throw BluetoothFailure($"create HID characteristic {id:x4}", result.Error);
        return result.Characteristic;
    }

    private async Task CreateStaticAsync(ushort id, byte[] bytes, CancellationToken ct)
    {
        var parameters = new GattLocalCharacteristicParameters
        {
            CharacteristicProperties = GattCharacteristicProperties.Read,
            ReadProtectionLevel = GattProtectionLevel.EncryptionRequired,
            StaticValue = Buffer(bytes)
        };
        GattLocalCharacteristicResult result = await provider!.Service.CreateCharacteristicAsync(Uuid(id), parameters).AsTask(ct);
        if (result.Error != BluetoothError.Success)
            throw BluetoothFailure($"create HID characteristic {id:x4}", result.Error);
    }

    private static async Task CreateReportReferenceAsync(GattLocalCharacteristic report, byte id, byte type, CancellationToken ct)
    {
        var parameters = new GattLocalDescriptorParameters
        {
            ReadProtectionLevel = GattProtectionLevel.EncryptionRequired,
            StaticValue = Buffer([id, type])
        };
        GattLocalDescriptorResult result = await report.CreateDescriptorAsync(Uuid(0x2908), parameters).AsTask(ct);
        if (result.Error != BluetoothError.Success)
            throw BluetoothFailure($"create Report Reference for report {id}", result.Error);
    }

    public async Task StartAsync(CancellationToken ct)
    {
        lock (stateLock)
        {
            ObjectDisposedException.ThrowIf(disposed, this);
            advertising.RequestStart();
            advertisingClock.Restart();
            advertisingAttempt++;
            log($"HID advertising attempt {advertisingAttempt}: connectable=true discoverable=true, initial provider state={provider!.AdvertisementStatus}.");
        }
        if (failure.Task.IsCompleted) throw await failure.Task;
        provider!.StartAdvertising(new GattServiceProviderAdvertisingParameters
        {
            IsConnectable = true,
            IsDiscoverable = true
        });
        if (failure.Task.IsCompleted) throw await failure.Task;
        Task finished = await Task.WhenAny(started.Task, failure.Task, Task.Delay(TimeSpan.FromSeconds(12), ct));
        ct.ThrowIfCancellationRequested();
        // When a fatal callback and Started race, failure always wins.
        if (failure.Task.IsCompleted) throw await failure.Task;
        if (finished != started.Task)
        {
            lock (stateLock)
            {
                if (advertising.Timeout() == AdvertisingAction.Failed)
                {
                    log($"HID advertising attempt {advertisingAttempt} timed out at {advertisingClock.ElapsedMilliseconds}ms; provider state={provider.AdvertisementStatus}.");
                    failure.TrySetResult(new TimeoutException(
                        "Windows did not start HID advertising within 12 seconds. Check Bluetooth advertising/discoverability policy, close other peripheral publishers and run --probe-advertising."));
                }
            }
            if (failure.Task.IsCompleted) throw await failure.Task;
        }
        await started.Task;
        if (failure.Task.IsCompleted) throw await failure.Task;
    }

    public async Task WaitForStableAdvertisingAsync(TimeSpan duration, CancellationToken ct)
    {
        if (failure.Task.IsCompleted) throw await failure.Task;
        await Task.WhenAny(failure.Task, Task.Delay(duration, ct));
        ct.ThrowIfCancellationRequested();
        if (failure.Task.IsCompleted) throw await failure.Task;
        lock (stateLock)
        {
            if (!advertising.Established || advertising.Failed || provider!.AdvertisementStatus is not
                (GattServiceProviderAdvertisementStatus.Started or GattServiceProviderAdvertisementStatus.StartedWithoutAllAdvertisementData))
                throw new InvalidOperationException("HID advertising did not remain active during the diagnostic check.");
            log($"HID advertising attempt {advertisingAttempt} stable for {duration.TotalSeconds:0.0}s; provider state={provider!.AdvertisementStatus}.");
        }
        if (failure.Task.IsCompleted) throw await failure.Task;
    }

    private void AdvertisementChanged(GattServiceProvider sender, GattServiceProviderAdvertisementStatusChangedEventArgs args)
    {
        lock (stateLock)
        {
            if (disposed || sender != provider) return;
            log($"HID advertising attempt {advertisingAttempt} +{advertisingClock.ElapsedMilliseconds}ms: reported={args.Status}, error={args.Error}, provider={sender.AdvertisementStatus}.");
            AdvertisingState observed = args.Status switch
            {
                GattServiceProviderAdvertisementStatus.Created => AdvertisingState.Created,
                GattServiceProviderAdvertisementStatus.Stopped => AdvertisingState.Stopped,
                GattServiceProviderAdvertisementStatus.Started => AdvertisingState.Started,
                GattServiceProviderAdvertisementStatus.Aborted => AdvertisingState.Aborted,
                GattServiceProviderAdvertisementStatus.StartedWithoutAllAdvertisementData => AdvertisingState.Partial,
                _ => AdvertisingState.Aborted
            };
            AdvertisingAction action = advertising.Observe(observed, args.Error != BluetoothError.Success);
            if (action == AdvertisingAction.Started)
            {
                if (!advertisingReported)
                {
                    advertisingReported = true;
                    status("advertising");
                }
                // A bonded client may already have restored CCCDs by the time
                // advertising starts, without another explicit CCCD write.
                try { ReconcileLocked("advertising started"); }
                catch (Exception e)
                {
                    log("Cannot inspect initial HID subscriptions: " + e.Message);
                    DropPeerLocked();
                    failure.TrySetResult(new InvalidOperationException("Cannot inspect HID subscriptions after advertising started.", e));
                }
                started.TrySetResult();
            }
            else if (action == AdvertisingAction.Failed)
            {
                DropPeerLocked();
                failure.TrySetResult(args.Error == BluetoothError.Success
                    ? new InvalidOperationException("Windows stopped HID advertising. Check Bluetooth advertising/discoverability policy and run --probe-advertising again.")
                    : BluetoothFailure("advertise the HID service", args.Error));
            }
            else if (action == AdvertisingAction.Pending && observed == AdvertisingState.Aborted)
            {
                log("Initial Aborted/Success notification is pending; waiting for Started within the 12-second startup deadline.");
            }
        }
    }

    private static Exception BluetoothFailure(string operation, BluetoothError error)
    {
        string action = error switch
        {
            BluetoothError.DisabledByPolicy => "Windows policy or a reserved service blocked publishing. Check Bluetooth permissions and administrator policy.",
            BluetoothError.RadioNotAvailable => "Turn Bluetooth on and check the adapter driver.",
            BluetoothError.ResourceInUse => "Close the other Bluetooth peripheral publisher and try again.",
            BluetoothError.NotSupported => "This Windows adapter/driver does not support local GATT publishing.",
            _ => "Check Bluetooth Settings and run porthole-ble-winrt --probe."
        };
        return new InvalidOperationException($"Cannot {operation}: {error}. {action}");
    }

    private void SubscribersChanged(GattLocalCharacteristic sender, object args)
    {
        try
        {
            lock (stateLock)
            {
                if (!disposed)
                {
                    ReportKind kind = reports.FirstOrDefault(p => p.Value == sender).Key;
                    ReconcileLocked($"CCCD {kind}");
                }
            }
        }
        catch (Exception e) { log("Cannot inspect HID subscriptions: " + e.Message); FailConnection(); }
    }

    private void ObserveSession(GattSession session, long ordinal)
    {
        Windows.Foundation.TypedEventHandler<GattSession, GattSessionStatusChangedEventArgs> callback =
            (sender, args) => SessionChanged(sender, args, ordinal);
        sessionCallbacks.Add(session, callback);
        session.SessionStatusChanged += callback;
        log($"Observing HID session s{ordinal}: state={session.SessionStatus}, ATT MTU={session.MaxPduSize}.");
    }

    private void ForgetSession(GattSession session, long ordinal)
    {
        if (sessionCallbacks.Remove(session, out var callback)) session.SessionStatusChanged -= callback;
        sessionPeers.Remove(session);
        closedObservations.Remove((session, ordinal));
        log($"Stopped observing HID session s{ordinal}.");
    }

    private void ReconcileLocked(string reason = "refresh")
    {
        long previousEpoch = gate.Epoch;
        var rawClients = new Dictionary<ReportKind, List<GattSubscribedClient>>();
        foreach (ReportKind kind in Enum.GetValues<ReportKind>())
            rawClients[kind] = reports.TryGetValue(kind, out GattLocalCharacteristic? characteristic)
                ? characteristic.SubscribedClients.ToList() : [];

        // Register the raw sessions FIRST. Filtering here used to discard the
        // only event source that could tell us when an initial session activates.
        rawSessions.Synchronize(rawClients.Values.SelectMany(c => c).Select(c => c.Session));
        if (closing || retiring || pipeLifetime.RestartRequired) return;
        if (everReady && activePeer is PeerSession ended && gate.CurrentPeer != ended)
        {
            DropPeerLocked(); // The runner ended; do not bind replacement targets during disposal.
            return;
        }

        var clients = new Dictionary<ReportKind, List<GattSubscribedClient>>();
        var ids = new HashSet<string>(StringComparer.Ordinal);
        var summaries = new List<string>();
        var missing = new List<string>();
        foreach ((ReportKind kind, List<GattSubscribedClient> raw) in rawClients)
        {
            var current = raw.Where(c => c.Session.SessionStatus == GattSessionStatus.Active).ToList();
            clients[kind] = current;
            foreach (GattSubscribedClient client in current) ids.Add(client.Session.DeviceId.Id);
            if (current.Count != 1) missing.Add(kind.ToString().ToLowerInvariant());
            string sessions = string.Join(",", raw.Select(c =>
                rawSessions.TryGetOrdinal(c.Session, out long ordinal)
                    ? $"s{ordinal}:{c.Session.SessionStatus}"
                    : "unobserved"));
            summaries.Add($"{kind.ToString().ToLowerInvariant()} raw={raw.Count} active={current.Count} sessions=[{sessions}]");
        }
        log($"HID subscription snapshot ({reason}): {string.Join("; ", summaries)}; active_peers={ids.Count}; missing={(missing.Count == 0 ? "none" : string.Join(",", missing))}; suspended={gate.HostSuspended}.");
        if (gate.IsReady && selectedTargets.Any(entry =>
            !rawSessions.IsCurrent(entry.Value.Session, entry.Value.Ordinal)
            || closedObservations.Contains((entry.Value.Session, entry.Value.Ordinal))
            || (clients[entry.Key].Count == 1 && clients[entry.Key][0].Session != entry.Value.Session)))
        {
            log("Ready HID target registration changed; retiring the original input generation.");
            DropPeerLocked();
            failure.TrySetResult(new InvalidOperationException("Bluetooth input registration changed; restarting with a fresh input pipe."));
            return;
        }
        if (ids.Count != 1)
        {
            if (ids.Count > 1) log("Multiple Bluetooth peers subscribed; input paused until exactly one phone remains.");
            DropPeerLocked();
            return;
        }
        string id = ids.Single();
        if (activePeer is null || activePeer.Value.ClientId != id)
        {
            DropPeerLocked();
            if (pipeLifetime.RestartRequired) return;
            activePeer = gate.Connect(id);
            ClearValuesLocked();
        }
        if (activePeer is not PeerSession peer) return;
        if (gate.IsReady && clients.Values.Any(subscribed => subscribed.Count != 1))
            BeginRetirementLocked();
        targets.Clear();
        selectedTargets.Clear();
        sessionPeers.Clear();
        foreach ((ReportKind kind, List<GattSubscribedClient> subscribed) in clients)
        {
            // Duplicate or partial subscription snapshots must never broadcast reports.
            bool enabled = subscribed.Count == 1;
            if (enabled)
            {
                GattSubscribedClient client = subscribed[0];
                GattSession session = client.Session;
                if (!rawSessions.TryGetOrdinal(session, out long ordinal)) continue;
                targets[kind] = client;
                selectedTargets[kind] = new CapturedTarget(peer, reports[kind], client, session, ordinal);
                sessionPeers[session] = peer;
            }
        }
        // Publish readiness only after every target is validated. Updating one
        // report at a time can turn two incomplete snapshots into a transient
        // all-ready state and start input before the final snapshot is usable.
        gate.SetSubscriptions(peer,
            targets.ContainsKey(ReportKind.Keyboard),
            targets.ContainsKey(ReportKind.Mouse),
            targets.ContainsKey(ReportKind.Consumer));
        if (gate.Epoch != previousEpoch) ClearValuesLocked();
        CheckPipeLifetimeLocked();
    }

    private void CheckPipeLifetimeLocked()
    {
        everReady |= gate.IsReady;
        if (!disposed && pipeLifetime.Observe(gate.IsReady))
        {
            targets.Clear();
            ClearValuesLocked();
            failure.TrySetResult(new InvalidOperationException(
                "Bluetooth input connection changed; restarting the helper with a fresh input pipe before reconnecting."));
        }
    }

    private void SessionChanged(GattSession sender, GattSessionStatusChangedEventArgs args, long ordinal)
    {
        try
        {
            lock (stateLock)
            {
                // An already-queued callback from a removed registration must not
                // affect the same COM session if it is observed again later.
                if (disposed || !rawSessions.IsCurrent(sender, ordinal)) return;
                bool present = reports.Values.Any(report => report.SubscribedClients.Any(c => c.Session == sender));
                if (!present)
                {
                    ReconcileLocked($"stale session s{ordinal}");
                    return;
                }
                GattSessionStatus current = sender.SessionStatus;
                log($"HID session s{ordinal} transition: reported={args.Status}, current={current}.");
                if (args.Status == GattSessionStatus.Closed && current == GattSessionStatus.Closed)
                {
                    closedObservations.Add((sender, ordinal));
                    if (sessionPeers.TryGetValue(sender, out PeerSession peer) && peer == activePeer)
                    {
                        DropPeerLocked();
                        log($"HID session s{ordinal} closed; queued input and cached reports cleared.");
                    }
                }
                // In particular, Active must refresh the CCCDs that may have been
                // received before the session reached its active state.
                ReconcileLocked($"session s{ordinal} {args.Status}");
            }
        }
        catch (Exception e)
        {
            log("HID session callback: " + e.Message);
            FailConnection();
        }
    }

    private void ClearValuesLocked()
    {
        foreach (byte[] bytes in lastReports.Values) Array.Clear(bytes);
        keyboardLeds[0] = 0;
    }

    private void DropPeerLocked()
    {
        BeginRetirementLocked();
        if (activePeer is PeerSession peer) gate.Disconnect(peer);
        activePeer = null;
        targets.Clear();
        selectedTargets.Clear();
        ClearValuesLocked();
        sessionPeers.Clear();
        CheckPipeLifetimeLocked();
    }

    private void FailConnection()
    {
        lock (stateLock) DropPeerLocked();
    }

    public async ValueTask<bool> NotifyAsync(DispatchToken token, ReportKind kind, ReadOnlyMemory<byte> payload, CancellationToken ct)
    {
        CapturedTarget target;
        lock (stateLock)
        {
            if (disposed || closing || retiring || pipeLifetime.RestartRequired || !gate.IsCurrent(token) || activePeer != token.Peer ||
                !selectedTargets.TryGetValue(kind, out target!) || target.Peer != token.Peer ||
                !IsCapturedTargetRegisteredLocked(target) || target.Client.MaxNotificationSize < payload.Length)
                return false;
        }
        ct.ThrowIfCancellationRequested();
        // Target the captured peer, never the all-subscribers overload. A reconnect cannot
        // redirect an already started WinRT operation to a different phone.
        GattClientNotificationResult result;
        try
        {
            Task<GattClientNotificationResult> notification;
            lock (stateLock)
            {
                if (disposed || closing || retiring || pipeLifetime.RestartRequired || !gate.IsCurrent(token)
                    || activePeer != token.Peer || !IsCapturedTargetRegisteredLocked(target))
                    return false;
                ct.ThrowIfCancellationRequested();
                // Serialize the native operation's start with session invalidation,
                // but release the lock before awaiting the OS completion.
                notification = target.Characteristic.NotifyValueAsync(Buffer(payload.ToArray()), target.Client).AsTask(ct);
            }
            result = await notification;
        }
        catch (OperationCanceledException) { throw; }
        catch (Exception e)
        {
            lock (stateLock)
            {
                if (activePeer == token.Peer && gate.IsCurrent(token)) DropPeerLocked();
            }
            log($"{kind} notification failed: {e.Message}");
            return false;
        }
        lock (stateLock)
        {
            if (closing || retiring || pipeLifetime.RestartRequired || !gate.IsCurrent(token)
                || activePeer != token.Peer || !IsCapturedTargetRegisteredLocked(target)) return false;
            if (result.Status != GattCommunicationStatus.Success)
            {
                log($"{kind} notification failed: {result.Status}; input session reset.");
                DropPeerLocked();
                return false;
            }
            lastReports[kind] = payload.ToArray();
            return true;
        }
    }

    private bool IsSelectedSessionLocked(GattSession session) =>
        !disposed && !closing && !retiring && !pipeLifetime.RestartRequired
        && activePeer is PeerSession peer && gate.CurrentPeer == peer
        && sessionPeers.TryGetValue(session, out PeerSession selected) && selected == peer
        && rawSessions.TryGetOrdinal(session, out long ordinal)
        && !closedObservations.Contains((session, ordinal))
        && session.SessionStatus == GattSessionStatus.Active;

    private async void ReadReport(GattLocalCharacteristic sender, GattReadRequestedEventArgs args) =>
        await RespondReadAsync(args, () =>
        {
            ReportKind kind = reports.FirstOrDefault(p => p.Value == sender).Key;
            if (!lastReports.TryGetValue(kind, out byte[]? bytes)) return [];
            // Startup reads are valid before subscription. Only the current
            // recipient may read live input cached by this provider.
            return gate.IsReady && IsSelectedSessionLocked(args.Session)
                ? bytes : new byte[bytes.Length];
        });

    private async void ReadKeyboardOutput(GattLocalCharacteristic sender, GattReadRequestedEventArgs args) =>
        await RespondReadAsync(args, () => IsSelectedSessionLocked(args.Session) ? keyboardLeds : [0]);

    private async void ReadProtocol(GattLocalCharacteristic sender, GattReadRequestedEventArgs args) =>
        await RespondReadAsync(args, () => [1]); // Descriptor declares Report protocol only.

    private async Task RespondReadAsync(GattReadRequestedEventArgs args, Func<byte[]> getValue)
    {
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(4));
        var deferral = args.GetDeferral();
        try
        {
            GattReadRequest? request = await args.GetRequestAsync().AsTask(deadline.Token);
            if (request is null) return;
            lock (stateLock)
            {
                byte[] value = getValue();
                if (request.Offset > value.Length) request.RespondWithProtocolError(0x07); // Invalid Offset.
                else request.RespondWithValue(Buffer(value[(int)request.Offset..]));
            }
        }
        catch (Exception e) { Program.Diagnostic("HID read request: " + e.Message); }
        finally { deferral.Complete(); }
    }

    private async void WriteKeyboardOutput(GattLocalCharacteristic sender, GattWriteRequestedEventArgs args) =>
        await RespondWriteAsync(args, value =>
        {
            lock (stateLock)
            {
                if (!IsSelectedSessionLocked(args.Session)) return false;
                keyboardLeds[0] = value;
                return true;
            }
        });

    private static async void WriteProtocol(GattLocalCharacteristic sender, GattWriteRequestedEventArgs args) =>
        await RespondWriteAsync(args, value => value == 1);

    private async void WriteControl(GattLocalCharacteristic sender, GattWriteRequestedEventArgs args) =>
        await RespondWriteAsync(args, value =>
        {
            if (value > 1) return false;
            lock (stateLock)
            {
                if (!IsSelectedSessionLocked(args.Session)) return false;
                gate.SetHostSuspended(value == 0);
                // HOGP Suspend signals the host's power mode. It does not remove
                // its CCCDs or change the identity of this ready input session.
                // This desktop keeps operating while retaining the connection.
                log($"HID control point {(value == 0 ? "Suspend" : "Exit Suspend")}: host_suspended={gate.HostSuspended}; preserving report subscriptions, dispatch epoch and current input state.");
            }
            return true;
        });

    private static async Task RespondWriteAsync(GattWriteRequestedEventArgs args, Func<byte, bool> accept)
    {
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(4));
        var deferral = args.GetDeferral();
        try
        {
            GattWriteRequest? request = await args.GetRequestAsync().AsTask(deadline.Token);
            if (request is null) return;
            byte? error = null;
            if (request.Offset != 0) error = 0x07;
            else if (request.Value.Length != 1) error = 0x0d; // Invalid Attribute Value Length.
            else
            {
                using DataReader reader = DataReader.FromBuffer(request.Value);
                if (!accept(reader.ReadByte())) error = 0x13; // Value Not Allowed.
            }
            if (request.Option == GattWriteOption.WriteWithResponse)
            {
                if (error is byte code) request.RespondWithProtocolError(code);
                else request.Respond();
            }
        }
        catch (Exception e) { Program.Diagnostic("HID write request: " + e.Message); }
        finally { deferral.Complete(); }
    }

    private static IBuffer Buffer(byte[] bytes)
    {
        using var writer = new DataWriter();
        writer.WriteBytes(bytes);
        return writer.DetachBuffer();
    }

    private void BeginRetirementLocked()
    {
        if (retiring || !everReady || activePeer is not PeerSession peer || targets.Count == 0) return;
        var captured = new List<RetiringReport<CapturedTarget>>();
        foreach ((ReportKind kind, CapturedTarget target) in selectedTargets)
            captured.Add(new RetiringReport<CapturedTarget>(kind, target));
        // Serialize this stop with normal NotifyValueAsync starts, then enqueue
        // zeros after all earlier operations. Microsoft's sample relies on
        // per-client FIFO; the API reference does not formally guarantee delivery
        // ordering, so portable tests cannot prove the remote phone's final state.
        retiring = true;
        retirement = RetireAsync(captured);
    }

    private bool IsCapturedTargetRegisteredLocked(CapturedTarget target)
        => rawSessions.IsCurrent(target.Session, target.Ordinal)
            && !closedObservations.Contains((target.Session, target.Ordinal))
            && target.Session.SessionStatus == GattSessionStatus.Active
            && target.Client.Session == target.Session
            && target.Client.Session.DeviceId.Id == target.Peer.ClientId
            && RetiringPeerCleanup.IsStillRegistered(
                new RetiringPeerRegistration<GattSession>(target.Session, target.Ordinal, target.Peer.ClientId, true),
                target.Characteristic.SubscribedClients.Select(client =>
                {
                    GattSession session = client.Session;
                    rawSessions.TryGetOrdinal(session, out long ordinal);
                    return new RetiringPeerRegistration<GattSession>(session, ordinal,
                        session.DeviceId.Id, session.SessionStatus == GattSessionStatus.Active);
                }));

    private Task<bool>? QueueRetiringNeutral(ReportKind kind, CapturedTarget target,
        ReadOnlyMemory<byte> value, CancellationToken _)
    {
        lock (stateLock)
        {
            if (disposed || !retiring || value.Length != HidDescriptor.GetReportLength(kind)
                || value.Span.IndexOfAnyExcept((byte)0) >= 0 || !IsCapturedTargetRegisteredLocked(target))
                return null;
            // This is exclusively a neutral payload for the captured client.
            // It deliberately cannot obtain another peer from the current map.
            // The coordinator bounds waiting. Do not retract an already queued
            // release or synchronously cancel a WinRT operation on that deadline.
            return CompleteRetiringNeutralAsync(target.Characteristic.NotifyValueAsync(
                Buffer(value.ToArray()), target.Client).AsTask());
        }
    }

    private static async Task<bool> CompleteRetiringNeutralAsync(Task<GattClientNotificationResult> notification)
    {
        GattClientNotificationResult result = await notification.ConfigureAwait(false);
        return result.Status == GattCommunicationStatus.Success;
    }

    private async Task RetireAsync(IReadOnlyList<RetiringReport<CapturedTarget>> captured)
    {
        NeutralCleanupResult result = await RetiringPeerCleanup.RunAsync(captured,
            QueueRetiringNeutral, TimeSpan.FromMilliseconds(200)).ConfigureAwait(false);
        log($"Retiring peer neutral cleanup: queued={result.Queued}, succeeded={result.Succeeded}, skipped={result.Skipped}, timed_out={result.TimedOut}.");
    }

    public ValueTask DisposeAsync()
    {
        lock (stateLock)
        {
            closing = true; // Block readiness/normal sends before any asynchronous cleanup.
            disposal ??= DisposeCoreAsync();
            return new ValueTask(disposal);
        }
    }

    private async Task DisposeCoreAsync()
    {
        Task cleanup;
        lock (stateLock)
        {
            if (disposed) return;
            advertising.RequestStop();
            DropPeerLocked();
            cleanup = retirement ?? Task.CompletedTask;
        }
        // The cleanup deadline is independent of stdin/app cancellation. Keep
        // raw session observers alive until then so stale generations stay stale.
        await cleanup.ConfigureAwait(false);
        lock (stateLock)
        {
            disposed = true;
            rawSessions.Dispose();
            if (provider is not null)
            {
                provider.AdvertisementStatusChanged -= AdvertisementChanged;
                try { provider.StopAdvertising(); }
                catch (Exception e) { log("Stopping HID advertisement: " + e.Message); }
            }
            foreach (GattLocalCharacteristic report in reports.Values)
            {
                report.SubscribedClientsChanged -= SubscribersChanged;
                report.ReadRequested -= ReadReport;
            }
            foreach (GattLocalCharacteristic item in readable)
            {
                item.ReadRequested -= ReadKeyboardOutput;
                item.ReadRequested -= ReadProtocol;
            }
            foreach (GattLocalCharacteristic item in writable)
            {
                item.WriteRequested -= WriteKeyboardOutput;
                item.WriteRequested -= WriteProtocol;
                item.WriteRequested -= WriteControl;
            }
            reports.Clear();
            readable.Clear();
            writable.Clear();
            provider = null;
        }
    }
}
