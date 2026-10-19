/*
 * Host test for display/components/core/cerbo.c. Run "make test_cerbo && ./test_cerbo" in display/test.
 * redproof.py removes or weakens every rule once (mutations/cerbo.py) and expects this test to fail.
 *
 * The test is the platform and the broker of the source: it carries out what cerbo_next() orders, and it
 * builds what a broker sends (mqtt_harness.h; the core cannot build a PUBLISH). What the display sends is
 * compared with the hand-written bytes of fixtures/mqtt_*.txt, the bundles of the Cerbo are the hand-written
 * fixtures/gx_values_*.json or are written where a scene needs them.
 *
 * The scenes use the same times unless they say otherwise: the display starts at 1000 and joins a network at
 * 5000. setup() ends with a session: CERBO_DO_OPEN at 5000, the connection stands at 5100, the CONNACK comes
 * at 5200 and the SUBACK at 5300 - so the first PINGREQ is due at 15300.
 */
#include <stdint.h>
#include <inttypes.h>
#include <float.h>
#include <math.h>
#include <unistd.h>
#include <sys/wait.h>
#include "test.h"
#include "mqtt_harness.h"
#include "cerbo.h"

#define GUARD       0xA5
#define HOST        "192.168.88.2"
#define OTHER       "192.168.88.9"
#define NAME        "WiCAN-Display-C9A8"
#define COUNT(a)    (sizeof(a) / sizeof((a)[0]))
#define ONE         "\"GX_A\":1"

// The source lies between guard bytes: a byte written before or behind it lands there and is seen, where
// the address sanitizer would only stop the program without naming a rule
typedef struct
{
	unsigned char before[128];
	cerbo_t cerbo;
	unsigned char behind[128];
} box_t;

static box_t box, snapshot;
static cerbo_t *const cerbo = &box.cerbo;
// Two tokens more than the reader is told of show whether it writes behind its room
static json_token_t work[CERBO_TOKENS + 2];
static uint64_t now;
static uint8_t wire[8192];
static char text[4096];
static bool written = true;         // every text of the test fitted into its room

static bool guards_intact(void)
{
	const unsigned char *behind_work = (const unsigned char *)&work[CERBO_TOKENS];
	size_t i;

	for(i = 0; i < sizeof(box.before); i++)
	{
		if(box.before[i] != GUARD || box.behind[i] != GUARD) return false;
	}
	for(i = 0; i < 2 * sizeof(json_token_t); i++)
	{
		if(behind_work[i] != 0x5A) return false;
	}
	return true;
}

// A display that starts at `start`
static void begin_at(uint64_t start, const char *host, bool safe_mode, const char *name)
{
	memset(&box, GUARD, sizeof(box));
	memset(work, 0x5A, sizeof(work));
	now = start;
	cerbo_init(cerbo, host, safe_mode, name, now);
}

static void begin(void)
{
	begin_at(1000, HOST, false, NAME);
}

static cerbo_do_t next(void)
{
	return cerbo_next(cerbo, now);
}

static cerbo_do_t next_at(uint64_t at)
{
	now = at;
	return next();
}

static cerbo_view_t view(void)
{
	return cerbo_view(cerbo, now);
}

static void feed(const uint8_t *bytes, size_t length)
{
	cerbo_bytes(cerbo, bytes, length, now, work, CERBO_TOKENS);
}

static void connack(uint8_t code)
{
	feed(wire, broker_connack(wire, 0, code));
}

static void suback(uint16_t id, uint8_t code)
{
	feed(wire, broker_suback(wire, id, code));
}

static void pingresp(void)
{
	feed(wire, broker_pingresp(wire));
}

static void publish(const char *topic, bool retained, const char *json)
{
	if(strlen(json) + 64 > sizeof(wire))
	{
		written = false;
		return;
	}
	feed(wire, broker_publish(wire, retained, topic, json, strlen(json)));
}

// A message on the topic of the source
static void say(const char *json)
{
	publish(CERBO_TOPIC, false, json);
}

// The text of a bundle of version 1 with these members of `values`
static const char *bundle_text(uint32_t boot, int64_t t, const char *values)
{
	int length = snprintf(text, sizeof(text), "{\"v\":1,\"boot\":%" PRIu32 ",\"t\":%" PRId64 ",\"values\":{%s}}", boot, t, values);

	if(length < 0 || (size_t)length >= sizeof(text)) written = false;
	return text;
}

static void bundle(uint32_t boot, int64_t t, const char *values)
{
	say(bundle_text(boot, t, values));
}

// Appends a bundle to the `length` bytes on the wire, for a scene that feeds more than one packet at once.
// Returns the new length.
static size_t wire_bundle(size_t length, uint32_t boot, int64_t t, const char *values)
{
	const char *json = bundle_text(boot, t, values);

	return length + broker_publish(wire + length, false, CERBO_TOPIC, json, strlen(json));
}

// A bundle with the one value GX_A arrives at `at`. Returns its lag.
static uint32_t arrive(uint64_t at, uint32_t boot, int64_t t)
{
	now = at;
	bundle(boot, t, ONE);
	return cerbo->lag_ms;
}

// The bytes of the last CERBO_DO_SEND are those of the fixture
static bool out_is(const char *path)
{
	uint8_t expected[MQTT_SUB_OUT_MAX];
	size_t expected_length = 0, length = 99;
	const uint8_t *out = cerbo_out(cerbo, &length);

	return read_hex(path, expected, sizeof(expected), &expected_length) && length == expected_length && memcmp(out, expected, length) == 0;
}

// What a broker reads in the bytes of the last CERBO_DO_SEND
static client_packet_t out_read(void)
{
	size_t length = 0;
	const uint8_t *out = cerbo_out(cerbo, &length);

	return broker_reads(out, length);
}

// A connection from the order to open at `at` to the session 300 ms later. false if an order did not come.
static bool connect_at(uint64_t at)
{
	bool right = true;

	now = at;
	if(next() != CERBO_DO_OPEN) right = false;
	now = at + 100;
	cerbo_opened(cerbo, now);
	if(next() != CERBO_DO_SEND) right = false;
	now = at + 200;
	connack(0);
	if(next() != CERBO_DO_SEND) right = false;
	now = at + 300;
	suback(CERBO_PACKET_ID, 0);
	return right && cerbo->phase == CERBO_SESSION;
}

// The display in a session since 5300
static bool setup(void)
{
	begin();
	now = 5000;
	cerbo_network(cerbo, true, now);
	return connect_at(5000);
}

// The places of a connection
enum
{
	AT_OPENING,             // CERBO_DO_OPEN was handed out at 5000
	AT_GREETING_UNSENT,     // the connection stands since 5100, CONNECT waits
	AT_GREETING,            // CONNECT was handed out
	AT_SUBSCRIBING_UNSENT,  // the CONNACK came at 5200, SUBSCRIBE waits
	AT_SUBSCRIBING,         // SUBSCRIBE was handed out
	AT_SESSION,             // the SUBACK came at 5300
	AT_SESSION_PING,        // a PINGREQ was handed out at 15300
	PLACES,
};

static bool place(int where)
{
	begin();
	now = 5000;
	cerbo_network(cerbo, true, now);
	if(next() != CERBO_DO_OPEN) return false;
	if(where == AT_OPENING) return cerbo->phase == CERBO_OPENING;
	now = 5100;
	cerbo_opened(cerbo, now);
	if(where == AT_GREETING_UNSENT) return cerbo->phase == CERBO_GREETING;
	if(next() != CERBO_DO_SEND) return false;
	if(where == AT_GREETING) return cerbo->phase == CERBO_GREETING;
	now = 5200;
	connack(0);
	if(where == AT_SUBSCRIBING_UNSENT) return cerbo->phase == CERBO_SUBSCRIBING;
	if(next() != CERBO_DO_SEND) return false;
	if(where == AT_SUBSCRIBING) return cerbo->phase == CERBO_SUBSCRIBING;
	now = 5300;
	suback(CERBO_PACKET_ID, 0);
	if(where == AT_SESSION) return cerbo->phase == CERBO_SESSION;
	now = 15300;
	return next() == CERBO_DO_SEND && cerbo->pings_open == 1;
}

static const value_t *value(const char *name)
{
	return values_find(&cerbo->values, name);
}

static bool number_is(const char *name, double expected)
{
	const value_t *found = value(name);

	return found != NULL && found->kind == VALUE_NUMBER && found->number == expected;
}

static bool state_is(const char *name, value_kind_t kind)
{
	const value_t *found = value(name);

	return found != NULL && found->kind == kind && found->number == 0;
}

static uint64_t seen(const char *name)
{
	const value_t *found = value(name);

	return found != NULL ? found->seen_ms : UINT64_MAX;
}

static value_age_t age_at(const char *name, uint64_t at)
{
	return cerbo_age(value(name), at);
}

static uint32_t refused(void)
{
	return cerbo->refused_invalid + cerbo->refused_version + cerbo->refused_retained + cerbo->refused_topic + cerbo->refused_long;
}

static void remember(void)
{
	memcpy(&snapshot, &box, sizeof(box));
}

// Everything a bundle that is taken changes is as it was remembered, and the connection is not to be closed
static bool nothing_taken(void)
{
	const cerbo_t *was = &snapshot.cerbo;

	return memcmp(&was->values, &cerbo->values, sizeof(values_t)) == 0 && was->has_bundle == cerbo->has_bundle && was->bundle_ms == cerbo->bundle_ms &&
	       was->has_boot == cerbo->has_boot && was->boot == cerbo->boot && was->has_clock == cerbo->has_clock && was->t_ms == cerbo->t_ms &&
	       was->lag_ms == cerbo->lag_ms && was->yield_rest == cerbo->yield_rest && was->quiet_since_ms == cerbo->quiet_since_ms &&
	       was->bundles == cerbo->bundles && was->boots == cerbo->boots && was->skipped == cerbo->skipped && was->distrusted == cerbo->distrusted &&
	       was->name_count == cerbo->name_count && memcmp(was->names, cerbo->names, sizeof(cerbo->names)) == 0 &&
	       was->phase == cerbo->phase && !cerbo->failed;
}

// The message was refused for the reason `counter` counts: that counter moved by one, no other one did, and
// nothing else changed
static bool refuses_on(const char *topic, bool retained, const char *json, const uint32_t *counter)
{
	uint32_t before = *counter;
	uint32_t all = refused();
	bool newer = cerbo->newer;

	remember();
	publish(topic, retained, json);
	return nothing_taken() && *counter == before + 1 && refused() == all + 1 && (cerbo->newer == newer || counter == &cerbo->refused_version);
}

static bool refuses(const char *json, const uint32_t *counter)
{
	return refuses_on(CERBO_TOPIC, false, json, counter);
}

// The bundle with these members of `values` was taken: `count` values are in the store, `skipped` more
// members were passed over than before
static bool takes(const char *values, int count, uint32_t skipped)
{
	uint32_t bundles = cerbo->bundles, before = cerbo->skipped, all = refused();

	// With t at the pace of the display no bundle has a lag
	bundle(7, (int64_t)now, values);
	return cerbo->bundles == bundles + 1 && cerbo->values.count == count && cerbo->skipped == before + skipped && refused() == all && !cerbo->failed;
}

/* ---------------------------------------------------------------------------------------------------
 * The examples
 */

static void test_constants(void)
{
	static const uint32_t waits[] = CERBO_RETRY_MS;

	check(strcmp(CERBO_PREFIX, "GX_") == 0 && strcmp(CERBO_TOPIC, "display/gx/values") == 0 && CERBO_VERSION == 1 && CERBO_PORT == 1883 && CERBO_PACKET_ID == 1,
	      "the keys begin with GX_, the topic is display/gx/values, the contract has version 1, the broker listens on 1883");
	check(CERBO_HOST_SIZE == 16 && CERBO_NAME_SIZE == 33 && strcmp(CERBO_CLIENT, "WiCANDisplay") == 0 && CERBO_TOKENS == 256 && CERBO_READ_MAX == 1024 && CERBO_TURN_MS == 20,
	      "an address has up to 15 bytes, a name up to 32; 256 tokens, 1024 bytes a report, 20 ms between two reads");
	check(CERBO_BUNDLE_MAX == 1400 && CERBO_BOOT_MAX == 2147483647 && CERBO_T_MAX == 9007199254740991, "a bundle has up to 1400 bytes, boot goes to 2^31-1, t to 2^53-1");
	check(CERBO_KEEPALIVE_S == 30 && CERBO_PING_MS == 10000 && CERBO_PONG_MS == 20000 && CERBO_SESSION_MS == 15000 && CERBO_GRACE_MS == 15000,
	      "keep alive 30 s, a PINGREQ every 10 s, 20 s for its answer, 15 s to a session, 15 s of waiting before not reachable");
	check(COUNT(waits) == 4 && waits[0] == 2000 && waits[1] == 5000 && waits[2] == 10000 && waits[3] == 30000, "the waits after a failed attempt are 2, 5, 10 and 30 s");
	check(CERBO_FRESH_MS == 5000 && CERBO_KEPT_MS == 60000 && CERBO_LIVE_MS == 10000 && CERBO_SILENT_MS == 20000,
	      "fresh below 5 s, kept below 60 s, live below 10 s, silent from 20 s on");
	check(CERBO_YIELD_PER_MILLE == 100 && CERBO_LAG_MAX_MS == 45000, "the base yields 100 ms a second, a lag above 45 s is not believed");
	check(CERBO_PING_MS + CERBO_PONG_MS <= CERBO_LAG_MAX_MS, "what arrives on a session that lives waited no longer than a lag that is believed");
	check(2 * CERBO_PING_MS < 1500u * CERBO_KEEPALIVE_S, "the PINGREQ come often enough for the keep alive time the broker was told");
	check(VALUE_NAME_SIZE == 33 && VALUES_MAX == 64, "the store holds 64 values with names of up to 32 bytes (values.h)");
	printf("  sizeof(cerbo_t) is %lu: the reader %lu, the values %lu\n", (unsigned long)sizeof(cerbo_t), (unsigned long)sizeof(mqtt_sub_reader_t), (unsigned long)sizeof(values_t));
	check(sizeof(void *) != 8 || sizeof(cerbo_t) == 7568, "cerbo_t has 7568 bytes (on a 64 bit host): about 7.4 KB of the external RAM");
	check(CERBO_NAMES_MAX == 64 && sizeof(((cerbo_t *)0)->names) == 2112 && sizeof(((cerbo_t *)0)->names[0]) == VALUE_NAME_SIZE,
	      "the source keeps the names of 64 keys, of up to 32 bytes each: 2112 bytes of the 7568");
}

static void test_off(void)
{
	static const uint64_t times[] = {1000, 5000, 5001, 20000, 100000, 4000000000ull};
	bool never = true;
	size_t i;

	begin_at(1000, "", false, NAME);
	cerbo_network(cerbo, true, 5000);
	for(i = 0; i < COUNT(times); i++)
	{
		if(next_at(times[i]) != CERBO_DO_NOTHING) never = false;
	}
	check(never && !cerbo_on(cerbo) && view() == CERBO_VIEW_OFF && cerbo_host(cerbo)[0] == '\0', "without an address there is never an order, and the source is off");

	begin_at(1000, HOST, true, NAME);
	cerbo_network(cerbo, true, 5000);
	never = true;
	for(i = 0; i < COUNT(times); i++)
	{
		if(next_at(times[i]) != CERBO_DO_NOTHING) never = false;
	}
	check(never && !cerbo_on(cerbo) && view() == CERBO_VIEW_OFF, "in safe mode there is never an order, whatever address is stored");
	cerbo_set_host(cerbo, OTHER, now);
	cerbo_network(cerbo, false, now);
	cerbo_network(cerbo, true, now);
	check(next() == CERBO_DO_NOTHING && next_at(now + 100000) == CERBO_DO_NOTHING && !cerbo_on(cerbo) && view() == CERBO_VIEW_OFF,
	      "in safe mode a new address and a new network open nothing either");

	begin();
	never = true;
	for(i = 0; i < COUNT(times); i++)
	{
		if(next_at(times[i]) != CERBO_DO_NOTHING) never = false;
	}
	check(never && cerbo_on(cerbo) && view() == CERBO_VIEW_NO_WIFI, "without a network there is never an order");

	begin_at(1000, NULL, false, NAME);
	cerbo_network(cerbo, true, 5000);
	check(!cerbo_on(cerbo) && next_at(5000) == CERBO_DO_NOTHING && cerbo_host(cerbo)[0] == '\0', "no address text is no address");
	begin_at(1000, "192.168.100.100", false, NAME);
	check(cerbo_on(cerbo) && strcmp(cerbo_host(cerbo), "192.168.100.100") == 0, "an address of 15 bytes is taken");
	begin_at(1000, "192.168.100.1000", false, NAME);
	cerbo_network(cerbo, true, 5000);
	check(!cerbo_on(cerbo) && cerbo_host(cerbo)[0] == '\0' && next_at(5000) == CERBO_DO_NOTHING, "an address of 16 bytes is none: it is not cut short");
	begin_at(1000, "an address that is far too long to be one", false, NAME);
	check(!cerbo_on(cerbo) && cerbo_host(cerbo)[0] == '\0' && guards_intact(), "a long text is no address, and nothing is written outside the source");
}

static void test_connect(void)
{
	uint8_t bytes[16];
	size_t length = 0;

	begin();
	check(cerbo_on(cerbo) && cerbo->phase == CERBO_IDLE && cerbo->values.count == 0 && next() == CERBO_DO_NOTHING && next_at(4999) == CERBO_DO_NOTHING,
	      "a display that starts has no connection and no value");
	now = 5000;
	cerbo_network(cerbo, true, now);
	check(next() == CERBO_DO_OPEN && strcmp(cerbo_host(cerbo), HOST) == 0 && cerbo->phase == CERBO_OPENING, "with an address and a network the order to open comes at once");
	check(next() == CERBO_DO_NOTHING && next_at(5050) == CERBO_DO_NOTHING && next_at(5099) == CERBO_DO_NOTHING, "the order to open comes exactly once");

	now = 5100;
	cerbo_opened(cerbo, now);
	check(next() == CERBO_DO_SEND && out_is("fixtures/mqtt_connect.txt"), "after the opening CONNECT goes out: the bytes of the fixture, with the keep alive time of 30 s");
	check(next() == CERBO_DO_NOTHING && next_at(5150) == CERBO_DO_NOTHING && out_is("fixtures/mqtt_connect.txt"), "CONNECT goes out exactly once, and its bytes hold");
	check(cerbo->phase == CERBO_GREETING && view() == CERBO_VIEW_CONNECTING, "a connection that stands is no session yet");

	now = 5200;
	check(read_hex("fixtures/mqtt_connack.txt", bytes, sizeof(bytes), &length), "fixture of a CONNACK");
	feed(bytes, length);
	check(next() == CERBO_DO_SEND && out_is("fixtures/mqtt_subscribe.txt"), "after a CONNACK that accepts SUBSCRIBE goes out: the bytes of the fixture");
	check(next() == CERBO_DO_NOTHING && next_at(5250) == CERBO_DO_NOTHING && out_is("fixtures/mqtt_subscribe.txt"), "SUBSCRIBE goes out exactly once");
	check(cerbo->phase == CERBO_SUBSCRIBING && cerbo->sessions == 0 && view() == CERBO_VIEW_CONNECTING, "a CONNACK makes no session yet");

	now = 5300;
	check(read_hex("fixtures/mqtt_suback.txt", bytes, sizeof(bytes), &length), "fixture of a SUBACK");
	feed(bytes, length);
	check(cerbo->phase == CERBO_SESSION && cerbo->sessions == 1 && view() == CERBO_VIEW_QUIET && next() == CERBO_DO_NOTHING, "with the SUBACK the session stands");
	check(setup() && cerbo->sessions == 1 && view() == CERBO_VIEW_QUIET, "setup() of this test ends in a session");
	check(guards_intact(), "connecting writes nothing outside the source");
}

// The client id of the CONNECT a display with this name sends, NULL if none goes out
static const char *client_of(const char *name)
{
	static client_packet_t read;

	begin_at(1000, HOST, false, name);
	now = 5000;
	cerbo_network(cerbo, true, now);
	if(next() != CERBO_DO_OPEN) return NULL;
	cerbo_opened(cerbo, now);
	if(next() != CERBO_DO_SEND) return NULL;
	read = out_read();
	return read.type == 1 && read.flags == 0x02 && read.keepalive_s == CERBO_KEEPALIVE_S ? read.text : NULL;
}

static bool client_is(const char *name, const char *expected)
{
	const char *client = client_of(name);

	return client != NULL && strcmp(client, expected) == 0;
}

static void test_name(void)
{
	check(client_is(NAME, "WiCANDisplayC9A8") && client_is("Bus 7", "Bus7"), "the client id is made of the name of the display");
	check(client_is("----", "WiCANDisplay") && client_is("", "WiCANDisplay") && client_is("\xC3\xA4 \xC3\xB6", "WiCANDisplay"),
	      "a name without a letter or digit: the display connects as WiCANDisplay");
	check(client_is(NULL, "WiCANDisplay"), "no name: the display connects as WiCANDisplay");
	check(client_is("------------------------------abcd", "ab"), "of a name longer than 32 bytes the first 32 count");
	check(client_is("--------------------------------ab", "WiCANDisplay"), "a name whose first 32 bytes have no letter or digit: WiCANDisplay");
	check(client_is("-------------------------------a", "a") && guards_intact(), "a name of 32 bytes counts to its last byte, and nothing is written outside the source");
}

static void test_opened(void)
{
	begin();
	cerbo_opened(cerbo, 2000);
	now = 5000;
	cerbo_network(cerbo, true, now);
	check(next() == CERBO_DO_OPEN && cerbo->phase == CERBO_OPENING && next() == CERBO_DO_NOTHING, "a report of an opening nobody ordered is ignored");
	check(place(AT_GREETING) && (cerbo_opened(cerbo, 5150), next()) == CERBO_DO_NOTHING && cerbo->phase == CERBO_GREETING, "a second report of the opening sends no second CONNECT");
	check(place(AT_SUBSCRIBING) && (cerbo_opened(cerbo, 5250), next()) == CERBO_DO_NOTHING && cerbo->phase == CERBO_SUBSCRIBING, "a report of the opening while subscribing is ignored");
	check(place(AT_SESSION) && (cerbo_opened(cerbo, 5350), next()) == CERBO_DO_NOTHING && cerbo->phase == CERBO_SESSION, "a report of the opening in a session is ignored");
}

/*
 * Every packet at every place of a connection. The letters: A a CONNACK that accepts, a one that refuses,
 * S a SUBACK that accepts, s one that refuses, q one that grants QoS 1, i one with another packet identifier,
 * P a PINGRESP, B a bundle, T a message on another topic, L a message too long for the reader, X broken bytes.
 */
static void send_letter(char letter)
{
	static char long_text[2001];

	switch(letter)
	{
		case 'A': connack(0); break;
		case 'a': connack(5); break;
		case 'S': suback(CERBO_PACKET_ID, 0); break;
		case 's': suback(CERBO_PACKET_ID, 0x80); break;
		case 'q': suback(CERBO_PACKET_ID, 1); break;
		case 'i': suback(CERBO_PACKET_ID + 1, 0); break;
		case 'P': pingresp(); break;
		case 'B': bundle(7, 1000, ONE); break;
		case 'T': publish("display/gx/other", false, bundle_text(7, 1000, ONE)); break;
		case 'L':
			memset(long_text, ' ', sizeof(long_text) - 1);
			say(long_text);
			break;
		default: feed((const uint8_t *)"\xE0\x00", 2); break;
	}
}

// At `where` every packet of `closing` gets the connection closed, and no packet of `staying` does
static bool place_takes(int where, const char *closing, const char *staying)
{
	const char *letter;

	for(letter = closing; *letter != '\0'; letter++)
	{
		if(!place(where)) return false;
		send_letter(*letter);
		if(next() != CERBO_DO_CLOSE || cerbo->phase != CERBO_IDLE) return false;
	}
	for(letter = staying; *letter != '\0'; letter++)
	{
		if(!place(where)) return false;
		send_letter(*letter);
		if(cerbo->failed || next() == CERBO_DO_CLOSE || cerbo->phase == CERBO_IDLE) return false;
	}
	return true;
}

static bool closes(int where, char letter)
{
	char closing[2] = {letter, '\0'};

	return place_takes(where, closing, "");
}

