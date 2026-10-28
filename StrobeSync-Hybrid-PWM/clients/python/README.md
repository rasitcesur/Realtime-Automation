# StrobeSync Python client

Install from the repository root:

```bash
python -m pip install -e ./clients/python
```

Usage:

```python
from strobesync import StrobeSyncClient

client = StrobeSyncClient(
    port="COM3",  # Linux: /dev/ttyUSB0; macOS: /dev/cu.usbserial-... 
    on_sensor_data=lambda i, v, m: print(i, v, m),
)
try:
    client.connect()  # Set reset_delay_sec=0 for a target without auto-reset.
    client.set_pwm_channel(channel=0, mode=1, level=75)
    client.configure_sensor(0, StrobeSyncClient.STREAM_BINARY_CONTINUOUS)
    client.trigger_realtime_sync(500)
    input("Receiving sensor data. Press Enter to stop. ")
finally:
    client.close()
```

Callbacks are executed on the receive thread. Keep them short and nonblocking.
The `latest_sensor_values` dictionary is maintained by the receive thread; the
original implementation does not lock multi-step reader operations. This client
uses a 2-second reset delay by default (use `connect(reset_delay_sec=0)` when
appropriate for an STM32 UART adapter).
