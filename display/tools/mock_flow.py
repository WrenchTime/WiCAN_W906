#!/usr/bin/env python3
"""Plays the Cerbo GX for a display on the bench: a broker for one who only subscribes, with the flow behind it.

The display reads a second source of values from the MQTT broker of a Victron Cerbo GX, where a flow of
Node-RED publishes one small JSON message about once a second, the bundle
(display/components/core/cerbo.h has the contract, version 1). This tool is both for a display - or for
display/host/gx_live, the same code on a PC - that is given the address of this PC as its "cerbo": it
listens on port 1883, takes CONNECT, SUBSCRIBE and PINGREQ as a broker does, and publishes bundles on
display/gx/values to whoever subscribed to that topic. And it can do everything wrong a broker or a flow can
do wrong, one fault at a time, to show what the display makes of it.

  python3 mock_flow.py                      a bundle a second, as the flow of the owner sends them
  python3 mock_flow.py --fault flood        ... a hundred a second
  python3 mock_flow.py --list               the faults, and what the display has to do with each
  python3 mock_flow.py --pieces 7 --pause 30    everything is written in pieces of 7 bytes, 30 ms apart
  python3 mock_flow.py --hold 20:12         from second 20 on nothing is written for 12 s, then all of it at once

The address of the PC goes into the page of the display (Gerät, Einstellungen, Cerbo). On this PC:
  ../host/gx_live/build/gx_live 127.0.0.1 60

It never connects to anything, and it publishes to nobody but its own subscribers: it CANNOT publish to the
broker of a Cerbo, where a text below W/ switches relays. What a client publishes here is not passed on
either; such a client is told so and its connection is closed - a display never does that.

What it is not: a broker. One topic, QoS 0, no sessions, no retained messages, no wildcards, no will, no
login. And the values are made up: 43 keys with the names of the flow of the owner, moving a little.
"""
import argparse
import math
import random
import selectors
import socket
import sys
import time

TOPIC = "display/gx/values"
PORT = 1883
BUNDLE_MAX = 1400       # bytes of a bundle; the flow sends none that is longer (cerbo.h: CERBO_BUNDLE_MAX)
VALUES_MAX = 64
KEEPALIVE_FACTOR = 1.5  # a broker ends a connection that sent nothing for this many keep-alives (MQTT 3.1.1, 3.1.2.10)

# What the flow publishes: (key, what it is around, how far it moves, decimals). "on"/"off" for the switches.
VALUES = (
    ("GX_BATT_SOC", 87.5, 2.0, 1), ("GX_BATT_V", 26.41, 0.3, 2), ("GX_BATT_I", -6.2, 4.0, 1), ("GX_BATT_P", -164, 100, 0),
    ("GX_CELL_MIN", 3.298, 0.01, 3), ("GX_CELL_MAX", 3.305, 0.01, 3), ("GX_SHUNT_SOC", 98, 1, 0), ("GX_STARTER_V", 12.63, 0.1, 2),
    ("GX_PV_P", 212, 150, 0), ("GX_DC_P", 302, 80, 0), ("GX_AC_OUT_P", 45, 30, 0), ("GX_MULTI_MODE", 3, 0, 0),
    ("GX_ALT_I", 15.5, 10.0, 1), ("GX_ALT_V", 28.12, 0.2, 2), ("GX_ALT_V1", 14.1, 0.2, 1), ("GX_ALT_VIN", 14.0, 0.2, 1),
    ("GX_ALT_T_BOARD", 41.5, 3.0, 1), ("GX_ALT_T_FET1", 44.0, 3.0, 1), ("GX_ALT_T_FET2", 43.5, 3.0, 1), ("GX_ALT_T_CAN", 30.0, 1.0, 1),
    ("GX_TANK_FRESH", 77, 1, 0), ("GX_TANK_GRAY", 60, 1, 0), ("GX_T_BOILER", 61.1, 2.0, 1), ("GX_T_GARAGE", 14.2, 0.5, 1),
    ("GX_T_WATER", 12.8, 0.5, 1), ("GX_T_DINETTE", 21.4, 0.5, 1), ("GX_T_OUTDOOR", 8.9, 0.5, 1), ("GX_T_HEATER_IN", 19.3, 0.5, 1),
    ("GX_SPEED", 0, 0, 0), ("GX_ALTITUDE", 412, 2, 0), ("GX_GPS_SATS", 11, 2, 0),
    ("GX_SW_B2B", "on", 0, 0), ("GX_SW_BOILER", "off", 0, 0), ("GX_SW_VALVE", "off", 0, 0), ("GX_SW_PUMP", "off", 0, 0),
    ("GX_LTE_RSSI", -67, 3, 0), ("GX_LTE_RSRP", -98, 3, 0), ("GX_LTE_SINR", 9, 2, 0), ("GX_LTE_RSRQ", -11, 1, 0),
    ("GX_LTE_TEMP", 41.0, 1.0, 1), ("GX_LTE_TX_MB", 1830, 0, 0), ("GX_LTE_RX_MB", 20417, 0, 0), ("GX_LTE_CONN", "on", 0, 0),
)