static void test_packets(void)
{
	size_t length;

	check(closes(AT_GREETING, 'a'), "a CONNACK that refuses closes");
	check(closes(AT_SUBSCRIBING, 's'), "a SUBACK that refuses closes");
	check(closes(AT_SUBSCRIBING, 'q'), "a SUBACK that grants QoS 1 closes: QoS 0 was asked for");
	check(closes(AT_SUBSCRIBING, 'i'), "a SUBACK with another packet identifier closes");
	check(closes(AT_SUBSCRIBING, 'A') && closes(AT_SUBSCRIBING_UNSENT, 'A'), "a second CONNACK closes");
	check(closes(AT_SESSION, 'A') && closes(AT_SESSION, 'a'), "a CONNACK in a session closes");
	check(closes(AT_GREETING_UNSENT, 'A'), "a CONNACK before CONNECT was handed out closes: it answers nothing");
	check(closes(AT_GREETING, 'S') && closes(AT_GREETING_UNSENT, 'S'), "a SUBACK without a SUBSCRIBE closes");
	check(closes(AT_SUBSCRIBING_UNSENT, 'S'), "a SUBACK before SUBSCRIBE was handed out closes");
	check(closes(AT_SESSION, 'S') && closes(AT_SESSION_PING, 'S'), "a second SUBACK closes");
	check(closes(AT_SESSION, 'P'), "a PINGRESP nobody asked for closes");
	check(closes(AT_GREETING, 'P') && closes(AT_SUBSCRIBING, 'P'), "a PINGRESP before the session closes");
	check(closes(AT_GREETING, 'B') && closes(AT_GREETING_UNSENT, 'B'), "a PUBLISH before the CONNACK closes");
	check(closes(AT_SUBSCRIBING_UNSENT, 'B'), "a PUBLISH before SUBSCRIBE was handed out closes: the display has not subscribed");
	check(closes(AT_GREETING, 'L') && closes(AT_SUBSCRIBING_UNSENT, 'L') && closes(AT_GREETING, 'T'), "a PUBLISH before the display subscribed closes, whatever it carries");
	check(closes(AT_SESSION, 'X') && closes(AT_GREETING, 'X') && closes(AT_SUBSCRIBING, 'X') && closes(AT_SESSION_PING, 'X'), "broken bytes close");
	check(connect_at(now + 2000) && next() == CERBO_DO_NOTHING && cerbo->phase == CERBO_SESSION && !cerbo->failed && (bundle(7, 1000, ONE), cerbo->bundles) == 1,
	      "the connection behind one that was closed for broken bytes is a fresh one");

	check(place_takes(AT_GREETING_UNSENT, "AaSsqiPBTLX", ""), "before CONNECT was handed out every packet is out of place");
	check(place_takes(AT_GREETING, "aSsqiPBTLX", "A"), "while greeting every packet but a CONNACK that accepts closes");
	check(place_takes(AT_SUBSCRIBING_UNSENT, "AaSsqiPBTLX", ""), "before SUBSCRIBE was handed out every packet is out of place");
	check(place_takes(AT_SUBSCRIBING, "AasqiPX", "SBTL"), "while subscribing a SUBACK that accepts and every PUBLISH are in place, nothing else");
	check(place_takes(AT_SESSION, "AaSsqiPX", "BTL"), "in a session a PUBLISH is in place, and nothing else while no PINGREQ waits");
	check(place_takes(AT_SESSION_PING, "AaSsqiX", "PBTL"), "in a session with a PINGREQ under way a PINGRESP is in place as well");

	// Without a connection that stands nothing is read
	check(place(AT_OPENING) && (send_letter('X'), send_letter('A'), send_letter('S'), next()) == CERBO_DO_NOTHING && cerbo->phase == CERBO_OPENING && !cerbo->failed,
	      "bytes before the connection stands are ignored");
	cerbo_opened(cerbo, now);
	check(next() == CERBO_DO_SEND && (connack(0), next()) == CERBO_DO_SEND && (suback(CERBO_PACKET_ID, 0), cerbo->phase) == CERBO_SESSION,
	      "... and the connection goes on as if they had not come");
	begin();
	send_letter('B');
	send_letter('X');
	now = 5000;
	cerbo_network(cerbo, true, now);
	check(cerbo->bundles == 0 && next() == CERBO_DO_OPEN && next() == CERBO_DO_NOTHING && !cerbo->failed, "bytes without a connection are ignored");

	// What follows a packet that closes is not read
	check(place(AT_SESSION), "a session");
	length = wire_bundle(broker_connack(wire, 0, 0), 7, 1000, ONE);
	feed(wire, length);
	check(cerbo->bundles == 0 && cerbo->values.count == 0 && cerbo->failed, "a bundle behind a packet out of place, in the same bytes, is not taken");
	send_letter('B');
	send_letter('P');
	check(cerbo->bundles == 0 && cerbo->values.count == 0 && cerbo->pong_ms == 0, "bytes that come before the order to close was handed out are not read either");
	check(next() == CERBO_DO_CLOSE && next() == CERBO_DO_NOTHING && next_at(now + 1999) == CERBO_DO_NOTHING && next_at(now + 1) == CERBO_DO_OPEN,
	      "the order to close comes once, and the next attempt follows after the wait");

	check(place(AT_SUBSCRIBING), "a display that subscribes");
	length = wire_bundle(broker_suback(wire, CERBO_PACKET_ID, 0x80), 7, 1000, ONE);
	feed(wire, length);
	check(cerbo->bundles == 0 && cerbo->sessions == 0 && next() == CERBO_DO_CLOSE, "a bundle behind a SUBACK that refuses is not taken");

	check(place(AT_SESSION), "a session once more");
	memcpy(wire, "\x32\x00", 2);
	length = wire_bundle(2, 7, 1000, ONE);
	feed(wire, length);
	check(cerbo->bundles == 0 && next() == CERBO_DO_CLOSE, "a bundle behind broken bytes is not taken");

	// A new connection begins with a new reader
	check(place(AT_SESSION), "a session whose last bytes are half a packet");
	feed((const uint8_t *)"\x30\x40\x00\x11" "disp", 8);
	cerbo_down(cerbo, now);
	check(connect_at(now + 2000) && cerbo->sessions == 2 && !cerbo->failed, "the reader begins anew with every connection: half a packet of the one before is forgotten");
	bundle(7, 1000, ONE);
	check(cerbo->bundles == 1 && number_is("GX_A", 1) && guards_intact(), "... and the bundle of the new connection is read");
}

static void test_session_time(void)
{
	check(place(AT_OPENING) && next_at(19999) == CERBO_DO_NOTHING && next_at(20000) == CERBO_DO_CLOSE && cerbo->phase == CERBO_IDLE,
	      "a connection that does not stand 15 s after the order to open is closed");
	check(next() == CERBO_DO_NOTHING && next_at(21999) == CERBO_DO_NOTHING && next_at(22000) == CERBO_DO_OPEN, "the next attempt follows 2 s after the order to close");
	check(place(AT_GREETING) && next_at(19999) == CERBO_DO_NOTHING && next_at(20000) == CERBO_DO_CLOSE, "a connection without a CONNACK is closed 15 s after the order to open");
	check(place(AT_SUBSCRIBING) && next_at(19999) == CERBO_DO_NOTHING && next_at(20000) == CERBO_DO_CLOSE,
	      "a connection without a session 15 s after the order to open is closed: the time counts from the order, not from the CONNACK");
	check(place(AT_GREETING_UNSENT) && next_at(20000) == CERBO_DO_CLOSE && next() == CERBO_DO_NOTHING, "when the time is over the connection is closed, also with a CONNECT that waits");
	check(next_at(22000) == CERBO_DO_OPEN && next() == CERBO_DO_NOTHING && next_at(22050) == CERBO_DO_NOTHING, "the CONNECT that waited is gone with its connection: nothing is sent while the next one opens");
	check(place(AT_SUBSCRIBING_UNSENT) && next_at(20000) == CERBO_DO_CLOSE && next() == CERBO_DO_NOTHING, "... and with a SUBSCRIBE that waits");
	check(place(AT_SESSION) && next_at(15299) == CERBO_DO_NOTHING && next_at(20000) == CERBO_DO_SEND && next_at(20001) == CERBO_DO_NOTHING && cerbo->phase == CERBO_SESSION,
	      "a session that stands is not closed 15 s after the order to open");
	check(place(AT_SUBSCRIBING) && (now = 19999, suback(CERBO_PACKET_ID, 0), next_at(20000)) == CERBO_DO_NOTHING && cerbo->phase == CERBO_SESSION,
	      "a SUBACK in the last millisecond makes the session");
}

// The connection that was opened at `opened_at` ends at `ended_at` with a report of the platform. true if the
// next order to open comes `wait` ms later and not a millisecond earlier.
static bool waits_after_down(uint64_t ended_at, uint32_t wait)
{
	cerbo_down(cerbo, ended_at);
	return next_at(ended_at) == CERBO_DO_NOTHING && next_at(ended_at + wait - 1) == CERBO_DO_NOTHING && next_at(ended_at + wait) == CERBO_DO_OPEN;
}

static void test_retry(void)
{
	begin();
	now = 5000;
	cerbo_network(cerbo, true, now);
	check(next() == CERBO_DO_OPEN && waits_after_down(5050, 2000), "after a failed attempt the next one waits 2 s, counted from the report");
	check(waits_after_down(7100, 5000), "after the second failed attempt 5 s");
	check(waits_after_down(12150, 10000), "after the third 10 s");
	check(waits_after_down(22200, 30000), "after the fourth 30 s");
	check(waits_after_down(52250, 30000) && waits_after_down(82300, 30000), "and 30 s after every further one");

	// A session in between
	now = 112300;
	cerbo_opened(cerbo, now);
	check(next() == CERBO_DO_SEND && (connack(0), next()) == CERBO_DO_SEND && (suback(CERBO_PACKET_ID, 0), cerbo->phase) == CERBO_SESSION, "the seventh attempt makes a session");
	check(waits_after_down(113000, 2000), "after a session that stood the waits start from the beginning: 2 s");
	check(waits_after_down(115100, 5000), "... and go on with 5 s");

	// A connection that stood without a session does not start them anew
	now = 120100;
	cerbo_opened(cerbo, now);
	check(next() == CERBO_DO_SEND && (connack(0), next()) == CERBO_DO_SEND && waits_after_down(120200, 10000), "a CONNACK is no session: the waits go on");

	// Closed by an order
	begin();
	now = 5000;
	cerbo_network(cerbo, true, now);
	check(next() == CERBO_DO_OPEN && next_at(20000) == CERBO_DO_CLOSE && next_at(21999) == CERBO_DO_NOTHING && next_at(22000) == CERBO_DO_OPEN &&
	      next_at(36999) == CERBO_DO_NOTHING && next_at(37000) == CERBO_DO_CLOSE && next_at(41999) == CERBO_DO_NOTHING && next_at(42000) == CERBO_DO_OPEN,
	      "an attempt that is closed by an order counts as failed as well: 2 s, then 5 s, from the order");
	cerbo_down(cerbo, 42010);
	cerbo_down(cerbo, 42020);
	cerbo_down(cerbo, 42030);
	check(next_at(52009) == CERBO_DO_NOTHING && next_at(52010) == CERBO_DO_OPEN, "a report without a connection is ignored: the waits do not move for it");
	check(next_at(67010) == CERBO_DO_CLOSE && (cerbo_down(cerbo, 67020), next_at(97009)) == CERBO_DO_NOTHING && next_at(97010) == CERBO_DO_OPEN,
	      "a report behind an order to close is ignored: the wait counts from the order");

	begin();
	cerbo_down(cerbo, 4000);
	now = 5000;
	cerbo_network(cerbo, true, now);
	check(next() == CERBO_DO_OPEN, "a report before any connection delays nothing");
}

static void test_network(void)
{
	// Three failed attempts, the wait is 10 s
	begin();
	now = 5000;
	cerbo_network(cerbo, true, now);
	check(next() == CERBO_DO_OPEN && waits_after_down(5050, 2000) && waits_after_down(7100, 5000), "two failed attempts");
	cerbo_down(cerbo, 12150);
	cerbo_network(cerbo, true, 13000);
	check(next_at(13000) == CERBO_DO_NOTHING && next_at(22149) == CERBO_DO_NOTHING, "the same network once more changes nothing: the wait goes on");
	cerbo_network(cerbo, false, 22149);
	check(next_at(22150) == CERBO_DO_NOTHING && next_at(60000) == CERBO_DO_NOTHING && view() == CERBO_VIEW_NO_WIFI, "without a network nothing is opened, however long the wait is over");
	cerbo_network(cerbo, false, 60000);
	cerbo_network(cerbo, true, 60000);
	check(next_at(60000) == CERBO_DO_OPEN, "with the network an attempt is due at once");
	check(waits_after_down(60050, 2000) && waits_after_down(62100, 5000), "with the network the waits start from the beginning");

	// A network that comes while a wait runs
	begin();
	now = 5000;
	cerbo_network(cerbo, true, now);
	check(next() == CERBO_DO_OPEN && waits_after_down(5050, 2000) && waits_after_down(7100, 5000), "two failed attempts once more");
	cerbo_down(cerbo, 12150);
	cerbo_network(cerbo, false, 12200);
	cerbo_network(cerbo, true, 12300);
	check(next_at(12300) == CERBO_DO_OPEN, "a network that comes back ends the wait that was running");

	// Lost with a connection
	check(setup() && (cerbo_network(cerbo, false, 6000), next_at(6000)) == CERBO_DO_CLOSE && cerbo->phase == CERBO_IDLE && next() == CERBO_DO_NOTHING,
	      "without the network a session is closed");
	check(next_at(100000) == CERBO_DO_NOTHING && (cerbo_network(cerbo, true, 100000), next()) == CERBO_DO_OPEN, "... and opened again at once when the network is back");
	check(place(AT_OPENING) && (cerbo_network(cerbo, false, 5050), next_at(5050)) == CERBO_DO_CLOSE, "without the network a connection that is still opening is closed");
	check(place(AT_SUBSCRIBING_UNSENT) && (cerbo_network(cerbo, false, 5250), next_at(5250)) == CERBO_DO_CLOSE && next() == CERBO_DO_NOTHING,
	      "without the network nothing more is sent: the order to close goes first");
	check(setup() && (cerbo_network(cerbo, false, 6000), cerbo_network(cerbo, true, 6001), next_at(6001)) == CERBO_DO_NOTHING && cerbo->phase == CERBO_SESSION,
	      "a network that was gone and is back before anybody asked leaves the session: if it is dead it ends by its own rules");
	check((cerbo_down(cerbo, 7000), next_at(8999)) == CERBO_DO_NOTHING && next_at(9000) == CERBO_DO_OPEN, "... and is opened anew after the wait");
}

// The session of the fresh connection that follows the order to open handed out last, at now
static bool session_now(void)
{
	cerbo_opened(cerbo, now);
	if(next() != CERBO_DO_SEND) return false;
	connack(0);
	if(next() != CERBO_DO_SEND) return false;
	suback(CERBO_PACKET_ID, 0);
	return cerbo->phase == CERBO_SESSION;
}

static void test_ping(void)
{
	check(setup() && next_at(15299) == CERBO_DO_NOTHING && next_at(15300) == CERBO_DO_SEND && out_is("fixtures/mqtt_pingreq.txt"),
	      "10 s after the SUBACK a PINGREQ goes out: the bytes of the fixture");
	check(next() == CERBO_DO_NOTHING && next_at(15301) == CERBO_DO_NOTHING && cerbo->pings_open == 1, "a PINGREQ goes out exactly once");
	now = 15310;
	pingresp();
	check(cerbo->pings_open == 0 && cerbo->pong_ms == 15310 && !cerbo->failed, "its PINGRESP is noted, with its time");
	check(next_at(25299) == CERBO_DO_NOTHING && next_at(25300) == CERBO_DO_SEND && out_is("fixtures/mqtt_pingreq.txt"),
	      "the next PINGREQ goes out 10 s after the one before, not after its answer");
	check(next_at(35299) == CERBO_DO_NOTHING && next_at(35300) == CERBO_DO_SEND && cerbo->pings_open == 2, "a PINGREQ goes out every 10 s, also while the one before waits for its answer");
	check(next_at(45299) == CERBO_DO_NOTHING && next_at(45300) == CERBO_DO_CLOSE && cerbo->phase == CERBO_IDLE, "a PINGREQ without its answer for 20 s closes the session");
	check(next_at(47299) == CERBO_DO_NOTHING && next_at(47300) == CERBO_DO_OPEN, "the session stood: the next attempt follows after 2 s");
	check(session_now() && next() == CERBO_DO_NOTHING && next_at(57299) == CERBO_DO_NOTHING && next_at(57300) == CERBO_DO_SEND && cerbo->pings_open == 1 && cerbo->phase == CERBO_SESSION,
	      "the next session knows nothing of the PINGREQ of the one before: it lives, and its first PINGREQ goes out after 10 s");
	cerbo_down(cerbo, 58000);
	check(next_at(60000) == CERBO_DO_OPEN && (cerbo_opened(cerbo, now), next()) == CERBO_DO_SEND && (pingresp(), next()) == CERBO_DO_CLOSE,
	      "a PINGRESP while greeting closes, also when a PINGREQ of the session before stayed without its answer");

	// The first of two is answered: the second has its own 20 s
	check(setup() && next_at(15300) == CERBO_DO_SEND && next_at(25300) == CERBO_DO_SEND, "two PINGREQ under way");
	now = 30000;
	pingresp();
	check(cerbo->pings_open == 1 && next_at(35299) == CERBO_DO_NOTHING && next_at(35300) == CERBO_DO_SEND,
	      "the answer to the first of two PINGREQ: 20 s after the first nothing is closed, the third goes out");
	check(next_at(45299) == CERBO_DO_NOTHING && next_at(45300) == CERBO_DO_CLOSE, "the second PINGREQ has its own 20 s");

	check(setup() && next_at(15300) == CERBO_DO_SEND && next_at(25300) == CERBO_DO_SEND, "two PINGREQ under way once more");
	now = 30000;
	pingresp();
	pingresp();
	check(cerbo->pings_open == 0 && !cerbo->failed && next_at(35300) == CERBO_DO_SEND && next_at(45300) == CERBO_DO_SEND && next_at(55299) == CERBO_DO_NOTHING &&
	      next_at(55300) == CERBO_DO_CLOSE, "both answered: the session lives until a later PINGREQ stays without an answer for 20 s");
	check(setup() && next_at(15300) == CERBO_DO_SEND && (now = 15400, pingresp(), pingresp(), cerbo->failed) && next() == CERBO_DO_CLOSE,
	      "a second PINGRESP to one PINGREQ closes");

	// Late calls
	check(setup() && next_at(16000) == CERBO_DO_SEND && next_at(25999) == CERBO_DO_NOTHING && next_at(26000) == CERBO_DO_SEND,
	      "a PINGREQ that was handed out late: the next one is due 10 s after it was handed out");
	check(setup() && next_at(15300) == CERBO_DO_SEND && next_at(40000) == CERBO_DO_CLOSE && next() == CERBO_DO_NOTHING,
	      "after a long pause of the caller the session is closed, not pinged: the order to close goes first");
	check(setup() && next_at(15300) == CERBO_DO_SEND && (now = 35299, pingresp(), next_at(35300)) == CERBO_DO_SEND && cerbo->phase == CERBO_SESSION,
	      "an answer in the last millisecond keeps the session");
	check(setup() && next_at(1000000) == CERBO_DO_SEND && next() == CERBO_DO_NOTHING && cerbo->phase == CERBO_SESSION,
	      "a session without a PINGREQ under way is not closed for silence, however late the caller asks");

	// Only in a session
	check(place(AT_GREETING) && next_at(15300) == CERBO_DO_NOTHING && place(AT_SUBSCRIBING) && next_at(15300) == CERBO_DO_NOTHING, "no PINGREQ before the session stands");
}

static void test_host(void)
{
	check(setup() && arrive(6000, 7, 1000) == 0 && cerbo->values.count == 1 && view() == CERBO_VIEW_LIVE, "a session with a value");
	cerbo_set_host(cerbo, HOST, 6500);
	check(cerbo->values.count == 1 && cerbo->has_bundle && !cerbo->moved && next_at(6500) == CERBO_DO_NOTHING && view() == CERBO_VIEW_LIVE, "the same address once more changes nothing");

	arrive(6600, 7, 4000);
	check(cerbo->lag_ms == 0 && arrive(9600, 7, 5000) == 1700, "a bundle that is late, so that there is a lag to forget");
	cerbo_set_host(cerbo, OTHER, 9700);
	check(cerbo->values.count == 0 && value("GX_A") == NULL && !cerbo->has_bundle && cerbo->lag_ms == 0 && !cerbo_showing(cerbo, 9700),
	      "another address: the values of the broker before are forgotten, and its lag");
	check(cerbo->bundles == 3 && cerbo->sessions == 1 && cerbo->boots == 1, "the counters stay");
	now = 9750;
	bundle(7, 6000, "\"GX_B\":2");
	pingresp();
	check(cerbo->values.count == 0 && cerbo->bundles == 3 && !cerbo->failed, "what the connection to the address before still brings is not read");
	check(view() != CERBO_VIEW_LIVE, "with another address the source does not deliver any more");
	check(next_at(9800) == CERBO_DO_CLOSE, "another address closes the connection first");
	check(next() == CERBO_DO_OPEN && strcmp(cerbo_host(cerbo), OTHER) == 0 && next() == CERBO_DO_NOTHING, "... and the new address is tried at once");
	check(session_now() && view() == CERBO_VIEW_QUIET, "a session with the new broker: nothing is live before its first bundle");
	check(arrive(9900, 7, 6000) == 0 && cerbo->boots == 2 && cerbo->bundles == 4 && cerbo->sessions == 2 && view() == CERBO_VIEW_LIVE,
	      "the same boot number from another broker is another run: counted");

	// Off
	cerbo_set_host(cerbo, "", 10000);
	check(!cerbo_on(cerbo) && cerbo->values.count == 0 && view() == CERBO_VIEW_OFF && next_at(10000) == CERBO_DO_CLOSE, "no address any more: the values are forgotten, the session is closed");
	check(next() == CERBO_DO_NOTHING && next_at(200000) == CERBO_DO_NOTHING && view() == CERBO_VIEW_OFF, "... and nothing is opened again");
	cerbo_set_host(cerbo, HOST, 200000);
	check(next() == CERBO_DO_OPEN && strcmp(cerbo_host(cerbo), HOST) == 0, "an address again: opened at once");
	check(setup() && (cerbo_set_host(cerbo, NULL, 6000), !cerbo_on(cerbo)) && next_at(6000) == CERBO_DO_CLOSE, "no text switches the source off");
	check(setup() && (cerbo_set_host(cerbo, "192.168.100.1000", 6000), !cerbo_on(cerbo)) && cerbo_host(cerbo)[0] == '\0' && next_at(6000) == CERBO_DO_CLOSE,
	      "an address of 16 bytes switches the source off: it is not cut short");
	check(setup() && (cerbo_set_host(cerbo, "192.168.100.100", 6000), cerbo_on(cerbo)) && strcmp(cerbo_host(cerbo), "192.168.100.100") == 0, "an address of 15 bytes is set");

	// While waiting, and while opening
	begin();
	now = 5000;
	cerbo_network(cerbo, true, now);
	check(next() == CERBO_DO_OPEN && waits_after_down(5050, 2000) && waits_after_down(7100, 5000), "two failed attempts");
	cerbo_down(cerbo, 12150);
	cerbo_set_host(cerbo, OTHER, 12200);
	check(next_at(12200) == CERBO_DO_OPEN && strcmp(cerbo_host(cerbo), OTHER) == 0, "another address ends the wait: it is tried at once");
	check(waits_after_down(12250, 2000), "with another address the waits start from the beginning");
	check(place(AT_OPENING) && (cerbo_set_host(cerbo, OTHER, 5050), next_at(5050)) == CERBO_DO_CLOSE && next() == CERBO_DO_OPEN && strcmp(cerbo_host(cerbo), OTHER) == 0,
	      "another address while opening: closed, and opened anew at once");
	check(place(AT_OPENING) && (cerbo_set_host(cerbo, OTHER, 5050), cerbo_opened(cerbo, 5060), next_at(5060)) == CERBO_DO_CLOSE && next() == CERBO_DO_OPEN,
	      "a connection to the address before that stands after all is closed, not greeted");
	check(setup() && (cerbo_set_host(cerbo, OTHER, 6000), cerbo_down(cerbo, 6010), next_at(6010)) == CERBO_DO_OPEN,
	      "the connection to the address before is lost before it was closed: the new address is tried at once all the same");
	check(setup() && (cerbo_set_host(cerbo, OTHER, 6000), cerbo_set_host(cerbo, HOST, 6001), next_at(6001)) == CERBO_DO_CLOSE && next() == CERBO_DO_OPEN &&
	      strcmp(cerbo_host(cerbo), HOST) == 0, "there and back again: the session is closed and opened anew");
	begin();
	cerbo_set_host(cerbo, OTHER, 2000);
	now = 5000;
	check(next() == CERBO_DO_NOTHING && (cerbo_network(cerbo, true, now), next()) == CERBO_DO_OPEN && strcmp(cerbo_host(cerbo), OTHER) == 0 && !cerbo->moved,
	      "another address without a connection: nothing to close");

	// A newer contract is one of the broker before
	check(setup() && (now = 6000, say("{\"v\":2}"), view()) == CERBO_VIEW_NEWER, "a session with a flow of a newer contract");
	cerbo_set_host(cerbo, OTHER, 6100);
	check(next_at(6100) == CERBO_DO_CLOSE && next() == CERBO_DO_OPEN && session_now() && view() == CERBO_VIEW_QUIET, "what was known of the flow of the broker before is forgotten");
	check(guards_intact(), "setting addresses writes nothing outside the source");
}

// The text of a fixture file in `text`
static bool load(const char *path)
{
	return read_fixture(path, text, sizeof(text));
}

