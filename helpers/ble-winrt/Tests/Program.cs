using System.Buffers.Binary;
using System.Text.RegularExpressions;
using Porthole.Ble.Core;
using Porthole.Ble.WinRT;

internal static class Program
{
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
    private static byte[] Frame(byte type, params byte[] payload) => [(byte)type, (byte)payload.Length, .. payload];
    private static byte[] Mouse(byte dx) => Frame(2, 0, dx, 0, 0, 0, 0, 0);
    private static byte[] Key(byte key) => Frame(1, 0, 0, key, 0, 0, 0, 0, 0);
    private static byte[] Sync(uint id) { byte[] b = new byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(b, id); return Frame(16, b); }
    private static byte[] Join(params byte[][] frames) => frames.SelectMany(f => f).ToArray();
    private static bool Nonzero(Notification n) => n.Value.Any(v => v != 0);
    private static async Task Until(Func<bool> ready)
    {
        for (int i = 0; i < 2000; i++)
        {
            if (ready()) return;
            await Task.Delay(1);
        }
        throw new TimeoutException("Expected asynchronous test state did not arrive.");
    }

    private static Task Descriptor()
    {
        DirectoryInfo? root = new(Directory.GetCurrentDirectory());
        while (root is not null && !File.Exists(Path.Combine(root.FullName, "src/hid/hid_reports.c")))
            root = root.Parent;
        Check(root is not null, "Run the descriptor check from the repository or a subdirectory.");
        string header = File.ReadAllText(Path.Combine(root!.FullName, "src/hid/hid_reports.h"));
        string source = File.ReadAllText(Path.Combine(root.FullName, "src/hid/hid_reports.c"));
        var ids = Regex.Matches(header, @"(PH_HID_REPORT_ID_\w+)\s*=\s*(\d+)")
            .ToDictionary(m => m.Groups[1].Value, m => byte.Parse(m.Groups[2].Value));
        string body = Regex.Match(source, @"ph_hid_report_descriptor\[\]\s*=\s*\{([\s\S]*?)\};").Groups[1].Value;
        body = Regex.Replace(body, @"/\*[\s\S]*?\*/", "");
        byte[] expected = body.Split(',').Select(t => t.Trim()).Where(t => t.Length != 0)
            .Select(t => ids.TryGetValue(t, out byte value) ? value : Convert.ToByte(t, t.StartsWith("0x") ? 16 : 10)).ToArray();
        Check(expected.SequenceEqual(HidDescriptor.ReportMap.ToArray()), "C/C# HID descriptors differ.");
        Check((byte)ReportKind.Keyboard == ids["PH_HID_REPORT_ID_KEYBOARD"] && HidDescriptor.GetReportLength(ReportKind.Keyboard) == 8, "Keyboard report mismatch.");
        Check((byte)ReportKind.Mouse == ids["PH_HID_REPORT_ID_MOUSE"] && HidDescriptor.GetReportLength(ReportKind.Mouse) == 7, "Mouse report mismatch.");
        Check((byte)ReportKind.Consumer == ids["PH_HID_REPORT_ID_CONSUMER"] && HidDescriptor.GetReportLength(ReportKind.Consumer) == 2, "Consumer report mismatch.");
        return Task.CompletedTask;
    }

    private static async Task Framing()
    {
        byte[] bytes = Join(Key(4), Mouse(3), Frame(3, 0x23, 2), Sync(0x80000001), Frame(17, 15, 0), Frame(18, 20, 0));
        foreach (Stream stream in new Stream[] { new MemoryStream(bytes), new SplitStream(bytes) })
        {
            var frames = new List<ProtocolFrame>(); var errors = new List<string>();
            await foreach (var frame in FrameReader.ReadAsync(stream, errors.Add)) frames.Add(frame);
            Check(frames.Count == 6 && errors.Count == 0, "Complete/split frames were lost.");
            Check(BinaryPrimitives.ReadUInt32LittleEndian(frames[3].Payload) == 0x80000001, "Sync endian mismatch.");
        }
        var valid = new List<ProtocolFrame>(); var bad = new List<string>();
        await foreach (var frame in FrameReader.ReadAsync(new SplitStream(Join(Frame(99, 1, 2, 3), Frame(2, 1), Key(5))), bad.Add)) valid.Add(frame);
        Check(valid.Count == 1 && valid[0].Payload[2] == 5 && bad.Count == 2, "Bad lengths/types did not resynchronize.");
        foreach (byte[] truncated in new[] { new byte[] { 1 }, new byte[] { 1, 8, 0 } })
        {
            bad.Clear();
            await foreach (var unused in FrameReader.ReadAsync(new MemoryStream(truncated), bad.Add)) throw new Exception("Truncated frame accepted.");
            Check(bad.Count == 1, "Truncation was silent.");
        }
    }

