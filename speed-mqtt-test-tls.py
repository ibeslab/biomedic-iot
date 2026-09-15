import os
import ssl
import time
import statistics
import threading
import uuid

import paho.mqtt.client as mqtt


# ============================================================
# Broker configuration
# ============================================================

BROKER = "4a48091aa0ac475d93f3f294b735ba3d.s1.eu.hivemq.cloud"
PORT = 8883

MQTT_USER = "Device01"
MQTT_PASS = "Device01"

if not MQTT_USER or not MQTT_PASS:
    raise RuntimeError(
        "Set MQTT_USER and MQTT_PASS environment variables first."
    )


# ============================================================
# Benchmark configuration
# ============================================================

# Start with these for a fair comparison with the other brokers.
N = 1000
RATE = 100.0

# Later, try:
# N = 10_000
# RATE = 1000.0

INTERVAL = 1.0 / RATE

QOS = 0
DRAIN_TIMEOUT = 20.0


RUN_ID = uuid.uuid4().hex[:8]

TOPIC = f"benchmark/mqtt-speed/{RUN_ID}"


# ============================================================
# Shared benchmark state
# ============================================================

pub_connected = threading.Event()
sub_connected = threading.Event()
subscribed = threading.Event()
all_received = threading.Event()

lock = threading.Lock()

sent = {}
latencies_ms = []

publish_errors = 0


# ============================================================
# Common MQTT configuration
# ============================================================

def configure_client(client):

    client.username_pw_set(
        MQTT_USER,
        MQTT_PASS
    )

    tls_context = ssl.create_default_context()

    client.tls_set_context(
        tls_context
    )


# ============================================================
# Publisher callbacks
# ============================================================

def on_pub_connect(
    client,
    userdata,
    flags,
    reason_code,
    properties
):

    print(
        "Publisher connected:",
        reason_code
    )

    if reason_code == 0:
        pub_connected.set()


# ============================================================
# Subscriber callbacks
# ============================================================

def on_sub_connect(
    client,
    userdata,
    flags,
    reason_code,
    properties
):

    print(
        "Subscriber connected:",
        reason_code
    )

    if reason_code == 0:

        sub_connected.set()

        client.subscribe(
            TOPIC,
            qos=QOS
        )


def on_subscribe(
    client,
    userdata,
    mid,
    reason_codes,
    properties
):

    print(
        "Subscribed:",
        TOPIC
    )

    subscribed.set()


def on_message(
    client,
    userdata,
    msg
):

    try:

        seq = int(
            msg.payload.decode()
        )

        now = time.perf_counter()

        with lock:

            t0 = sent.pop(
                seq,
                None
            )

            if t0 is not None:

                dt_ms = (
                    now - t0
                ) * 1000.0

                latencies_ms.append(
                    dt_ms
                )

                if len(latencies_ms) == N:
                    all_received.set()

    except Exception as e:

        print(
            "Decode error:",
            e
        )


# ============================================================
# Create publisher
# ============================================================

pub = mqtt.Client(
    mqtt.CallbackAPIVersion.VERSION2,
    client_id=f"mqtt-speed-pub-{RUN_ID}"
)

pub.on_connect = on_pub_connect

configure_client(pub)


# ============================================================
# Create subscriber
# ============================================================

sub = mqtt.Client(
    mqtt.CallbackAPIVersion.VERSION2,
    client_id=f"mqtt-speed-sub-{RUN_ID}"
)

sub.on_connect = on_sub_connect
sub.on_subscribe = on_subscribe
sub.on_message = on_message

configure_client(sub)


# ============================================================
# Connect subscriber first
# ============================================================

print(
    f"Connecting to {BROKER}:{PORT} using TLS ..."
)

print(
    f"Topic: {TOPIC}"
)


sub.connect(
    BROKER,
    PORT,
    keepalive=30
)

sub.loop_start()


if not sub_connected.wait(timeout=10):

    sub.loop_stop()

    raise RuntimeError(
        "Subscriber connection timeout "
        "or authentication failed"
    )