static void test_fixtures(void)
{
	static json_token_t tokens[CERBO_TOKENS];

	check(setup() && load("fixtures/gx_values_parked.json"), "fixture of a bundle with all 43 values: the camper parked in the evening");
	check(strlen(text) == 831 && json_parse(text, strlen(text), tokens, CERBO_TOKENS) == 95, "the bundle with 43 values has 831 bytes and needs 95 tokens");
	now = 6000;
	say(text);
	check(cerbo->bundles == 1 && cerbo->values.count == 43 && cerbo->skipped == 0 && refused() == 0 && cerbo->boot == 1836452071 && cerbo->t_ms == 86400123,
	      "the bundle with 43 values is taken: every value, boot and t");
	check(number_is("GX_BATT_SOC", 87.5) && number_is("GX_BATT_V", 26.41) && number_is("GX_BATT_I", -6.2) && number_is("GX_BATT_P", -164) &&
	      number_is("GX_CELL_MIN", 3.298) && number_is("GX_PV_P", 0) && number_is("GX_LTE_RSRP", -98) && number_is("GX_LTE_RX_MB", 18850),
	      "numbers with and without a fraction, negative ones and zero");
	check(state_is("GX_SW_B2B", VALUE_OFF) && state_is("GX_SW_VALVE", VALUE_ON) && state_is("GX_SW_PUMP", VALUE_OFF) && state_is("GX_LTE_CONN", VALUE_ON), "on and off");
	check(strcmp(cerbo->values.items[0].name, "GX_BATT_SOC") == 0 && strcmp(cerbo->values.items[42].name, "GX_LTE_CONN") == 0 && seen("GX_T_BOILER") == 6000 &&
	      seen("GX_LTE_CONN") == 6000, "the values stand in the order of the bundle, each seen when it arrived");
	check(view() == CERBO_VIEW_LIVE && cerbo_showing(cerbo, now), "with a bundle the source delivers");

	check(load("fixtures/gx_values_driving.json") && strlen(text) == 801, "fixture of a bundle with 41 values: under way, two devices have no value");
	now = 7000;
	say(text);
	check(cerbo->bundles == 2 && cerbo->values.count == 41 && cerbo->boots == 2 && cerbo->t_ms == 5000000123 && cerbo->lag_ms == 0, "t above 2^32 is read");
	check(value("GX_AC_OUT_P") == NULL && value("GX_ALT_T_CAN") == NULL, "a bundle replaces the store: what is missing in it is gone");
	check(number_is("GX_BATT_SOC", 71.3) && number_is("GX_T_OUTDOOR", -3.5) && number_is("GX_SPEED", 87) && state_is("GX_SW_B2B", VALUE_ON) && seen("GX_BATT_SOC") == 7000,
	      "... and what it holds is renewed");

	check(load("fixtures/gx_values_few.json"), "fixture of a bundle with six values: most devices are off");
	now = 8000;
	say(text);
	check(cerbo->bundles == 3 && cerbo->values.count == 6 && cerbo->boot == 1 && cerbo->t_ms == 0 && number_is("GX_BATT_SOC", 100) && number_is("GX_T_OUTDOOR", -0.5) &&
	      state_is("GX_LTE_CONN", VALUE_OFF) && value("GX_SPEED") == NULL, "boot 1 and t 0 are read; six values are left");

	check(load("fixtures/gx_values_unknown.json"), "fixture of a bundle with members the contract does not know, on every level");
	now = 9000;
	say(text);
	check(cerbo->bundles == 4 && cerbo->boot == 77 && cerbo->t_ms == 5000 && refused() == 0, "members the contract does not know are passed over: the bundle is taken");
	check(cerbo->values.count == 2 && number_is("GX_BATT_SOC", 55.5) && state_is("GX_SW_B2B", VALUE_ON) && cerbo->skipped == 2,
	      "a member of values that is an object or an array is passed over and counted");
	check(guards_intact(), "the bundles write nothing outside the source");
}

static void test_before_suback(void)
{
	check(place(AT_SUBSCRIBING), "a display that waits for the SUBACK");
	now = 5250;
	bundle(7, 1000, "\"GX_A\":1,\"GX_B\":\"on\"");
	check(cerbo->bundles == 1 && cerbo->values.count == 2 && number_is("GX_A", 1) && seen("GX_A") == 5250 && !cerbo->failed, "a bundle before the SUBACK is taken");
	check(cerbo->phase == CERBO_SUBSCRIBING && view() == CERBO_VIEW_CONNECTING && cerbo_showing(cerbo, now), "it makes no session, but its values show");
	now = 5300;
	suback(CERBO_PACKET_ID, 0);
	check(cerbo->phase == CERBO_SESSION && view() == CERBO_VIEW_LIVE, "with the SUBACK the source delivers at once");
	check(arrive(9300, 7, 2000) == 2645 && seen("GX_A") == 6655, "the lag of the next bundle is measured by the one before the SUBACK");
}

// A bundle with `count` values GX_K00, GX_K01, ... and then the members of `more`
static const char *many(int count, const char *more)
{
	static char values[2048];
	size_t length = 0;
	int i;

	for(i = 0; i < count; i++)
	{
		int added = snprintf(values + length, sizeof(values) - length, "%s\"GX_K%02d\":%d", i > 0 ? "," : "", i, i);

		if(added < 0 || (size_t)added >= sizeof(values) - length)
		{
			written = false;
			break;
		}
		length += (size_t)added;
	}
	if(length + strlen(more) + 1 > sizeof(values)) written = false;
	else strcpy(values + length, more);
	return values;
}

static void test_values(void)
{
	static const char *const taken[] = {
		"GX_A", "GX_0", "GX__", "GX_Z9", "GX_A_B_9", "GX_AZ09_", "GX_12345678901234567890123456789", "GX_\\u0041BC",
	};
	static const char *const passed[] = {
		"A", "ENGINE_RPM", "gx_A", "Gx_A", "gX_A", "GX_a", "GX_Ab", "GX_bA", "GX_A-B", "GX_A B", "GX_A.B", "GX_", "GX", "G", "", "XGX_A", " GX_A", "GX_A ",
		"GX_\xC3\x84", "GX_A\xC3\xA4", "GX_\\u00e4", "GX_A\\u0000", "GX_@", "GX_[", "GX_/", "GX_:", "GX_^", "GX_`", "GX_z", "GX_{",
		"GX_123456789012345678901234567890", "GX_1234567890123456789012345678901234567890",
	};
	static const char *const numbers[] = {"0", "-0", "1", "-4.8", "99.7", "1e3", "1E-2", "12.50", "123456789012", "-0.001", "12345678901234567890123456789012345678901234567"};
	static const double number_values[] = {0, 0, 1, -4.8, 99.7, 1000, 0.01, 12.5, 123456789012.0, -0.001, 12345678901234567890123456789012345678901234567.0};
	static const char *const others[] = {
		"true", "false", "null", "\"On\"", "\"ON\"", "\"o\"", "\"onn\"", "\"of\"", "\"offf\"", "\"\"", "\"1\"", "\" on\"", "\"on \"", "{}", "[]", "{\"on\":1}", "[1]", "[\"on\"]",
		"123456789012345678901234567890123456789012345678",
	};
	char members[256];
	bool right = true;
	size_t i;
	int length;

	check(setup(), "a session for the rules of the values");
	now = 6000;

	// Keys
	for(i = 0; i < COUNT(taken); i++)
	{
		length = snprintf(members, sizeof(members), "\"%s\":5", taken[i]);
		if(length < 0 || (size_t)length >= sizeof(members)) written = false;
		if(!takes(members, 1, 0)) right = false;
	}
	check(right && value("GX_ABC") != NULL, "keys of GX_ and 1 to 29 capitals, digits and underscores are taken, a key of 32 bytes among them, an escaped one as it reads");
	right = true;
	for(i = 0; i < COUNT(passed); i++)
	{
		length = snprintf(members, sizeof(members), "\"GX_FIRST\":1,\"%s\":5,\"GX_LAST\":2", passed[i]);
		if(length < 0 || (size_t)length >= sizeof(members)) written = false;
		if(!takes(members, 2, 1) || !number_is("GX_FIRST", 1) || !number_is("GX_LAST", 2))
		{
			printf("  the key %s was not passed over\n", passed[i]);
			right = false;
		}
	}
	check(right, "a key without the prefix, with small letters, with another character, of the prefix alone or of 33 bytes is passed over and counted; the members around it are taken");
	check(takes("\"ENGINE_RPM\":900,\"GX_ENGINE_RPM\":1", 1, 1) && value("ENGINE_RPM") == NULL && number_is("GX_ENGINE_RPM", 1), "a value named as one of the WiCAN is not kept");
	check(takes("\"GX_123456789012345678901234567890\":1", 0, 1) && takes("\"GX_12345678901234567890123456789\":1", 1, 0) &&
	      number_is("GX_12345678901234567890123456789", 1), "a key of 33 bytes is passed over, one of 32 is kept with every byte");

	// Values
	right = true;
	for(i = 0; i < COUNT(numbers); i++)
	{
		length = snprintf(members, sizeof(members), "\"GX_N\":%s", numbers[i]);
		if(length < 0 || (size_t)length >= sizeof(members)) written = false;
		if(!takes(members, 1, 0) || !number_is("GX_N", number_values[i])) right = false;
	}
	check(right, "numbers are taken: whole ones, fractions, exponents, negative ones, one of 47 characters");
	check(takes("\"GX_N\":1e999,\"GX_M\":-1e999", 2, 0) && number_is("GX_N", INFINITY) && number_is("GX_M", -INFINITY),
	      "a number beyond every measure is kept as it reads: without end (fmt_number() shows a dash)");
	check(takes("\"GX_S\":\"on\",\"GX_T\":\"off\",\"GX_U\":\"\\u006fn\"", 3, 0) && state_is("GX_S", VALUE_ON) && state_is("GX_T", VALUE_OFF) && state_is("GX_U", VALUE_ON),
	      "\"on\" and \"off\" are taken");
	right = true;
	for(i = 0; i < COUNT(others); i++)
	{
		length = snprintf(members, sizeof(members), "\"GX_FIRST\":1,\"GX_X\":%s,\"GX_LAST\":\"on\"", others[i]);
		if(length < 0 || (size_t)length >= sizeof(members)) written = false;
		if(!takes(members, 2, 1) || value("GX_X") != NULL || !state_is("GX_LAST", VALUE_ON))
		{
			printf("  the value %s was not passed over\n", others[i]);
			right = false;
		}
	}
	check(right, "a value that is neither a number nor on or off is passed over and counted: true, null, other texts, objects, arrays, a number of 48 characters");

	// The snapshot
	check(takes("\"GX_A\":1,\"GX_B\":2,\"GX_C\":\"on\"", 3, 0) && (now = 7000, takes("\"GX_B\":22,\"GX_D\":4", 2, 0)), "two bundles, the second without two of the values");
	check(value("GX_A") == NULL && value("GX_C") == NULL && number_is("GX_B", 22) && number_is("GX_D", 4) && seen("GX_B") == 7000 &&
	      strcmp(cerbo->values.items[0].name, "GX_B") == 0, "a bundle replaces the store: what is missing in it is gone at once");
	check((now = 8000, takes("", 0, 0)) && value("GX_B") == NULL && view() == CERBO_VIEW_LIVE && cerbo_showing(cerbo, now), "a bundle without values is a bundle: the store is empty, the source delivers");

	// The same key twice
	check(takes("\"GX_A\":1,\"GX_B\":2,\"GX_A\":3", 2, 0) && number_is("GX_A", 3) && number_is("GX_B", 2), "of two members with the same key the last counts");
	check(takes("\"GX_A\":1,\"GX_A\":\"on\"", 1, 0) && state_is("GX_A", VALUE_ON) && takes("\"GX_A\":\"off\",\"GX_A\":7.5", 1, 0) && number_is("GX_A", 7.5),
	      "the last one counts with its kind: a state leaves no number behind");
	check(takes("\"GX_A\":1,\"GX_A\":true", 1, 1) && number_is("GX_A", 1) && takes("\"GX_A\":null,\"GX_A\":5", 1, 1) && number_is("GX_A", 5),
	      "a member that is passed over does not count as the last one");
	check(takes("\"GX_A\":1,\"GX_\\u0041\":2", 1, 0) && number_is("GX_A", 2), "two keys that read the same are the same key");

	// The room
	check(takes(many(64, ""), 64, 0) && number_is("GX_K00", 0) && number_is("GX_K63", 63), "64 values are kept");
	check(takes(many(65, ""), 64, 1) && number_is("GX_K63", 63) && value("GX_K64") == NULL, "the 65th member is counted, not kept");
	check(takes(many(70, ""), 64, 6) && value("GX_K69") == NULL && value("GX_K64") == NULL, "nor are the ones behind it");
	check(takes(many(65, ",\"GX_K00\":777,\"GX_K63\":\"off\""), 64, 1) && number_is("GX_K00", 777) && state_is("GX_K63", VALUE_OFF),
	      "a key that is in the store needs no room: it is renewed behind the 65th as well");
	check(takes(many(64, ",\"A\":1,\"GX_X\":null"), 64, 2), "members that are passed over need no room");
	check(guards_intact(), "the values write nothing outside the source");
}

// The names the source keeps are these, in this order: the keys without their prefix, with a blank between two
static bool names_are(const char *list)
{
	const char *rest = list;
	int count = 0;
	bool same = true;

	while(*rest != '\0')
	{
		size_t length = strcspn(rest, " ");
		char expected[VALUE_NAME_SIZE] = CERBO_PREFIX;

		if(length + sizeof(CERBO_PREFIX) > sizeof(expected)) return false;
		memcpy(expected + sizeof(CERBO_PREFIX) - 1, rest, length);
		if(count >= cerbo->name_count || strcmp(cerbo->names[count], expected) != 0) same = false;
		count++;
		rest += length;
		if(*rest == ' ') rest++;
	}
	if(count != cerbo->name_count) same = false;
	if(!same)
	{
		printf("  wanted the names %s, got %d:", list, cerbo->name_count);
		for(count = 0; count < cerbo->name_count && count < CERBO_NAMES_MAX; count++) printf(" %s", cerbo->names[count] + sizeof(CERBO_PREFIX) - 1);
		printf("\n");
	}
	return same;
}

// The `count` names from place `from` on are GX_<letter><first>, GX_<letter><first + 1>, ... with two digits each
static bool names_row(int from, char letter, int first, int count)
{
	char expected[VALUE_NAME_SIZE];
	int i;

	for(i = 0; i < count; i++)
	{
		int length = snprintf(expected, sizeof(expected), "GX_%c%02d", letter, first + i);

		if(length < 0 || (size_t)length >= sizeof(expected)) written = false;
		if(from + i >= cerbo->name_count || strcmp(cerbo->names[from + i], expected) != 0)
		{
			printf("  wanted the name %s at place %d of %d, got %s\n", expected, from + i, cerbo->name_count, from + i < cerbo->name_count ? cerbo->names[from + i] : "none");
			return false;
		}
	}
	return true;
}

// Every key of the store is among the names
static bool store_is_named(void)
{
	int i, k;

	for(i = 0; i < cerbo->values.count; i++)
	{
		k = 0;
		while(k < cerbo->name_count && strcmp(cerbo->names[k], cerbo->values.items[i].name) != 0) k++;
		if(k == cerbo->name_count) return false;
	}
	return true;
}

// A bundle with `count` values GX_<letter><first>, GX_<letter><first + 1>, ... and then the members of `more`
static const char *row(char letter, int first, int count, const char *more)
{
	static char values[2048];
	size_t length = 0;
	int i;

	for(i = 0; i < count; i++)
	{
		int added = snprintf(values + length, sizeof(values) - length, "%s\"GX_%c%02d\":%d", i > 0 ? "," : "", letter, first + i, i);

		if(added < 0 || (size_t)added >= sizeof(values) - length)
		{
			written = false;
			break;
		}
		length += (size_t)added;
	}
	if(length + strlen(more) + 1 > sizeof(values)) written = false;
	else strcpy(values + length, more);
	return values;
}

// The keys the source keeps the names of, for the editor of the web interface
static void test_names(void)
{
	static const char first[] = "B A C D E F I H";
	const char *refused_bundle;
	int i;

	// A source that starts over memory that held something
	memset(&box, 0xA5, sizeof(box));
	cerbo_init(cerbo, HOST, false, NAME, 1000);
	check(cerbo->name_count == 0, "a source that starts keeps no name, whatever lay in its memory");

	check(setup() && cerbo->name_count == 0, "a session for the names: none before the first bundle");
	now = 6000;
	check(takes("\"GX_B\":1,\"GX_A\":\"on\",\"GX_C\":-2.5", 3, 0) && names_are("B A C"), "the keys of the first bundle are noted, in the order of the bundle");
	now = 7000;
	check(takes("\"GX_C\":3,\"GX_D\":\"off\",\"GX_B\":4,\"GX_E\":0", 4, 0) && value("GX_A") == NULL && names_are("B A C D E"),
	      "a bundle without one of the keys and with two new ones: the name of the missing key stays where it is, the new ones come behind the others in their order");
	check(takes("", 0, 0) && cerbo->values.count == 0 && names_are("B A C D E"), "a bundle without values empties the store and leaves every name");
	check(takes("\"GX_E\":1,\"GX_F\":1,\"GX_F\":\"on\",\"GX_E\":2", 2, 0) && names_are("B A C D E F"), "a key that stands twice in a bundle is noted once, and one that is noted is not noted again");

	// What no answer names
	check(takes("\"GX_G\":1e999,\"GX_H\":-1e999,\"GX_I\":5", 3, 0) && value("GX_G") != NULL && value("GX_H") != NULL && names_are("B A C D E F I"),
	      "a key whose value is a number beyond a double is in the store and not among the names: GET /api/values does not name it either");
	check(takes("\"GX_G\":1e999,\"GX_H\":7", 2, 0) && names_are(first), "with the first value that has a text its key is noted, behind the others");
	check(takes("\"GX_B\":1e999,\"GX_A\":-1e999", 2, 0) && names_are(first), "a name stays when its key comes with a number without end");
	check(takes("\"gx_J\":1,\"GX_K\":null,\"J\":2,\"GX_L\":true,\"GX_\":3,\"GX_M\":\"On\"", 0, 6) && names_are(first),
	      "a member that is passed over - a key that is none, a value that is none - is not noted");

	// What is refused
	refused_bundle = bundle_text(7, (int64_t)now, "\"GX_R\":1");
	check(refuses_on(CERBO_TOPIC, true, refused_bundle, &cerbo->refused_retained) && refuses_on("display/gx/other", false, refused_bundle, &cerbo->refused_topic) &&
	      refuses("{\"v\":1,\"boot\":0,\"t\":1,\"values\":{\"GX_R\":1}}", &cerbo->refused_invalid) &&
	      refuses("{\"v\":2,\"boot\":7,\"t\":1,\"values\":{\"GX_R\":1}}", &cerbo->refused_version) && names_are(first),
	      "a bundle that is refused - retained, on another topic, with a boot that is none, of a newer contract - notes no key");

	// What takes no name away, and what takes them all
	check(takes("\"GX_B\":1", 1, 0) && cerbo_shown(cerbo, now + CERBO_KEPT_MS) == 0 && names_are(first), "the names stay when the values are gone");
	now += 100;
	cerbo_down(cerbo, now);
	check(cerbo->phase == CERBO_IDLE && names_are(first), "... when the session ends");
	cerbo_network(cerbo, false, now);
	cerbo_network(cerbo, true, now);
	check(names_are(first), "... when the network is lost and joined again");
	cerbo_set_host(cerbo, HOST, now);
	check(names_are(first), "... and when the address it has is set again");
	check(connect_at(now) && takes("\"GX_N\":1", 1, 0) && names_are("B A C D E F I H N"), "a session later the names are there, and a new key comes behind them");
	cerbo_set_host(cerbo, OTHER, now);
	check(cerbo->name_count == 0, "another address forgets the names with the values: the keys of that broker are not the keys of this one");
	check(next() == CERBO_DO_CLOSE && next() == CERBO_DO_OPEN && session_now() && takes("\"GX_Z\":1,\"GX_B\":2", 2, 0) && names_are("Z B"),
	      "the keys of the new broker are noted from the first place on");
	cerbo_set_host(cerbo, "", now);
	check(cerbo->name_count == 0, "no address forgets them as well");

	// The room
	check(setup() && (now = 6000, takes(many(64, ""), 64, 0)) && cerbo->name_count == 64 && names_row(0, 'K', 0, 64), "64 keys in one bundle: 64 names, in their order");
	check(takes(many(70, ",\"GX_K00\":5"), 64, 6) && cerbo->name_count == 64 && names_row(0, 'K', 0, 64), "the members that found no room in the store are not noted");
	check(takes(row('K', 0, 63, ",\"GX_NEW\":1"), 64, 0) && cerbo->name_count == 64 && names_row(0, 'K', 0, 63) && strcmp(cerbo->names[63], "GX_NEW") == 0,
	      "a new key while 64 names are kept: the oldest name the bundle did not bring gives way - here the last one, the only one - and the new one is the last");
	check(takes("\"GX_X1\":1", 1, 0) && cerbo->name_count == 64 && names_row(0, 'K', 1, 62) && strcmp(cerbo->names[62], "GX_NEW") == 0 && strcmp(cerbo->names[63], "GX_X1") == 0,
	      "a bundle of one new key: the first name gives way, the names behind it move up");
	check(takes("\"GX_K01\":1,\"GX_X2\":2", 2, 0) && cerbo->name_count == 64 && strcmp(cerbo->names[0], "GX_K01") == 0 && names_row(1, 'K', 3, 60) &&
	      strcmp(cerbo->names[61], "GX_NEW") == 0 && strcmp(cerbo->names[62], "GX_X1") == 0 && strcmp(cerbo->names[63], "GX_X2") == 0,
	      "the first name is in the bundle: it stays, and the second gives way");
	check(takes("\"GX_K01\":1e999,\"GX_X3\":3", 2, 0) && strcmp(cerbo->names[0], "GX_K01") == 0 && names_row(1, 'K', 4, 59) && strcmp(cerbo->names[60], "GX_NEW") == 0 &&
	      strcmp(cerbo->names[63], "GX_X3") == 0,
	      "the first name is in the bundle with a number without end: the bundle brought its key, it stays as well, and the next one gives way");
	check(takes(row('N', 0, 64, ""), 64, 0) && cerbo->name_count == 64 && names_row(0, 'N', 0, 64) && store_is_named(),
	      "64 new keys while 64 names are kept: every old name gives way, one after the other, and the names are those of the bundle in its order");
	check(takes(row('N', 32, 32, ",\"GX_P00\":0,\"GX_P01\":1"), 34, 0) && names_row(0, 'N', 2, 62) && names_row(62, 'P', 0, 2) && store_is_named(),
	      "two new keys behind 32 that are kept: the two oldest names the bundle did not bring give way");
	for(i = 0; i < 200 && store_is_named(); i++) takes(row((char)('A' + i % 7), i % 60, 1 + i % 40, i % 3 == 0 ? ",\"GX_ODD\":\"off\"" : ""), 1 + i % 40 + (i % 3 == 0), 0);
	check(i == 200 && cerbo->name_count == 64 && store_is_named(), "200 bundles of 1 to 41 keys out of 647: after each of them every key of the store is among the 64 names");

	// A name of every length
	check(setup() && (now = 6000, takes("\"GX_12345678901234567890123456789\":1,\"GX_1\":2", 2, 0)) && names_are("12345678901234567890123456789 1") &&
	      strlen(cerbo->names[0]) == 32, "a key of 32 bytes is noted with every byte, one of four bytes behind it");
	check(guards_intact(), "the names write nothing outside the source");
}

// A bundle of exactly `size` bytes: one value and a member the contract does not know, as long as it takes
static const char *sized(size_t size)
{
	static const char head[] = "{\"v\":1,\"boot\":7,\"t\":1000,\"values\":{\"GX_A\":1},\"pad\":\"";
	size_t fill = size - (sizeof(head) - 1) - 2;

	if(size < sizeof(head) + 2 || size >= sizeof(text))
	{
		written = false;
		return "";
	}
	memcpy(text, head, sizeof(head) - 1);
	memset(text + sizeof(head) - 1, 'x', fill);
	strcpy(text + sizeof(head) - 1 + fill, "\"}");
	return text;
}

// A bundle with a member the contract does not know: an array of `count` numbers. It needs 11 + count tokens.
static const char *with_array(int count)
{
	size_t length = (size_t)snprintf(text, sizeof(text), "{\"v\":1,\"boot\":7,\"t\":1000,\"values\":{},\"x\":[");
	int i;

	for(i = 0; i < count && length + 4 < sizeof(text); i++)
	{
		text[length++] = '1';
		text[length++] = i + 1 < count ? ',' : ']';
	}
	if(i < count) written = false;
	text[length++] = '}';
	text[length] = '\0';
	return text;
}