    private static Task ReadinessAndPipeLifetime()
    {
        using var f = new Fixture(false);
        f.Gate.SetSubscribed(f.Peer, ReportKind.Mouse, true);
        Check(!f.Gate.IsReady, "Mouse alone marked composite ready.");
        f.Gate.SetSubscribed(f.Peer, ReportKind.Keyboard, true);
        Check(!f.Gate.IsReady, "Missing consumer marked ready.");
        f.Gate.SetSubscribed(f.Peer, ReportKind.Consumer, true);
        var old = new DispatchToken(f.Peer, f.Gate.Epoch);
        Check(f.Gate.IsCurrent(old), "Full subscriptions not ready.");
        Check(f.Gate.Connect("phone-B") is null && f.Gate.CurrentPeer == f.Peer, "Second peer replaced active client.");
        f.Gate.SetSubscribed(f.Peer, ReportKind.Mouse, false);
        Check(!f.Gate.IsCurrent(old) && !f.Gate.IsReady, "Subscription loss retained stale epoch.");
        f.Gate.SetSubscribed(f.Peer, ReportKind.Mouse, true);
        Check(f.Gate.IsReady && !f.Gate.IsCurrent(old), "Resubscribe revived old epoch.");
        f.Gate.Disconnect(f.Peer);
        var replacement = f.Gate.Connect("phone-A")!.Value;
        f.Gate.SetSubscribed(f.Peer, ReportKind.Mouse, true);
        Check(!f.Gate.IsReady && replacement.Id != f.Peer.Id, "Stale session event affected reconnect.");
        var lifetime = new PipeSessionLifetime();
        Check(!lifetime.Observe(false) && !lifetime.Observe(false) && !lifetime.Observe(true), "Initial discovery requires restart.");
        Check(lifetime.Observe(false) && lifetime.Observe(true) && lifetime.RestartRequired, "Pipe lifetime fault was not sticky.");
        return Task.CompletedTask;
    }

    private static async Task EmptyFenceAndEof()
    {
        using var f = new Fixture();
        await f.Runner().RunAsync(new MemoryStream(Sync(37)));
        Check(f.Status.Contains("sync 37"), "Ready empty fence did not acknowledge.");
        var sent = f.Notifier.Sent.ToArray();
        Check(sent.Length == 6 && sent.All(n => !Nonzero(n)), "Neutral priming/EOF releases missing.");
        Check(sent.Take(3).Select(n => n.Kind).SequenceEqual(Enum.GetValues<ReportKind>()), "Priming order mismatch.");
        Check(!f.Gate.IsReady, "EOF left session active.");
        using var keys = new Fixture();
        await keys.Runner().RunAsync(new MemoryStream(Join(Key(4), Key(0), Frame(3, 0x23, 2), Frame(3, 0, 0))));
        var reports = keys.Notifier.Sent.ToArray();
        Check(reports.Count(Nonzero) == 2 && reports.TakeLast(3).All(n => !Nonzero(n)), "Key/consumer drain or releases were lost.");
    }

    private static Task AdvertisingStartup()
    {
        var transient = new AdvertisingStartupState();
        Check(transient.Observe(AdvertisingState.Aborted, false) == AdvertisingAction.Ignored,
            "Unrequested callback changed startup.");
        transient.RequestStart();
        Check(transient.Observe(AdvertisingState.Created, false) == AdvertisingAction.Pending,
            "Created was treated as advertising.");
        Check(transient.Observe(AdvertisingState.Aborted, false) == AdvertisingAction.Pending
            && !transient.Established && !transient.Failed, "Initial Aborted(Success) was fatal or ready.");
        Check(transient.Observe(AdvertisingState.Started, false) == AdvertisingAction.Started
            && transient.Established, "Transient abort could not recover to Started.");
        Check(transient.Observe(AdvertisingState.Started, false) == AdvertisingAction.Ignored,
            "Duplicate Started emitted a second transition.");
        Check(transient.Observe(AdvertisingState.Aborted, false) == AdvertisingAction.Failed
            && transient.Observe(AdvertisingState.Started, false) == AdvertisingAction.Failed,
            "Established abort or late Started bypassed sticky failure.");

        var partial = new AdvertisingStartupState(); partial.RequestStart();
        Check(partial.Observe(AdvertisingState.Partial, false) == AdvertisingAction.Started
            && partial.Established && partial.Timeout() == AdvertisingAction.Started,
            "Partial advertising was not accepted.");
        Check(partial.Observe(AdvertisingState.Stopped, false) == AdvertisingAction.Failed,
            "Established Stopped was not fatal.");

        var neverStarted = new AdvertisingStartupState(); neverStarted.RequestStart();
        Check(neverStarted.Observe(AdvertisingState.Aborted, false) == AdvertisingAction.Pending
            && neverStarted.Observe(AdvertisingState.Aborted, false) == AdvertisingAction.Pending
            && neverStarted.Timeout() == AdvertisingAction.Failed
            && neverStarted.Observe(AdvertisingState.Started, false) == AdvertisingAction.Failed,
            "Repeated initial aborts extended readiness beyond the startup deadline.");
        foreach (AdvertisingState state in Enum.GetValues<AdvertisingState>())
        {
            var error = new AdvertisingStartupState(); error.RequestStart();
            Check(error.Observe(state, true) == AdvertisingAction.Failed && error.Failed,
                "Real Bluetooth error was not immediately fatal.");
        }
        var unexpectedStop = new AdvertisingStartupState(); unexpectedStop.RequestStart();
        Check(unexpectedStop.Observe(AdvertisingState.Stopped, false) == AdvertisingAction.Failed,
            "Pre-start Stopped was accidentally tolerated.");
        foreach (bool established in new[] { false, true })
        {
            var stopped = new AdvertisingStartupState(); stopped.RequestStart();
            if (established) stopped.Observe(AdvertisingState.Started, false);
            stopped.RequestStop();
            foreach (AdvertisingState state in Enum.GetValues<AdvertisingState>())
                Check(stopped.Observe(state, true) == AdvertisingAction.Ignored,
                    "Expected stop/cancellation callback mutated the attempt.");
            Check(stopped.Timeout() == AdvertisingAction.Ignored && !stopped.Failed,
                "Intentional shutdown created a startup failure.");
        }
        return Task.CompletedTask;
    }

