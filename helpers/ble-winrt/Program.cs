using System.Runtime.InteropServices;
using System.Reflection;
using System.Text;
using Porthole.Ble.Core;

namespace Porthole.Ble.WinRT;

internal static class Program
{
    private static readonly object OutputLock = new();

    private static void Status(string line)
    {
        lock (OutputLock)
        {
            Console.Out.WriteLine(SingleLine(line));
            Console.Out.Flush();
        }
    }

    internal static void Diagnostic(string line)
    {
        lock (OutputLock)
        {
            Console.Error.WriteLine("[porthole-winrt] " + SingleLine(line));
            Console.Error.Flush();
        }
    }

    internal static string SingleLine(string line) =>
        new(line.Select(c => char.IsControl(c) ? ' ' : c).ToArray());

    private static async Task<int> Main(string[] args)
    {
        Console.OutputEncoding = new UTF8Encoding(false);
        if (args.Length == 1 && args[0] == "--descriptor")
        {
            Console.WriteLine(Convert.ToHexString(HidDescriptor.ReportMap.Span).ToLowerInvariant());
            return 0;
        }
        if (args.Length == 1 && args[0] is "--help" or "-h")
        {
            Console.WriteLine("porthole-ble-winrt [--probe | --probe-advertising | --descriptor]");
            Console.WriteLine("Without arguments, reads Porthole binary input frames from stdin.");
            Console.WriteLine("--probe checks the local adapter and creates the HID service without advertising.");
            Console.WriteLine("--probe-advertising additionally advertises HID for two seconds; the PC can briefly be discovered.");
            Console.WriteLine("Pair the PC's Windows Bluetooth name; this helper does not rename the computer.");
            return 0;
        }
        bool probe = args.Length == 1 && args[0] == "--probe";
        bool probeAdvertising = args.Length == 1 && args[0] == "--probe-advertising";
        if (args.Length != 0 && !probe && !probeAdvertising)
        {
            Status("error invalid arguments; use --help");
            return 2;
        }
        if (!OperatingSystem.IsWindowsVersionAtLeast(10, 0, 19041))
        {
            Status("error Windows 10 version 2004 or later is required for the WinRT Bluetooth helper");
            return 1;
        }

        using var shutdown = new CancellationTokenSource();
        string build = typeof(Program).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "unknown";
        Diagnostic($"Helper build {build}; PID {Environment.ProcessId}; .NET {Environment.Version}.");
        ConsoleCancelEventHandler onCancel = (_, e) => { e.Cancel = true; shutdown.Cancel(); };
        Console.CancelKeyPress += onCancel;
        using var gate = new SessionGate(Status);
        await using var peripheral = new WinRtHidPeripheral(gate, Status, Diagnostic);
        try
        {
            await peripheral.InitializeAsync(shutdown.Token);
            if (probe)
            {
                Status("probe ok peripheral=true hid_service=1812");
                return 0;
            }
            Status("ready");
            await peripheral.StartAsync(shutdown.Token);
            if (probeAdvertising)
            {
                await peripheral.WaitForStableAdvertisingAsync(TimeSpan.FromSeconds(2), shutdown.Token);
                if (peripheral.Failure.IsCompleted) throw await peripheral.Failure;
                Status("probe ok advertising=true stable_seconds=2 hid_service=1812");
                return 0;
            }
            using Stream input = Console.OpenStandardInput();
            var runner = new ProtocolRunner(gate, peripheral, Status);
            Task running = runner.RunAsync(input, shutdown.Token);
            if (!peripheral.Failure.IsCompleted)
                await Task.WhenAny(running, peripheral.Failure);
            if (peripheral.Failure.IsCompleted)
            {
                shutdown.Cancel();
                // A broken native pipe must not prevent this helper from terminating.
                try { await running.WaitAsync(TimeSpan.FromSeconds(3)); }
                catch (Exception e) when (e is OperationCanceledException or TimeoutException) { }
                throw await peripheral.Failure;
            }
            await running;
            if (peripheral.Failure.IsCompleted) throw await peripheral.Failure;
            return 0;
        }
        catch (OperationCanceledException) when (shutdown.IsCancellationRequested)
        {
            return 0;
        }
        catch (Exception e)
        {
            Status("error " + DescribeFailure(e));
            Diagnostic($"{e.GetType().Name} HRESULT=0x{e.HResult:x8}: {e.Message}");
            return 1;
        }
        finally
        {
            shutdown.Cancel();
            Console.CancelKeyPress -= onCancel;
        }
    }

    private static string DescribeFailure(Exception error)
    {
        if (error is OperationCanceledException)
            return "Windows Bluetooth did not respond in time. Check that Bluetooth is on, update the adapter driver and run --probe again.";
        if (error is UnauthorizedAccessException || error.HResult == unchecked((int)0x80070005))
            return "Bluetooth access was denied. Enable Bluetooth access in Windows Privacy settings and allow the Windows pairing prompt; run --probe again.";
        if (error is COMException && error.HResult == unchecked((int)0x80070422))
            return "The Windows Bluetooth service is disabled. Enable Bluetooth Support Service and the Bluetooth radio, then run --probe again.";
        return SingleLine(error.Message);
    }
}