static void test_refused(void)
{
	static const char *const invalid[] = {
		"", " ", "x", "[1]", "5", "\"text\"", "null", "true", "{", "{}", "{\"v\":1", "{\"v\":1,\"boot\":7,\"t\":1000,\"values\":{}} x", "{\"v\":1,\"boot\":7,\"t\":1000,\"values\":{},}",
		"{\"v\":1,\"boot\":7,\"t\":1000,\"values\":{\"GX_A\":1}", "{\"v\":1,\"boot\":7,\"t\":1000,\"values\":{\"GX_A\":}}", "[{\"v\":1,\"boot\":7,\"t\":1000,\"values\":{}}]",
		"{\"boot\":7,\"t\":1000,\"values\":{}}", "{\"V\":1,\"boot\":7,\"t\":1000,\"values\":{}}",
		"{\"v\":0,\"boot\":7,\"t\":1000,\"values\":{}}", "{\"v\":-1,\"boot\":7,\"t\":1000,\"values\":{}}", "{\"v\":1.0,\"boot\":7,\"t\":1000,\"values\":{}}",
		"{\"v\":1e0,\"boot\":7,\"t\":1000,\"values\":{}}", "{\"v\":\"1\",\"boot\":7,\"t\":1000,\"values\":{}}", "{\"v\":true,\"boot\":7,\"t\":1000,\"values\":{}}",
		"{\"v\":null,\"boot\":7,\"t\":1000,\"values\":{}}", "{\"v\":[1],\"boot\":7,\"t\":1000,\"values\":{}}", "{\"v\":99999999999999999999,\"boot\":7,\"t\":1000,\"values\":{}}",
		"{\"v\":1,\"t\":1000,\"values\":{}}", "{\"v\":1,\"boot\":0,\"t\":1000,\"values\":{}}", "{\"v\":1,\"boot\":-1,\"t\":1000,\"values\":{}}",
		"{\"v\":1,\"boot\":2147483648,\"t\":1000,\"values\":{}}", "{\"v\":1,\"boot\":4294967303,\"t\":1000,\"values\":{}}", "{\"v\":1,\"boot\":1.5,\"t\":1000,\"values\":{}}",
		"{\"v\":1,\"boot\":7.0,\"t\":1000,\"values\":{}}", "{\"v\":1,\"boot\":\"7\",\"t\":1000,\"values\":{}}", "{\"v\":1,\"boot\":null,\"t\":1000,\"values\":{}}",
		"{\"v\":1,\"boot\":1e3,\"t\":1000,\"values\":{}}", "{\"v\":1,\"boot\":99999999999999999999,\"t\":1000,\"values\":{}}",
		"{\"v\":1,\"boot\":7,\"values\":{}}", "{\"v\":1,\"boot\":7,\"t\":-1,\"values\":{}}", "{\"v\":1,\"boot\":7,\"t\":1.5,\"values\":{}}",
		"{\"v\":1,\"boot\":7,\"t\":1000.0,\"values\":{}}", "{\"v\":1,\"boot\":7,\"t\":1e3,\"values\":{}}", "{\"v\":1,\"boot\":7,\"t\":\"1000\",\"values\":{}}",
		"{\"v\":1,\"boot\":7,\"t\":null,\"values\":{}}", "{\"v\":1,\"boot\":7,\"t\":9007199254740992,\"values\":{}}", "{\"v\":1,\"boot\":7,\"t\":99999999999999999999,\"values\":{}}",
		"{\"v\":1,\"boot\":7,\"t\":1000}", "{\"v\":1,\"boot\":7,\"t\":1000,\"values\":[]}", "{\"v\":1,\"boot\":7,\"t\":1000,\"values\":5}",
		"{\"v\":1,\"boot\":7,\"t\":1000,\"values\":\"GX_A\"}", "{\"v\":1,\"boot\":7,\"t\":1000,\"values\":null}", "{\"v\":1,\"boot\":7,\"t\":1000,\"Values\":{}}",
		"{\"v\":1,\"boot\":7,\"t\":1000,\"values\":{},\"x\":[[[[[[[[1]]]]]]]]}", "{\"v\":1,\"boot\":7,\"t\":1000,\"values\":{\"GX_A\":[[[[[[[1]]]]]]]}}",
	};
	static const char *const newer[] = {
		"{\"v\":2,\"boot\":7,\"t\":1000,\"values\":{\"GX_A\":5}}", "{\"v\":2}", "{\"v\":3,\"boot\":\"x\",\"values\":7}", "{\"v\":2147483648,\"boot\":7,\"t\":1000,\"values\":{}}",
		"{\"v\":9223372036854775807}", "{\"x\":{},\"v\":2}",
	};
	static const char *const topics[] = {"display/gx/value", "display/gx/values2", "display/gx/valueS", "Display/gx/values", "display/gx/values/", "x", "display", "display/gx/valuet"};
	static char long_json[2400];
	static json_token_t small[140];
	static json_token_t probe[300];
	bool right = true;
	size_t i, length;

	check(setup() && arrive(6000, 7, 1000) == 0 && arrive(9000, 7, 2000) == 1700 && number_is("GX_A", 1), "a session with a bundle that had a lag");
	now = 9500;

	for(i = 0; i < COUNT(invalid); i++)
	{
		if(!refuses(invalid[i], &cerbo->refused_invalid))
		{
			printf("  not refused as invalid: %.70s\n", invalid[i]);
			right = false;
		}
	}
	check(right && cerbo->refused_invalid == COUNT(invalid) && view() == CERBO_VIEW_LIVE,
	      "what is no bundle of the contract changes nothing and is counted: no JSON object, v, boot, t or values missing or of another kind, "
	      "boot 0 and 2^31, t negative, with a fraction and 2^53, nine levels");
	check(refuses("{\"v\":1,\"boot\":0,\"t\":1000,\"values\":{}}", &cerbo->refused_invalid) && refuses("{\"v\":1,\"boot\":2147483648,\"t\":1000,\"values\":{}}", &cerbo->refused_invalid),
	      "boot 0 and boot 2^31 are refused");
	check(refuses("{\"v\":1,\"boot\":7,\"t\":-1,\"values\":{}}", &cerbo->refused_invalid) && refuses("{\"v\":1,\"boot\":7,\"t\":1000.5,\"values\":{}}", &cerbo->refused_invalid) &&
	      refuses("{\"v\":1,\"boot\":7,\"t\":9007199254740992,\"values\":{}}", &cerbo->refused_invalid), "t negative, t with a fraction and t 2^53 are refused");

	// The limits that are taken
	remember();
	say("{\"v\":1,\"boot\":2147483647,\"t\":9007199254740991,\"values\":{\"GX_A\":2}}");
	check(cerbo->bundles == snapshot.cerbo.bundles + 1 && cerbo->boot == 2147483647 && cerbo->t_ms == 9007199254740991 && number_is("GX_A", 2), "boot 2^31-1 and t 2^53-1 are taken");
	say("{\"v\":1,\"boot\":1,\"t\":0,\"values\":{\"GX_A\":3}}");
	check(cerbo->boot == 1 && cerbo->t_ms == 0 && number_is("GX_A", 3), "boot 1 and t 0 are taken");
	say("{\"values\":{\"GX_A\":4},\"t\":17,\"boot\":7,\"v\":1}");
	check(cerbo->boot == 7 && cerbo->t_ms == 17 && number_is("GX_A", 4), "the members may come in any order");
	say("{\"v\":1,\"v\":2,\"boot\":8,\"boot\":0,\"t\":18,\"t\":-1,\"values\":{\"GX_A\":5},\"values\":[]}");
	check(cerbo->boot == 8 && cerbo->t_ms == 18 && number_is("GX_A", 5) && !cerbo->newer, "of two members v, boot, t or values the first counts");
	say("{\"v\":1,\"boot\":7,\"t\":1000,\"values\":{\"GX_A\":6},\"x\":[[[[[[[1]]]]]]]}");
	check(number_is("GX_A", 6), "eight levels are read");
	check(refuses("{\"v\":2,\"v\":1,\"boot\":7,\"t\":1000,\"values\":{}}", &cerbo->refused_version) && number_is("GX_A", 6), "... also where the first v is a newer one");

	// A newer contract
	check(setup() && arrive(6000, 7, 1000) == 0 && view() == CERBO_VIEW_LIVE, "a session that delivers");
	now = 6500;
	right = true;
	for(i = 0; i < COUNT(newer); i++)
	{
		cerbo->newer = false;
		if(!refuses(newer[i], &cerbo->refused_version) || !cerbo->newer || view() != CERBO_VIEW_NEWER) right = false;
	}
	check(right && number_is("GX_A", 1) && cerbo->values.count == 1, "a bundle of a newer version changes nothing, is counted, and is to be seen: the view says so");
	check(cerbo_showing(cerbo, now) && age_at("GX_A", now) == VALUE_AGE_FRESH, "the values of the bundle before stay and grow old");
	check(refuses("{\"v\":1,\"boot\":0,\"t\":1000,\"values\":{}}", &cerbo->refused_invalid) && refuses("nothing", &cerbo->refused_invalid) && view() == CERBO_VIEW_NEWER,
	      "what cannot be read does not take the newer contract back");
	check(refuses_on("display/gx/other", false, bundle_text(7, 2000, ONE), &cerbo->refused_topic) && refuses_on(CERBO_TOPIC, true, bundle_text(7, 2000, ONE), &cerbo->refused_retained) &&
	      view() == CERBO_VIEW_NEWER, "... nor does a message that is refused for another reason");
	check(arrive(7000, 7, 2000) == 0 && !cerbo->newer && view() == CERBO_VIEW_LIVE, "a bundle that is taken ends it");

	// Retained, and another topic
	check(refuses_on(CERBO_TOPIC, true, bundle_text(7, 3000, "\"GX_B\":2"), &cerbo->refused_retained) && value("GX_B") == NULL, "a retained bundle changes nothing and is counted");
	right = true;
	for(i = 0; i < COUNT(topics); i++)
	{
		if(!refuses_on(topics[i], false, bundle_text(7, 3000, "\"GX_B\":2"), &cerbo->refused_topic)) right = false;
	}
	check(right && value("GX_B") == NULL, "a bundle on another topic changes nothing and is counted: shorter, longer, of the same length, with another capital");
	remember();
	bundle_text(7, 3000, ONE);
	length = broker_publish_head(wire, 0x30, "display/gx/values\0", 18, strlen(text), 1);
	memcpy(wire + length, text, strlen(text));
	feed(wire, length + strlen(text));
	check(nothing_taken() && cerbo->refused_topic == snapshot.cerbo.refused_topic + 1, "a topic with a zero byte behind the right text is another topic");
	check(refuses_on("display/gx/other", true, bundle_text(7, 3000, ONE), &cerbo->refused_topic), "another topic is judged before the retained flag");
	check(refuses_on("display/gx/other", false, sized(1401), &cerbo->refused_topic) && refuses_on(CERBO_TOPIC, true, sized(1401), &cerbo->refused_retained),
	      "the topic and the retained flag are judged before the length");
	check(refuses_on(CERBO_TOPIC, true, "no bundle", &cerbo->refused_retained), "the retained flag is judged before the text");

	// Too long
	remember();
	say(sized(1400));
	check(strlen(text) == 1400 && cerbo->bundles == snapshot.cerbo.bundles + 1 && refused() == snapshot.cerbo.refused_invalid + snapshot.cerbo.refused_version +
	      snapshot.cerbo.refused_retained + snapshot.cerbo.refused_topic + snapshot.cerbo.refused_long && number_is("GX_A", 1), "a bundle of 1400 bytes is taken");
	check(refuses(sized(1401), &cerbo->refused_long) && strlen(text) == 1401, "a bundle of 1401 bytes changes nothing and is counted as too long");
	check(refuses(sized(1517), &cerbo->refused_long) && strlen(text) == 1517, "a bundle that fills the room of the reader, 1517 bytes, is too long");
	check(refuses(sized(1518), &cerbo->refused_long) && refuses(sized(4000), &cerbo->refused_long), "a message the reader lets go by is counted as too long");
	memset(long_json, 'y', sizeof(long_json) - 1);
	check(refuses_on("display/gx/other", true, long_json, &cerbo->refused_long), "a message the reader lets go by is not looked at: its topic may be any");
	check(refuses("{\"v\":2,\"pad\":\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"}",
	              &cerbo->refused_version), "a short bundle of a newer version is still one of a newer version");

	// The tokens
	remember();
	say(with_array(245));
	check(json_parse(text, strlen(text), work, CERBO_TOKENS) == 256 && cerbo->bundles == snapshot.cerbo.bundles + 1, "a bundle that needs 256 tokens is taken");
	check(refuses(with_array(246), &cerbo->refused_invalid) && json_parse(text, strlen(text), probe, 300) == 257, "a bundle that needs 257 tokens is refused");
	memset(small, 0x5A, sizeof(small));
	length = wire_bundle(0, 7, 5000, many(64, ""));
	remember();
	cerbo_bytes(cerbo, wire, length, now, small, 136);
	check(nothing_taken() && cerbo->refused_invalid == snapshot.cerbo.refused_invalid + 1 && small[136].start == 0x5A5A5A5A, "a bundle of 64 values with room for 136 tokens is refused, and no token is written behind the room");
	cerbo_bytes(cerbo, wire, length, now, small, 137);
	check(cerbo->values.count == 64 && number_is("GX_K63", 63) && small[137].start == 0x5A5A5A5A, "a bundle of 64 values needs 137 tokens");
	memset(small, 0x5A, sizeof(small));
	remember();
	cerbo_bytes(cerbo, wire, length, now, small, 0);
	cerbo_bytes(cerbo, wire, length, now, NULL, 0);
	check(nothing_taken() && cerbo->refused_invalid == snapshot.cerbo.refused_invalid + 2 && small[0].start == 0x5A5A5A5A, "without room for tokens a bundle is refused, and nothing is written");
	check(guards_intact(), "refused bundles write nothing outside the source");
}

static void test_lag(void)
{
	static const uint32_t burst[] = {6200, 5200, 4200, 3200, 2200, 1200, 200, 0};
	bool right = true, fresh = true;
	uint32_t lag = 0;
	size_t i;
	int k;

	// A display younger than the flow
	check(setup() && arrive(6000, 7, 100000) == 0 && seen("GX_A") == 6000 && cerbo->boots == 1 && cerbo->distrusted == 0,
	      "a display that is younger than the flow (the bundle is older than the display): the first bundle has no lag");
	check(arrive(7000, 7, 101000) == 0 && arrive(8000, 7, 102000) == 0 && seen("GX_A") == 8000, "bundles that arrive as they were sent have no lag");
	check(arrive(11000, 7, 103000) == 1700 && seen("GX_A") == 9300, "a bundle that arrives 2 s late has a lag of 1700 ms: 2000 less what the base yielded in 3 s");
	check(age_at("GX_A", 11000) == VALUE_AGE_FRESH && age_at("GX_A", 14299) == VALUE_AGE_FRESH && age_at("GX_A", 14300) == VALUE_AGE_OLD,
	      "a bundle that is late is as old as its lag: it is dimmed 5 s after the time it should have arrived");
	check(arrive(11000, 7, 104000) == 700 && seen("GX_A") == 10300 && arrive(11000, 7, 105000) == 0 && seen("GX_A") == 11000, "the bundles behind it in the same moment catch up: the lag is never below 0");

	// A burst
	check(setup() && arrive(6000, 7, 50000) == 0, "a session, and then the radio link pauses for 8 s");
	for(i = 0; i < COUNT(burst); i++)
	{
		if(arrive(14000, 7, 51000 + 1000 * (int64_t)i) != burst[i] || seen("GX_A") != 14000 - burst[i]) right = false;
		if(i == 1 && age_at("GX_A", 14000) != VALUE_AGE_OLD) right = false;
		if(i == 2 && age_at("GX_A", 14000) != VALUE_AGE_FRESH) right = false;
	}
	check(right && age_at("GX_A", 14000) == VALUE_AGE_FRESH && seen("GX_A") == 14000, "in a burst the bundles that piled up are old, and the last one is fresh");

	// Another boot, another session
	check(arrive(15000, 8, 5) == 0 && cerbo->boots == 2 && seen("GX_A") == 15000, "the first bundle of another boot has no lag, whatever its t is");
	check(arrive(18000, 8, 1005) == 1700 && cerbo->boots == 2, "the lag is measured within the boot");
	check(arrive(18500, 7, 60000) == 0 && cerbo->boots == 3, "the boot before once more is another boot");
	check(setup() && arrive(6000, 7, 50000) == 0 && arrive(7000, 7, 51000) == 0, "a session with two bundles");
	cerbo_down(cerbo, 7500);
	check(connect_at(9500) && arrive(30000, 7, 52000) == 0 && seen("GX_A") == 30000 && cerbo->boots == 1 && cerbo->distrusted == 0,
	      "the first bundle of a session has no lag, whatever the session before saw: nothing was kept for a clean session");
	check(arrive(33000, 7, 53000) == 1700, "... and the next one is measured by it");

	// A clock that lost 11 s
	check(setup() && arrive(6000, 7, 100000) == 0 && arrive(7000, 7, 101000) == 0 && arrive(8000, 7, 91000) == 10900, "the clock of the flow lost 11 s: a lag of 10.9 s");
	for(k = 1; k <= 70; k++)
	{
		lag = arrive(8000 + 1000 * (uint64_t)k, 7, 91000 + 1000 * (int64_t)k);
		if(lag != 10900 - 100 * (uint32_t)k) right = false;
		if(k == 59 && (lag != 5000 || age_at("GX_A", now) != VALUE_AGE_OLD)) right = false;
		if(k >= 60 && age_at("GX_A", now) != VALUE_AGE_FRESH) fresh = false;
	}
	check(right && fresh && lag == 3900 && cerbo->distrusted == 0, "a clock that lost 11 s is fresh again after 60 s, and stays so: the base yields 100 ms with every second");

	// Two clocks that drift apart
	check(setup() && arrive(6000, 7, 0) == 0, "a session for two clocks");
	right = true;
	for(k = 1; k <= 600; k++)
	{
		if(arrive(6000 + 1010 * (uint64_t)k, 7, 1000 * (int64_t)k) != 0) right = false;
	}
	check(right && cerbo->distrusted == 0 && seen("GX_A") == now, "a display whose clock runs 1 % fast sees no lag in ten minutes");
	check(setup() && arrive(6000, 7, 0) == 0, "a session for two clocks once more");
	for(k = 1; k <= 600; k++)
	{
		if(arrive(6000 + 990 * (uint64_t)k, 7, 1000 * (int64_t)k) != 0) right = false;
	}
	check(right && arrive(now + 1000, 7, 31536000000000) == 0 && arrive(now + 1000, 7, 31536000001000) == 0 && cerbo->distrusted == 0,
	      "two clocks that drift apart make no lag: not a display that runs slow, not a Cerbo that set its clock a thousand years on");

	// The rest of the yield
	check(setup() && arrive(6000, 7, 0) == 0 && arrive(6005, 7, 0) == 5 && cerbo->yield_rest == 500 && arrive(6010, 7, 0) == 9 && cerbo->yield_rest == 0,
	      "what the yield leaves over is carried on: two steps of 5 ms yield 1 ms");
	for(k = 3; k <= 200; k++) lag = arrive(6000 + 5 * (uint64_t)k, 7, 0);
	check(lag == 900 && cerbo->yield_rest == 0, "200 steps of 5 ms yield 100 ms");
	check(setup() && arrive(6000, 7, 0) == 0 && arrive(6990, 7, 0) == 891 && cerbo->yield_rest == 0, "a step of 990 ms yields 99 ms");
	check(setup() && arrive(6000, 7, 0) == 0 && arrive(18345, 7, 10000) == 1111 && cerbo->yield_rest == 500, "a step of 12345 ms yields 1234 ms and half a one");
	check(setup() && arrive(6000, 7, 0) == 0 && arrive(6005, 7, 0) == 5 && arrive(6010, 8, 0) == 0 && cerbo->yield_rest == 0 && arrive(6015, 8, 0) == 5 && cerbo->yield_rest == 500,
	      "with another boot the yield begins without a rest");
	check(setup() && arrive(6000, 7, 0) == 0 && arrive(6005, 7, 0) == 5 && cerbo->yield_rest == 500, "a session with half a millisecond left over");
	cerbo_down(cerbo, 6006);
	check(connect_at(8006) && arrive(8400, 7, 0) == 0 && cerbo->yield_rest == 0 && arrive(8405, 7, 0) == 5 && cerbo->yield_rest == 500, "with another session the yield begins without a rest");

	// A lag nobody believes
	check(setup() && arrive(6000, 7, 0) == 0 && arrive(56000, 7, 0) == 45000 && cerbo->distrusted == 0 && seen("GX_A") == 11000 && age_at("GX_A", 56000) == VALUE_AGE_OLD,
	      "a lag of 45 s is believed");
	check(setup() && arrive(6000, 7, 0) == 0 && arrive(56001, 7, 0) == 0 && cerbo->distrusted == 1 && seen("GX_A") == 56001 && age_at("GX_A", 56001) == VALUE_AGE_FRESH,
	      "a lag of 45001 ms is not believed: the bundle counts as fresh, and it is counted");
	check(arrive(57001, 7, 1000) == 0 && arrive(60001, 7, 2000) == 1700 && cerbo->distrusted == 1, "the bundle that was not believed is the new base");
	check(setup() && arrive(6000, 7, 0) == 0 && arrive(56005, 7, 0) == 0 && cerbo->distrusted == 1 && cerbo->yield_rest == 0 && arrive(56010, 7, 0) == 5 && cerbo->yield_rest == 500,
	      "with a lag that is not believed the yield begins without a rest");
	check(setup() && arrive(6000, 7, 9007199254740991) == 0 && arrive(7000, 7, 0) == 0 && cerbo->distrusted == 1, "a t that falls from 2^53-1 to 0 is a lag nobody believes");
	check(setup() && arrive(6000, 7, 50000) == 0 && arrive(7000, 7, 20000) == 30900 && cerbo->distrusted == 0, "a t that steps back by 30 s is a lag of 30.9 s");
	check(setup() && arrive(6000, 7, 0) == 0 && arrive(UINT64_MAX, 7, 5) == 0 && cerbo->distrusted == 1 && seen("GX_A") == UINT64_MAX,
	      "a step of the time of the display to its very end is a lag nobody believes");

	// Two flows
	check(setup(), "a session with two flows that send in turns");
	right = true;
	for(k = 0; k < 10; k++)
	{
		if(arrive(6000 + 500 * (uint64_t)k, k % 2 == 0 ? 7 : 9, k % 2 == 0 ? 1000 * (int64_t)k : 77000000 + 3 * (int64_t)k) != 0 || !number_is("GX_A", 1)) right = false;
		if(cerbo->boots != (uint32_t)k + 1 || seen("GX_A") != now) right = false;
	}
	check(right && cerbo->bundles == 10 && cerbo->distrusted == 0, "two senders in turns: the values stay and are fresh, and boots counts every change");

	// The edges of the time
	begin_at(0, HOST, false, NAME);
	cerbo_network(cerbo, true, 0);
	check(connect_at(0) && arrive(400, 7, 50000) == 0 && arrive(1400, 7, 20000) == 30900 && seen("GX_A") == 0 && age_at("GX_A", 1400) == VALUE_AGE_FRESH,
	      "a value is not seen before the display started: a lag longer than the display runs gives the time 0");
	begin_at(9223372036854775808ull, HOST, false, NAME);
	cerbo_network(cerbo, true, now);
	check(connect_at(now) && arrive(9223372036854776808ull, 7, 1000) == 0 && arrive(9223372036854779808ull, 7, 2000) == 1700 && seen("GX_A") == 9223372036854778108ull,
	      "the lag is reckoned the same at a time of 2^63");
	begin_at(UINT64_MAX - 20000, HOST, false, NAME);
	cerbo_network(cerbo, true, now);
	check(connect_at(now) && arrive(UINT64_MAX - 10000, 7, 1000) == 0 && arrive(UINT64_MAX - 7000, 7, 2000) == 1700 && arrive(UINT64_MAX, 7, 9000) == 1000,
	      "... and at the end of the time");
	check(setup() && arrive(6000, 7, 4294967000) == 0 && arrive(7000, 7, 4294968000) == 0 && arrive(10000, 7, 4294969000) == 1700 && cerbo->t_ms == 4294969000,
	      "t above 2^32 is read, and the lag across it is right");
	check(setup() && arrive(6000, 7, 9007199254730991) == 0 && arrive(9000, 7, 9007199254731991) == 1700 && arrive(19000, 7, 9007199254740991) == 1700,
	      "t up to 2^53-1 is reckoned with");
	check(guards_intact() && written, "the lag writes nothing outside the source, and every text of the test fitted");
}

