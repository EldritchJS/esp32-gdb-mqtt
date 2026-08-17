#!/usr/bin/env python3
"""Bridge between GDB's TCP connection and MQTT topics for remote debugging."""

import argparse
import socket
import threading
import sys

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("paho-mqtt not installed. pip install paho-mqtt", file=sys.stderr)
    sys.exit(1)


class GdbMqttBridge:
    def __init__(self, broker_host, broker_port, device_id, gdb_port):
        self.broker_host = broker_host
        self.broker_port = broker_port
        self.device_id = device_id
        self.gdb_port = gdb_port
        self.cmd_topic = f"device/{device_id}/gdb/cmd"
        self.resp_topic = f"device/{device_id}/gdb/resp"
        self.gdb_conn = None
        self.mqtt_client = None
        self.running = True

    def on_mqtt_connect(self, client, userdata, flags, rc, properties=None):
        if rc == 0:
            print(f"[mqtt] Connected to broker, subscribing to {self.resp_topic}")
            client.subscribe(self.resp_topic, qos=1)
        else:
            print(f"[mqtt] Connection failed: rc={rc}")

    def on_mqtt_message(self, client, userdata, msg):
        if self.gdb_conn:
            try:
                data = msg.payload
                print(f"[esp→gdb] {data!r}")
                self.gdb_conn.sendall(data)
            except (BrokenPipeError, ConnectionResetError):
                print("[gdb] Connection lost")
                self.gdb_conn = None

    def mqtt_thread(self):
        self.mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        self.mqtt_client.on_connect = self.on_mqtt_connect
        self.mqtt_client.on_message = self.on_mqtt_message
        self.mqtt_client.connect(self.broker_host, self.broker_port, keepalive=60)
        self.mqtt_client.loop_forever()

    def gdb_recv_thread(self):
        while self.running:
            if not self.gdb_conn:
                threading.Event().wait(0.1)
                continue
            try:
                data = self.gdb_conn.recv(4096)
                if not data:
                    print("[gdb] Disconnected")
                    self.gdb_conn = None
                    continue
                print(f"[gdb→esp] {data!r}")
                self.mqtt_client.publish(self.cmd_topic, data, qos=1)
            except (ConnectionResetError, OSError):
                self.gdb_conn = None

    def run(self):
        mqtt_t = threading.Thread(target=self.mqtt_thread, daemon=True)
        mqtt_t.start()

        recv_t = threading.Thread(target=self.gdb_recv_thread, daemon=True)
        recv_t.start()

        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("0.0.0.0", self.gdb_port))
        server.listen(1)

        print(f"[bridge] Listening on port {self.gdb_port}")
        print(f"[bridge] MQTT broker: {self.broker_host}:{self.broker_port}")
        print(f"[bridge] Device: {self.device_id}")
        print(f"[bridge] Attach GDB: target remote localhost:{self.gdb_port}")

        while self.running:
            try:
                conn, addr = server.accept()
                print(f"[gdb] Connection from {addr}")
                self.gdb_conn = conn
            except KeyboardInterrupt:
                self.running = False
                break

        server.close()


def main():
    parser = argparse.ArgumentParser(description="GDB-MQTT bridge")
    parser.add_argument("--broker", default="localhost",
                        help="MQTT broker hostname (default: localhost)")
    parser.add_argument("--broker-port", type=int, default=1883,
                        help="MQTT broker port (default: 1883)")
    parser.add_argument("--device", default="esp32c3-001",
                        help="Device ID for MQTT topics (default: esp32c3-001)")
    parser.add_argument("--port", type=int, default=3333,
                        help="TCP port for GDB to connect to (default: 3333)")
    args = parser.parse_args()

    bridge = GdbMqttBridge(args.broker, args.broker_port, args.device, args.port)
    bridge.run()


if __name__ == "__main__":
    main()