if not subscribed.wait(timeout=10):

    sub.loop_stop()

    raise RuntimeError(
        "MQTT subscription timeout"
    )


# ============================================================
# Connect publisher
# ============================================================

pub.connect(
    BROKER,
    PORT,
    keepalive=30
)

pub.loop_start()


if not pub_connected.wait(timeout=10):

    pub.loop_stop()
    sub.loop_stop()

    raise RuntimeError(
        "Publisher connection timeout "
        "or authentication failed"
    )


# ============================================================
# Benchmark
# ============================================================

print()

print(
    f"Sending {N} messages "
    f"at approximately {RATE:.0f} msg/s..."
)

print()


start = time.perf_counter()


for i in range(N):

    target = (
        start
        + i * INTERVAL
    )

    delay = (
        target
        - time.perf_counter()
    )

    # Important:
    # yield CPU time to Paho network threads.
    if delay > 0:

        time.sleep(
            delay
        )


    with lock:

        sent[i] = (
            time.perf_counter()
        )


    info = pub.publish(
        TOPIC,
        str(i),
        qos=QOS,
        retain=False
    )


    if info.rc != mqtt.MQTT_ERR_SUCCESS:

        with lock:

            sent.pop(
                i,
                None
            )

        publish_errors += 1

        print(
            f"Publish error at {i}: "
            f"{mqtt.error_string(info.rc)}"
        )


send_finished = time.perf_counter()


# ============================================================
# Drain pending traffic
# ============================================================

all_received.wait(
    timeout=DRAIN_TIMEOUT
)

test_finished = time.perf_counter()


# ============================================================
# Shutdown
# ============================================================

pub.loop_stop()
sub.loop_stop()

pub.disconnect()
sub.disconnect()


# ============================================================
# Results
# ============================================================

with lock:

    received = len(
        latencies_ms
    )

    pending = len(
        sent
    )

    samples = sorted(
        latencies_ms
    )


send_elapsed = (
    send_finished - start
)

total_elapsed = (
    test_finished - start
)

actual_rate = (
    N / send_elapsed
)


print()

print(
    "===== MQTT TLS BENCHMARK ====="
)

print(
    f"Broker          : "
    f"{BROKER}:{PORT}"
)

print(
    "TLS             : enabled"
)

print(
    f"QoS             : {QOS}"
)

print(
    f"Target rate     : "
    f"{RATE:.1f} msg/s"
)

print(
    f"Actual rate     : "
    f"{actual_rate:.1f} msg/s"
)

print(
    f"Sent attempts   : {N}"
)

print(
    f"Publish errors  : "
    f"{publish_errors}"
)

print(
    f"Received        : "
    f"{received}"
)

print(
    f"Pending/timeout : "
    f"{pending}"
)

print(
    f"Completeness    : "
    f"{100.0 * received / N:.2f}%"
)

print(
    f"Send time       : "
    f"{send_elapsed:.3f} s"
)

print(
    f"Total time      : "
    f"{total_elapsed:.3f} s"
)


# ============================================================
# Latency statistics
# ============================================================

if samples:

    def percentile(p):

        rank = (
            p / 100.0
        ) * (
            len(samples) - 1
        )

        lo = int(rank)

        hi = min(
            lo + 1,
            len(samples) - 1
        )

        frac = (
            rank - lo
        )

        return (
            samples[lo]
            * (1.0 - frac)
            + samples[hi]
            * frac
        )


    print()

    print(
        f"App RTT mean    : "
        f"{statistics.mean(samples):.3f} ms"
    )

    print(
        f"App RTT median  : "
        f"{statistics.median(samples):.3f} ms"
    )

    print(
        f"App RTT P95     : "
        f"{percentile(95):.3f} ms"
    )

    print(
        f"App RTT P99     : "
        f"{percentile(99):.3f} ms"
    )

    print(
        f"App RTT min     : "
        f"{min(samples):.3f} ms"
    )

    print(
        f"App RTT max     : "
        f"{max(samples):.3f} ms"
    )