static void test_ages(void)
{
	value_t one;

	memset(&one, 0, sizeof(one));
	one.seen_ms = 100000;
	check(cerbo_age(&one, 100000) == VALUE_AGE_FRESH && cerbo_age(&one, 104999) == VALUE_AGE_FRESH, "a value seen less than 5 s ago is fresh");
	check(cerbo_age(&one, 105000) == VALUE_AGE_OLD && cerbo_age(&one, 159999) == VALUE_AGE_OLD, "from 5 s to 60 s it is old: shown dimmed");
	check(cerbo_age(&one, 160000) == VALUE_AGE_GONE && cerbo_age(&one, UINT64_MAX) == VALUE_AGE_GONE, "from 60 s on it is gone");
	check(cerbo_age(&one, 99999) == VALUE_AGE_FRESH && cerbo_age(&one, 0) == VALUE_AGE_FRESH, "a time before the value was seen counts as no time passed");
	check(cerbo_age(NULL, 0) == VALUE_AGE_GONE && cerbo_age(NULL, 100000) == VALUE_AGE_GONE, "no value is gone");
	check(VALUE_FRESH_MS == 3000 && VALUE_KEPT_MS == 10000 && values_age(&one, 104999) == VALUE_AGE_OLD && values_age(&one, 159999) == VALUE_AGE_GONE,
	      "the limits of the WiCAN stay 3 s and 10 s");

	check(cerbo_key("GX_A") && cerbo_key("GX_") && cerbo_key("GX_lower") && cerbo_key("GX_BATT_SOC"), "a key that begins with GX_ is one of the Cerbo");
	check(!cerbo_key("ENGINE_RPM") && !cerbo_key("GX") && !cerbo_key("G") && !cerbo_key("") && !cerbo_key("gx_A") && !cerbo_key("XGX_A") && !cerbo_key("GXA_") && !cerbo_key("Gx_A") &&
	      !cerbo_key("gX_A"), "every other key is one of the WiCAN");
	check(!cerbo_key(NULL), "no key is no key of the Cerbo");
}

// The numbers the info line and GET /api/info take from the source (app.h, app_web.h)
static void test_numbers(void)
{
	static const struct
	{
		uint32_t invalid, version, retained, topic, too_long;
		uint32_t sum;
		const char *what;
	} sums[] = {
		{1, 0, 0, 0, 0, 1, "refused: a message that is no bundle of the contract is one"},
		{0, 1, 0, 0, 0, 1, "refused: a bundle of a newer contract is one"},
		{0, 0, 1, 0, 0, 1, "refused: a retained bundle is one"},
		{0, 0, 0, 1, 0, 1, "refused: a message on another topic is one"},
		{0, 0, 0, 0, 1, 1, "refused: a message that is too long is one"},
		{1, 20, 300, 4000, 50000, 54321, "refused: the five counters together"},
		{UINT32_MAX, 0, 0, 0, 0, UINT32_MAX, "refused: the largest number one counter holds"},
		{UINT32_MAX, 1, 0, 0, 0, UINT32_MAX, "refused: more than 32 bit hold are the largest number, not 0"},
		{0x80000000u, 0, 0, 0, 0x80000000u, UINT32_MAX, "refused: two halves of 2^32 are the largest number, not 0"},
		{0xFFFFFFFEu, 0, 0, 1, 0, UINT32_MAX, "refused: exactly the largest number is that number"},
		{0xFFFFFFFDu, 0, 1, 0, 0, 0xFFFFFFFEu, "refused: one below the largest number is not rounded up to it"},
		{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, "refused: five counters at their end are the largest number"},
		{0, 0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u, UINT32_MAX, "refused: four quarters of 2^32 are the largest number, not 0"},
	};
	uint32_t before;
	size_t i;
	int n;

	begin();
	check(cerbo_refused(cerbo) == 0 && cerbo_shown(cerbo, now) == 0 && cerbo_shown(cerbo, UINT64_MAX) == 0, "a source that has heard nothing has refused nothing and has nothing to show");

	// Each reason counts, once for each message
	check(setup(), "a session for the numbers");
	now = 6000;
	before = cerbo_refused(cerbo);
	say("kein JSON");
	check(cerbo->refused_invalid == 1 && cerbo_refused(cerbo) == before + 1, "refused: a message that is no JSON is counted");
	say("{\"v\":2,\"boot\":7,\"t\":1000,\"values\":{}}");
	check(cerbo->refused_version == 1 && cerbo_refused(cerbo) == before + 2, "refused: a bundle of a newer contract is counted");
	publish(CERBO_TOPIC, true, bundle_text(7, 1000, ONE));
	check(cerbo->refused_retained == 1 && cerbo_refused(cerbo) == before + 3, "refused: a retained bundle is counted");
	publish("display/gx/other", false, bundle_text(7, 1000, ONE));
	publish("display/gx/other", false, bundle_text(7, 1000, ONE));
	check(cerbo->refused_topic == 2 && cerbo_refused(cerbo) == before + 5, "refused: two messages on another topic are counted, each of them");
	check(cerbo->bundles == 0 && cerbo->values.count == 0 && cerbo_shown(cerbo, now) == 0 && !cerbo->failed, "refused: none of them brought a value");
	bundle(7, 1000, ONE ",\"lower\":1,\"GX_B\":null");
	check(cerbo->bundles == 1 && cerbo->skipped == 2 && cerbo_refused(cerbo) == before + 5, "refused: a bundle that is taken is none, and members that are passed over are no messages");

	// The sum, also where it has no room in 32 bit
	for(i = 0; i < COUNT(sums); i++)
	{
		begin();
		cerbo->refused_invalid = sums[i].invalid;
		cerbo->refused_version = sums[i].version;
		cerbo->refused_retained = sums[i].retained;
		cerbo->refused_topic = sums[i].topic;
		cerbo->refused_long = sums[i].too_long;
		remember();
		check(cerbo_refused(cerbo) == sums[i].sum && memcmp(&snapshot, &box, sizeof(box)) == 0, sums[i].what);
	}
	begin();
	cerbo->bundles = 1000;
	cerbo->sessions = 2000;
	cerbo->boots = 3000;
	cerbo->skipped = 4000;
	cerbo->distrusted = 5000;
	cerbo->lag_ms = 6000;
	check(cerbo_refused(cerbo) == 0, "refused: bundles, sessions, boots, members passed over, lags not believed and the lag are no refused messages");

	// What the source has to show: the values of the last bundle, until they are gone
	check(setup(), "a session for the values to show");
	now = 6000;
	bundle(7, 1000, "\"GX_A\":1,\"GX_B\":\"on\",\"GX_C\":-2.5");
	remember();
	check(cerbo_shown(cerbo, 6000) == 3 && cerbo_shown(cerbo, 10999) == 3, "shown: the three values of a bundle, fresh");
	check(cerbo_shown(cerbo, 11000) == 3 && cerbo_shown(cerbo, 65999) == 3, "shown: old values count as well, up to 60 s");
	check(cerbo_shown(cerbo, 66000) == 0 && cerbo_shown(cerbo, UINT64_MAX) == 0, "shown: from 60 s on they are gone, and nothing is left to show");
	check(cerbo_shown(cerbo, 0) == 3 && cerbo_shown(cerbo, 5999) == 3, "shown: a time before the values were seen counts as no time passed");
	check(memcmp(&snapshot, &box, sizeof(box)) == 0, "shown: asking changes nothing of the source");
	now = 7000;
	bundle(7, 2000, "\"GX_B\":\"off\"");
	check(cerbo_shown(cerbo, 7000) == 1 && cerbo_shown(cerbo, 66999) == 1 && cerbo_shown(cerbo, 67000) == 0, "shown: a bundle with one value leaves one to show: what it does not name is not there");
	now = 8000;
	bundle(7, 3000, "");
	check(cerbo_shown(cerbo, 8000) == 0 && cerbo_showing(cerbo, 8000) && view() == CERBO_VIEW_LIVE, "shown: a bundle without values leaves none, although the source delivers");

	// What is counted is what GET /api/values names. A number beyond the range of a double is taken into the
	// store as infinite, as the store of the adapter takes one, has no text and stands in no answer.
	check(setup(), "a session for numbers without end");
	now = 6000;
	bundle(7, 1000, "\"GX_A\":1,\"GX_N\":1e999,\"GX_B\":\"on\"");
	check(cerbo->values.count == 3 && number_is("GX_N", INFINITY) && cerbo_shown(cerbo, 6000) == 2 && cerbo_shown(cerbo, 65999) == 2 && cerbo_shown(cerbo, 66000) == 0,
	      "shown: a number beyond a double - 1e999 - is in the store as infinite and is not counted: of three values two are named by GET /api/values");
	now = 7000;
	bundle(7, 2000, "\"GX_M\":-1e999,\"GX_A\":1");
	check(cerbo->values.count == 2 && number_is("GX_M", -INFINITY) && cerbo_shown(cerbo, 7000) == 1, "shown: nor is a number beyond the range on the negative side, -1e999");
	now = 8000;
	bundle(7, 3000, "\"GX_N\":1e999,\"GX_M\":-1e999");
	check(cerbo->values.count == 2 && cerbo_shown(cerbo, 8000) == 0 && cerbo_showing(cerbo, 8000), "shown: a bundle of nothing but such numbers leaves two values in the store and none to count");
	now = 9000;
	bundle(7, 4000, "\"GX_N\":1e308,\"GX_M\":-1e308,\"GX_Z\":0,\"GX_T\":1e-300");
	check(cerbo->values.count == 4 && number_is("GX_N", 1e308) && cerbo_shown(cerbo, 9000) == 4, "shown: the largest numbers a double holds, zero and a very small one are numbers like every other: counted");
	// What no bundle brings, written into the store: the count follows the answer there as well
	cerbo->values.items[0].number = NAN;
	check(cerbo_shown(cerbo, 9000) == 3, "shown: a value that is no number at all is not counted");
	cerbo->values.items[0].number = 5;
	cerbo->values.items[1].kind = (value_kind_t)3;
	check(cerbo_shown(cerbo, 9000) == 3, "shown: a value whose kind is none of the three is not counted");
	cerbo->values.items[1].kind = VALUE_OFF;
	cerbo->values.items[1].number = INFINITY;
	cerbo->values.items[2].kind = VALUE_ON;
	cerbo->values.items[2].number = NAN;
	check(cerbo_shown(cerbo, 9000) == 4, "shown: a switch is counted whatever its number holds - it is written as on or off");
	cerbo->values.items[0].number = INFINITY;
	cerbo->values.items[3].number = 1;
	check(cerbo_shown(cerbo, 9000) == 3, "shown: the first value of the store without end, the last a number: the last is counted, the first is not");

	// A bundle that arrived late: its values are as old as its lag
	check(setup() && arrive(6000, 7, 1000) == 0 && arrive(9000, 7, 2000) == 1700 && cerbo_shown(cerbo, 67299) == 1 && cerbo_shown(cerbo, 67300) == 0,
	      "shown: a value of a bundle with a lag of 1700 ms is gone 1700 ms sooner");
	// Each value by its own time
	check(setup(), "a session for values of three ages");
	now = 6000;
	bundle(7, 1000, "\"GX_A\":1,\"GX_B\":2,\"GX_C\":3,\"GX_D\":4");
	cerbo->values.items[0].seen_ms = 1000;
	cerbo->values.items[2].seen_ms = 3000;
	check(cerbo_shown(cerbo, 60999) == 4 && cerbo_shown(cerbo, 61000) == 3 && cerbo_shown(cerbo, 62999) == 3 && cerbo_shown(cerbo, 63000) == 2 && cerbo_shown(cerbo, 65999) == 2 &&
	      cerbo_shown(cerbo, 66000) == 0, "shown: every value is counted by its own time, the first and one in the middle as well");

	// A full store
	check(setup(), "a session for a full store");
	now = 6000;
	n = snprintf(text, sizeof(text), "{\"v\":1,\"boot\":7,\"t\":1000,\"values\":{");
	for(i = 0; i < VALUES_MAX; i++) n += snprintf(text + n, sizeof(text) - (size_t)n, "%s\"GX_%u\":%u", i > 0 ? "," : "", (unsigned)i, (unsigned)i);
	snprintf(text + n, sizeof(text) - (size_t)n, "}}");
	say(text);
	check(cerbo->bundles == 1 && cerbo->values.count == VALUES_MAX && cerbo_shown(cerbo, 6000) == 64 && cerbo_shown(cerbo, 65999) == 64 && cerbo_shown(cerbo, 66000) == 0 && guards_intact(),
	      "shown: a full store has 64 values to show, the last of them as well");

	// The values stay when the session ends, and go with another address
	cerbo_down(cerbo, 7000);
	check(cerbo->phase == CERBO_IDLE && cerbo_shown(cerbo, 7000) == 64, "shown: the values stay to be shown when the session ends");
	cerbo_set_host(cerbo, OTHER, 8000);
	check(cerbo_shown(cerbo, 8000) == 0, "shown: with another address nothing of the old one is left to show");
}

static void test_views(void)
{
	// Off, and without a network
	begin_at(1000, "", false, NAME);
	check(view() == CERBO_VIEW_OFF && (cerbo_network(cerbo, true, 5000), cerbo_view(cerbo, 5000)) == CERBO_VIEW_OFF, "no address: off, with and without a network");
	begin_at(1000, HOST, true, NAME);
	check(view() == CERBO_VIEW_OFF && (cerbo_network(cerbo, true, 5000), cerbo_view(cerbo, 5000)) == CERBO_VIEW_OFF && cerbo->safe_mode, "safe mode: off, with and without a network");
	begin();
	check(view() == CERBO_VIEW_NO_WIFI && cerbo_view(cerbo, 1000000) == CERBO_VIEW_NO_WIFI, "an address and no network: no WiFi, however long");

	// Waiting, then not reachable
	now = 5000;
	cerbo_network(cerbo, true, now);
	check(view() == CERBO_VIEW_CONNECTING && cerbo_view(cerbo, 19999) == CERBO_VIEW_CONNECTING && cerbo_view(cerbo, 20000) == CERBO_VIEW_UNREACHABLE,
	      "without a session the display waits for 15 s after it joined the network, then the Cerbo is not reachable");
	check(next() == CERBO_DO_OPEN && waits_after_down(6000, 2000) && waits_after_down(9000, 5000) && cerbo_view(cerbo, 19999) == CERBO_VIEW_CONNECTING &&
	      cerbo_view(cerbo, 20000) == CERBO_VIEW_UNREACHABLE, "attempts that fail do not begin the waiting anew");
	cerbo_opened(cerbo, 14100);
	check(next_at(14100) == CERBO_DO_SEND && (connack(0), next()) == CERBO_DO_SEND && cerbo_view(cerbo, 19999) == CERBO_VIEW_CONNECTING &&
	      cerbo_view(cerbo, 20000) == CERBO_VIEW_UNREACHABLE, "a connection without a session is still waiting, and then not reachable");
	cerbo_down(cerbo, 25000);
	check(cerbo_view(cerbo, 25000) == CERBO_VIEW_UNREACHABLE, "the end of a connection that made no session does not begin the waiting anew");
	cerbo_network(cerbo, false, 26000);
	cerbo_network(cerbo, true, 30000);
	check(cerbo_view(cerbo, 30000) == CERBO_VIEW_CONNECTING && cerbo_view(cerbo, 44999) == CERBO_VIEW_CONNECTING && cerbo_view(cerbo, 45000) == CERBO_VIEW_UNREACHABLE,
	      "joining a network again begins the waiting anew");
	cerbo_set_host(cerbo, OTHER, 50000);
	check(cerbo_view(cerbo, 50000) == CERBO_VIEW_CONNECTING && cerbo_view(cerbo, 64999) == CERBO_VIEW_CONNECTING && cerbo_view(cerbo, 65000) == CERBO_VIEW_UNREACHABLE,
	      "another address begins the waiting anew");
	cerbo_set_host(cerbo, OTHER, 70000);
	cerbo_network(cerbo, true, 70000);
	check(cerbo_view(cerbo, 70000) == CERBO_VIEW_UNREACHABLE, "the same address and the same network once more do not");

	// A session
	check(setup() && view() == CERBO_VIEW_QUIET && cerbo_view(cerbo, 15299) == CERBO_VIEW_QUIET, "a session without a bundle: waiting, nothing is known");
	check(arrive(6000, 7, 1000) == 0 && view() == CERBO_VIEW_LIVE && cerbo_view(cerbo, 15999) == CERBO_VIEW_LIVE && cerbo_view(cerbo, 16000) == CERBO_VIEW_QUIET,
	      "a bundle taken less than 10 s ago: the source delivers; from 10 s on it is quiet");

	// A pause of the radio link: nothing comes, no bundle and no PINGRESP
	check(next_at(15300) == CERBO_DO_SEND && next_at(25300) == CERBO_DO_SEND && cerbo_view(cerbo, 26000) == CERBO_VIEW_QUIET && cerbo_view(cerbo, 35299) == CERBO_VIEW_QUIET,
	      "a pause of the radio link of 20 s and more never blames the flow: no PINGRESP comes");
	check(next_at(35300) == CERBO_DO_CLOSE && view() == CERBO_VIEW_CONNECTING && cerbo_view(cerbo, 50299) == CERBO_VIEW_CONNECTING && cerbo_view(cerbo, 50300) == CERBO_VIEW_UNREACHABLE,
	      "when the session ends the display waits for 15 s, then the Cerbo is not reachable");
	check(cerbo_showing(cerbo, 35300) && cerbo_showing(cerbo, 65999) && !cerbo_showing(cerbo, 66000) && age_at("GX_A", 65999) == VALUE_AGE_OLD,
	      "the values stay for 60 s after they were seen, also when the session has ended");

	// A flow that is silent while the broker answers
	check(setup() && arrive(6000, 7, 1000) == 0 && next_at(15300) == CERBO_DO_SEND, "a session, a bundle, and then the flow is silent");
	now = 15310;
	pingresp();
	check(cerbo_view(cerbo, 16000) == CERBO_VIEW_QUIET && cerbo_view(cerbo, 26000) == CERBO_VIEW_QUIET && cerbo_view(cerbo, 1000000) == CERBO_VIEW_QUIET,
	      "a PINGRESP 9 s after the last bundle proves nothing about the silence behind it: quiet, however long");
	check(next_at(25300) == CERBO_DO_SEND && (now = 25999, pingresp(), cerbo_view(cerbo, 26000)) == CERBO_VIEW_QUIET, "a PINGRESP 19999 ms after the last bundle does not either");
	check(next_at(35300) == CERBO_DO_SEND && (now = 35310, pingresp(), view()) == CERBO_VIEW_SILENT && cerbo_view(cerbo, 1000000) == CERBO_VIEW_SILENT,
	      "a flow that is silent while the broker answers a PINGREQ: 20 s after the last bundle the view says so");
	check(arrive(36000, 7, 31000) == 0 && view() == CERBO_VIEW_LIVE && cerbo_view(cerbo, 46000) == CERBO_VIEW_QUIET, "a bundle ends the silence, and the PINGRESP from before it proves nothing any more");
	check(setup() && arrive(6000, 7, 1000) == 0 && next_at(15300) == CERBO_DO_SEND && next_at(25300) == CERBO_DO_SEND && (now = 26000, pingresp(), view()) == CERBO_VIEW_SILENT,
	      "a PINGRESP exactly 20 s after the last bundle is the proof");
	check(setup() && next_at(15300) == CERBO_DO_SEND && (now = 15310, pingresp(), view()) == CERBO_VIEW_QUIET && next_at(25300) == CERBO_DO_SEND &&
	      (now = 25310, pingresp(), view()) == CERBO_VIEW_SILENT, "without any bundle the silence counts from the SUBACK: the PINGRESP 20 s after it is the proof");
	check(setup() && next_at(15300) == CERBO_DO_SEND && (now = 25299, pingresp(), view()) == CERBO_VIEW_QUIET && cerbo_view(cerbo, 1000000) == CERBO_VIEW_QUIET,
	      "... and the one a millisecond before is not");

	// A PINGRESP from before the last bundle
	check(setup() && next_at(15300) == CERBO_DO_SEND && next_at(25300) == CERBO_DO_SEND && (now = 25400, pingresp(), view()) == CERBO_VIEW_SILENT, "a session that is silent");
	check(arrive(25500, 7, 1000) == 0 && view() == CERBO_VIEW_LIVE && cerbo_view(cerbo, 35500) == CERBO_VIEW_QUIET && cerbo_view(cerbo, 45500) == CERBO_VIEW_QUIET,
	      "a PINGRESP from before the last bundle says nothing about the silence behind it");

	// Another session
	check(setup() && next_at(15300) == CERBO_DO_SEND && next_at(25300) == CERBO_DO_SEND && (now = 25400, pingresp(), view()) == CERBO_VIEW_SILENT, "a session that is silent once more");
	cerbo_down(cerbo, 26000);
	check(view() == CERBO_VIEW_CONNECTING && connect_at(28000) && view() == CERBO_VIEW_QUIET && cerbo_view(cerbo, 38299) == CERBO_VIEW_QUIET,
	      "a new session begins quiet: the PINGRESP of the session before proves nothing");
	check(setup() && arrive(6000, 7, 1000) == 0 && (cerbo_down(cerbo, 7000), view()) == CERBO_VIEW_CONNECTING && connect_at(9000) && view() == CERBO_VIEW_LIVE &&
	      cerbo_view(cerbo, 15999) == CERBO_VIEW_LIVE && cerbo_view(cerbo, 16000) == CERBO_VIEW_QUIET, "a new session within 10 s of the last bundle delivers: the values are those of this broker");

	// A newer contract
	check(setup() && arrive(6000, 7, 1000) == 0 && (now = 6500, say("{\"v\":2}"), view()) == CERBO_VIEW_NEWER && cerbo_view(cerbo, 1000000) == CERBO_VIEW_NEWER,
	      "a bundle of a newer contract: the view says so, before live and before silent");
	cerbo_down(cerbo, 7000);
	check(view() == CERBO_VIEW_CONNECTING && cerbo_view(cerbo, 22000) == CERBO_VIEW_UNREACHABLE, "without a session the newer contract is not what is wrong");
	check(connect_at(9000) && view() == CERBO_VIEW_NEWER && arrive(9500, 7, 2000) == 0 && view() == CERBO_VIEW_LIVE, "in the next session it is, until a bundle is taken");
	check(place(AT_SUBSCRIBING) && (say("{\"v\":2}"), view()) == CERBO_VIEW_CONNECTING && (suback(CERBO_PACKET_ID, 0), view()) == CERBO_VIEW_NEWER, "a newer bundle before the SUBACK shows with the session");
	cerbo_network(cerbo, false, 6000);
	check(view() == CERBO_VIEW_NO_WIFI, "without a network that is what is wrong, whatever the session shows");

	// What shows
	check(setup() && !cerbo_showing(cerbo, now) && (now = 6000, bundle(7, 1000, ""), cerbo_showing(cerbo, now)) && cerbo_showing(cerbo, 15999) && !cerbo_showing(cerbo, 16000),
	      "a source that delivers shows, also a bundle without a value; without a value nothing shows when it is quiet");
	check(setup() && arrive(6000, 7, 1000) == 0 && cerbo_showing(cerbo, 16000) && cerbo_view(cerbo, 16000) == CERBO_VIEW_QUIET && cerbo_showing(cerbo, 65999) && !cerbo_showing(cerbo, 66000),
	      "a quiet source shows as long as a value is not gone");
	cerbo_network(cerbo, false, 7000);
	check(next_at(7000) == CERBO_DO_CLOSE && view() == CERBO_VIEW_NO_WIFI && cerbo_showing(cerbo, 7000) && cerbo_showing(cerbo, 65999) && !cerbo_showing(cerbo, 66000),
	      "the values stay for 60 s when the network is gone");
	check(setup() && arrive(6000, 7, 1000) == 0 && arrive(9000, 7, 2000) == 1700 && seen("GX_A") == 7300 && cerbo_showing(cerbo, 67299) && !cerbo_showing(cerbo, 67300),
	      "the 60 s count from the time the values were seen, less their lag");
}

static void test_clock(void)
{
	// A time before the latest one seen lets no time pass
	check(setup() && next_at(15300) == CERBO_DO_SEND && cerbo_next(cerbo, 1000) == CERBO_DO_NOTHING && cerbo_next(cerbo, 0) == CERBO_DO_NOTHING &&
	      next_at(25299) == CERBO_DO_NOTHING && next_at(25300) == CERBO_DO_SEND, "a question with an older time changes nothing");
	check(setup() && (now = 6000, bundle(7, 1000, ONE), now = 3000, bundle(7, 2000, ONE), seen("GX_A")) == 6000 && cerbo->bundle_ms == 6000 && cerbo->lag_ms == 0,
	      "a bundle reported with an older time arrived at the latest time seen");
	check(setup() && (cerbo_down(cerbo, 9000), cerbo_down(cerbo, 100), next_at(10999)) == CERBO_DO_NOTHING && next_at(11000) == CERBO_DO_OPEN && cerbo_next(cerbo, 500) == CERBO_DO_NOTHING,
	      "the wait counts from the latest time seen");
	begin();
	cerbo_network(cerbo, false, 9000);
	cerbo_network(cerbo, true, 2000);
	check(cerbo_view(cerbo, 9000) == CERBO_VIEW_CONNECTING && cerbo_view(cerbo, 23999) == CERBO_VIEW_CONNECTING && cerbo_view(cerbo, 24000) == CERBO_VIEW_UNREACHABLE &&
	      cerbo_next(cerbo, 2000) == CERBO_DO_OPEN && cerbo->open_ms == 9000, "a network reported with an older time was joined at the latest time seen");
	check(setup() && arrive(6000, 7, 1000) == 0 && next_at(20000) == CERBO_DO_SEND && cerbo_view(cerbo, 0) == CERBO_VIEW_QUIET && cerbo_view(cerbo, 15999) == CERBO_VIEW_QUIET,
	      "a view asked for an older time is the view of the latest time seen");
	check(setup() && (cerbo_set_host(cerbo, OTHER, 9000), cerbo_set_host(cerbo, HOST, 3000), cerbo_opened(cerbo, 4000), next_at(1000)) == CERBO_DO_CLOSE && cerbo->clock_ms == 9000,
	      "the time never runs backwards, whatever a call brings");
	begin_at(50000, HOST, false, NAME);
	cerbo_network(cerbo, true, 100);
	check(cerbo_next(cerbo, 200) == CERBO_DO_OPEN && cerbo->open_ms == 50000 && cerbo_next(cerbo, 64999) == CERBO_DO_NOTHING && cerbo_next(cerbo, 65000) == CERBO_DO_CLOSE,
	      "a display that started at 50000 never sees an earlier time");

	// At the end of the time nothing wraps
	begin_at(UINT64_MAX - 30000, HOST, false, NAME);
	cerbo_network(cerbo, true, now);
	check(connect_at(now) && next_at(UINT64_MAX - 19701) == CERBO_DO_NOTHING && next_at(UINT64_MAX - 19700) == CERBO_DO_SEND && next_at(UINT64_MAX - 9700) == CERBO_DO_SEND &&
	      next_at(UINT64_MAX) == CERBO_DO_NOTHING && view() == CERBO_VIEW_QUIET, "at the end of the time a session goes on as ever");
	cerbo_down(cerbo, UINT64_MAX);
	check(next() == CERBO_DO_NOTHING && view() == CERBO_VIEW_CONNECTING, "at the end of the time a wait never ends, and nothing wraps");
}

