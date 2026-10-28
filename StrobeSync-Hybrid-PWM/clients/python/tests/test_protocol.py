import unittest
from strobesync import StrobeSyncClient


class CaptureSerial:
    is_open = True

    def __init__(self):
        self.writes = []

    def write(self, data):
        self.writes.append(data)
        return len(data)


class ClientProtocolTests(unittest.TestCase):
    def setUp(self):
        self.client = StrobeSyncClient("MOCK")
        self.fake = CaptureSerial()
        self.client._serial = self.fake

    def test_pwm_frame(self):
        self.client.set_pwm_channel(0, 1, 75)
        self.assertEqual(self.fake.writes, [b"AABEL\n"])

    def test_pwm_clamps_0_to_100(self):
        self.client.set_pwm_channel(5, 0, 120)
        self.assertEqual(self.fake.writes[0], bytes([65, 70, 65, 71, 69, 10]))

    def test_sensor_binary_stream_command(self):
        self.client.configure_sensor(0, StrobeSyncClient.STREAM_BINARY_CONTINUOUS)
        self.assertEqual(self.fake.writes, [b"AGD\n"])

    def test_warning_frame(self):
        self.client.set_warning_pin(1, True)
        self.assertEqual(self.fake.writes, [b"AM B\n".replace(b" ", b"")])

    def test_sync_base26_500ms(self):
        self.client.trigger_realtime_sync(500)
        self.assertEqual(self.fake.writes, [b"AZATG\n"])

    def test_ascii_parse(self):
        received = []
        self.client.on_sensor_data = lambda *args: received.append(args)
        self.client._parse_ascii_line("SENS:1:1023")
        self.assertEqual(self.client.latest_sensor_values[1], 1023)
        self.assertEqual(received, [(1, 1023, "ASCII")])

    def test_binary_telemetry_and_mixed_ascii_stream(self):
        received = []
        self.client.on_sensor_data = lambda *args: received.append(args)

        class StreamSerial:
            is_open = True

            def __init__(self, payload, owner):
                self.payload = payload
                self.owner = owner

            @property
            def in_waiting(self):
                return len(self.payload)

            def read(self, count):
                if not self.payload:
                    self.owner._running = False
                    return b""
                output = self.payload[:count]
                self.payload = self.payload[count:]
                return output

        # XOR for [0xAA, 0x02, 0x04, 0x00] is 0xAC.
        packet = bytes([0xAA, 2, 4, 0, 0xAC])
        self.client._serial = StreamSerial(b"SENS:0:42\n" + packet, self.client)
        self.client._running = True
        self.client._rx_loop()
        self.assertEqual(received, [(0, 42, "ASCII"), (2, 1024, "BINARY")])
        self.assertEqual(self.client.latest_sensor_values[0], 42)
        self.assertEqual(self.client.latest_sensor_values[2], 1024)

    def test_reject_invalid_pwm_channel(self):
        with self.assertRaises(ValueError):
            self.client.set_pwm_channel(6, 0, 50)

    def test_reject_invalid_sensor(self):
        with self.assertRaises(ValueError):
            self.client.configure_sensor(4, 1)


if __name__ == "__main__":
    unittest.main()
