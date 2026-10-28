using System;
using System.IO.Ports;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace StrobeSyncController
{
    public enum SensorStreamMode : byte
    {
        Off = 0,
        AsciiContinuous = 1,
        AsciiSingleRead = 2,
        BinaryContinuous = 3,
        BinarySingleRead = 4
    }

    public readonly struct SensorTelemetryEventArgs
    {
        public byte SensorId { get; }
        public ushort Value { get; }
        public bool IsBinaryFrame { get; }
        public long TimestampTicks { get; }

        public SensorTelemetryEventArgs(byte sensorId, ushort value, bool isBinaryFrame)
        {
            SensorId = sensorId;
            Value = value;
            IsBinaryFrame = isBinaryFrame;
            TimestampTicks = DateTime.UtcNow.Ticks;
        }
    }

    /// <summary>
    /// Low-latency asynchronous UART controller and 5-byte binary frame parser
    /// for the AVR Hybrid-PWM and Sensor Task Scheduler.
    /// </summary>
    public sealed class StrobeSyncClient : IDisposable
    {
        private const byte BinarySyncHeader = 0xAA;
        private const int BinaryFrameSize = 5;
        private const byte BaseOffset = (byte)'A'; // 0x41

        private readonly SerialPort _serialPort;
        private readonly object _txLock = new object();
        private CancellationTokenSource? _cts;
        private Task? _rxTask;

        // Thread-safe latest sensor cache
        private readonly ushort[] _latestSensorValues = new ushort[4];

        public event Action<SensorTelemetryEventArgs>? OnSensorDataReceived;

        public StrobeSyncClient(string portName, int baudRate = 115200)
        {
            _serialPort = new SerialPort(portName, baudRate, Parity.None, 8, StopBits.One)
            {
                ReadBufferSize = 4096,
                WriteBufferSize = 1024,
                DtrEnable = false,
                RtsEnable = false
            };
        }

        public void Connect()
        {
            if (_serialPort.IsOpen) return;

            _serialPort.Open();
            _serialPort.DiscardInBuffer();
            _serialPort.DiscardOutBuffer();

            _cts = new CancellationTokenSource();
            _rxTask = Task.Run(() => ReceiveLoopAsync(_cts.Token));
        }

        public void Disconnect()
        {
            _cts?.Cancel();
            try { _rxTask?.Wait(500); } catch { /* Ignore cancellation exceptions */ }

            if (_serialPort.IsOpen)
            {
                _serialPort.Close();
            }
        }

        public ushort GetLatestSensorValue(int sensorIndex)
        {
            if ((uint)sensorIndex >= 4) throw new ArgumentOutOfRangeException(nameof(sensorIndex));
            return Volatile.Read(ref _latestSensorValues[sensorIndex]);
        }

        // ====================================================================
        // HARDWARE CONTROL COMMANDS
        // ====================================================================

        /// <summary>
        /// Updates PWM duty cycle and mode for a channel (0-5).
        /// </summary>
        public void SetPwmChannel(byte channel, bool syncBlinkMode, byte dutyCyclePercent)
        {
            if (channel >= 6) throw new ArgumentOutOfRangeException(nameof(channel));
            byte clampedLevel = Math.Min((byte)100, dutyCyclePercent);

            byte highNibble = (byte)((clampedLevel >> 4) & 0x0F);
            byte lowNibble  = (byte)(clampedLevel & 0x0F);

            Span<byte> frame = stackalloc byte[6]
            {
                BaseOffset,
                (byte)(BaseOffset + channel),
                (byte)(BaseOffset + (syncBlinkMode ? 1 : 0)),
                (byte)(BaseOffset + highNibble),
                (byte)(BaseOffset + lowNibble),
                (byte)'\n'
            };

            SendRawFrame(frame);
        }

        /// <summary>
        /// Configures sensor streaming mode or triggers an immediate single reading (Sensors 0-3).
        /// </summary>
        public void ConfigureSensor(byte sensorIndex, SensorStreamMode mode)
        {
            if (sensorIndex >= 4) throw new ArgumentOutOfRangeException(nameof(sensorIndex));

            Span<byte> frame = stackalloc byte[4]
            {
                BaseOffset,
                (byte)(BaseOffset + 6 + sensorIndex),
                (byte)(BaseOffset + (byte)mode),
                (byte)'\n'
            };

            SendRawFrame(frame);
        }

        /// <summary>
        /// Sets warning output pin state (0 -> A4, 1 -> A5).
        /// </summary>
        public void SetWarningPin(byte warningIndex, bool active)
        {
            if (warningIndex >= 2) throw new ArgumentOutOfRangeException(nameof(warningIndex));

            Span<byte> frame = stackalloc byte[4]
            {
                BaseOffset,
                (byte)(BaseOffset + 11 + warningIndex),
                (byte)(BaseOffset + (active ? 1 : 0)),
                (byte)'\n'
            };

            SendRawFrame(frame);
        }

        /// <summary>
        /// Initiates the real-time synchronization countdown (Command ID 25).
        /// Base-26 encoded delay up to 17,575 ms.
        /// </summary>
        public void TriggerRealtimeSync(int delayMs)
        {
            int clampedDelay = Math.Clamp(delayMs, 0, 17575);
            int p1 = clampedDelay / (26 * 26);
            int rem = clampedDelay % (26 * 26);
            int p2 = rem / 26;
            int p3 = rem % 26;

            Span<byte> frame = stackalloc byte[6]
            {
                BaseOffset,
                (byte)(BaseOffset + 25),
                (byte)(BaseOffset + p1),
                (byte)(BaseOffset + p2),
                (byte)(BaseOffset + p3),
                (byte)'\n'
            };

            SendRawFrame(frame);
        }

        private void SendRawFrame(ReadOnlySpan<byte> frame)
        {
            if (!_serialPort.IsOpen) throw new InvalidOperationException("Serial port is not open.");
            byte[] buffer = frame.ToArray();
            lock (_txLock)
            {
                _serialPort.Write(buffer, 0, buffer.Length);
            }
        }

        // ====================================================================
        // ASYNCHRONOUS BINARY (0xAA) & ASCII STATE MACHINE PARSER
        // ====================================================================

        private async Task ReceiveLoopAsync(CancellationToken ct)
        {
            byte[] readChunk = new byte[256];
            byte[] ringBuffer = new byte[1024];
            int head = 0;
            int tail = 0;

            StringBuilder asciiBuilder = new StringBuilder(32);

            while (!ct.IsCancellationRequested && _serialPort.IsOpen)
            {
                try
                {
                    int bytesRead = await _serialPort.BaseStream.ReadAsync(readChunk, 0, readChunk.Length, ct).ConfigureAwait(false);
                    if (bytesRead <= 0) continue;

                    for (int i = 0; i < bytesRead; i++)
                    {
                        ringBuffer[head] = readChunk[i];
                        head = (head + 1) & 1023;
                    }

                    // Process buffered bytes
                    while (tail != head)
                    {
                        int available = (head - tail) & 1023;
                        byte firstByte = ringBuffer[tail];

                        // 1. Check for 5-Byte Binary Frame (0xAA)
                        if (firstByte == BinarySyncHeader)
                        {
                            if (available < BinaryFrameSize)
                                break; // Wait for full 5-byte frame to arrive

                            byte sensorId = ringBuffer[(tail + 1) & 1023];
                            byte valHigh  = ringBuffer[(tail + 2) & 1023];
                            byte valLow   = ringBuffer[(tail + 3) & 1023];
                            byte checksum = ringBuffer[(tail + 4) & 1023];

                            byte expectedCrc = (byte)(BinarySyncHeader ^ sensorId ^ valHigh ^ valLow);

                            if (checksum == expectedCrc && sensorId < 4)
                            {
                                ushort value = (ushort)((valHigh << 8) | valLow);
                                Volatile.Write(ref _latestSensorValues[sensorId], value);
                                OnSensorDataReceived?.Invoke(new SensorTelemetryEventArgs(sensorId, value, true));

                                tail = (tail + BinaryFrameSize) & 1023;
                            }
                            else
                            {
                                // Corrupted byte or false header match: advance by 1 byte to resync
                                tail = (tail + 1) & 1023;
                            }
                        }
                        // 2. Process ASCII Telemetry Character
                        else
                        {
                            tail = (tail + 1) & 1023;
                            char ch = (char)firstByte;

                            if (ch == '\n')
                            {
                                if (asciiBuilder.Length > 0)
                                {
                                    ParseAsciiTelemetry(asciiBuilder.ToString());
                                    asciiBuilder.Clear();
                                }
                            }
                            else if (ch != '\r')
                            {
                                if (asciiBuilder.Length < 32)
                                    asciiBuilder.Append(ch);
                                else
                                    asciiBuilder.Clear();
                            }
                        }
                    }
                }
                catch (OperationCanceledException)
                {
                    break;
                }
                catch (Exception)
                {
                    if (ct.IsCancellationRequested) break;
                }
            }
        }

        private void ParseAsciiTelemetry(string line)
        {
            // Expected format: "SENS:<id>:<val>"
            if (!line.StartsWith("SENS:", StringComparison.Ordinal)) return;

            string[] parts = line.Split(':');
            if (parts.Length == 3 &&
                byte.TryParse(parts[1], out byte sensorId) &&
                sensorId < 4 &&
                ushort.TryParse(parts[2], out ushort value))
            {
                Volatile.Write(ref _latestSensorValues[sensorId], value);
                OnSensorDataReceived?.Invoke(new SensorTelemetryEventArgs(sensorId, value, false));
            }
        }

        public void Dispose()
        {
            Disconnect();
            _serialPort.Dispose();
            _cts?.Dispose();
        }
    }
}