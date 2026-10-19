"""Checks the stand-in for the Cerbo (mock_flow.py) against the contract of the bundle and against what a
subscriber sees of a broker.

The contract is written down a second time here, from display/components/core/cerbo.h and not from
mock_flow.py: what the flow of the mock publishes without a fault has to pass it, and what it publishes with
a fault has to be found - so the check is no blind one, and a fault is what its name says. The broker is
asked over a real connection of this PC, by a client that sends what a display sends.

The mock keeps time by a clock it is given. Here that clock runs fifty times as fast as the one of the PC:
ten seconds of a fault take a fifth of a second.

  python -m unittest -v          in display/tools, as the CI does
"""
import contextlib
import io
import json
import math
import os
import re
import socket
import threading
import time
import unittest

import mock_flow

FAST = 50
CERBO_H = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "components", "core", "cerbo.h")
KEY = re.compile(r"GX_[A-Z0-9_]{1,29}")


def tokens(value):
    """As the JSON reader of the display counts (json.h): one for every value, one more for every key"""
    if isinstance(value, dict):
        return 1 + sum(1 + tokens(member) for member in value.values())
    if isinstance(value, list):
        return 1 + sum(tokens(member) for member in value)
    return 1


def depth(value):
    members = value.values() if isinstance(value, dict) else value if isinstance(value, list) else None
    return 0 if members is None else 1 + max([depth(member) for member in members] or [0])


def whole(value, least, most):
    return type(value) is int and least <= value <= most


def contract_problems(payload):
    """What keeps these bytes from being a bundle of version 1 the display takes whole (cerbo.h)"""
    found = []
    if len(payload) > 1400:
        found.append("%d bytes, more than 1400" % len(payload))
    try:
        bundle = json.loads(payload.decode("utf-8"))
    except ValueError:
        return found + ["no JSON"]
    if not isinstance(bundle, dict):
        return found + ["no object"]
    if depth(bundle) > 8:
        found.append("deeper than 8 levels")
    if tokens(bundle) > 256:
        found.append("%d tokens, more than 256" % tokens(bundle))
    if not whole(bundle.get("v"), 1, 1):
        found.append("v is not the whole number 1")
    if not whole(bundle.get("boot"), 1, 2**31 - 1):
        found.append("boot is no whole number from 1 to 2^31-1")
    if not whole(bundle.get("t"), 0, 2**53 - 1):
        found.append("t is no whole number from 0 to 2^53-1")
    values = bundle.get("values")
    if not isinstance(values, dict):
        return found + ["values is no object"]
    if len(values) > 64:
        found.append("%d values, more than 64" % len(values))
    for key, value in values.items():
        if KEY.fullmatch(key) is None:
            found.append("a key the display does not keep: %s" % key)
        if value not in ("on", "off") and (type(value) not in (int, float) or not math.isfinite(value)):
            found.append("a value that is neither a number nor on or off: %s" % key)
    if b" " in payload or b"\n" in payload:
        found.append("sent with spaces or line ends")
    return found


def bundles(fault, seconds=60, step=1.0):
    """What the flow publishes with this fault in a minute: (seconds, topic, payload, retain, qos) of each tick"""
    flow = mock_flow.Flow(fault)
    return [(tick * step,) + flow.bundle(1000.0 + tick * step) for tick in range(int(seconds / step))]


def read(payload):
    return json.loads(payload.decode("utf-8"))