    private static async Task PacingAndOrder()
    {
        using var f = new Fixture();
        await f.Runner().RunAsync(new MemoryStream(Join(Mouse(1), Frame(18, 50, 0), Frame(17, 1, 0), Mouse(2), Sync(9))));
        var moves = f.Notifier.Sent.Where(n => n.Kind == ReportKind.Mouse && Nonzero(n)).ToArray();
        Check(moves.Length == 2 && moves[0].Value[1] == 1 && moves[1].Value[1] == 2, "Mouse FIFO changed.");
        Check(moves[1].At - moves[0].At >= 65 && f.Clock.Delays.Contains(5), "Pacing/pause/minimum interval violated.");
        Check(f.Status.Contains("sync 9"), "Successful mouse fence lost.");
    }

    private static async Task IndependentLanes()
    {
        using var f = new Fixture(); using var stream = new PushStream();
        Task run = f.Runner().RunAsync(stream);
        stream.Push(Join(Mouse(1), Frame(18, 44, 1), Mouse(2), Sync(11), Key(4), Key(0)));
        await Until(() => f.Notifier.Sent.Any(n => n.Kind == ReportKind.Keyboard && Nonzero(n)));
        Check(!f.Notifier.Sent.Any(n => n.Kind == ReportKind.Mouse && n.Value[1] == 2), "Pause did not block motion lane.");
        Check(!f.Status.Contains("sync 11"), "Fence escaped pending pause.");
        await Until(() => f.Clock.HasPending(300));
        f.Clock.Advance(300);
        await Until(() => f.Status.Contains("sync 11"));
        stream.Complete(); await run;
    }

    private static async Task AwaitDispatchAndFailure()
    {
        using var f = new Fixture(); using var stream = new PushStream();
        var blocked = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        f.Notifier.BeforeSend = async (_, kind, bytes, ct) =>
        {
            if (kind == ReportKind.Mouse && bytes[1] == 7) { blocked.TrySetResult(); await release.Task.WaitAsync(ct); }
            return true;
        };
        Task run = f.Runner().RunAsync(stream);
        stream.Push(Join(Mouse(7), Sync(77)));
        await blocked.Task;
        Check(!f.Status.Contains("sync 77"), "Fence acknowledged before dispatch success.");
        release.SetResult(); await Until(() => f.Status.Contains("sync 77"));
        stream.Complete(); await run;
        using var failed = new Fixture();
        failed.Notifier.BeforeSend = (_, kind, bytes, _) => Task.FromResult(!(kind == ReportKind.Mouse && bytes[1] == 9));
        await failed.Runner().RunAsync(new MemoryStream(Join(Mouse(9), Sync(99))));
        Check(!failed.Status.Contains("sync 99"), "Failed notification was acknowledged.");
        Check(failed.Notifier.Sent.TakeLast(3).All(n => !Nonzero(n)), "Failure lost zero releases.");
    }

