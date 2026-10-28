import serial
import threading
import time
from typing import Callable, Optional, Dict


class StrobeSyncClient:
    """
    Asynchronous UART client for the AVR Hybrid-PWM & Sensor Task Scheduler.
    Supports both 5-byte deterministic binary frames (0xAA) and legacy ASCII frames.
    """

    BINARY_HEADER = 0xAA
    BINARY_FRAME_SIZE = 5
    BASE_OFFSET = ord('A')  # 0x41 (65) - Safe printable ASCII offset

    # Sensor Stream Modes
    STREAM_OFF = 0
    STREAM_ASCII_CONTINUOUS = 1
    STREAM_ASCII_SINGLE = 2
    STREAM_BINARY_CONTINUOUS = 3
    STREAM_BINARY_SINGLE = 4

    def __init__(
        self,
        port: str,
        baudrate: int = 115200,
        on_sensor_data: Optional[Callable[[int, int, str], None]] = None
    ):
        self.port = port
        self.baudrate = baudrate
        self.on_sensor_data = on_sensor_data

        self._serial: Optional[serial.Serial] = None
        self._running = False
        self._rx_thread: Optional[threading.Thread] = None
        self._tx_lock = threading.Lock()

        # Latest thread-safe sensor values cache
        self.latest_sensor_values: Dict[int, int] = {0: 0, 1: 0, 2: 0, 3: 0}

    def connect(self, reset_delay_sec: float = 2.0) -> None:
        """Opens the serial port and starts the background parser thread."""
        self._serial = serial.Serial(
            port=self.port,
            baudrate=self.baudrate,
            timeout=0.01  # Non-blocking short read timeout
        )
        # Wait for Arduino DTR auto-reset if applicable
        if reset_delay_sec > 0:
            time.sleep(reset_delay_sec)

        self._serial.reset_input_buffer()
        self._running = True
        self._rx_thread = threading.Thread(target=self._rx_loop, daemon=True)
        self._rx_thread.start()

    def close(self) -> None:
        """Stops the parser thread and closes the serial connection."""
        self._running = False
        if self._rx_thread and self._rx_thread.is_alive():
            self._rx_thread.join(timeout=1.0)
        if self._serial and self._serial.is_open:
            self._serial.close()

    def _send_raw_frame(self, payload: bytes) -> None:
        """Thread-safe transmission of a newline-terminated command frame."""
        if not self._serial or not self._serial.is_open:
            raise RuntimeError("Serial port is not open.")
        with self._tx_lock:
            self._serial.write(payload + b'\n')

    # ========================================================================
    # HARDWARE CONTROL COMMANDS
    # ========================================================================

    def set_pwm_channel(self, channel: int, mode: int, level: int) -> None:
        """
        Configures a PWM channel (0-5).
        :param channel: Channel index (0 to 5)
        :param mode: 0 = Continuous, 1 = Synchronized Blink
        :param level: Duty cycle percentage (0 to 100)
        """
        if not (0 <= channel <= 5):
            raise ValueError("Channel must be between 0 and 5.")
        level = max(0, min(100, level))
        ModeBit = 1 if mode > 0 else 0

        high_nibble = (level >> 4) & 0x0F
        low_nibble = level & 0x0F

        b = self.BASE_OFFSET
        frame = bytes([
            b,
            b + channel,
            b + ModeBit,
            b + high_nibble,
            b + low_nibble
        ])
        self._send_raw_frame(frame)

    def configure_sensor(self, sensor_idx: int, stream_mode: int) -> None:
        """
        Configures sensor telemetry mode or requests a single immediate snapshot.
        :param sensor_idx: Sensor index (0 to 3)
        :param stream_mode: 0=Off, 1=ASCII Stream, 2=ASCII Single, 3=Bin Stream, 4=Bin Single
        """
        if not (0 <= sensor_idx <= 3):
            raise ValueError("Sensor index must be between 0 and 3.")
        b = self.BASE_OFFSET
        cmd_target = 6 + sensor_idx
        frame = bytes([b, b + cmd_target, b + stream_mode])
        self._send_raw_frame(frame)

    def set_warning_pin(self, warn_idx: int, state: bool) -> None:
        """
        Controls warning output pins (0 -> A4, 1 -> A5).
        """
        if warn_idx not in (0, 1):
            raise ValueError("Warning index must be 0 or 1.")
        b = self.BASE_OFFSET
        cmd_target = 11 + warn_idx
        frame = bytes([b, b + cmd_target, b + (1 if state else 0)])
        self._send_raw_frame(frame)

    def trigger_realtime_sync(self, delay_ms: int) -> None:
        """
        Triggers the master real-time synchronization countdown (Command ID 25).
        Encodes delay_ms in base-26 format: p3 + p2*26 + p1*676 (Max: 17,575 ms).
        """
        delay_ms = max(0, min(17575, delay_ms))
        p1 = delay_ms // (26 * 26)
        rem = delay_ms % (26 * 26)
        p2 = rem // 26
        p3 = rem % 26

        b = self.BASE_OFFSET
        frame = bytes([b, b + 25, b + p1, b + p2, b + p3])
        self._send_raw_frame(frame)

    # ========================================================================
    # ASYNCHRONOUS HYBRID STREAM PARSER (BINARY 0xAA + ASCII)
    # ========================================================================

    def _rx_loop(self) -> None:
        buffer = bytearray()
        ascii_buffer = bytearray()

        while self._running and self._serial and self._serial.is_open:
            try:
                waiting = self._serial.in_waiting
                chunk = self._serial.read(waiting if waiting > 0 else 1)
                if not chunk:
                    continue

                buffer.extend(chunk)

                # Hybrid State Machine: Scan buffer for 5-byte Binary Frames or ASCII lines
                while len(buffer) > 0:
                    # Case 1: Potential Binary Frame Header (0xAA)
                    if buffer[0] == self.BINARY_HEADER:
                        if len(buffer) < self.BINARY_FRAME_SIZE:
                            break  # Wait for remaining bytes of the 5-byte packet

                        sensor_id = buffer[1]
                        val_high = buffer[2]
                        val_low = buffer[3]
                        checksum = buffer[4]

                        expected_crc = self.BINARY_HEADER ^ sensor_id ^ val_high ^ val_low

                        if checksum == expected_crc and sensor_id < 4:
                            value = (val_high << 8) | val_low
                            self.latest_sensor_values[sensor_id] = value
                            if self.on_sensor_data:
                                self.on_sensor_data(sensor_id, value, "BINARY")
                            del buffer[:self.BINARY_FRAME_SIZE]
                        else:
                            # False 0xAA header or corrupted byte: shift by 1 to re-sync
                            del buffer[0]

                    # Case 2: ASCII Stream Character ("SENS:0:1023\n")
                    else:
                        byte_val = buffer.pop(0)
                        if byte_val == ord('\n'):
                            if ascii_buffer:
                                self._parse_ascii_line(ascii_buffer.decode('ascii', errors='ignore'))
                                ascii_buffer.clear()
                        elif byte_val != ord('\r'):
                            ascii_buffer.append(byte_val)
                            if len(ascii_buffer) > 32:
                                ascii_buffer.clear()  # Overflow guard

            except Exception as e:
                if self._running:
                    print(f"[UART RX Error]: {e}")

    def _parse_ascii_line(self, line: str) -> None:
        if line.startswith("SENS:"):
            parts = line.split(":")
            if len(parts) == 3:
                try:
                    sensor_id = int(parts[1])
                    value = int(parts[2])
                    self.latest_sensor_values[sensor_id] = value
                    if self.on_sensor_data:
                        self.on_sensor_data(sensor_id, value, "ASCII")
                except ValueError:
                    pass


# ============================================================================
# USAGE EXAMPLE
# ============================================================================
if __name__ == "__main__":
    def handle_telemetry(sensor_id: int, value: int, mode: str):
        print(f"[{mode}] Sensor {sensor_id} -> {value}")

    client = StrobeSyncClient(port="COM3", on_sensor_data=handle_telemetry)
    # client.connect()
    # client.set_pwm_channel(channel=0, mode=1, level=75)
    # client.configure_sensor(sensor_idx=0, stream_mode=StrobeSyncClient.STREAM_BINARY_CONTINUOUS)
    # client.trigger_realtime_sync(delay_ms=500)
