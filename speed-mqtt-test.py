import time
import statistics
import threading
import uuid

import paho.mqtt.client as mqtt


BROKER = "10.21.72.3"
PORT = 1883

CLIENT_ID = f"mqtt-speed-{uuid.uuid4().hex[:8]}"
TOPIC = f"benchmark/mqtt-speed/{CLIENT_ID}"

N = 10_000
RATE = 1000.0
INTERVAL = 1.0 / RATE


sent = {}
latencies_ms = []

connected = threading.Event()
subscribed = threading.Event()


def on_connect(client, userdata, flags, reason_code, properties):
    print("Connected:", reason_code)

    client.subscribe(
        TOPIC,
        qos=0
    )

    connected.set()


def on_subscribe(client, userdata, mid, reason_codes, properties):
    subscribed.set()


def on_message(client, userdata, msg):
    try:
        seq = int(msg.payload.decode())

        t0 = sent.pop(seq, None)

        if t0 is not None:
            dt_ms = (
                time.perf_counter() - t0
            ) * 1000.0

            latencies_ms.append(dt_ms)

    except Exception as e:
        print("Decode error:", e)


client = mqtt.Client(
    mqtt.CallbackAPIVersion.VERSION2,
    client_id=CLIENT_ID
)

client.on_connect = on_connect
client.on_subscribe = on_subscribe
client.on_message = on_message


print(f"Connecting to {BROKER}:{PORT} ...")

client.connect(
    BROKER,
    PORT,
    keepalive=30
)

client.loop_start()


if not connected.wait(timeout=5):
    raise RuntimeError("MQTT connection timeout")

if not subscribed.wait(timeout=5):
    raise RuntimeError("MQTT subscription timeout")


print()
print(f"Sending {N} messages at approximately {RATE:.0f} msg/s...")
print(f"Topic: {TOPIC}")
print()


start = time.perf_counter()

for i in range(N):

    target = start + i * INTERVAL

    while True:
        now = time.perf_counter()
        remaining = target - now

        if remaining <= 0:
            break

        if remaining > 0.002:
            time.sleep(remaining - 0.001)

    sent[i] = time.perf_counter()

    info = client.publish(
        TOPIC,
        str(i),
        qos=0,
        retain=False
    )

    if info.rc != mqtt.MQTT_ERR_SUCCESS:
        print(
            f"Publish error at {i}: "
            f"{mqtt.error_string(info.rc)}"
        )


send_finished = time.perf_counter()

# Give delayed packets time to return
time.sleep(3)

test_finished = time.perf_counter()


client.loop_stop()
client.disconnect()


received = len(latencies_ms)
lost = N - received

send_elapsed = send_finished - start
total_elapsed = test_finished - start

actual_rate = N / send_elapsed


print()
print("===== MQTT 1000 msg/s BENCHMARK =====")
print(f"Broker       : {BROKER}:{PORT}")
print(f"Sent         : {N}")
print(f"Received     : {received}")
print(f"Lost         : {lost}")
print(f"Completeness : {100 * received / N:.2f}%")
print(f"Send time    : {send_elapsed:.3f} s")
print(f"Actual rate  : {actual_rate:.1f} msg/s")
print(f"Total time   : {total_elapsed:.3f} s")


if latencies_ms:

    latencies_ms.sort()

    def percentile(p):
        index = int(
            (p / 100.0) * len(latencies_ms)
        )

        index = min(
            max(index, 0),
            len(latencies_ms) - 1
        )

        return latencies_ms[index]

    print()
    print(f"RTT mean     : {statistics.mean(latencies_ms):.3f} ms")
    print(f"RTT median   : {statistics.median(latencies_ms):.3f} ms")
    print(f"RTT P95      : {percentile(95):.3f} ms")
    print(f"RTT P99      : {percentile(99):.3f} ms")
    print(f"RTT min      : {min(latencies_ms):.3f} ms")
    print(f"RTT max      : {max(latencies_ms):.3f} ms")