/*
 * A stream cut at every byte. What a broker sends in a session - bundles that are taken and that are refused
 * for every reason, a message that goes by, a PINGRESP - is fed in one piece, byte by byte and with one cut at
 * every position: the source has to be the same afterwards, to its last byte.
 */
static uint8_t cut_stream[12000];
static box_t cut_whole;

// The source in a session with a PINGREQ under way and a first bundle, at 15400
static bool cut_start(void)
{
	if(!place(AT_SESSION_PING)) return false;
	now = 15350;
	bundle(7, 1000, "\"GX_A\":1,\"GX_B\":\"on\"");
	now = 15400;
	return cerbo->bundles == 1;
}

static size_t cut_add(size_t length, bool retained, const char *topic, const char *json)
{
	return length + broker_publish(cut_stream + length, retained, topic, json, strlen(json));
}

static void test_cut(void)
{
	static char long_json[2100];
	size_t length = 0, cut;
	bool same = true;

	memset(long_json, 'z', sizeof(long_json) - 1);
	check(load("fixtures/gx_values_parked.json"), "the bundle with 43 values for the stream");
	length = cut_add(length, false, CERBO_TOPIC, text);
	length = cut_add(length, false, "display/gx/other", bundle_text(7, 2000, ONE));
	length = cut_add(length, true, CERBO_TOPIC, bundle_text(7, 2000, ONE));
	length = cut_add(length, false, CERBO_TOPIC, "{\"v\":2}");
	length = cut_add(length, false, CERBO_TOPIC, "{\"v\":1,\"boot\":0}");
	length = cut_add(length, false, CERBO_TOPIC, sized(1401));
	length = cut_add(length, false, CERBO_TOPIC, long_json);
	length += broker_pingresp(cut_stream + length);
	check(load("fixtures/gx_values_driving.json"), "the bundle with 41 values for the stream");
	length = cut_add(length, false, CERBO_TOPIC, text);
	length = cut_add(length, false, CERBO_TOPIC, bundle_text(204771133, 5000000200, "\"GX_A\":1,\"x\":2,\"GX_C\":\"off\",\"GX_A\":3"));
	length = cut_add(length, false, CERBO_TOPIC, "{\"v\":1,\"boot\":204771133,\"t\":5000000100,\"values\":{\"GX_Z\":-1.5,\"GX_Y\":null},\"more\":[1,{\"a\":2}]}");

	check(cut_start(), "a session with a PINGREQ under way");
	feed(cut_stream, length);
	memcpy(&cut_whole, &box, sizeof(box));
	check(cerbo->bundles == 5 && cerbo->refused_topic == 1 && cerbo->refused_retained == 1 && cerbo->refused_version == 1 && cerbo->refused_invalid == 1 && cerbo->refused_long == 2 &&
	      cerbo->skipped == 2 && cerbo->boots == 3 && cerbo->pings_open == 0 && cerbo->pong_ms == 15400 && !cerbo->failed && cerbo->values.count == 1 && number_is("GX_Z", -1.5) &&
	      cerbo->lag_ms == 100 && !cerbo->newer, "the stream in one piece: five bundles taken, six messages refused for five reasons, a PINGRESP, a lag at the end");

	if(!cut_start()) same = false;
	for(cut = 0; cut < length; cut++) feed(cut_stream + cut, 1);
	check(same && memcmp(&cut_whole, &box, sizeof(box)) == 0, "the stream byte by byte leaves the source as the stream in one piece, to its last byte");
	for(cut = 1; cut < length; cut++)
	{
		if(!cut_start()) same = false;
		feed(cut_stream, cut);
		feed(cut_stream + cut, length - cut);
		if(memcmp(&cut_whole, &box, sizeof(box)) != 0)
		{
			if(same) printf("  the stream cut at byte %lu of %lu leaves another source\n", (unsigned long)cut, (unsigned long)length);
			same = false;
		}
	}
	check(same && length > 5000, "the stream cut at every one of its bytes leaves the source as the stream in one piece");

	// The packets of the greeting, cut as well
	length = broker_connack(cut_stream, 0, 0);
	for(cut = 0; cut <= length; cut++)
	{
		if(!place(AT_GREETING)) same = false;
		feed(cut_stream, cut);
		if(cut < length && (cerbo->phase != CERBO_GREETING || cerbo->send)) same = false;
		feed(cut_stream + cut, length - cut);
		if(cerbo->phase != CERBO_SUBSCRIBING || next() != CERBO_DO_SEND || !out_is("fixtures/mqtt_subscribe.txt")) same = false;
	}
	length = cut_add(0, false, CERBO_TOPIC, bundle_text(7, 1000, ONE));
	length += broker_suback(cut_stream + length, CERBO_PACKET_ID, 0);
	length = cut_add(length, false, CERBO_TOPIC, bundle_text(7, 2000, "\"GX_B\":2"));
	for(cut = 0; cut <= length; cut++)
	{
		if(!place(AT_SUBSCRIBING)) same = false;
		feed(cut_stream, cut);
		feed(cut_stream + cut, length - cut);
		if(cerbo->phase != CERBO_SESSION || cerbo->bundles != 2 || !number_is("GX_B", 2) || cerbo->values.count != 1 || cerbo->sessions != 1 || cerbo->failed) same = false;
	}
	check(same, "a CONNACK, and a SUBACK between two bundles, cut at every byte: the connection goes its way");
	check(guards_intact() && written, "the cut streams write nothing outside the source");
}

// A message of `size` bytes on the topic of the source arrives from 6100 on, as fast as the platform reads:
// CERBO_READ_MAX bytes every CERBO_TURN_MS, and the platform asks for its orders in every turn. Returns the
// bytes of it that arrived when the session ended, or all of them; *sends: the packets sent meanwhile.
static size_t flood(size_t size, int *sends, bool *quiet)
{
	static uint8_t piece[CERBO_READ_MAX];
	uint8_t head[32];
	size_t sent;

	memset(piece, '{', sizeof(piece));
	now = 6100;
	feed(head, broker_publish_head(head, 0x30, CERBO_TOPIC, strlen(CERBO_TOPIC), size, 1));
	*sends = 0;
	*quiet = true;
	for(sent = 0; sent < size; sent += sizeof(piece))
	{
		cerbo_do_t order;

		now += CERBO_TURN_MS;
		feed(piece, sizeof(piece));
		if(cerbo->failed || cerbo->bundles != 1) *quiet = false;
		order = next();
		if(order == CERBO_DO_SEND) *sends += 1;
		if(order == CERBO_DO_CLOSE) return sent;
	}
	return size;
}

static void test_megabyte(void)
{
	bool quiet = true;
	int sends = 0;

	// 51 KB a second: a megabyte takes 20.5 s
	check(setup() && arrive(6000, 7, 1000) == 0, "a session with a value, and then a message of a megabyte comes");
	remember();
	check(flood(1048576, &sends, &quiet) == 1048576 && quiet && sends == 2 && now == 26580 && cerbo->phase == CERBO_SESSION && cerbo->refused_long == 1 && refused() == 1,
	      "a message of a megabyte goes by in 20.5 s: nothing of it is taken, it is counted as too long");
	check(memcmp(&snapshot.cerbo.values, &cerbo->values, sizeof(values_t)) == 0 && number_is("GX_A", 1) && age_at("GX_A", now) == VALUE_AGE_OLD && view() == CERBO_VIEW_QUIET,
	      "the values of the bundle before it are untouched: they grew old meanwhile");
	now = 26600;
	pingresp();
	pingresp();
	check(cerbo->pings_open == 0 && !cerbo->failed && next_at(35300) == CERBO_DO_SEND && cerbo->phase == CERBO_SESSION && arrive(35400, 7, 30400) == 0 && view() == CERBO_VIEW_LIVE,
	      "the PINGRESP behind the megabyte come 11 s after the first PINGREQ: in time, the session goes on");

	check(setup() && arrive(6000, 7, 1000) == 0, "a session with a value, and then a message of two megabytes comes");
	check(flood(2097152, &sends, &quiet) < 2097152 && quiet && sends == 2 && now == 35300 && cerbo->phase == CERBO_IDLE && cerbo->refused_long == 0,
	      "a message that takes longer than a PINGREQ may wait ends the session: the PINGRESP lies behind it");
	check(number_is("GX_A", 1) && age_at("GX_A", now) == VALUE_AGE_OLD && cerbo_showing(cerbo, now) && guards_intact(), "the values are untouched and old");
}

/* ---------------------------------------------------------------------------------------------------
 * The rules of cerbo.h a second time, for the walk below. In another shape: the model is told of packets,
 * not of bytes (told_next() in mqtt_harness.h reads the wire for it), and of what the flow of the walk wrote
 * into a message, not of JSON. A connection is a handful of facts instead of a phase, every wait is the time
 * at which it ends instead of a time and a duration, the PINGREQ under way are a list with one time each, and
 * the lag is reckoned with d and base as cerbo.h writes it down. All values of a bundle have one time.
 */
typedef struct
{
	char name[VALUE_NAME_SIZE];
	value_kind_t kind;
	double number;
} model_value_t;

typedef enum
{
	FATE_TAKEN,
	FATE_GONE_BY,       // too long for the reader
	FATE_TOPIC,
	FATE_RETAINED,
	FATE_LONG,
	FATE_INVALID,
	FATE_NEWER,
	FATES,
} fate_t;

// A message as the flow of the walk wrote it, and what has to become of it
typedef struct
{
	fate_t fate;
	uint32_t boot;
	int64_t t;
	int count;
	model_value_t values[VALUES_MAX];
	uint32_t skipped;
} message_t;

typedef struct
{
	char host[CERBO_HOST_SIZE];
	char client[MQTT_SUB_ID_MAX + 1];
	bool safe, joined;
	uint64_t time;

	bool socket;                // there is a connection, or one is being opened
	bool stands;
	int said;                   // handed out on it: 0 nothing, 1 CONNECT, 2 SUBSCRIBE
	int heard;                  // 0 nothing, 1 the CONNACK, 2 the SUBACK: a session
	bool doomed, moved;
	uint64_t session_due;       // a session has to stand before this time
	uint64_t attempt_due;       // the next attempt may begin at this time
	int ended;                  // connections that ended since the last session, or since the last new start
	uint64_t patience_until;    // "waiting" before this time, "not reachable" from it on
	uint64_t pings[4];          // when the PINGREQ without an answer were handed out, the oldest first
	int ping_count;
	uint64_t ping_due;
	uint64_t hush_from;
	bool proven;                // a PINGRESP came when the silence had lasted its time

	model_value_t values[VALUES_MAX];
	int count;
	uint64_t seen;

	// The names, in another shape than the source keeps them: each where it found a place, with the number of
	// the noting that brought it. The order is in those numbers; a name that gives way is written over in place.
	struct
	{
		char name[VALUE_NAME_SIZE];
		uint64_t noted;
		uint32_t sum;               // of the bytes of the name: two names with different sums are different names
	} names[VALUES_MAX];
	int name_count;
	uint64_t notings;
	// ... and as a caller reads them, made anew whenever one was noted (model_names())
	char named[VALUES_MAX][VALUE_NAME_SIZE];
	uint64_t names_told, names_looked;  // for the walk: the noting up to which it compared every name, and its turns since
	uint32_t given_way;             // names that gave way to a new one

	bool delivered;
	uint64_t delivered_at;
	bool newer;
	bool boot_known;
	uint32_t boot;
	bool based;
	int64_t base;
	uint64_t rest, yield_from;
	uint32_t lag;
	uint32_t bundles, sessions, boots, invalid, version, retained, topic, too_long, skipped, distrusted;
} model_t;