class TheFlow(unittest.TestCase):
    def test_the_check_reads_the_contract(self):
        good = b"{\"v\":1,\"boot\":7,\"t\":0,\"values\":{\"GX_A\":1.5,\"GX_SW_B2B\":\"on\"}}"
        self.assertEqual(contract_problems(good), [])
        for broken in (good[:-1], b"[1]", good.replace(b"\"v\":1", b"\"v\":2"), good.replace(b"\"v\":1", b"\"v\":1.0"),
                       good.replace(b"\"v\":1", b"\"v\":true"), good.replace(b"\"boot\":7", b"\"boot\":0"),
                       good.replace(b"\"boot\":7", b"\"boot\":2147483648"), good.replace(b"\"t\":0", b"\"t\":-1"),
                       good.replace(b"\"t\":0", b"\"t\":9007199254740992"), good.replace(b"\"t\":0,", b""),
                       good.replace(b"GX_A", b"gx_a"), good.replace(b"GX_A", b"GX_" + b"A" * 30), good.replace(b"GX_A", b"ENGINE_RPM"),
                       good.replace(b"1.5", b"\"auto\""), good.replace(b"1.5", b"null"), good.replace(b"1.5", b"1e999"),
                       good.replace(b"1.5", b"true"), good.replace(b",\"values\"", b", \"values\""),
                       good.replace(b"\"values\":{", b"\"values\":[{").replace(b"}}", b"}]}")):
            self.assertNotEqual(contract_problems(broken), [], broken)
        self.assertEqual(tokens(read(good)), 13)

    def test_a_bundle_without_a_fault_is_of_the_contract(self):
        sent = bundles("none", seconds=300)
        for seconds, topic, payload, retain, qos in sent:
            self.assertEqual(contract_problems(payload), [], payload)
            self.assertEqual((topic, retain, qos), ("display/gx/values", False, 0))
        first = read(sent[0][2])
        # The 43 keys of the flow of the owner, five of them switches; a bundle of his has 782 to 788 bytes
        self.assertEqual(len(first["values"]), 43)
        self.assertEqual(sorted(key for key, value in first["values"].items() if value in ("on", "off")),
                         ["GX_LTE_CONN", "GX_SW_B2B", "GX_SW_BOILER", "GX_SW_PUMP", "GX_SW_VALVE"])
        self.assertTrue(700 <= min(len(entry[2]) for entry in sent) and max(len(entry[2]) for entry in sent) <= 900)
        self.assertEqual(tokens(first), 95)
        # One run of the flow: one boot, and a clock that follows the time and never runs backwards
        self.assertEqual({read(entry[2])["boot"] for entry in sent}, {first["boot"]})
        self.assertEqual([read(entry[2])["t"] for entry in sent], [1000 * tick for tick in range(300)])
        # The values move: a display that shows the same number for ever shows nothing
        self.assertNotEqual(first["values"]["GX_BATT_SOC"], read(sent[10][2])["values"]["GX_BATT_SOC"])

    def test_every_fault_of_the_payload_is_one(self):
        # Each of these the display refuses or cuts down: the check has to find something in every bundle
        for fault, word in (("no_json", "no JSON"), ("types", ""), ("deep", "deeper"), ("tokens", "tokens"), ("long", "more than 1400"),
                            ("version", "v is not"), ("many", "70 values"), ("names", "a key the display does not keep: ENGINE_RPM"),
                            ("numbers", "neither a number")):
            with self.subTest(fault):
                for seconds, topic, payload, retain, qos in bundles(fault, seconds=20):
                    found = contract_problems(payload)
                    self.assertNotEqual(found, [], payload)
                    self.assertTrue(any(word in text for text in found), found)
                    self.assertEqual((topic, retain, qos), ("display/gx/values", False, 0))
        # ... and of "types" every member in turn
        said = {text for entry in bundles("types", seconds=9) for text in contract_problems(entry[2])}
        self.assertEqual(said, {"v is not the whole number 1", "boot is no whole number from 1 to 2^31-1",
                                "t is no whole number from 0 to 2^53-1", "values is no object"})
        # What the display passes over in "names", and nothing else: eight members
        names = contract_problems(bundles("names", seconds=1)[0][2])
        self.assertEqual(len(names), 8, names)

    def test_tokens_and_length_are_two_limits(self):
        # Each of the two faults reaches the limit it is named for and no other one. Until 2026-10-12 "tokens"
        # was a bundle of more than 1400 bytes: the display refused it for its length, as it does with "long",
        # and no fault reached the limit of the tokens (seen with gx_live: too long 60, invalid 0).
        self.assertEqual((mock_flow.TOKENS_MAX, mock_flow.BUNDLE_MAX), (256, 1400))
        for seconds, topic, payload, retain, qos in bundles("tokens", seconds=300):
            found = contract_problems(payload)
            self.assertEqual(len(found), 1, found)
            self.assertRegex(found[0], r"^\d+ tokens, more than 256$")
            self.assertLessEqual(len(payload), mock_flow.BUNDLE_MAX)
            self.assertEqual(tokens(read(payload)), 267)
        for seconds, topic, payload, retain, qos in bundles("long", seconds=300):
            found = contract_problems(payload)
            self.assertEqual(len(found), 1, found)
            self.assertRegex(found[0], r"^\d+ bytes, more than 1400$")
            self.assertLessEqual(tokens(read(payload)), mock_flow.TOKENS_MAX)

    def test_late_is_later_than_a_value_is_fresh(self):
        # The fault has to reach the limit it is there for. A bundle that is held back for LATE_S arrives with a
        # lag of that time less what the base of the lag gave way in the gap in front of it (cerbo.h: a tenth of
        # LATE_S and the second to the bundle before), and only a lag of CERBO_FRESH_MS and more dims a value.
        # The limit is read from the header: when it went from 5 to 10 s on 2026-10-12 the fault still held
        # back for 8 s, a lag of 7.1 s at first, and the display would have shown nothing of it.
        with open(CERBO_H, encoding="utf-8") as handle:
            header = handle.read()
        fresh = int(re.search(r"^#define CERBO_FRESH_MS +(\d+)u ", header, re.M).group(1))
        per_mille = int(re.search(r"^#define CERBO_YIELD_PER_MILLE +(\d+) ", header, re.M).group(1))

        def first_lag(held_s):
            return held_s * 1000 - (held_s + 1) * per_mille

        self.assertEqual((fresh, per_mille), (10000, 100))
        self.assertEqual(first_lag(mock_flow.LATE_S), 10700)
        self.assertGreater(first_lag(mock_flow.LATE_S), fresh)
        self.assertEqual(first_lag(8), 7100)
        self.assertLess(first_lag(8), fresh)

    def test_faults_of_the_wire_leave_the_payload_alone(self):
        for fault, wrong in (("topic", ("display/gx/other", False, 0)), ("retained", ("display/gx/values", True, 0)),
                             ("qos1", ("display/gx/values", False, 1))):
            with self.subTest(fault):
                for seconds, topic, payload, retain, qos in bundles(fault, seconds=5):
                    self.assertEqual(contract_problems(payload), [])
                    self.assertEqual((topic, retain, qos), wrong)

    def test_faults_of_the_clock_and_of_the_run(self):
        # Every bundle of these is one of the contract; what is wrong lies between them
        sent = {fault: [read(entry[2]) for entry in bundles(fault)] for fault in ("missing", "restart", "two_flows", "clock_lost")}
        for fault in sent:
            for entry in bundles(fault):
                self.assertEqual(contract_problems(entry[2]), [], fault)
        self.assertEqual([len(bundle["values"]) for bundle in sent["missing"][:4]], [43, 22, 43, 22])
        # Another boot every 20 s, each with a clock of its own
        self.assertEqual(len({bundle["boot"] for bundle in sent["restart"]}), 3)
        self.assertEqual([bundle["t"] for bundle in sent["restart"][18:22]], [18000, 19000, 0, 1000])
        # Two runs in turn
        self.assertEqual(len({bundle["boot"] for bundle in sent["two_flows"]}), 2)
        self.assertNotEqual(sent["two_flows"][0]["boot"], sent["two_flows"][1]["boot"])
        self.assertEqual(sent["two_flows"][0]["boot"], sent["two_flows"][2]["boot"])
        # Eleven seconds lost once, after 20 s, and the clock goes on from there
        self.assertEqual([bundle["t"] for bundle in sent["clock_lost"][18:23]], [18000, 19000, 9000, 10000, 11000])

    def test_a_fault_has_a_name_and_a_line(self):
        with self.assertRaises(ValueError):
            mock_flow.Flow("nonsense")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(mock_flow.main(["--list"]), 0)
        lines = output.getvalue().splitlines()
        self.assertEqual([line.split()[0] for line in lines], list(mock_flow.FAULTS))
        self.assertTrue(all("Display: " in line for line in lines))
        # What the display does, as it was measured with gx_live on 2026-10-12 - not that a megabyte "goes by"
        # and that a flood is read as it comes, which the list said before and the display does not do
        said = {line.split()[0]: line for line in lines}
        self.assertIn("the megabyte never goes by", said["huge"])
        self.assertIn("the session ends and is made anew each time", said["huge"])
        self.assertNotIn("it goes by", said["huge"])
        self.assertIn("falls behind", said["flood"])
        self.assertIn("the session ends", said["flood"])
        self.assertNotIn("Display: live", said["flood"])
        self.assertIn("less than 1400 bytes", said["tokens"])
        # ... and what it does since 2026-10-12, when a value of the Cerbo became fresh for 10 s and the waits
        # stopped beginning anew with every SUBACK: a clock that lost 11 s is forgotten after 20 s, not after a
        # minute, and a broker that breaks every session is asked every 30 s in the end, not every 2 s
        self.assertIn("after 20 s for good", said["clock_lost"])
        self.assertNotIn("a minute later", said["clock_lost"])
        self.assertIn("twelve seconds", said["late"])
        self.assertIn("the values dimmed until the lag is below 10 s", said["late"])
        self.assertIn("after 2, 5, 10 and then every 30 s", said["garbage"])
        self.assertIn("\"Cerbo nicht erreichbar\"", said["garbage"])
        for fault in ("qos1", "puback"):
            self.assertIn("tries again as with garbage", said[fault])
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as stopped:
            mock_flow.main(["--fault", "nonsense"])
        self.assertEqual(stopped.exception.code, 2)