# The faults: what is sent, and what the display has to do with it (cerbo.h, mqtt_sub.h; the design, section 5)
FAULTS = {
    "none": "bundles of the contract. Display: live, the values fresh",
    "silent": "the broker answers, the flow publishes nothing. Display: \"Warte auf Cerbo\", after 20 s and a PINGRESP "
              "\"Cerbo sendet keine Werte\"",
    "mute": "the connection is taken and nothing is ever answered. Display: gives up after 15 s, tries again, "
            "\"Cerbo nicht erreichbar\"",
    "refuse_connect": "CONNACK with return code 5. Display: closes, tries again after 2, 5, 10, 30 s",
    "refuse_subscribe": "SUBACK with return code 0x80. Display: closes, tries again",
    "close": "the broker closes the connection every ten seconds. Display: a new session each time, the values stay",
    "garbage": "bytes that are no MQTT. Display: closes, tries again",
    "qos1": "a PUBLISH with QoS 1. Display: closes (it cannot acknowledge), tries again",
    "puback": "a packet a subscriber does not get. Display: closes, tries again",
    "no_json": "a payload that is no JSON. Display: refused and counted, the values grow old",
    "types": "v, boot, t or values of the wrong type or out of range, in turn. Display: refused and counted",
    "deep": "a bundle nested deeper than the reader goes. Display: refused and counted",
    "tokens": "a bundle with more tokens than the reader has room for. Display: refused and counted",
    "long": "a bundle of more than 1400 bytes. Display: refused and counted",
    "huge": "a message of 1 MB every 30 s, bundles in between. Display: it goes by, nothing of it is kept; the bundles "
            "behind it are late",
    "length_only": "the head of a message of 500 bytes, and then nothing. Display: waits, ends the session when its "
                   "PINGREQ stays without an answer",
    "topic": "bundles on another topic. Display: refused and counted",
    "retained": "bundles with the RETAIN flag. Display: refused and counted",
    "version": "bundles of version 2. Display: refused, \"Cerbo-Flow ist neuer als dieses Display\"",
    "flood": "a hundred bundles a second. Display: live; each is read, the last one counts",
    "many": "70 values in a bundle. Display: 64 are kept, 6 counted as passed over",
    "names": "keys without GX_, in small letters, too long, among good ones. Display: those are passed over",
    "numbers": "numbers beyond every measure among good ones. Display: a dash where the number has no text",
    "missing": "half of the values are missing from every second bundle. Display: those are dashes at once, no yellow ring",
    "late": "from second 15 of every half minute on the bundles are held back for eight seconds. Display: values as "
            "old as their lag (dimmed), lag_ms of /api/info",
    "clock_lost": "after 20 s the clock of the flow loses eleven seconds. Display: the values dimmed, fresh again about "
                  "a minute later",
    "two_flows": "two runs of the flow publish in turn. Display: the values stay, boots of /api/info runs",
    "restart": "the flow starts anew every 20 s: another boot, t from 0. Display: no lag, boots counts",
}


def fixed_header(first, remaining):
    """The first byte and the Remaining Length (MQTT 3.1.1, 2.2.3)"""
    out = bytearray([first])
    while True:
        byte = remaining % 128
        remaining //= 128
        out.append(byte | 0x80 if remaining > 0 else byte)
        if remaining == 0:
            return bytes(out)