static const uint32_t MODEL_WAITS[] = CERBO_RETRY_MS;
static const char ID_CHARACTERS[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

static void model_reach(model_t *m, uint64_t at)
{
	if(at > m->time) m->time = at;
}

static void model_anew(model_t *m)
{
	m->ended = 0;
	m->attempt_due = m->time;
	m->patience_until = m->time + CERBO_GRACE_MS;
}

static void model_init(model_t *m, const char *host, bool safe, const char *name, uint64_t at)
{
	size_t i, count = 0;

	memset(m, 0, sizeof(*m));
	m->time = at;
	m->safe = safe;
	if(host != NULL && strlen(host) < CERBO_HOST_SIZE) strcpy(m->host, host);
	for(i = 0; name != NULL && i < CERBO_NAME_SIZE - 1 && name[i] != '\0'; i++)
	{
		if((unsigned char)name[i] < 128 && strchr(ID_CHARACTERS, name[i]) != NULL && count < MQTT_SUB_ID_MAX) m->client[count++] = name[i];
	}
	if(count == 0) strcpy(m->client, CERBO_CLIENT);
	model_anew(m);
}

static void model_set_host(model_t *m, const char *host, uint64_t at)
{
	char taken[CERBO_HOST_SIZE] = "";

	model_reach(m, at);
	if(host != NULL && strlen(host) < CERBO_HOST_SIZE) strcpy(taken, host);
	if(strcmp(taken, m->host) == 0) return;

	strcpy(m->host, taken);
	m->count = 0;
	m->name_count = 0;
	m->notings++;
	m->delivered = false;
	m->newer = false;
	m->boot_known = false;
	m->lag = 0;
	if(m->socket) m->moved = true;
	model_anew(m);
}

static void model_network(model_t *m, bool up, uint64_t at)
{
	model_reach(m, at);
	if(up && !m->joined) model_anew(m);
	m->joined = up;
}

static void model_gone(model_t *m)
{
	if(m->heard == 2) m->patience_until = m->time + CERBO_GRACE_MS;
	m->socket = false;
	m->heard = 0;
	m->ping_count = 0;
	if(m->moved)
	{
		m->moved = false;
		return;
	}
	m->attempt_due = m->time + MODEL_WAITS[m->ended < 3 ? m->ended : 3];
	m->ended++;
}

// What cerbo_next() has to return. *sends: the packet of CERBO_DO_SEND - C, S or P.
static cerbo_do_t model_next(model_t *m, uint64_t at, char *sends)
{
	*sends = '-';
	model_reach(m, at);
	if(!m->socket)
	{
		if(m->host[0] == '\0' || m->safe || !m->joined || m->time < m->attempt_due) return CERBO_DO_NOTHING;
		m->socket = true;
		m->stands = false;
		m->said = 0;
		m->heard = 0;
		m->doomed = false;
		m->session_due = m->time + CERBO_SESSION_MS;
		return CERBO_DO_OPEN;
	}
	if(!m->joined || m->moved || m->doomed || (m->heard < 2 && m->time >= m->session_due) || (m->ping_count > 0 && m->time >= m->pings[0] + CERBO_PONG_MS))
	{
		model_gone(m);
		return CERBO_DO_CLOSE;
	}
	if(m->heard == 2)
	{
		if(m->time < m->ping_due) return CERBO_DO_NOTHING;
		if(m->ping_count < 4) m->pings[m->ping_count] = m->time;
		m->ping_count++;
		m->ping_due = m->time + CERBO_PING_MS;
		*sends = 'P';
		return CERBO_DO_SEND;
	}
	if(m->stands && m->said == 0)
	{
		m->said = 1;
		*sends = 'C';
		return CERBO_DO_SEND;
	}
	if(m->heard == 1 && m->said == 1)
	{
		m->said = 2;
		*sends = 'S';
		return CERBO_DO_SEND;
	}
	return CERBO_DO_NOTHING;
}

static void model_opened(model_t *m, uint64_t at)
{
	model_reach(m, at);
	if(!m->socket || m->stands) return;
	m->stands = true;
	m->based = false;
}

static void model_down(model_t *m, uint64_t at)
{
	model_reach(m, at);
	if(m->socket) model_gone(m);
}

// Whether what arrives is read at all
static bool model_reads(const model_t *m)
{
	return m->socket && m->stands && !m->doomed && !m->moved;
}

static void model_connack(model_t *m, uint8_t code)
{
	if(m->said == 1 && m->heard == 0 && code == 0) m->heard = 1;
	else m->doomed = true;
}

static void model_suback(model_t *m, uint16_t id, uint8_t code)
{
	if(m->heard != 1 || m->said != 2 || id != CERBO_PACKET_ID || code != 0)
	{
		m->doomed = true;
		return;
	}
	m->heard = 2;
	m->sessions++;
	m->ended = 0;
	m->ping_count = 0;
	m->ping_due = m->time + CERBO_PING_MS;
	m->hush_from = m->time;
	m->proven = false;
}

static void model_pingresp(model_t *m)
{
	if(m->heard != 2 || m->ping_count == 0)
	{
		m->doomed = true;
		return;
	}
	m->ping_count--;
	memmove(m->pings, m->pings + 1, sizeof(m->pings) - sizeof(m->pings[0]));
	if(m->time - m->hush_from >= CERBO_SILENT_MS) m->proven = true;
}

// The names of the model in the order a caller reads them: the one noted first comes first
static void model_names(model_t *m)
{
	int i, k;

	for(i = 0; i < m->name_count; i++)
	{
		int earlier = 0;

		for(k = 0; k < m->name_count; k++)
		{
			if(m->names[k].noted < m->names[i].noted) earlier++;
		}
		strcpy(m->named[earlier], m->names[i].name);
	}
}

// A number of a name that two different names seldom share: the walk compares millions of names, and most
// pairs are told apart by it
static uint32_t name_sum(const char *name)
{
	uint32_t sum = 2166136261u;

	for(; *name != '\0'; name++) sum = (sum ^ (unsigned char)*name) * 16777619u;
	return sum;
}

static bool message_names(const uint32_t *sums, const message_t *message, const char *name, uint32_t sum)
{
	int i;

	for(i = 0; i < message->count; i++)
	{
		if(sums[i] == sum && strcmp(message->values[i].name, name) == 0) return true;
	}
	return false;
}

// The keys of a message that is taken, noted by the rules of cerbo.h: what has a text and is not noted yet; and
// where there is no place left, in the place of the name noted longest ago among those the message does not name
static void model_note(model_t *m, const message_t *message)
{
	uint32_t sums[VALUES_MAX];
	uint64_t before = m->notings;
	int i, k;

	for(i = 0; i < message->count; i++) sums[i] = name_sum(message->values[i].name);
	for(i = 0; i < message->count; i++)
	{
		const model_value_t *value = &message->values[i];
		int place = m->name_count;

		// The largest double is the largest number that has a text
		if(value->kind == VALUE_NUMBER && !(fabs(value->number) <= DBL_MAX)) continue;
		k = 0;
		while(k < m->name_count && (m->names[k].sum != sums[i] || strcmp(m->names[k].name, value->name) != 0)) k++;
		if(k < m->name_count) continue;

		if(m->name_count == VALUES_MAX)
		{
			place = -1;
			for(k = 0; k < m->name_count; k++)
			{
				if(message_names(sums, message, m->names[k].name, m->names[k].sum)) continue;
				if(place < 0 || m->names[k].noted < m->names[place].noted) place = k;
			}
			// A message names at most as many keys as there are places, and this one of them has none
			if(place < 0) continue;
			m->given_way++;
		}
		else m->name_count++;
		strcpy(m->names[place].name, value->name);
		m->names[place].sum = sums[i];
		m->names[place].noted = ++m->notings;
	}
	if(m->notings != before) model_names(m);
	// Every bundle that is taken lets the walk look at all the names again, also one that should note nothing
	m->notings++;
}

static void model_take(model_t *m, const message_t *message)
{
	int64_t d = (int64_t)m->time - message->t;

	if(!m->boot_known || m->boot != message->boot)
	{
		m->boots++;
		m->based = false;
	}
	if(m->based)
	{
		uint64_t yield = (m->time - m->yield_from) * CERBO_YIELD_PER_MILLE + m->rest;
		int64_t risen = m->base + (int64_t)(yield / 1000);

		m->rest = yield % 1000;
		m->base = d < risen ? d : risen;
		if(d - m->base > CERBO_LAG_MAX_MS)
		{
			m->distrusted++;
			m->based = false;
		}
	}
	if(!m->based)
	{
		m->base = d;
		m->rest = 0;
	}
	m->based = true;
	m->boot_known = true;
	m->boot = message->boot;
	m->yield_from = m->time;
	m->lag = (uint32_t)(d - m->base);
	m->seen = m->time > m->lag ? m->time - m->lag : 0;

	m->count = message->count;
	memcpy(m->values, message->values, sizeof(m->values));
	model_note(m, message);
	m->skipped += message->skipped;
	m->newer = false;
	m->delivered = true;
	m->delivered_at = m->time;
	m->hush_from = m->time;
	m->proven = false;
	m->bundles++;
}

static void model_message(model_t *m, const message_t *message)
{
	if(m->said < 2)
	{
		m->doomed = true;
		return;
	}
	switch(message->fate)
	{
		case FATE_GONE_BY:
		case FATE_LONG:     m->too_long++; break;
		case FATE_TOPIC:    m->topic++; break;
		case FATE_RETAINED: m->retained++; break;
		case FATE_INVALID:  m->invalid++; break;
		case FATE_NEWER:
			m->version++;
			m->newer = true;
			break;
		default:            model_take(m, message); break;
	}
}

static cerbo_view_t model_view(const model_t *m, uint64_t at)
{
	uint64_t t = at > m->time ? at : m->time;

	if(m->safe || m->host[0] == '\0') return CERBO_VIEW_OFF;
	if(!m->joined) return CERBO_VIEW_NO_WIFI;
	if(!m->socket || m->heard < 2) return t < m->patience_until ? CERBO_VIEW_CONNECTING : CERBO_VIEW_UNREACHABLE;
	if(m->newer) return CERBO_VIEW_NEWER;
	if(m->delivered && t < m->delivered_at + CERBO_LIVE_MS) return CERBO_VIEW_LIVE;
	return m->proven ? CERBO_VIEW_SILENT : CERBO_VIEW_QUIET;
}

static value_age_t model_age(const model_t *m, uint64_t at)
{
	if(at < m->seen + CERBO_FRESH_MS) return VALUE_AGE_FRESH;
	if(at < m->seen + CERBO_KEPT_MS) return VALUE_AGE_OLD;
	return VALUE_AGE_GONE;
}

// The values GET /api/values names of what the last bundle brought: all of them, less the numbers without end
static int model_shown(const model_t *m, uint64_t at)
{
	int shown = 0;

	if(model_age(m, at) == VALUE_AGE_GONE) return 0;
	for(int i = 0; i < m->count; i++)
	{
		// The largest double is the largest number that has a text
		if(m->values[i].kind != VALUE_NUMBER || fabs(m->values[i].number) <= DBL_MAX) shown++;
	}
	return shown;
}

static bool model_showing(const model_t *m, uint64_t at)
{
	return model_view(m, at) == CERBO_VIEW_LIVE || (m->count > 0 && model_age(m, at) != VALUE_AGE_GONE);
}

/* ---------------------------------------------------------------------------------------------------
 * A random broker and a random world. The walk is the platform: it carries out the orders of the source,
 * with a radio link that pauses, a broker that answers right, wrong or not at all, and a flow that writes
 * bundles of every kind - right ones with clocks that drift, jump and begin anew, and wrong ones for every
 * reason cerbo.h names. The bytes reach the source in pieces of any size. After every call the source is
 * compared with the model, and these promises of cerbo.h are watched on their own, without the model:
 *   - an order to open comes only without a connection, an order to send or to close only with one
 *   - every packet that is sent begins with 0x10, 0x82 or 0xC0: on a connection first one CONNECT with the
 *     client id of the display, a clean session and nothing else; then, after a CONNACK that accepted, at most
 *     one SUBSCRIBE to the one topic with QoS 0; PINGREQ only after the SUBACK
 *   - never more than two PINGREQ wait for their answer
 *   - every value in the store has a key of the Cerbo, no key is there twice, and no value was seen later than
 *     the latest time the source knows; every key of the store whose value has a text is among the names, and
 *     no name is there twice
 *   - in safe mode and without an address nothing is opened
 *   - after any walk a healthy broker, a healthy flow and two and a half minutes lead to a source that delivers
 */

#define WALKS           120
#define WALK_CALLS      5000
#define WALK_HEAL_MS    150000u
#define WIRE_ROOM       (256 * 1024)
#define WIRED           256

enum
{
	PROMISE_ORDERS,
	PROMISE_SENT,
	PROMISE_PINGS,
	PROMISE_STORE,
	PROMISE_OFF,
	PROMISE_HEALS,
	PROMISE_HARNESS,        // the walk itself lost its place: its wire and its notes disagree
	PROMISES,
};

typedef struct
{
	long calls, different;
	long broken[PROMISES];
	long orders[CERBO_DO_CLOSE + 1];
	long views[CERBO_VIEW_LIVE + 1];
	long fates[FATES];                  // messages that were read, by what became of them
	long named_full, given_way;         // bundles taken while 64 names were kept, and names that gave way to a new one
	long forgotten;                     // addresses set that forgot names
	long sessions, downs, pauses, naps, hosts, networks, stale;
	long lagged, old, distrusted, boots, full, over, empty, cuts, bursts, before_suback, two_pings, out_of_place, broken_bytes, refusals, ignored;
	long endless;                       // bundles taken that brought a number without end: a value in the store that is not counted
	long healed, healed_off;
} walk_result_t;

typedef struct
{
	char what;              // A CONNACK, S SUBACK, P PINGRESP, M a message, X broken bytes
	message_t message;      // of M
} wired_t;

static struct
{
	uint64_t time;
	bool socket, open;                  // of the platform
	bool connack_came, suback_came;     // what the display has read on this connection: for the promises
	int connects_sent, subscribes_sent;
	bool radio;
	uint64_t radio_back;
	char unheard[16];                   // what the display sent while the link paused, in its order
	int unheard_count;
	bool heard_connect, heard_subscribe;    // what the broker got
	bool dead;                          // the bytes on the wire are broken from some point on: told_next() said so
	uint8_t wire[WIRE_ROOM];
	size_t wire_length, delivered, told_at;
	wired_t wired[WIRED];               // what the packets on the wire are, the oldest first
	int wired_first, wired_count;
	// the flow
	uint32_t boot, other_boot;
	int64_t t, other_t;
	uint64_t flow_at, publish_at;
	uint64_t asleep_until;              // the flow sends nothing before this time: somebody stopped it
	uint32_t rate;                      // thousandths of the pace of the display
	bool two_flows, turn;
	bool healing;
} world;

static model_t model;
static uint32_t walk_state;
static char flow_text[8192];
static uint8_t flow_packet[8192 + 64];

// Never two rolls in one expression whose order C leaves open - among the arguments of one call, or on both
// sides of an operator: gcc and clang work them out in different orders, and the walks would be other walks
static uint32_t roll(uint32_t below)
{
	walk_state = walk_state * 1664525u + 1013904223u;
	return (walk_state >> 8) % below;
}

static void world_reset_connection(void)
{
	world.socket = false;
	world.open = false;
	world.connack_came = false;
	world.suback_came = false;
	world.connects_sent = 0;
	world.subscribes_sent = 0;
	world.unheard_count = 0;
	world.heard_connect = false;
	world.heard_subscribe = false;
	world.dead = false;
	world.wire_length = 0;
	world.delivered = 0;
	world.told_at = 0;
	world.wired_first = 0;
	world.wired_count = 0;
}

// Room on the wire for a packet of `length` bytes and a note of it; NULL if the broker has to drop it
static wired_t *wire_room(size_t length, char what)
{
	wired_t *note;

	if(world.wired_count >= WIRED || world.wire_length + length > WIRE_ROOM) return NULL;
	note = &world.wired[(world.wired_first + world.wired_count) % WIRED];
	world.wired_count++;
	note->what = what;
	return note;
}

static void wire_bytes(const uint8_t *bytes, size_t length)
{
	memcpy(world.wire + world.wire_length, bytes, length);
	world.wire_length += length;
}

static void wire_connack(uint8_t code)
{
	uint8_t bytes[8];

	if(wire_room(4, 'A') != NULL) wire_bytes(bytes, broker_connack(bytes, 0, code));
}

static void wire_suback(uint16_t id, uint8_t code)
{
	uint8_t bytes[8];

	if(wire_room(5, 'S') != NULL) wire_bytes(bytes, broker_suback(bytes, id, code));
}

static void wire_pingresp(void)
{
	uint8_t bytes[8];

	if(wire_room(2, 'P') != NULL) wire_bytes(bytes, broker_pingresp(bytes));
}

// Something no broker may send; the reader calls it broken
static void wire_broken(void)
{
	static const char *const wrong[] = {"\xE0\x00", "\x32\x05\x00\x01" "a\x00\x01", "\x30\x80\x80\x80\x80", "\x20\x03\x00\x00\x00", "\x20\x02\x01\x00", "\x30\x03\x00\x00" "a",
	                                    "\x30\x04\x00\x09" "ab", "\x90\x02\x00\x01", "\xD0\x01\x00", "\x00", "\xC0\x00", "\x30\x01"};
	static const size_t lengths[] = {2, 7, 5, 5, 4, 5, 6, 4, 3, 1, 2, 2};
	uint32_t which = roll(COUNT(lengths));

	if(wire_room(lengths[which], 'X') != NULL) wire_bytes((const uint8_t *)wrong[which], lengths[which]);
}

/* The flow of the walk */

typedef struct
{
	const char *json;       // as it is written
	const char *name;       // as it reads, NULL: no key of the contract
} flow_key_t;

typedef struct
{
	const char *json;
	value_kind_t kind;
	double number;
	bool taken;
	int tokens;
} flow_value_t;

static const flow_key_t FLOW_KEYS[] = {
	{"GX_A", "GX_A"}, {"GX_0", "GX_0"}, {"GX__", "GX__"}, {"GX_BATT_SOC", "GX_BATT_SOC"}, {"GX_SW_B2B", "GX_SW_B2B"},
	{"GX_12345678901234567890123456789", "GX_12345678901234567890123456789"}, {"GX_\\u0041", "GX_A"}, {"GX_\\u004b07", "GX_K07"},
	{"A", NULL}, {"ENGINE_RPM", NULL}, {"gx_A", NULL}, {"GX_a", NULL}, {"GX_", NULL}, {"GX", NULL}, {"", NULL}, {"GX_A-B", NULL}, {" GX_A", NULL},
	{"GX_123456789012345678901234567890", NULL}, {"GX_\xC3\xA4", NULL}, {"GX_\\u00e4", NULL}, {"GX_K0\\u0000", NULL},
};

static const flow_value_t FLOW_VALUES[] = {
	{"0", VALUE_NUMBER, 0, true, 1}, {"1", VALUE_NUMBER, 1, true, 1}, {"-4.8", VALUE_NUMBER, -4.8, true, 1}, {"99.7", VALUE_NUMBER, 99.7, true, 1},
	{"26.54", VALUE_NUMBER, 26.54, true, 1}, {"1e3", VALUE_NUMBER, 1000, true, 1}, {"1E-2", VALUE_NUMBER, 0.01, true, 1}, {"-0", VALUE_NUMBER, 0, true, 1},
	{"123456789012", VALUE_NUMBER, 123456789012.0, true, 1}, {"12.50", VALUE_NUMBER, 12.5, true, 1}, {"3.31", VALUE_NUMBER, 3.31, true, 1}, {"-127", VALUE_NUMBER, -127, true, 1},
	{"1e999", VALUE_NUMBER, INFINITY, true, 1}, {"-1e999", VALUE_NUMBER, -INFINITY, true, 1},
	{"\"on\"", VALUE_ON, 0, true, 1}, {"\"off\"", VALUE_OFF, 0, true, 1}, {"\"on\"", VALUE_ON, 0, true, 1}, {"\"off\"", VALUE_OFF, 0, true, 1},
	{"true", VALUE_NUMBER, 0, false, 1}, {"null", VALUE_NUMBER, 0, false, 1}, {"\"On\"", VALUE_NUMBER, 0, false, 1}, {"\"\"", VALUE_NUMBER, 0, false, 1},
	{"\"1\"", VALUE_NUMBER, 0, false, 1}, {"{}", VALUE_NUMBER, 0, false, 1}, {"[]", VALUE_NUMBER, 0, false, 1}, {"{\"on\":1}", VALUE_NUMBER, 0, false, 3},
	{"[1,2]", VALUE_NUMBER, 0, false, 3}, {"[\"on\"]", VALUE_NUMBER, 0, false, 2}, {"123456789012345678901234567890123456789012345678", VALUE_NUMBER, 0, false, 1},
};

static size_t flow_length;

static void flow_add(const char *more)
{
	size_t length = strlen(more);

	if(flow_length + length >= sizeof(flow_text))
	{
		written = false;
		return;
	}
	memcpy(flow_text + flow_length, more, length + 1);
	flow_length += length;
}

// One member of `values`, written and noted for the model. in_row: the member is GX_K<in_row> with a number,
// so that a bundle fills the store to a known place; -1: any key, any value. Returns the tokens it needs.
static int flow_member(message_t *message, bool first, int in_row)
{
	char key[48], number[24];
	const char *key_json, *name, *value_json;
	flow_value_t chosen;
	uint32_t pick = in_row < 0 ? roll(100) : 0;
	int i;

	if(pick < 72)
	{
		int length = snprintf(key, sizeof(key), "GX_K%02u", in_row < 0 ? (unsigned)roll(72) : (unsigned)in_row);

		if(length < 0 || (size_t)length >= sizeof(key)) written = false;
		key_json = key;
		name = key;
	}
	else
	{
		const flow_key_t *listed = &FLOW_KEYS[roll(COUNT(FLOW_KEYS))];

		key_json = listed->json;
		name = listed->name;
	}

	pick = in_row < 0 ? roll(100) : 0;
	if(pick < 40)
	{
		int whole = (int)roll(100000) - 50000;
		int length = snprintf(number, sizeof(number), "%d", whole);

		if(length < 0 || (size_t)length >= sizeof(number)) written = false;
		chosen.json = number;
		chosen.kind = VALUE_NUMBER;
		chosen.number = whole;
		chosen.taken = true;
		chosen.tokens = 1;
	}
	else
	{
		// The values that are taken come first in the list
		uint32_t which = pick < 85 ? roll(18) : roll(COUNT(FLOW_VALUES));

		chosen = FLOW_VALUES[which];
	}
	value_json = chosen.json;

	if(!first) flow_add(",");
	flow_add("\"");
	flow_add(key_json);
	flow_add("\":");
	flow_add(value_json);

	if(name == NULL || !chosen.taken)
	{
		message->skipped++;
		return 1 + chosen.tokens;
	}
	for(i = 0; i < message->count; i++)
	{
		if(strcmp(message->values[i].name, name) == 0) break;
	}
	if(i == message->count)
	{
		if(message->count == VALUES_MAX)
		{
			message->skipped++;
			return 1 + chosen.tokens;
		}
		message->count++;
		memset(&message->values[i], 0, sizeof(message->values[i]));
		strcpy(message->values[i].name, name);
	}
	message->values[i].kind = chosen.kind;
	message->values[i].number = chosen.number;
	return 1 + chosen.tokens;
}

// The clock of the flow at the time `at` of the world, with everything a clock does
static void flow_tick(uint64_t at)
{
	uint64_t elapsed = at > world.flow_at ? at - world.flow_at : 0;
	uint32_t whim = world.healing ? 1000 : roll(1000);
	uint32_t high, low;

	if(at > world.flow_at) world.flow_at = at;
	world.t += (int64_t)(elapsed * world.rate / 1000);
	world.other_t += (int64_t)elapsed;
	if(whim < 8)
	{
		// The clock was set back: the flow loses time
		static const int64_t lost[] = {500, 2000, 11000, 60000};

		world.t -= lost[roll(COUNT(lost))];
		if(world.t < 0) world.t = 0;
	}
	else if(whim < 14)
	{
		static const int64_t gained[] = {700, 5000, 3600000, 31536000000};

		world.t += gained[roll(COUNT(gained))];
	}
	else if(whim < 18)
	{
		// Node-RED started anew
		static const int64_t starts[] = {0, 0, 7000, 4294960000, 9007199254730000};

		high = roll(32768);
		low = roll(65536);
		world.boot = 1 + (high * 65536u + low) % 2147483647u;
		world.t = starts[roll(COUNT(starts))];
	}
	else if(whim < 21) world.two_flows = !world.two_flows;
	else if(whim < 25)
	{
		static const uint32_t rates[] = {1000, 1000, 990, 1010, 1100, 900};

		world.rate = rates[roll(COUNT(rates))];
	}
}

// Writes the message the flow sends at `at` to flow_packet and notes what has to become of it. Returns its
// length.
static size_t flow_write(message_t *message, uint64_t at)
{
	static const char *const topics[] = {"display/gx/value", "display/gx/values2", "display/gx/valueS", "Display/gx/values", "x", "display/gx/values/"};
	static const char *const versions[] = {"2", "3", "2147483648", "9223372036854775807"};
	static const char *const bad_v[] = {NULL, "0", "-1", "1.0", "\"1\"", "true", "99999999999999999999"};
	static const char *const bad_boot[] = {NULL, "0", "2147483648", "-5", "1.5", "\"7\"", "4294967303"};
	static const char *const bad_t[] = {NULL, "-1", "1.5", "9007199254740992", "\"12\"", "1e3"};
	static const char *const bad_values[] = {NULL, "[]", "5", "null", "\"GX_A\""};
	static const char *const extras[] = {"\"flow\":\"x\"", "\"n\":null", "\"note\":{\"a\":[1,2]}", "\"t2\":17", "\"V\":2"};
	static const int extra_tokens[] = {2, 2, 6, 2, 2};
	const char *topic = CERBO_TOPIC;
	const char *v_text = "1", *boot_text, *t_text, *values_text = "";
	char boot_number[24], t_number[24];
	bool retained = false, unreadable = false, newer = false, invalid = false;
	uint32_t kind = world.healing ? 0 : roll(100);
	uint32_t members, size, extra, i;
	int tokens = 1, length, padding = 1;
	size_t head, pad = 0;

	memset(message, 0, sizeof(*message));
	flow_tick(at);
	world.turn = !world.turn;
	if(world.two_flows && world.turn)
	{
		message->boot = world.other_boot;
		message->t = world.other_t;
	}
	else
	{
		message->boot = world.boot;
		message->t = world.t;
	}
	length = snprintf(boot_number, sizeof(boot_number), "%" PRIu32, message->boot);
	if(length < 0 || (size_t)length >= sizeof(boot_number)) written = false;
	length = snprintf(t_number, sizeof(t_number), "%" PRId64, message->t);
	if(length < 0 || (size_t)length >= sizeof(t_number)) written = false;
	boot_text = boot_number;
	t_text = t_number;
	if(message->t > CERBO_T_MAX) invalid = true;

	if(!world.healing)
	{
		if(roll(25) == 0) topic = topics[roll(COUNT(topics))];
		if(roll(30) == 0) retained = true;
		if(roll(12) == 0) padding = 2 + (int)roll(3);
	}
	if(kind >= 70 && kind < 76)
	{
		v_text = versions[roll(COUNT(versions))];
		newer = true;
	}
	else if(kind >= 76 && kind < 86)
	{
		uint32_t which = roll(4);

		invalid = true;
		if(which == 0) v_text = bad_v[roll(COUNT(bad_v))];
		else if(which == 1) boot_text = bad_boot[roll(COUNT(bad_boot))];
		else if(which == 2) t_text = bad_t[roll(COUNT(bad_t))];
		else values_text = bad_values[roll(COUNT(bad_values))];
		// A wrong v counts before a newer one could, and nothing behind a newer v is looked at
		if(which == 0) newer = false;
	}

	// The members of the bundle, in one of two orders, with members the contract does not know between them
	flow_length = 0;
	flow_text[0] = '\0';
	flow_add("{");
	extra = (world.healing || roll(4) != 0) ? COUNT(extras) : roll(COUNT(extras));
	if(extra < COUNT(extras))
	{
		flow_add(extras[extra]);
		flow_add(",");
		tokens += extra_tokens[extra];
	}
	if(v_text != NULL)
	{
		flow_add("\"v\":");
		flow_add(v_text);
		flow_add(",");
		tokens += 2;
	}
	if(boot_text != NULL)
	{
		flow_add("\"boot\":");
		flow_add(boot_text);
		flow_add(",");
		tokens += 2;
	}
	if(t_text != NULL)
	{
		flow_add("\"t\":");
		flow_add(t_text);
		flow_add(",");
		tokens += 2;
	}
	if(values_text == NULL) flow_add("\"end\":0");
	else if(values_text[0] != '\0')
	{
		flow_add("\"values\":");
		flow_add(values_text);
	}
	else
	{
		size = world.healing ? 2 : roll(20);
		if(size < 2) members = 0;
		else if(size < 12) members = 1 + roll(12);
		else if(size < 16) members = 38 + roll(8);
		else if(size < 18) members = 64;
		else members = 65 + roll(8);
		flow_add("\"values\":{");
		if(world.healing)
		{
			flow_add("\"GX_HEAL\":1");
			tokens += 2;
			message->count = 1;
			strcpy(message->values[0].name, "GX_HEAL");
			message->values[0].kind = VALUE_NUMBER;
			message->values[0].number = 1;
		}
		// The large bundles count their keys up, so that the store is full at a known member
		for(i = 0; i < members && flow_length < 3600; i++) tokens += flow_member(message, i == 0 && !world.healing, members >= 64 ? (int)i : -1);
		flow_add("}");
	}
	tokens += 2;

	// What makes the text one the reader cannot read
	if(kind >= 86 && kind < 88)
	{
		flow_add(",\"x\":[[[[[[[[1]]]]]]]]");
		unreadable = true;
	}
	else if(kind >= 88 && kind < 90)
	{
		flow_add(",\"x\":[");
		for(i = 0; i < 260; i++) flow_add(i == 0 ? "1" : ",1");
		flow_add("]");
		tokens += 262;
	}
	flow_add("}");
	if(tokens > CERBO_TOKENS) unreadable = true;
	if(kind >= 90 && kind < 92)
	{
		// Not all of it
		flow_length -= 1 + roll((uint32_t)flow_length - 1);
		flow_text[flow_length] = '\0';
		unreadable = true;
	}
	else if(kind == 92)
	{
		flow_add(" x");
		unreadable = true;
	}
	else if(kind == 93)
	{
		memmove(flow_text + 1, flow_text, flow_length + 1);
		flow_text[0] = '[';
		flow_length++;
		flow_add("]");
		unreadable = true;
	}
	else if(kind == 94)
	{
		flow_length = 0;
		flow_text[0] = '\0';
		unreadable = true;
	}
	else if(kind >= 95 && kind < 99 && flow_length + 40 < 1400)
	{
		// As long as a bundle may be, a byte longer, as long as the reader keeps, a byte longer than that, or longer
		static const size_t sizes[] = {1400, 1401, 1517, 1518, 2600};
		size_t wanted = sizes[roll(COUNT(sizes))];

		flow_text[flow_length - 1] = '\0';
		flow_length--;
		flow_add(",\"pad\":\"");
		pad = wanted - flow_length - 2;
		if(flow_length + pad + 3 < sizeof(flow_text))
		{
			memset(flow_text + flow_length, 'p', pad);
			flow_length += pad;
			flow_text[flow_length] = '\0';
		}
		flow_add("\"}");
		if(tokens + 2 > CERBO_TOKENS) unreadable = true;
	}

	// What becomes of it, in the order of cerbo.h
	if(2 + strlen(topic) + flow_length > MQTT_SUB_PACKET_MAX) message->fate = FATE_GONE_BY;
	else if(strcmp(topic, CERBO_TOPIC) != 0) message->fate = FATE_TOPIC;
	else if(retained) message->fate = FATE_RETAINED;
	else if(flow_length > CERBO_BUNDLE_MAX) message->fate = FATE_LONG;
	else if(unreadable) message->fate = FATE_INVALID;
	else if(newer) message->fate = FATE_NEWER;
	else if(invalid) message->fate = FATE_INVALID;
	else message->fate = FATE_TAKEN;

	head = broker_publish_head(flow_packet, retained ? 0x31 : 0x30, topic, strlen(topic), flow_length, padding);
	memcpy(flow_packet + head, flow_text, flow_length);
	return head + flow_length;
}

// The flow sends a message at `at`, and the broker passes it on: it lies on the wire until the radio link
// carries it
static void world_publish(uint64_t at)
{
	static message_t message;
	size_t length = flow_write(&message, at);
	wired_t *note = wire_room(length, 'M');

	if(note == NULL) return;
	note->message = message;
	wire_bytes(flow_packet, length);
}

// The broker gets a packet of the display and answers it, mostly as it should
static void broker_hears(char what)
{
	uint32_t mood = world.healing ? 99 : roll(100);

	if(what == 'C')
	{
		world.heard_connect = true;
		if(mood < 3) return;
		if(mood < 6) wire_connack((uint8_t)(1 + roll(5)));
		else if(mood < 7) wire_suback(CERBO_PACKET_ID, 0);
		else if(mood < 8) world_publish(world.time);
		else if(mood < 10) wire_broken();
		else wire_connack(0);
	}
	else if(what == 'S')
	{
		world.heard_subscribe = true;
		world.publish_at = world.time;
		if(mood < 3) return;
		if(mood < 5) wire_suback(CERBO_PACKET_ID, 0x80);
		else if(mood < 6) wire_suback(CERBO_PACKET_ID, 1);
		else if(mood < 7) wire_suback(CERBO_PACKET_ID + 1, 0);
		else if(mood < 8) wire_connack(0);
		else if(mood < 10) wire_broken();
		else
		{
			// MQTT 3.1.1, 3.8.4: a message may come before the SUBACK
			if(mood < 30) world_publish(world.time);
			wire_suback(CERBO_PACKET_ID, 0);
		}
	}
	else
	{
		if(mood < 4) return;
		wire_pingresp();
		if(mood < 5) wire_pingresp();
	}
}

static const char *const WALK_HOSTS[] = {HOST, HOST, HOST, HOST, HOST, OTHER, OTHER, "10.0.0.1", "", NULL, "192.168.100.1000"};

static void promise(bool kept, int which, walk_result_t *result)
{
	if(!kept) result->broken[which]++;
}

// The packets that became complete in the bytes the display has got: the model is told of each
static void world_tell(uint64_t at, walk_result_t *result)
{
	told_t told;
	int complete = 0;

	model_reach(&model, at);
	while(!world.dead)
	{
		mqtt_sub_kind_t kind = told_next(world.wire, world.delivered, &world.told_at, &told);
		wired_t *note;
		bool reads = model_reads(&model);
		bool doomed = model.doomed;

		if(kind == MQTT_SUB_NONE) break;
		if(world.wired_count == 0)
		{
			promise(false, PROMISE_HARNESS, result);
			world.dead = true;
			break;
		}
		note = &world.wired[world.wired_first];
		world.wired_first = (world.wired_first + 1) % WIRED;
		world.wired_count--;

		if(kind == MQTT_SUB_BROKEN)
		{
			promise(note->what == 'X', PROMISE_HARNESS, result);
			world.dead = true;
			if(reads)
			{
				model.doomed = true;
				result->broken_bytes++;
			}
			break;
		}
		if(!reads)
		{
			result->ignored++;
			continue;
		}
		if(kind == MQTT_SUB_CONNACK)
		{
			promise(note->what == 'A', PROMISE_HARNESS, result);
			model_connack(&model, told.code);
			if(!model.doomed) world.connack_came = true;
			else if(told.code != 0) result->refusals++;
		}
		else if(kind == MQTT_SUB_SUBACK)
		{
			promise(note->what == 'S', PROMISE_HARNESS, result);
			model_suback(&model, told.id, told.code);
			if(!model.doomed)
			{
				world.suback_came = true;
				result->sessions++;
			}
			else if(told.code != 0) result->refusals++;
		}
		else if(kind == MQTT_SUB_PINGRESP)
		{
			promise(note->what == 'P', PROMISE_HARNESS, result);
			model_pingresp(&model);
		}
		else
		{
			promise(note->what == 'M' && (kind == MQTT_SUB_SKIPPED) == (note->message.fate == FATE_GONE_BY), PROMISE_HARNESS, result);
			model_message(&model, &note->message);
			if(!model.doomed)
			{
				result->fates[note->message.fate]++;
				if(note->message.fate == FATE_TAKEN)
				{
					complete++;
					if(model.heard < 2) result->before_suback++;
					if(model.lag > 0) result->lagged++;
					if(model.lag >= CERBO_FRESH_MS) result->old++;
					if(model.count == VALUES_MAX) result->full++;
					if(model.count == VALUES_MAX && note->message.skipped > 0) result->over++;
					if(model.count == 0) result->empty++;
					if(model_shown(&model, model.seen) < model.count) result->endless++;
					if(model.name_count == VALUES_MAX) result->named_full++;
				}
			}
		}
		if(model.doomed && !doomed) result->out_of_place++;
	}
	if(complete >= 3) result->bursts++;
	if(world.delivered > world.told_at && !world.dead) result->cuts++;

	// What was read needs no room any more
	if(world.told_at > 0)
	{
		memmove(world.wire, world.wire + world.told_at, world.wire_length - world.told_at);
		world.wire_length -= world.told_at;
		world.delivered -= world.told_at;
		world.told_at = 0;
	}
}

// Some bytes of the wire reach the display
static void world_deliver(uint64_t at, walk_result_t *result)
{
	size_t pending = world.wire_length - world.delivered;
	uint32_t kind = world.healing ? 9 : roll(10);
	size_t size;

	if(kind < 3) size = 1 + roll(16);
	else if(kind < 8) size = 1 + roll(CERBO_READ_MAX);
	else size = pending;
	if(size > pending) size = pending;

	cerbo_bytes(cerbo, world.wire + world.delivered, size, at, work, CERBO_TOKENS);
	world.delivered += size;
	world_tell(at, result);
}

// The source and the model say the same about everything a caller can ask
static const char *walk_differs(uint64_t at)
{
	static const uint64_t steps[] = {1, 999, 4999, 5000, 9999, 10000, 14999, 15000, 19999, 20000, 59999, 60000};
	uint64_t other;
	int i;

	if(cerbo_on(cerbo) != (model.host[0] != '\0' && !model.safe)) return "on";
	if(strcmp(cerbo_host(cerbo), model.host) != 0) return "the address";
	if((cerbo->phase == CERBO_IDLE) != !model.socket) return "a connection";
	if((cerbo->phase == CERBO_SESSION) != (model.socket && model.heard == 2)) return "a session";
	if(cerbo_view(cerbo, at) != model_view(&model, at)) return "the view";
	if(cerbo_showing(cerbo, at) != model_showing(&model, at)) return "showing";
	other = at + steps[roll(COUNT(steps))];
	if(cerbo_view(cerbo, other) != model_view(&model, other)) return "the view later";
	if(cerbo_showing(cerbo, other) != model_showing(&model, other)) return "showing later";
	if(cerbo_view(cerbo, model.patience_until - 1) != model_view(&model, model.patience_until - 1)) return "the view at the end of the waiting";
	if(cerbo_view(cerbo, model.delivered_at + CERBO_LIVE_MS) != model_view(&model, model.delivered_at + CERBO_LIVE_MS)) return "the view when live ends";
	if(cerbo_showing(cerbo, model.seen + CERBO_KEPT_MS - 1) != model_showing(&model, model.seen + CERBO_KEPT_MS - 1)) return "showing at the end of the values";

	if(cerbo->bundles != model.bundles || cerbo->sessions != model.sessions || cerbo->boots != model.boots) return "the counters of bundles, sessions or boots";
	if(cerbo->refused_invalid != model.invalid || cerbo->refused_version != model.version || cerbo->refused_retained != model.retained ||
	   cerbo->refused_topic != model.topic || cerbo->refused_long != model.too_long) return "the counters of refused messages";
	if(cerbo_refused(cerbo) != model.invalid + model.version + model.retained + model.topic + model.too_long) return "the number of refused messages";
	if(cerbo_shown(cerbo, at) != model_shown(&model, at)) return "the number of values to show";
	if(cerbo_shown(cerbo, other) != model_shown(&model, other)) return "the number of values to show later";
	if(cerbo->skipped != model.skipped) return "the counter of skipped members";
	if(cerbo->distrusted != model.distrusted) return "the counter of lags not believed";
	if(cerbo->lag_ms != model.lag) return "the lag";

	if(cerbo->values.count != model.count) return "the number of values";
	for(i = 0; i < model.count; i++)
	{
		// In the order of the bundle, as the flow of the walk noted them
		const value_t *found = &cerbo->values.items[i];

		if(strcmp(found->name, model.values[i].name) != 0) return "the key of a value";
		if(found->kind != model.values[i].kind || found->number != model.values[i].number) return "a value";
		if(found->seen_ms != model.seen) return "the time a value was seen";
		if(cerbo_age(found, at) != model_age(&model, at) || cerbo_age(found, other) != model_age(&model, other)) return "the age of a value";
	}

	// The names: all of them behind every call that may have noted or forgotten one, and one of them in turn
	// behind every other call - nothing else may move a name
	if(cerbo->name_count != model.name_count) return "the number of names";
	if(model.name_count > 0 && model.names_told == model.notings)
	{
		i = (int)(model.names_looked++ % (uint64_t)model.name_count);
		if(strcmp(cerbo->names[i], model.named[i]) != 0) return "a name that nothing moved";
		return NULL;
	}
	model.names_told = model.notings;
	for(i = 0; i < model.name_count; i++)
	{
		if(strcmp(cerbo->names[i], model.named[i]) != 0) return "a name, or its place";
	}
	return NULL;
}

static bool key_of_the_contract(const char *name)
{
	size_t length = strlen(name), i;

	if(length < 4 || length > 32 || memcmp(name, "GX_", 3) != 0) return false;
	for(i = 3; i < length; i++)
	{
		if(strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", name[i]) == NULL) return false;
	}
	return true;
}

static void walk_promises(walk_result_t *result)
{
	static uint32_t looked_at = UINT32_MAX;
	const values_t *values = &cerbo->values;
	bool right = values->count >= 0 && values->count <= VALUES_MAX;
	int i, k;

	for(i = 0; right && i < values->count; i++)
	{
		if(!key_of_the_contract(values->items[i].name) || values->items[i].seen_ms > cerbo->clock_ms) right = false;
		// Every key against every other one only when a bundle was taken: nothing else fills the store
		for(k = 0; k < i && looked_at != cerbo->bundles; k++)
		{
			if(strcmp(values->items[i].name, values->items[k].name) == 0) right = false;
		}
	}
	// ... and then every key of the store that has a text against the names: it is one of them, and no name is
	// there twice
	for(i = 0; right && i < values->count && looked_at != cerbo->bundles; i++)
	{
		const value_t *item = &values->items[i];

		if(item->kind == VALUE_NUMBER && !(fabs(item->number) <= DBL_MAX)) continue;
		k = 0;
		while(k < cerbo->name_count && strcmp(cerbo->names[k], item->name) != 0) k++;
		if(k == cerbo->name_count) right = false;
	}
	if(cerbo->name_count < 0 || cerbo->name_count > CERBO_NAMES_MAX) right = false;
	for(i = 0; right && i < cerbo->name_count && looked_at != cerbo->bundles; i++)
	{
		if(!key_of_the_contract(cerbo->names[i])) right = false;
		for(k = 0; k < i; k++)
		{
			if(strcmp(cerbo->names[i], cerbo->names[k]) == 0) right = false;
		}
	}
	looked_at = cerbo->bundles;
	promise(right, PROMISE_STORE, result);
	promise(cerbo->pings_open <= 2, PROMISE_PINGS, result);
	if(cerbo->pings_open == 2) result->two_pings++;
}

// One call of cerbo_next(), carried out by the platform of the walk. false if the order is another one than
// the model's.
static bool walk_next(uint64_t at, walk_result_t *result)
{
	char sends;
	cerbo_do_t want = model_next(&model, at, &sends);
	cerbo_do_t got = cerbo_next(cerbo, at);
	client_packet_t read;
	size_t length = 0;
	const uint8_t *out;

	result->orders[got <= CERBO_DO_CLOSE ? got : 0]++;
	if(got != want) return false;

	if(got == CERBO_DO_OPEN)
	{
		promise(!world.socket, PROMISE_ORDERS, result);
		promise(!model.safe && model.host[0] != '\0' && strcmp(cerbo_host(cerbo), model.host) == 0, PROMISE_OFF, result);
		world_reset_connection();
		world.socket = true;
	}
	else if(got == CERBO_DO_CLOSE)
	{
		promise(world.socket, PROMISE_ORDERS, result);
		world_reset_connection();
	}
	else if(got == CERBO_DO_SEND)
	{
		promise(world.socket && world.open, PROMISE_ORDERS, result);
		out = cerbo_out(cerbo, &length);
		read = broker_reads(out, length);
		if(sends == 'C')
		{
			promise(length > 0 && out[0] == 0x10 && read.type == 1 && read.flags == 0x02 && read.keepalive_s == CERBO_KEEPALIVE_S && strcmp(read.text, model.client) == 0 &&
			        world.connects_sent == 0 && world.subscribes_sent == 0, PROMISE_SENT, result);
			world.connects_sent++;
		}
		else if(sends == 'S')
		{
			promise(length > 0 && out[0] == 0x82 && read.type == 8 && read.id == CERBO_PACKET_ID && read.qos == 0 && strcmp(read.text, CERBO_TOPIC) == 0 &&
			        world.connects_sent == 1 && world.subscribes_sent == 0 && world.connack_came, PROMISE_SENT, result);
			world.subscribes_sent++;
		}
		else
		{
			promise(length == 2 && out[0] == 0xC0 && read.type == 12 && world.suback_came, PROMISE_SENT, result);
		}

		// The platform sends, the broker hears - at once, or when the radio link is back
		if(!world.healing && roll(150) == 0)
		{
			cerbo_down(cerbo, at);
			model_down(&model, at);
			world_reset_connection();
			result->downs++;
		}
		else if(world.radio) broker_hears(sends);
		else if(world.unheard_count < (int)sizeof(world.unheard)) world.unheard[world.unheard_count++] = sends;
	}
	return true;
}

// The world goes on: the radio link, the broker and the flow
static void world_turns(uint64_t at, walk_result_t *result)
{
	int i;

	if(!world.radio && world.time >= world.radio_back) world.radio = true;
	if(world.radio && !world.healing && roll(400) == 0)
	{
		static const uint32_t pauses[] = {300, 2000, 8000, 21000, 35000, 70000};

		world.radio = false;
		world.radio_back = world.time + pauses[roll(COUNT(pauses))];
		result->pauses++;
	}

	// The flow sends about once a second, whether the radio link carries it or not: what it sent during a
	// pause lies on the wire, each bundle with the t of its own second
	if(!world.healing && roll(700) == 0)
	{
		static const uint32_t naps[] = {12000, 25000, 45000, 90000};

		world.asleep_until = world.time + naps[roll(COUNT(naps))];
		result->naps++;
	}
	while(world.heard_subscribe && world.publish_at + 1000 <= world.time && world.wired_count < WIRED - 8)
	{
		world.publish_at += (world.healing || roll(8) != 0) ? 1000 : 137;
		if(world.publish_at >= world.asleep_until) world_publish(world.publish_at);
	}
	if(!world.radio) return;

	// What the display sent during the pause arrives now, behind everything the broker piled up meanwhile
	for(i = 0; i < world.unheard_count; i++) broker_hears(world.unheard[i]);
	world.unheard_count = 0;

	if(world.socket && !world.open)
	{
		uint32_t fate = world.healing ? 0 : roll(40);

		if(fate < 12)
		{
			world.open = true;
			cerbo_opened(cerbo, at);
			model_opened(&model, at);
		}
		else if(fate == 12)
		{
			cerbo_down(cerbo, at);
			model_down(&model, at);
			world_reset_connection();
			result->downs++;
		}
	}
	else if(world.open && !world.healing)
	{
		uint32_t whim = roll(3000);

		if(whim < 4)
		{
			cerbo_down(cerbo, at);
			model_down(&model, at);
			world_reset_connection();
			result->downs++;
		}
		else if(whim < 6) wire_broken();
		else if(whim < 8) wire_pingresp();
		else if(whim < 9) wire_connack(0);
		else if(whim < 10) wire_suback(CERBO_PACKET_ID, 0);
		else if(whim < 12 && !world.heard_subscribe) world_publish(world.time);
	}
}

static void walk_report(uint32_t seed, int call, const char *what, uint64_t at)
{
	printf("  walk %lu, call %d at %" PRIu64 " ms: the source and the model differ in %s; view %d and %d, %d and %d values, lag %lu and %lu, bundles %lu and %lu\n",
	       (unsigned long)seed, call, at, what, (int)cerbo_view(cerbo, at), (int)model_view(&model, at), cerbo->values.count, model.count,
	       (unsigned long)cerbo->lag_ms, (unsigned long)model.lag, (unsigned long)cerbo->bundles, (unsigned long)model.bundles);
}

static bool walk(uint32_t seed, walk_result_t *result)
{
	static const uint64_t starts[] = {0, 1000, 4294960000ull, 1099511627776ull};
	static const char *const names[] = {NAME, NAME, "Bus 7", "----", NULL, "a-very-long-name-of-a-display-0123456789"};
	static const uint32_t steps[] = {1, 1, 5, 20, 20, 20, 50, 100, 100, 250, 1000, 5000, 20000};
	const char *host, *name, *what;
	bool safe;
	uint64_t stamp, heal_until;
	uint32_t high, low, pick;
	int call;

	walk_state = seed;
	memset(&world, 0, sizeof(world));
	world.time = starts[roll(COUNT(starts))];
	host = WALK_HOSTS[roll(COUNT(WALK_HOSTS))];
	name = names[roll(COUNT(names))];
	safe = roll(16) == 0;
	world.radio = true;
	world.rate = 1000;
	world.flow_at = world.time;
	high = roll(32768);
	low = roll(65536);
	world.boot = 1 + (high * 65536u + low) % 2147483647u;
	world.other_boot = world.boot == 5 ? 6 : 5;
	world.t = roll(3) == 0 ? 0 : 86400000 + (int64_t)roll(1000000);
	world.other_t = 77;

	memset(&box, GUARD, sizeof(box));
	memset(work, 0x5A, sizeof(work));
	cerbo_init(cerbo, host, safe, name, world.time);
	model_init(&model, host, safe, name, world.time);
	if(roll(4) != 0)
	{
		cerbo_network(cerbo, true, world.time);
		model_network(&model, true, world.time);
	}

	heal_until = 0;
	for(call = 0; call < WALK_CALLS || world.time < heal_until; call++)
	{
		if(call == WALK_CALLS - 1)
		{
			// From here on everything is well: the address is set, the network is there, the broker answers
			// as it should, and the flow sends a bundle a second
			world.healing = true;
			world.radio = true;
			world.two_flows = false;
			world.rate = 1000;
			world.boot = 4242;
			world.t = 0;
			world.asleep_until = 0;
			cerbo_set_host(cerbo, HOST, world.time);
			model_set_host(&model, HOST, world.time);
			cerbo_network(cerbo, true, world.time);
			model_network(&model, true, world.time);
			heal_until = world.time + WALK_HEAL_MS;
		}

		// The time goes on; sometimes a call brings a time that another task read a moment ago
		world.time += world.healing ? 100 : steps[roll(COUNT(steps))];
		stamp = world.time;
		if(!world.healing && roll(8) == 0)
		{
			uint32_t back = roll(400);

			stamp = world.time > back ? world.time - back : 0;
			result->stale++;
		}

		world_turns(stamp, result);
		pick = world.healing ? 0 : roll(100);
		if(pick < 55)
		{
			if(!walk_next(stamp, result))
			{
				walk_report(seed, call, "the order", stamp);
				return false;
			}
		}
		else if(pick < 96)
		{
			// Bytes arrive, if the radio link carries any
		}
		else if(pick < 98)
		{
			bool up = roll(5) != 0;

			cerbo_network(cerbo, up, stamp);
			model_network(&model, up, stamp);
			result->networks++;
		}
		else if(pick < 99)
		{
			if(roll(2) == 0)
			{
				int named = model.name_count;

				host = WALK_HOSTS[roll(COUNT(WALK_HOSTS))];
				cerbo_set_host(cerbo, host, stamp);
				model_set_host(&model, host, stamp);
				result->hosts++;
				if(model.name_count < named) result->forgotten++;
			}
		}
		else
		{
			// A report nobody expects
			if(roll(2) == 0)
			{
				cerbo_opened(cerbo, stamp);
				model_opened(&model, stamp);
				if(model.socket && model.stands) world.open = world.socket;
			}
			else
			{
				cerbo_down(cerbo, stamp);
				model_down(&model, stamp);
				world_reset_connection();
				result->downs++;
			}
		}
		if(world.open && world.radio && world.wire_length > world.delivered && (world.healing || pick >= 40)) world_deliver(stamp, result);

		result->calls++;
		result->views[model_view(&model, stamp)]++;
		walk_promises(result);
		what = walk_differs(stamp);
		if(what != NULL)
		{
			walk_report(seed, call, what, stamp);
			return false;
		}
	}

	// A healthy world has healed whatever the walk left behind
	if(safe)
	{
		promise(cerbo_view(cerbo, world.time) == CERBO_VIEW_OFF && cerbo->phase == CERBO_IDLE && cerbo->sessions == 0 && cerbo->bundles == 0, PROMISE_OFF, result);
		result->healed_off++;
	}
	else
	{
		const value_t *healed = values_find(&cerbo->values, "GX_HEAL");

		promise(cerbo_view(cerbo, world.time) == CERBO_VIEW_LIVE && healed != NULL && cerbo_age(healed, world.time) == VALUE_AGE_FRESH && cerbo_showing(cerbo, world.time),
		        PROMISE_HEALS, result);
		result->healed++;
	}
	result->given_way += (long)model.given_way;
	result->distrusted += (long)cerbo->distrusted;
	result->boots += (long)cerbo->boots;
	promise(guards_intact(), PROMISE_STORE, result);
	return true;
}

static void test_walk(void)
{
	walk_result_t result;
	int ends[2];
	int status = -1;
	bool complete = false, kept = true, every_view = true, every_fate = true;
	pid_t child;
	int i;

	memset(&result, 0, sizeof(result));
	fflush(stdout);
	if(pipe(ends) != 0)
	{
		check(false, "the walk has a pipe for its result");
		return;
	}

	// In a child process: a crash or a hang of the module is then a failed check here, not the end of the test
	alarm(600);
	child = fork();
	if(child == 0)
	{
		uint32_t seed;

		close(ends[0]);
		alarm(300);
		for(seed = 1; seed <= WALKS; seed++)
		{
			if(!walk(seed, &result)) result.different++;
		}
		fflush(stdout);
		_exit(write(ends[1], &result, sizeof(result)) == (ssize_t)sizeof(result) ? 0 : 1);
	}
	close(ends[1]);
	if(child > 0)
	{
		complete = read(ends[0], &result, sizeof(result)) == (ssize_t)sizeof(result);
		if(waitpid(child, &status, 0) != child) status = -1;
	}
	close(ends[0]);
	alarm(0);

	printf("  walk: %ld calls, %ld with an older time; orders %ld nothing, %ld open, %ld send, %ld close; %ld sessions, %ld connections lost, %ld pauses of the radio link, "
	       "%ld naps of the flow, %ld addresses set, %ld networks reported\n",
	       result.calls, result.stale, result.orders[CERBO_DO_NOTHING], result.orders[CERBO_DO_OPEN], result.orders[CERBO_DO_SEND], result.orders[CERBO_DO_CLOSE],
	       result.sessions, result.downs, result.pauses, result.naps, result.hosts, result.networks);
	printf("  walk: views %ld off, %ld no wifi, %ld connecting, %ld unreachable, %ld quiet, %ld silent, %ld newer, %ld live\n",
	       result.views[CERBO_VIEW_OFF], result.views[CERBO_VIEW_NO_WIFI], result.views[CERBO_VIEW_CONNECTING], result.views[CERBO_VIEW_UNREACHABLE],
	       result.views[CERBO_VIEW_QUIET], result.views[CERBO_VIEW_SILENT], result.views[CERBO_VIEW_NEWER], result.views[CERBO_VIEW_LIVE]);
	printf("  walk: messages %ld taken, %ld gone by, %ld on another topic, %ld retained, %ld too long, %ld invalid, %ld of a newer version; %ld packets ignored, "
	       "%ld out of place, %ld times broken bytes, %ld refusals\n",
	       result.fates[FATE_TAKEN], result.fates[FATE_GONE_BY], result.fates[FATE_TOPIC], result.fates[FATE_RETAINED], result.fates[FATE_LONG], result.fates[FATE_INVALID],
	       result.fates[FATE_NEWER], result.ignored, result.out_of_place, result.broken_bytes, result.refusals);
	printf("  walk: %ld bundles taken while 64 names were kept, %ld names gave way to a new one, %ld addresses set forgot names\n", result.named_full, result.given_way, result.forgotten);
	printf("  walk: bundles %ld with a lag, %ld of them old, %ld lags not believed, %ld changes of boot, %ld with 64 values, %ld of them with more members, %ld without a value, "
	       "%ld before the SUBACK; %ld deliveries of three bundles and more, %ld that ended inside a packet; %ld calls with two PINGREQ under way\n",
	       result.lagged, result.old, result.distrusted, result.boots, result.full, result.over, result.empty, result.before_suback, result.bursts, result.cuts, result.two_pings);
	printf("  walk: %ld bundles taken with a number without end among their values\n", result.endless);
	printf("  walk: %ld walks healed, %ld stayed off in safe mode; promises broken: orders %ld, sent packets %ld, PINGREQ %ld, store %ld, off %ld, healing %ld, the walk itself %ld\n",
	       result.healed, result.healed_off, result.broken[PROMISE_ORDERS], result.broken[PROMISE_SENT], result.broken[PROMISE_PINGS], result.broken[PROMISE_STORE],
	       result.broken[PROMISE_OFF], result.broken[PROMISE_HEALS], result.broken[PROMISE_HARNESS]);

	for(i = 0; i < PROMISES; i++)
	{
		if(result.broken[i] != 0) kept = false;
	}
	for(i = 0; i <= CERBO_VIEW_LIVE; i++)
	{
		if(result.views[i] < 1000) every_view = false;
	}
	for(i = 0; i < FATES; i++)
	{
		if(result.fates[i] < 300) every_fate = false;
	}
	check(complete && status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "120 random walks with a random broker and a random world: no crash and no hang");
	check(complete && result.different == 0 && result.calls >= (long)WALKS * WALK_CALLS, "120 random walks: the source and the model agree after every call");
	check(complete && result.broken[PROMISE_HARNESS] == 0, "the walk kept its own place: what its broker wrote is what its model was told of");
	check(complete && result.broken[PROMISE_ORDERS] == 0, "an order to open comes only without a connection, an order to send or close only with one");
	check(complete && result.broken[PROMISE_SENT] == 0,
	      "every packet that was sent begins with 0x10, 0x82 or 0xC0: one CONNECT, then at most one SUBSCRIBE after its CONNACK, PINGREQ only after the SUBACK");
	check(complete && result.broken[PROMISE_PINGS] == 0, "never more than two PINGREQ wait for an answer");
	check(complete && result.broken[PROMISE_STORE] == 0, "the store holds keys of the Cerbo only, none twice, no value seen in the future, and nothing is written outside the source; "
	                                                       "every key of it that has a text is among the names, and no name is there twice");
	check(complete && result.broken[PROMISE_OFF] == 0, "in safe mode and without an address nothing is opened");
	check(complete && result.broken[PROMISE_HEALS] == 0 && kept && result.healed + result.healed_off == WALKS && result.healed > 90 && result.healed_off > 2,
	      "after every walk a healthy broker and a healthy flow lead to a source that delivers fresh values within two and a half minutes");
	check(complete && result.orders[CERBO_DO_OPEN] > 5000 && result.orders[CERBO_DO_SEND] > 10000 && result.orders[CERBO_DO_CLOSE] > 2000 && result.orders[CERBO_DO_NOTHING] > 100000,
	      "the walk gets every order in numbers");
	check(complete && every_view, "the walk reaches every view in numbers");
	check(complete && every_fate, "the walk brings messages of every fate in numbers");
	check(complete && result.sessions > 2000 && result.downs > 500 && result.pauses > 500 && result.hosts > 500 && result.networks > 1000 && result.stale > 50000 &&
	      result.out_of_place > 300 && result.broken_bytes > 100 && result.refusals > 100 && result.ignored > 100,
	      "the walk reaches sessions, lost connections, pauses, new addresses, lost networks, older times, packets out of place, broken bytes, refusals and ignored packets in numbers");
	check(complete && result.named_full > 1000 && result.given_way > 1000 && result.forgotten > 100,
	      "the walk takes bundles while 64 names are kept, lets names give way to new ones and forgets names with an address in numbers");
	check(complete && result.lagged > 2000 && result.old > 300 && result.distrusted > 100 && result.boots > 1000 && result.full > 200 && result.over > 100 && result.empty > 200 &&
	      result.before_suback > 100 && result.bursts > 200 && result.cuts > 20000 && result.two_pings > 500,
	      "the walk reaches bundles that are late and old, lags not believed, new boots, full and empty stores, bundles before the SUBACK, bursts, cut packets and two PINGREQ under way in numbers");
	check(complete && result.endless > 1000, "the walk takes bundles with a number without end among their values, which the count of the values to show leaves out, in numbers");
}

int main(void)
{
	test_constants();
	test_off();
	test_connect();
	test_name();
	test_opened();
	test_packets();
	test_session_time();
	test_retry();
	test_network();
	test_ping();
	test_host();
	test_fixtures();
	test_before_suback();
	test_values();
	test_names();
	test_refused();
	test_lag();
	test_ages();
	test_numbers();
	test_views();
	test_clock();
	test_cut();
	test_megabyte();
	test_walk();
	return test_end();
}