class Subscriber:
    """A client that sends what a display sends, over a connection of this PC"""
    CONNECT = b"\x10\x1c\x00\x04MQTT\x04\x02\x00\x1e\x00\x10WiCANDisplayTest"
    SUBSCRIBE = b"\x82\x16\x00\x01\x00\x11display/gx/values\x00"
    PINGREQ = b"\xc0\x00"

    def __init__(self, port):
        self.connection = socket.create_connection(("127.0.0.1", port), timeout=2)
        self.buffer = b""
        self.pieces = []        # the sizes of what each recv() brought

    def send(self, data):
        self.connection.sendall(data)

    def more(self, timeout):
        """Reads once. False when nothing came in that time or the broker closed (self.closed says which)."""
        self.connection.settimeout(timeout)
        try:
            data = self.connection.recv(65536)
        except socket.timeout:
            return False
        except OSError:
            data = b""
        self.closed = data == b""
        self.pieces.append(len(data))
        self.buffer += data
        return data != b""

    def packet(self, timeout=2.0):
        """The next whole packet as (first byte, body); None when none came in that time"""
        until = time.monotonic() + timeout
        while True:
            data = self.buffer
            at, remaining, shift = 1, 0, 0
            while at < len(data):
                remaining |= (data[at] & 0x7F) << shift
                shift += 7
                at += 1
                if not data[at - 1] & 0x80:
                    if len(data) >= at + remaining:
                        self.buffer = data[at + remaining:]
                        return data[0], data[at:at + remaining]
                    break
            left = until - time.monotonic()
            if left <= 0 or not self.more(left):
                return None

    def raw(self, timeout=0.3):
        """Everything that comes in this time, as bytes"""
        until = time.monotonic() + timeout
        while time.monotonic() < until and self.more(until - time.monotonic()):
            pass
        data, self.buffer = self.buffer, b""
        return data

    def subscribe(self, keepalive=True):
        # Without a keep-alive (3.1.2.10: 0 switches it off) for a test that takes longer than one and a half
        # of them on the fast clock and has no reason to ping
        self.send(self.CONNECT if keepalive else self.CONNECT.replace(b"\x00\x1e", b"\x00\x00"))
        connack = self.packet()
        self.send(self.SUBSCRIBE)
        return connack, self.packet()

    def until_closed(self, most=200):
        """Reads packets until the broker closes or nothing comes for a second, `most` at the most - a broker
        that never closes must not hold a test for ever. Returns how many came."""
        count = 0
        while count < most and self.packet(1.0) is not None:
            count += 1
        return count

    def close(self):
        self.connection.close()