    private static async Task EpochIsolation()
    {
        using var f = new Fixture(); using var stream = new PushStream();
        var blocked = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        f.Notifier.BeforeSend = async (_, kind, bytes, ct) =>
        {
            if (kind == ReportKind.Mouse && bytes[1] == 1) { blocked.TrySetResult(); await Task.Delay(Timeout.Infinite, ct); }
            return true;
        };
        Task run = f.Runner().RunAsync(stream);
        byte[] old = Join(Mouse(1), Mouse(2), Sync(111));
        stream.Push(old); await blocked.Task;
        await Until(() => stream.BytesRead == old.Length); // all old bytes already parsed
        f.Gate.Disconnect(f.Peer);
        var peer = f.Gate.Connect("phone-B")!.Value; f.Subscribe(peer);
        stream.Push(Join(Mouse(3), Sync(222))); stream.Complete(); await run;
        Check(!f.Status.Contains("sync 111") && f.Status.Contains("sync 222"), "Stale/current fence isolation failed.");
        Check(f.Notifier.Sent.Where(Nonzero).All(n => n.Token.Peer == peer && n.Value[1] == 3), "Old reports crossed clients.");
        Check(f.Notifier.Sent.Count(n => n.Token.Peer == peer && !Nonzero(n)) >= 6, "New client was not primed/released.");
    }

    private static async Task BadInputAndOverflow()
    {
        using var malformed = new Fixture();
        await malformed.Runner().RunAsync(new MemoryStream(Join(Mouse(1), Frame(2, 0), Sync(91))));
        Check(!malformed.Status.Contains("sync 91") && malformed.Status.Any(s => s.Contains("bad frame")), "Malformed report falsely fenced.");
        using var unready = new Fixture(false);
        await unready.Runner().RunAsync(new MemoryStream(Mouse(1)));
        Check(unready.Notifier.Sent.IsEmpty && unready.Status.Any(s => s.Contains("before reports are ready")), "Input bypassed readiness.");
        using var overflow = new Fixture();
        overflow.Notifier.BeforeSend = async (_, kind, bytes, ct) =>
        { if (kind == ReportKind.Mouse && Nonzero(new(default, kind, bytes, 0))) await Task.Delay(Timeout.Infinite, ct); return true; };
        byte[] many = Join(Enumerable.Range(0, 100).Select(_ => Mouse(1)).Append(Sync(92)).ToArray());
        await overflow.Runner(new RunnerOptions { MotionCapacity = 2 }).RunAsync(new MemoryStream(many));
        Check(overflow.Status.Any(s => s.Contains("queue full")) && !overflow.Status.Contains("sync 92"), "Overflow silently lost work.");
    }

    private static async Task BoundedEofAndNotification()
    {
        using var f = new Fixture();
        Task run = f.Runner().RunAsync(new MemoryStream(Join(Key(4), Frame(18, 255, 255), Key(0), Mouse(2), Sync(45))));
        await Until(() => f.Clock.HasPending(65535) && f.Clock.HasPending(750));
        f.Clock.Advance(750); await run;
        Check(f.Status.Any(s => s.Contains("drain timed out")) && !f.Status.Contains("sync 45"), "EOF drain was not bounded.");
        Check(f.Notifier.Sent.TakeLast(3).All(n => !Nonzero(n)), "Timed-out drain left held input.");
        using var stalled = new Fixture();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        stalled.Notifier.BeforeSend = async (_, kind, bytes, ct) =>
        { if (kind == ReportKind.Mouse && bytes[1] == 8) { entered.TrySetResult(); await Task.Delay(Timeout.Infinite, ct); } return true; };
        run = stalled.Runner().RunAsync(new MemoryStream(Join(Mouse(8), Sync(88))));
        await entered.Task; await Until(() => stalled.Clock.HasPending(500));
        stalled.Clock.Advance(500); await run;
        Check(!stalled.Status.Contains("sync 88") && stalled.Status.Any(s => s.StartsWith("error")), "Stalled notification acknowledged or hung.");
    }

    private static async Task<int> Main()
    {
        (string Name, Func<Task> Run)[] tests =
        [
            ("descriptor matches C", Descriptor), ("split/complete/malformed framing", Framing),
            ("readiness, stale sessions and pipe lifetime", ReadinessAndPipeLifetime),
            ("advertising startup transitions", AdvertisingStartup),
            ("empty fence, priming and EOF releases", EmptyFenceAndEof), ("motion order and pacing", PacingAndOrder),
            ("independent keyboard lane", IndependentLanes), ("dispatch ACK and failure", AwaitDispatchAndFailure),
            ("queued epoch/client isolation", EpochIsolation), ("invalid input and bounded queues", BadInputAndOverflow),
            ("bounded notification and EOF drain", BoundedEofAndNotification)
        ];
        int failed = 0;
        foreach (var test in tests)
        {
            try { await test.Run().WaitAsync(TimeSpan.FromSeconds(5)); Console.WriteLine($"PASS {test.Name}"); }
            catch (Exception error) { failed++; Console.Error.WriteLine($"FAIL {test.Name}: {error}"); }
        }
        Console.WriteLine($"{tests.Length - failed}/{tests.Length} portable Windows HID core checks passed");
        return failed == 0 ? 0 : 1;
    }
}