def publish(topic, payload, retain=False, qos=0, declared=None):
    """A PUBLISH (3.3). declared: the length the head names, if it is to be another than the one that follows."""
    name = topic.encode("utf-8")
    body = len(name).to_bytes(2, "big") + name + (b"\x00\x01" if qos else b"") + payload
    return fixed_header(0x30 | (qos << 1) | (1 if retain else 0), len(body) if declared is None else declared) + body


def number_text(value, decimals):
    """A number as the function node of the flow writes it: rounded, without a useless zero"""
    text = "%.*f" % (decimals, value)
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return "0" if text in ("-0", "") else text


class Flow:
    """The function node "Bündel": one message with every tick, by the contract or with one thing wrong"""

    def __init__(self, fault="none", seed=1):
        if fault not in FAULTS:
            raise ValueError("no fault named %s" % fault)
        self.fault = fault
        self.random = random.Random(seed)
        self.boot = self.random.randint(1, 2**31 - 1)
        self.other_boot = self.random.randint(1, 2**31 - 1)
        self.count = 0          # ticks so far
        self.began = None       # the time of the first tick: t counts from here

    def values(self, seconds):
        """The values at this moment, as the text of the member `values`"""
        members = []
        for index, (key, around, moves, decimals) in enumerate(VALUES):
            if isinstance(around, str):
                members.append("\"%s\":\"%s\"" % (key, around))
            else:
                members.append("\"%s\":%s" % (key, number_text(around + moves * math.sin(seconds / 7 + index), decimals)))
        return members

    def bundle(self, now):
        """The payload of this tick as bytes, and the topic and flags it goes out with: (topic, payload, retain, qos)"""
        if self.began is None:
            self.began = now
        seconds = now - self.began
        t = int(seconds * 1000)
        boot = self.boot
        version = 1
        members = self.values(seconds)
        topic, retain, qos = TOPIC, False, 0
        fault = self.fault
        self.count += 1

        if fault == "restart":
            # Another run every 20 s: another boot, and its clock begins anew
            run = int(seconds // 20)
            boot = (self.boot + run - 1) % (2**31 - 1) + 1
            t = int((seconds - 20 * run) * 1000)
        elif fault == "two_flows" and self.count % 2 == 0:
            boot, t = self.other_boot, t + 123456
        elif fault == "clock_lost" and seconds >= 20:
            t -= 11000
        elif fault == "version":
            version = 2
        elif fault == "topic":
            topic = "display/gx/other"
        elif fault == "retained":
            retain = True
        elif fault == "qos1":
            qos = 1
        elif fault == "many":
            members += ["\"GX_MORE_%d\":%d" % (number, number) for number in range(70 - len(members))]
        elif fault == "names":
            members += ["\"ENGINE_RPM\":812", "\"gx_small\":1", "\"GX_\":2", "\"GX_A-B\":3", "\"GX_%s\":4" % ("X" * 30),
                        "\"GX_TEXT\":\"auto\"", "\"GX_NULL\":null", "\"GX_LIST\":[1]"]
        elif fault == "numbers":
            members += ["\"GX_HUGE\":1e999", "\"GX_TINY\":-1e999", "\"GX_LONG\":123456789012345678901234567890",
                        "\"GX_SMALL\":0.000000000000000000001"]
        elif fault == "missing" and self.count % 2 == 0:
            members = members[::2]
        elif fault == "long":
            members += ["\"GX_PAD_%02d\":%d.%s" % (number, number, "123456789" * 4) for number in range(20)]

        text = "{\"v\":%d,\"boot\":%d,\"t\":%d,\"values\":{%s}}" % (version, boot, t, ",".join(members))
        if fault == "no_json":
            text = text[:-1] if self.count % 2 else "bundle " + text
        elif fault == "types":
            wrong = (("\"v\":1,", "\"v\":\"1\","), ("\"v\":1,", "\"v\":1.0,"), ("\"v\":1,", "\"v\":0,"), ("\"v\":1,", ""),
                     ("\"boot\":%d," % boot, "\"boot\":0,"), ("\"boot\":%d," % boot, "\"boot\":%d," % 2**31),
                     ("\"t\":%d," % t, "\"t\":-1,"), ("\"t\":%d," % t, "\"t\":%d.5," % t),
                     ("\"values\":{", "\"values\":[{"))[self.count % 9]
            text = text.replace(wrong[0], wrong[1], 1)
            if wrong[1].endswith("[{"):
                text = text[:-1] + "]}"
        elif fault == "deep":
            text = text[:-1] + ",\"more\":%s1%s}" % ("[" * 9, "]" * 9)
        elif fault == "tokens":
            text = text[:-1] + ",\"more\":[%s]}" % ",".join("1" for _ in range(300))
        return topic, text.encode("utf-8"), retain, qos


class Client:
    def __init__(self, connection, peer, now):
        self.connection = connection
        self.peer = "%s:%d" % peer
        self.received = bytearray()
        self.waiting = []       # (not before this time, bytes), in the order they are to leave
        self.greeted = False
        self.subscribed = False
        self.keepalive = 0
        self.heard = now        # when it last sent a packet
        self.since = now        # when it connected
        self.write_at = 0.0     # no piece leaves before this
        self.closing = False    # closed once everything that waits has left
        self.stuck = False      # in the middle of a message that never ends: nothing follows, no answer either
        self.sent = 0           # bundles


class Broker:
    """A broker for subscribers of one topic, and the flow that publishes on it"""

    def __init__(self, host="0.0.0.0", port=PORT, fault="none", period=1.0, pieces=0, pause=0.0, hold=None, clock=time.monotonic,
                 say=None):
        self.flow = Flow(fault)
        self.fault = fault
        self.period = 0.01 if fault == "flood" else period
        self.pieces = pieces
        self.pause = pause
        self.hold = hold        # (from this second of the run on, for this many seconds): nothing is written
        self.clock = clock
        self.say = say or (lambda text: None)
        self.selector = selectors.DefaultSelector()
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind((host, port))
        self.listener.listen(8)
        self.listener.setblocking(False)
        self.selector.register(self.listener, selectors.EVENT_READ, None)
        self.port = self.listener.getsockname()[1]
        self.clients = []
        self.began = clock()
        self.tick_at = self.began + self.period
        self.extra_at = self.began + 5      # the first message of megabytes, or head without a body
        self.published = 0
        self.refused = 0        # clients that sent what a subscriber does not send

    # -------------------------------------------------------------------------------- what is written

    def queue(self, client, data, now, delay=0.0):
        client.waiting.append((now + delay, bytes(data)))

    def held(self, now):
        """Nothing is written right now: the pause of --hold, as a radio link that stands"""
        return self.hold is not None and self.hold[0] <= now - self.began < self.hold[0] + self.hold[1]

    def write(self, client, now):
        """Hands what is due to the socket: whole, or a piece at a time"""
        while client.waiting and not self.held(now) and now >= client.write_at and client.waiting[0][0] <= now:
            due, data = client.waiting[0]
            piece = data[:self.pieces] if self.pieces else data
            try:
                count = client.connection.send(piece)
            except (BlockingIOError, InterruptedError):
                return
            except OSError:
                self.drop(client, "gone while it was written to")
                return
            if count < len(data):
                client.waiting[0] = (due, data[count:])
            else:
                client.waiting.pop(0)
            if self.pieces:
                client.write_at = now + self.pause
        if client.closing and not client.waiting:
            self.drop(client, "closed by the broker")

    # -------------------------------------------------------------------------------- what is read

    def packets(self, client):
        """The whole packets in what the client sent, as (first byte, body)"""
        found = []
        data = client.received
        while len(data) >= 2:
            remaining, shift, at = 0, 0, 1
            while True:
                if at >= len(data):
                    return found
                byte = data[at]
                remaining |= (byte & 0x7F) << shift
                shift += 7
                at += 1
                if not byte & 0x80:
                    break
                if shift > 21:
                    return found + [(0, b"")]
            if len(data) < at + remaining:
                break
            found.append((data[0], bytes(data[at:at + remaining])))
            del data[:at + remaining]
        return found

    def take(self, client, first, body, now):
        """One packet of a client. A subscriber sends CONNECT, SUBSCRIBE and PINGREQ; whoever sends anything else
        is no display, and gets nothing more."""
        kind = first >> 4
        client.heard = now
        if self.fault == "mute" or client.stuck:
            return None
        if kind == 1 and not client.greeted:
            # 3.1: the protocol name, level 4, the flags, the keep-alive, the client id
            if len(body) < 12 or body[:7] != b"\x00\x04MQTT\x04":
                return self.refuse(client, "a CONNECT that is not MQTT 3.1.1")
            flags = body[7]
            client.keepalive = int.from_bytes(body[8:10], "big")
            name = body[12:12 + int.from_bytes(body[10:12], "big")].decode("utf-8", "replace")
            if flags != 0x02:
                self.say("%s: CONNECT with the flags 0x%02X - a display sets Clean Session and nothing else (no will, no name)"
                         % (client.peer, flags))
            client.greeted = True
            self.say("%s: CONNECT as \"%s\", keep-alive %d s" % (client.peer, name, client.keepalive))
            self.queue(client, b"\x20\x02\x00" + (b"\x05" if self.fault == "refuse_connect" else b"\x00"), now)
            if self.fault == "refuse_connect":
                client.closing = True
        elif kind == 8 and first == 0x82 and client.greeted:
            # 3.8: the packet identifier, then topic filters with a requested QoS each
            identifier = body[:2]
            topic = body[4:4 + int.from_bytes(body[2:4], "big")].decode("utf-8", "replace")
            self.say("%s: SUBSCRIBE to \"%s\"" % (client.peer, topic))
            if self.fault == "refuse_subscribe" or topic != TOPIC:
                self.queue(client, b"\x90\x03" + identifier + b"\x80", now)
                client.closing = self.fault == "refuse_subscribe"
                return None
            self.queue(client, b"\x90\x03" + identifier + b"\x00", now)
            client.subscribed = True
            if self.fault == "garbage":
                self.queue(client, b"HTTP/1.1 400 Bad Request\r\n\r\n", now)
            elif self.fault == "puback":
                self.queue(client, b"\x40\x02\x00\x01", now)
        elif first == 0xC0 and client.greeted:
            self.queue(client, b"\xD0\x00", now)
        elif first == 0xE0:
            client.closing = True
        else:
            return self.refuse(client, "a PUBLISH" if kind == 3 else "a packet with the first byte 0x%02X" % first)
        return None

    def refuse(self, client, what):
        self.refused += 1
        self.say("%s: sent %s - not passed on, connection closed. A display never sends that." % (client.peer, what))
        self.drop(client, None)

    def drop(self, client, why):
        if client in self.clients:
            self.clients.remove(client)
            try:
                self.selector.unregister(client.connection)
            except (KeyError, ValueError):
                pass
            # Said before the connection ends: whoever waits for the end reads it at once
            if why:
                self.say("%s: %s after %.0f s, %d bundles" % (client.peer, why, self.clock() - client.since, client.sent))
            client.connection.close()

    # -------------------------------------------------------------------------------- the flow

    def tick(self, now):
        """The flow publishes: a bundle to every subscriber, with what the fault adds or takes away"""
        seconds = now - self.began
        if self.fault in ("silent", "mute"):
            return
        topic, payload, retain, qos = self.flow.bundle(now)
        packet = publish(topic, payload, retain, qos)
        delay = 8.0 if self.fault == "late" and seconds % 30 >= 15 else 0.0
        extra = b""
        if now >= self.extra_at and self.fault in ("huge", "length_only"):
            self.extra_at = now + 30
            # A megabyte on the topic of the bundles; or the head of 500 bytes and not one of them
            extra = publish(TOPIC, b"x" * (1 << 20)) if self.fault == "huge" else publish(TOPIC, b"", declared=500)
        for client in list(self.clients):
            if not client.subscribed:
                continue
            if extra:
                self.queue(client, extra, now)
                if self.fault == "length_only":
                    client.subscribed = False
                    client.stuck = True
                    continue
            self.queue(client, packet, now, delay)
            client.sent += 1
        self.published += 1

    # -------------------------------------------------------------------------------- the loop

    def step(self, wait=0.01):
        """One turn: what arrived is read, the flow ticks when its time has come, what is due is written"""
        # Pieces leave a few milliseconds apart: no turn may take longer than that while some wait
        if self.pieces and any(client.waiting for client in self.clients):
            wait = min(wait, 0.001)
        for key, _ in self.selector.select(wait):
            if key.data is None:
                try:
                    connection, peer = self.listener.accept()
                except OSError:
                    continue
                connection.setblocking(False)
                connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                client = Client(connection, peer, self.clock())
                self.clients.append(client)
                self.selector.register(connection, selectors.EVENT_READ, client)
                self.say("%s: connected" % client.peer)
                continue
            client = key.data
            try:
                data = client.connection.recv(4096)
            except (BlockingIOError, InterruptedError):
                continue
            except OSError:
                data = b""
            if not data:
                self.drop(client, "closed by the client")
                continue
            client.received += data
            for first, body in self.packets(client):
                if client in self.clients:
                    self.take(client, first, body, self.clock())

        now = self.clock()
        while now >= self.tick_at:
            self.tick(self.tick_at)
            self.tick_at += self.period
        for client in list(self.clients):
            if self.fault == "close" and now - client.since >= 10:
                client.closing = True
            # As a broker does: a client that is not heard of for one and a half keep-alives is gone
            if client.keepalive and now - client.heard > client.keepalive * KEEPALIVE_FACTOR:
                self.drop(client, "silent for more than %.0f s" % (client.keepalive * KEEPALIVE_FACTOR))
                continue
            self.write(client, now)

    def serve(self, seconds=None, stop=None):
        try:
            while (seconds is None or self.clock() - self.began < seconds) and not (stop is not None and stop.is_set()):
                self.step()
        finally:
            self.close()

    def close(self):
        for client in list(self.clients):
            self.drop(client, None)
        try:
            self.selector.unregister(self.listener)
        except (KeyError, ValueError):
            pass
        self.listener.close()
        self.selector.close()


def hold_of(text):
    """--hold FROM:FOR as two numbers of seconds"""
    try:
        start, length = (float(part) for part in text.split(":"))
    except ValueError:
        raise argparse.ArgumentTypeError("FROM:FOR in seconds, as 20:12")
    if start < 0 or length <= 0:
        raise argparse.ArgumentTypeError("FROM:FOR in seconds, as 20:12")
    return start, length


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="0.0.0.0", help="the address to listen on (all of this PC)")
    parser.add_argument("--port", type=int, default=PORT, help="the display knows only %d" % PORT)
    parser.add_argument("--fault", default="none", choices=sorted(FAULTS), metavar="NAME", help="what is wrong, see --list")
    parser.add_argument("--period", type=float, default=1.0, help="seconds between two bundles")
    parser.add_argument("--pieces", type=int, default=0, metavar="BYTES", help="everything is written in pieces of this size")
    parser.add_argument("--pause", type=float, default=20, metavar="MS", help="... this far apart")
    parser.add_argument("--hold", type=hold_of, metavar="FROM:FOR", help="nothing is written for FOR seconds, FROM seconds after the start")
    parser.add_argument("--seconds", type=float, help="stop after this long")
    parser.add_argument("--list", action="store_true", help="the faults, and what the display has to do with each")
    options = parser.parse_args(argv)

    if options.list:
        for name, what in FAULTS.items():
            print("%-18s%s" % (name, what))
        return 0
    if options.period <= 0 or options.pieces < 0 or options.pause < 0:
        parser.error("--period above 0, --pieces and --pause not below 0")

    began = time.monotonic()

    def say(text):
        print("%7.1f s  %s" % (time.monotonic() - began, text), flush=True)

    try:
        broker = Broker(options.host, options.port, options.fault, options.period, options.pieces, options.pause / 1000, options.hold,
                        say=say)
    except OSError as error:
        print("cannot listen on %s:%d: %s" % (options.host, options.port, error))
        return 1
    say("listening on %s:%d, topic %s, fault: %s - %s" % (options.host, broker.port, TOPIC, options.fault, FAULTS[options.fault]))
    said = began
    try:
        while options.seconds is None or time.monotonic() - began < options.seconds:
            broker.step()
            if time.monotonic() - said >= 10:
                said = time.monotonic()
                say("%d bundles published, %d subscribers" % (broker.published, sum(1 for client in broker.clients if client.subscribed)))
    except KeyboardInterrupt:
        pass
    finally:
        broker.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