def message(packet):
    """Topic and payload of a PUBLISH with QoS 0"""
    first, body = packet
    size = int.from_bytes(body[:2], "big")
    return body[2:2 + size].decode("utf-8"), body[2 + size:]


class TheBroker(unittest.TestCase):
    def serve(self, fault="none", **settings):
        """A broker on a port of its own, with a clock that runs FAST times as fast; and what it said"""
        said = []
        broker = mock_flow.Broker("127.0.0.1", 0, fault, clock=lambda: time.monotonic() * FAST, say=said.append, **settings)
        stop = threading.Event()
        thread = threading.Thread(target=broker.serve, kwargs={"stop": stop})
        thread.start()

        def end():
            stop.set()
            thread.join()
        self.addCleanup(end)
        return broker, said

    def client(self, broker):
        subscriber = Subscriber(broker.port)
        self.addCleanup(subscriber.close)
        return subscriber

    def test_a_subscriber_gets_bundles(self):
        broker, said = self.serve()
        display = self.client(broker)
        # Nothing comes before the subscription, however long the flow has run
        display.send(Subscriber.CONNECT)
        self.assertEqual(display.packet(), (0x20, b"\x00\x00"))
        self.assertIsNone(display.packet(0.2))
        display.send(Subscriber.SUBSCRIBE)
        self.assertEqual(display.packet(), (0x90, b"\x00\x01\x00"))
        times = []
        for _ in range(5):
            packet = display.packet()
            # QoS 0, not retained, on the one topic
            self.assertEqual(packet[0], 0x30)
            topic, payload = message(packet)
            self.assertEqual(topic, "display/gx/values")
            self.assertEqual(contract_problems(payload), [])
            times.append(read(payload)["t"])
        # A bundle a second by the clock of the flow
        self.assertEqual([later - earlier for earlier, later in zip(times, times[1:])], [1000] * 4)
        # A ping is answered, between the bundles
        display.send(Subscriber.PINGREQ)
        kinds = [display.packet()[0] for _ in range(3)]
        self.assertIn(0xD0, kinds)
        self.assertTrue(any("CONNECT as \"WiCANDisplayTest\", keep-alive 30 s" in line for line in said), said)
        self.assertFalse(any("flags" in line for line in said), said)

    def test_the_one_topic_and_no_other(self):
        broker, said = self.serve()
        display = self.client(broker)
        display.send(Subscriber.CONNECT)
        display.packet()
        # A pattern is no topic of this broker: refused, and no bundle follows
        display.send(b"\x82\x06\x00\x07\x00\x01#\x00")
        self.assertEqual(display.packet(), (0x90, b"\x00\x07\x80"))
        self.assertIsNone(display.packet(0.2))

    def test_a_subscribe_without_a_topic_gets_no_answer(self):
        # [MQTT-3.8.3-3]: a SUBSCRIBE names at least one topic. Until 2026-10-12 one without a body was answered
        # with 90 03 80: a SUBACK that announces three bytes and has one. Now the connection ends, as for
        # everything a display never sends.
        for packet in (b"\x82\x00", b"\x82\x02\x00\x01", b"\x82\x04\x00\x01\x00\x00", b"\x82\x05\x00\x01\x00\x00\x00",
                       b"\x82\x06\x00\x01\x00\x05ab", b"\x82\x05\x00\x01\x00\x01a"):
            with self.subTest(packet):
                broker, said = self.serve()
                display = self.client(broker)
                display.send(Subscriber.CONNECT)
                self.assertEqual(display.packet(), (0x20, b"\x00\x00"))
                display.send(packet)
                self.assertEqual(display.raw(0.3), b"")
                self.assertTrue(display.closed)
                self.assertTrue(any("sent a SUBSCRIBE without a topic filter and its QoS" in line for line in said), said)
        # The shortest one that names a topic is answered: a topic of one character, refused as no topic of this broker
        broker, said = self.serve()
        display = self.client(broker)
        display.send(Subscriber.CONNECT)
        display.packet()
        display.send(b"\x82\x06\x00\x09\x00\x01a\x00")
        self.assertEqual(display.packet(), (0x90, b"\x00\x09\x80"))

    def test_what_a_client_sent_is_printed_with_nothing_that_steers_a_terminal(self):
        # The client id and the topic are texts of whoever connects. An escape sequence, a line of its own or a
        # mark that turns the writing round is printed as its number, and a long text is cut.
        self.assertEqual(mock_flow.shown("WiCANDisplayC9A8"), "WiCANDisplayC9A8")
        self.assertEqual(mock_flow.shown("Küche °C"), "Küche °C")
        self.assertEqual(mock_flow.shown("a\x1b[2J\r\nb\x00\x7f\u202e"), "a\\u001b[2J\\u000d\\u000ab\\u0000\\u007f\\u202e")
        self.assertEqual(mock_flow.shown("x" * 65), "x" * 64 + "... (65 characters)")
        self.assertEqual(mock_flow.shown("x" * 64), "x" * 64)
        broker, said = self.serve()
        display = self.client(broker)
        name = b"\x1b[31mred\x1b[0m\r\nfake line\x07"
        display.send(mock_flow.fixed_header(0x10, 12 + len(name)) + b"\x00\x04MQTT\x04\x02\x00\x1e" + len(name).to_bytes(2, "big") + name)
        self.assertEqual(display.packet(), (0x20, b"\x00\x00"))
        topic = b"display/\x1b]0;title\x07\n"
        display.send(mock_flow.fixed_header(0x82, 5 + len(topic)) + b"\x00\x01" + len(topic).to_bytes(2, "big") + topic + b"\x00")
        self.assertEqual(display.packet(), (0x90, b"\x00\x01\x80"))
        lines = [line for line in said if "CONNECT as" in line or "SUBSCRIBE to" in line]
        self.assertEqual(len(lines), 2, said)
        self.assertTrue(all(line.isprintable() for line in said), said)
        self.assertIn("CONNECT as \"\\u001b[31mred\\u001b[0m\\u000d\\u000afake line\\u0007\"", lines[0])
        self.assertIn("SUBSCRIBE to \"display/\\u001b]0;title\\u0007\\u000a\"", lines[1])

    def test_a_packet_too_long_for_a_subscriber_is_not_waited_for(self):
        # A length of 256 MB, and the first of its bytes: the connection ends at once, nothing is kept until the
        # packet is whole. The longest packet a display sends has 64 bytes.
        broker, said = self.serve()
        display = self.client(broker)
        display.send(Subscriber.CONNECT)
        display.packet()
        display.send(b"\x82\xff\xff\xff\x7f" + b"x" * 2000)
        self.assertEqual(display.raw(0.3), b"")
        self.assertTrue(display.closed)
        self.assertTrue(any("sent a packet of more than 1024 bytes - not passed on" in line for line in said), said)
        self.assertEqual(broker.clients, [])
        # ... also by a broker that answers nothing: it must not keep what never ends either
        broker, said = self.serve("mute")
        display = self.client(broker)
        display.send(Subscriber.CONNECT)
        self.assertEqual(display.raw(0.2), b"")
        self.assertFalse(getattr(display, "closed", False))
        display.send(b"\x82\xff\xff\xff\x7f" + b"x" * 2000)
        self.assertEqual(display.raw(0.3), b"")
        self.assertTrue(getattr(display, "closed", False))
        self.assertEqual(broker.clients, [])
        # ... and one of exactly the limit is waited for, byte by byte
        broker, said = self.serve()
        display = self.client(broker)
        display.send(b"\x10\x80\x08" + b"\x00\x04MQTT\x04\x02\x00\x1e\x03\xf4" + b"n" * 500)
        self.assertEqual(display.raw(0.2), b"")
        self.assertFalse(getattr(display, "closed", False))
        display.send(b"n" * 512)
        self.assertEqual(display.packet(), (0x20, b"\x00\x00"))

    def test_what_waits_for_a_client_that_does_not_read_has_a_limit(self):
        # A subscriber without a keep-alive that never reads: what waits for it must not grow for ever. The limit
        # is that of the broker of a Cerbo, 8 MB. Told without a clock and without the buffers of this PC: the
        # broker is handed a connection and bytes for it, and writes when it is told to.
        self.assertEqual(mock_flow.WAITING_MAX, 8 * 1024 * 1024)
        said = []
        broker = mock_flow.Broker("127.0.0.1", 0, clock=lambda: 0.0, say=said.append)
        self.addCleanup(broker.close)
        ours, theirs = socket.socketpair()
        self.addCleanup(theirs.close)
        ours.setblocking(False)
        theirs.setblocking(False)
        client = mock_flow.Client(ours, ("127.0.0.1", 1), 0.0)
        broker.clients.append(client)
        megabyte = b"x" * (1024 * 1024)

        def read_all():
            """The broker writes what it can, the client reads it, until nothing waits; returns the bytes read"""
            count = 0
            while client.waiting:
                broker.write(client, 0.0)
                try:
                    while True:
                        count += len(theirs.recv(1 << 20))
                except BlockingIOError:
                    pass
            return count

        # A client that reads is never short of the limit, however much went through
        for _ in range(3):
            for _ in range(5):
                broker.queue(client, megabyte, 0.0)
            self.assertEqual(client.waiting_bytes, 5 * 1024 * 1024)
            self.assertEqual(read_all(), 5 * 1024 * 1024)
            self.assertEqual((client.waiting_bytes, client.waiting), (0, []))
        self.assertEqual((broker.clients, said), ([client], []))
        # One that does not: 8 MB may wait, one byte more ends the connection
        for _ in range(8):
            broker.queue(client, megabyte, 0.0)
        broker.write(client, 0.0)
        self.assertEqual(broker.clients, [client])
        self.assertLessEqual(client.waiting_bytes, mock_flow.WAITING_MAX)
        self.assertGreater(client.waiting_bytes, 7 * 1024 * 1024)
        missing = mock_flow.WAITING_MAX - client.waiting_bytes
        broker.queue(client, b"x" * missing, 0.0)
        self.assertEqual((broker.clients, said, client.waiting_bytes), ([client], [], mock_flow.WAITING_MAX))
        broker.queue(client, b"x", 0.0)
        self.assertEqual(broker.clients, [])
        self.assertEqual(said, ["127.0.0.1:1: does not read, %d bytes wait for it: closed after 0 s, 0 bundles" % (mock_flow.WAITING_MAX + 1)])
        # Its socket is closed: the other end reads what was under way, and then the end
        theirs.setblocking(True)
        theirs.settimeout(2)
        while theirs.recv(1 << 20) != b"":
            pass

    def test_a_broker_that_resets(self):
        # The fault for display/host/gx_live: CONNACK, and then a reset in place of a close. What the client
        # sends next meets a connection that is gone - on a PC that is a signal which ends a program that does
        # not ignore it (Python does).
        broker, said = self.serve("reset")
        display = self.client(broker)
        display.send(Subscriber.CONNECT)
        # The CONNACK is sent before the reset. A PC hands it out if it was there first (macOS does, 2026-10-12);
        # one that throws away what it had received when the reset comes hands out nothing.
        self.assertIn(display.packet(), ((0x20, b"\x00\x00"), None))
        time.sleep(0.1)
        with self.assertRaises(OSError):
            for _ in range(20):
                display.send(Subscriber.SUBSCRIBE)
                time.sleep(0.02)
        self.assertTrue(any("reset by the broker" in line for line in said), said)
        self.assertEqual(broker.clients, [])
        self.assertFalse(any("SUBSCRIBE" in line for line in said), said)
        # A reset and not a close: who reads on meets the error, where a connection that was closed just ends.
        # Only the reset lets the very next send() of a display fail (and raise the signal, on a PC).
        reader = socket.create_connection(("127.0.0.1", broker.port), timeout=2)
        self.addCleanup(reader.close)
        reader.sendall(Subscriber.CONNECT)
        seen = b""
        with self.assertRaises(ConnectionResetError):
            while True:
                part = reader.recv(16)
                self.assertNotEqual(part, b"", "closed, not reset")
                seen += part
        self.assertIn(seen, (b"\x20\x02\x00\x00", b""))

    def test_who_publishes_is_no_display(self):
        broker, said = self.serve()
        listener = self.client(broker)
        self.assertEqual(listener.subscribe(), ((0x20, b"\x00\x00"), (0x90, b"\x00\x01\x00")))
        other = self.client(broker)
        other.subscribe()
        other.send(mock_flow.publish("display/gx/values", b"{\"v\":1,\"boot\":1,\"t\":0,\"values\":{\"GX_FORGED\":1}}"))
        other.send(mock_flow.publish("W/c0619ab1234/system/0/Relay/0/State", b"{\"value\":1}"))
        # The connection of the one who published ends ...
        other.until_closed()
        self.assertTrue(other.closed)
        # ... and nothing of what it sent reaches anybody
        for _ in range(5):
            topic, payload = message(listener.packet())
            self.assertEqual(topic, "display/gx/values")
            self.assertNotIn(b"GX_FORGED", payload)
        self.assertEqual(broker.refused, 1)
        self.assertTrue(any("sent a PUBLISH - not passed on" in line for line in said), said)

    def test_faults_of_the_broker(self):
        # What a display must not take for a session
        for fault, connack, suback in (("refuse_connect", (0x20, b"\x00\x05"), None), ("refuse_subscribe", (0x20, b"\x00\x00"), (0x90, b"\x00\x01\x80"))):
            with self.subTest(fault):
                broker, said = self.serve(fault)
                display = self.client(broker)
                self.assertEqual(display.subscribe(), (connack, suback))
                self.assertIsNone(display.packet(0.3))
                self.assertTrue(display.closed)
        with self.subTest("mute"):
            broker, said = self.serve("mute")
            display = self.client(broker)
            display.send(Subscriber.CONNECT + Subscriber.SUBSCRIBE + Subscriber.PINGREQ)
            self.assertEqual(display.raw(0.3), b"")
        with self.subTest("silent"):
            broker, said = self.serve("silent")
            display = self.client(broker)
            display.subscribe()
            display.send(Subscriber.PINGREQ)
            self.assertEqual(display.raw(0.3), b"\xd0\x00")
        for fault, sent in (("garbage", b"HTTP/1.1 400 Bad Request\r\n\r\n"), ("puback", b"\x40\x02\x00\x01")):
            with self.subTest(fault):
                broker, said = self.serve(fault)
                display = self.client(broker)
                display.send(Subscriber.CONNECT + Subscriber.SUBSCRIBE)
                self.assertTrue(display.raw(0.1).startswith(b"\x20\x02\x00\x00\x90\x03\x00\x01\x00" + sent))

    def test_faults_of_the_messages(self):
        for fault, first, topic in (("retained", 0x31, "display/gx/values"), ("qos1", 0x32, "display/gx/values"),
                                    ("topic", 0x30, "display/gx/other")):
            with self.subTest(fault):
                broker, said = self.serve(fault)
                display = self.client(broker)
                display.subscribe()
                packet = display.packet()
                self.assertEqual((packet[0], message(packet)[0]), (first, topic))
        with self.subTest("flood"):
            broker, said = self.serve("flood")
            display = self.client(broker)
            display.subscribe()
            # A hundred a second by the clock of the flow: a second of it is a fiftieth here
            began = time.monotonic()
            for _ in range(200):
                self.assertEqual(contract_problems(message(display.packet())[1]), [])
            self.assertLess(time.monotonic() - began, 1.0)
        with self.subTest("huge"):
            broker, said = self.serve("huge")
            display = self.client(broker)
            display.subscribe()
            sizes = []
            while len(sizes) < 400 and (1 << 20) + 19 not in sizes:
                sizes.append(len(display.packet(5.0)[1]))
            # A megabyte behind its topic after five seconds, bundles before it and behind it
            self.assertIn((1 << 20) + 19, sizes[3:])
            self.assertEqual(contract_problems(message(display.packet())[1]), [])
        with self.subTest("length_only"):
            broker, said = self.serve("length_only")
            display = self.client(broker)
            display.subscribe()
            data = display.raw(0.4)
            # After five seconds the head of a message of 500 bytes with its topic, and then nothing - no
            # answer to a ping either: that would be read as a part of the message
            head = b"\x30\xf4\x03\x00\x11display/gx/values"
            self.assertTrue(data.endswith(head), data[-40:])
            display.send(Subscriber.PINGREQ)
            self.assertEqual(display.raw(0.3), b"")

    def test_the_broker_closes(self):
        broker, said = self.serve("close")
        display = self.client(broker)
        display.subscribe()
        began = time.monotonic()
        count = display.until_closed()
        # After ten seconds of its clock, with the bundles of that time delivered
        self.assertTrue(display.closed)
        self.assertTrue(8 <= count <= 11, count)
        self.assertTrue(0.15 <= time.monotonic() - began <= 1.5)

    def test_pieces(self):
        # As a link that delivers a few bytes at a time: the stream is the same, cut anywhere
        broker, said = self.serve(pieces=5, pause=0.2)
        display = self.client(broker)
        self.assertEqual(display.subscribe(keepalive=False), ((0x20, b"\x00\x00"), (0x90, b"\x00\x01\x00")))
        self.assertEqual(contract_problems(message(display.packet(10.0))[1]), [])
        # About 800 bytes arrived in pieces of five: far more reads than a whole bundle takes
        self.assertGreater(len(display.pieces), 40)
        self.assertLessEqual(sorted(display.pieces)[len(display.pieces) // 2], 5)

    def test_hold(self):
        # Nothing is written from second 5 on for 10 s, as over a radio link that stands; then all of it at once
        broker, said = self.serve(hold=(5.0, 10.0))
        display = self.client(broker)
        display.subscribe()
        times = []
        arrived = []
        while len(times) < 20:
            packet = display.packet()
            times.append(read(message(packet)[1])["t"])
            arrived.append(time.monotonic())
        # No bundle is lost, and they come in their order
        self.assertEqual([later - earlier for earlier, later in zip(times, times[1:])], [1000] * 19)
        # One gap of about ten seconds of the clock of the flow, and behind it what piled up in one go
        gaps = [(later - earlier) * FAST for earlier, later in zip(arrived, arrived[1:])]
        long = [gap for gap in gaps if gap > 5]
        self.assertEqual(len(long), 1, gaps)
        self.assertTrue(8 <= long[0] <= 14, long)
        behind = gaps[gaps.index(long[0]) + 1:gaps.index(long[0]) + 8]
        self.assertLess(max(behind), 3.0, gaps)

    def test_late(self):
        # From second 15 of every half minute on each bundle leaves twelve seconds after it was made
        broker, said = self.serve("late")
        display = self.client(broker)
        display.subscribe(keepalive=False)
        late = []
        while len(late) < 32:
            packet = display.packet()
            made = read(message(packet)[1])["t"] / 1000
            late.append((made, time.monotonic() * FAST - broker.flow.began - made))
        on_time = [delay for made, delay in late if made % 30 < 15]
        held = [delay for made, delay in late if made % 30 >= 15]
        self.assertTrue(on_time and held)
        self.assertEqual(mock_flow.LATE_S, 12.0)
        self.assertGreater(min(held), mock_flow.LATE_S - 1, late)
        # The bundles behind the held ones wait in their order: none overtakes
        self.assertEqual([made for made, delay in late], sorted(made for made, delay in late))
        self.assertLess(min(on_time), 2.0, late)

    def test_a_client_that_falls_silent_is_dropped(self):
        # As a broker does: one and a half keep-alives without a packet, and the connection ends. A display
        # pings every ten seconds.
        broker, said = self.serve()
        display = self.client(broker)
        display.subscribe()
        began = time.monotonic()
        display.until_closed()
        self.assertTrue(display.closed)
        self.assertTrue(40 <= (time.monotonic() - began) * FAST <= 90, (time.monotonic() - began) * FAST)
        self.assertTrue(any("silent for more than 45 s" in line for line in said), said)


if __name__ == "__main__":
    unittest.main()
