using System;
using StrobeSyncController;

string portName = args.Length > 0 ? args[0] : "COM3";
using var client = new StrobeSyncClient(portName);
client.OnSensorDataReceived += evt =>
    Console.WriteLine($"[{(evt.IsBinaryFrame ? "BINARY" : "ASCII")}] Sensor {evt.SensorId}: {evt.Value}");

try
{
    client.Connect();
    client.SetPwmChannel(channel: 0, syncBlinkMode: true, dutyCyclePercent: 75);
    client.ConfigureSensor(0, SensorStreamMode.BinaryContinuous);
    client.TriggerRealtimeSync(500);
    Console.WriteLine("Receiving telemetry. Press Enter to exit.");
    Console.ReadLine();
}
catch (Exception ex)
{
    Console.Error.WriteLine($"Serial error: {ex.Message}");
    Environment.ExitCode = 1;
}
finally
{
    client.Disconnect();
}
