# StrobeSync C# client

Requires the .NET 8 SDK and access to NuGet to restore `System.IO.Ports`.

```bash
dotnet build clients/csharp/StrobeSyncController/StrobeSyncController.csproj
```

Sample console app:

```bash
dotnet run --project examples/csharp/StrobeSyncConsole/StrobeSyncConsole.csproj -- COM3
```

The sample defaults to `COM3`, uses the device's UART at 115200 8N1,
and must be connected to the appropriate board/USB-UART adapter.
The firmware may reset when a port is opened through some Arduino adapters.
Sensor callbacks run on the receive task; keep them nonblocking.
