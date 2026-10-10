/*
 * Host test for display/components/core/mqtt_sub.c. Run "make test_mqtt_sub && ./test_mqtt_sub" in display/test.
 * redproof.py removes or weakens every rule once (mutations/mqtt_sub.py) and expects this test to fail.
 *
 * The packets are hand-written files of bytes (fixtures/mqtt_*.txt, the notation is in mqtt_harness.h), or
 * are built by the broker of the tests in that header: mqtt_sub.h cannot build what a broker sends. The
 * reader under test lies between guard bytes, and after every call of it the test looks at what it took.
 */
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include "test.h"
#include "mqtt_harness.h"
#include "mqtt_sub.h"

#define GUARD       0xA5
#define NAME        "WiCAN-Display-C9A8"
#define TOPIC       "display/gx/values"
#define COUNT(a)    (sizeof(a) / sizeof((a)[0]))
// The characters of a client id every broker has to take (MQTT 3.1.1, 3.1.3.1)
#define ID_CHARACTERS   "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"

// A byte written before or behind the reader lands in the guards and is seen there, where the address
// sanitizer would only stop the program without naming a rule
typedef struct
{
	unsigned char before[64];
	mqtt_sub_reader_t reader;
	unsigned char behind[64];
} box_t;

static box_t box;
static mqtt_sub_reader_t *const reader = &box.reader;
static mqtt_sub_packet_t packet;
static uint8_t marker;              // what `packet` points to before a call
static bool in_step = true;         // after every call *data and *length named the same rest

static bool guards_intact(void)
{
	size_t i;

	for(i = 0; i < sizeof(box.before); i++)
	{
		if(box.before[i] != GUARD || box.behind[i] != GUARD) return false;
	}
	return true;
}

// A packet no reader hands out: what a call leaves of it shows whether it wrote
static void mark_packet(void)
{
	packet.code = 77;
	packet.id = 7777;
	packet.retained = true;
	packet.topic = &marker;
	packet.topic_length = 55;
	packet.payload = &marker;
	packet.payload_length = 66;
}

static bool packet_marked(void)
{
	return packet.code == 77 && packet.id == 7777 && packet.retained && packet.topic == &marker && packet.topic_length == 55 &&
	       packet.payload == &marker && packet.payload_length == 66;
}

// The members no kind but PUBLISH names
static bool no_message(void)
{
	return !packet.retained && packet.topic == NULL && packet.topic_length == 0 && packet.payload == NULL && packet.payload_length == 0;
}

// A fresh reader between fresh guards
static void start(void)
{
	memset(&box, GUARD, sizeof(box));
	mqtt_sub_begin(reader);
	mark_packet();
}

// One call with the bytes at *data. Returns the kind; *left bytes were not taken.
static mqtt_sub_kind_t take(const uint8_t *bytes, size_t length, size_t *left)
{
	const uint8_t *data = bytes;
	size_t rest = length;
	mqtt_sub_kind_t kind = mqtt_sub_read(reader, &data, &rest, &packet);

	// Without bytes there may be no pointer to add to
	if(rest > length || (rest == length ? data != bytes : data != bytes + (length - rest))) in_step = false;
	if(left != NULL) *left = rest;
	return kind;
}

// One call with all the bytes of a text of hex digits, for the short packets of the examples below
static mqtt_sub_kind_t take_hex(const char *digits, size_t *left)
{
	static uint8_t bytes[64];
	size_t count = 0;

	for(; digits[0] != '\0' && digits[1] != '\0' && count < sizeof(bytes); digits += 2)
	{
		bytes[count++] = (uint8_t)(hex_digit(digits[0]) * 16 + hex_digit(digits[1]));
	}
	return take(bytes, count, left);
}

static bool topic_is(const char *text)
{
	return packet.topic != NULL && packet.topic_length == strlen(text) && memcmp(packet.topic, text, packet.topic_length) == 0;
}

static bool payload_is(const void *bytes, size_t length)
{
	return packet.payload != NULL && packet.payload_length == length && memcmp(packet.payload, bytes, length) == 0;
}

/* ---------------------------------------------------------------------------------------------------
 * A stream in pieces, against the rules written once more (told_next() in mqtt_harness.h)
 */

typedef struct
{
	long kinds[MQTT_SUB_BROKEN + 1];    // what the reader returned, by kind
	long cuts[6];                       // pieces that ended 1, 2, 3, 4 and more bytes into a packet that is kept,
	                                    // and, the last, inside one that goes by
} tally_t;

static size_t cut_position;
static uint32_t random_state;

// Never two rolls in one expression whose order C leaves open: gcc and clang work them out in different orders
static uint32_t roll(uint32_t below)
{
	random_state = random_state * 1664525u + 1013904223u;
	return (random_state >> 8) % below;
}

static size_t piece_whole(size_t at, size_t left)
{
	(void)at;
	return left;
}

static size_t piece_byte(size_t at, size_t left)
{
	(void)at;
	(void)left;
	return 1;
}

// Two pieces: the cut is at cut_position
static size_t piece_cut(size_t at, size_t left)
{
	return at < cut_position && cut_position - at < left ? cut_position - at : left;
}

// Mostly a few bytes, sometimes as many as a segment brings, sometimes all there is
static size_t piece_random(size_t at, size_t left)
{
	uint32_t kind = roll(10);
	size_t size;

	(void)at;
	if(kind < 6) size = 1 + roll(8);
	else if(kind < 9) size = 1 + roll(1500);
	else size = left;
	return size < left ? size : left;
}

static bool same_packet(const uint8_t *stream, const told_t *told)
{
	if(packet.code != told->code || packet.id != told->id || packet.retained != told->retained) return false;
	if(told->kind != MQTT_SUB_PUBLISH) return no_message();

	// Topic and payload lie in the room of the reader, and hold the bytes of the stream
	if(packet.topic < reader->room || packet.payload < packet.topic + packet.topic_length ||
	   packet.payload + packet.payload_length > reader->room + MQTT_SUB_PACKET_MAX) return false;
	if(packet.topic_length != told->topic_length || packet.payload_length != told->payload_length) return false;
	return memcmp(packet.topic, stream + told->topic_at, told->topic_length) == 0 &&
	       memcmp(packet.payload, stream + told->payload_at, told->payload_length) == 0;
}

// Plays the stream through a fresh reader, in pieces of the sizes `piece` names. With every call the reader
// has to return what told_next() tells of the bytes it has taken so far - a packet with its last byte, never
// earlier and never later -, has to take exactly the bytes of that packet, and has to leave the rest of the
// piece. false at the first difference.
static bool plays(const uint8_t *stream, size_t length, size_t (*piece)(size_t, size_t), tally_t *tally)
{
	size_t fed = 0, told_at = 0;
	bool dead = false;
	told_t told;

	start();
	while(fed < length)
	{
		size_t size = piece(fed, length - fed);
		const uint8_t *data = stream + fed;
		size_t left = size;

		if(size == 0 || size > length - fed) return false;
		for(;;)
		{
			size_t before = left;
			mqtt_sub_kind_t kind;
			size_t taken;

			mark_packet();
			kind = mqtt_sub_read(reader, &data, &left, &packet);
			taken = (size_t)(data - stream);
			if(taken + left != fed + size) return false;
			if(dead)
			{
				// Nothing more is read
				if(kind != MQTT_SUB_BROKEN || left != before || !packet_marked()) return false;
				break;
			}
			if(told_next(stream, taken, &told_at, &told) != kind) return false;
			if(tally != NULL) tally->kinds[kind]++;
			if(kind == MQTT_SUB_NONE)
			{
				if(left != 0 || !packet_marked()) return false;
				if(tally != NULL && taken > told_at)
				{
					size_t into = taken - told_at;
					size_t probe = told_at;
					told_t whole;

					if(told_next(stream, length, &probe, &whole) == MQTT_SUB_SKIPPED && into > 4) tally->cuts[5]++;
					else tally->cuts[into < 5 ? into - 1 : 4]++;
				}
				break;
			}
			if(told_at != taken) return false;
			if(kind == MQTT_SUB_BROKEN)
			{
				if(!packet_marked()) return false;
				dead = true;
				if(left == 0) break;
				continue;
			}
			if(!same_packet(stream, &told)) return false;
		}
		fed += size;
	}
	return guards_intact();
}

// The stream in one piece, byte by byte, and with one cut at every position
static bool plays_every_way(const uint8_t *stream, size_t length)
{
	if(!plays(stream, length, piece_whole, NULL) || !plays(stream, length, piece_byte, NULL)) return false;
	for(cut_position = 1; cut_position < length; cut_position++)
	{
		if(!plays(stream, length, piece_cut, NULL)) return false;
	}
	return true;
}

/* ---------------------------------------------------------------------------------------------------
 * The examples
 */

static void test_constants(void)
{
	check(MQTT_SUB_PACKET_MAX == 1536 && MQTT_SUB_OUT_MAX == 64 && MQTT_SUB_ID_MAX == 23,
	      "the reader keeps 1536 bytes of a packet, a built packet has at most 64, a client id at most 23");
}

static void test_notation(void)
{
	static const uint8_t expected[] = {0x00, 0x7F, 0x80, 0xFF, 'a', ' ', 'b', 0x0A, '#', 0xC0};
	uint8_t bytes[32];
	size_t length = 0;

	check(read_hex("fixtures/mqtt_notation.txt", bytes, sizeof(bytes), &length) && length == sizeof(expected) &&
	      memcmp(bytes, expected, sizeof(expected)) == 0, "the notation of the fixtures: bytes, texts and comments");
	check(!read_hex("fixtures/mqtt_notation.txt", bytes, sizeof(expected) - 1, &length) && length == 0,
	      "the notation of the fixtures: a file with more bytes than the room is refused");
}

// The client id a broker reads in the CONNECT built for this name, NULL if no packet is built
static const char *id_of(const char *name)
{
	static client_packet_t read;
	uint8_t out[MQTT_SUB_OUT_MAX];
	size_t length = mqtt_sub_connect(out, sizeof(out), name, 30);

	if(length == 0) return NULL;
	read = broker_reads(out, length);
	return read.type == 1 ? read.text : NULL;
}

static bool id_is(const char *name, const char *expected)
{
	const char *id = id_of(name);

	return id != NULL && strcmp(id, expected) == 0;
}

static bool all_guard(const uint8_t *bytes, size_t length)
{
	size_t i;

	for(i = 0; i < length; i++)
	{
		if(bytes[i] != GUARD) return false;
	}
	return true;
}

static void test_connect(void)
{
	uint8_t expected[MQTT_SUB_OUT_MAX];
	uint8_t out[MQTT_SUB_OUT_MAX + 16];
	size_t expected_length = 0;
	size_t length;
	int single, kept = 0;
	bool right = true;

	check(read_hex("fixtures/mqtt_connect.txt", expected, sizeof(expected), &expected_length) && expected_length == 30, "fixture of a CONNECT");
	memset(out, GUARD, sizeof(out));
	length = mqtt_sub_connect(out, MQTT_SUB_OUT_MAX, NAME, 30);
	check(length == expected_length && memcmp(out, expected, expected_length) == 0 && all_guard(out + length, sizeof(out) - length),
	      "CONNECT for WiCAN-Display-C9A8 and 30 s: the 30 bytes of the fixture, and nothing behind them");
	check(length == 30 && out[0] == 0x10 && out[1] == 28, "CONNECT begins with 0x10 and its length without the first two bytes");
	check(length == 30 && memcmp(out + 2, "\0\4MQTT", 6) == 0 && out[8] == 4, "CONNECT names MQTT 3.1.1: the protocol MQTT, level 4");
	check(length == 30 && out[9] == 0x02, "CONNECT asks for a clean session and has no will, no user name and no password: the connect flags are 0x02");
	check(length == 30 && out[12] == 0 && out[13] == 16 && memcmp(out + 14, "WiCANDisplayC9A8", 16) == 0,
	      "the payload of the CONNECT is the client id with its length, and nothing else");

	check(mqtt_sub_connect(out, MQTT_SUB_OUT_MAX, NAME, 0x1234) == 30 && out[10] == 0x12 && out[11] == 0x34,
	      "the keep alive time is written with its high byte first");
	check(mqtt_sub_connect(out, MQTT_SUB_OUT_MAX, NAME, 0) == 30 && out[10] == 0 && out[11] == 0 &&
	      mqtt_sub_connect(out, MQTT_SUB_OUT_MAX, NAME, 65535) == 30 && out[10] == 0xFF && out[11] == 0xFF &&
	      mqtt_sub_connect(out, MQTT_SUB_OUT_MAX, NAME, 300) == 30 && out[10] == 1 && out[11] == 44,
	      "the keep alive time is written as it is: 0, 65535 and 300 s");

	check(id_is(NAME, "WiCANDisplayC9A8"), "the client id are the letters and digits of the name");
	check(id_is("a-b_c.d e/f:g", "abcdefg") && id_is("--x--", "x") && id_is("Z9z0A", "Z9z0A"), "everything but letters and digits is left out, wherever it stands");
	check(id_is("K\xC3\xA4" "fer 7 \xE2\x82\xAC", "Kfer7") && id_is("\xFF" "a\x80" "b\x7F", "ab"), "bytes above 127 are left out, for both kinds of char");
	check(id_is("@[`{/:AZaz09", "AZaz09"), "the characters next to the letters and digits are left out");
	check(id_is("12345678901234567890123", "12345678901234567890123"), "a name of 23 letters and digits is the client id");
	check(id_is("123456789012345678901234", "12345678901234567890123") && id_is("abcdefghijklmnopqrstuvwxyz0123456789", "abcdefghijklmnopqrstuvw"),
	      "of more than 23 letters and digits the first 23 are the client id");
	check(id_is("1-2-3-4-5-6-7-8-9-0-1-2-3-4-5-6-7-8-9-0-1-2-3-4-5", "12345678901234567890123"),
	      "the 23 count letters and digits, not the bytes of the name");
	check(mqtt_sub_connect(out, MQTT_SUB_OUT_MAX, "12345678901234567890123", 30) == 37 && out[1] == 35 && out[13] == 23,
	      "the longest CONNECT has 37 bytes");

	// Every byte as a name of its own
	for(single = 1; single < 256; single++)
	{
		char name[2] = {(char)single, '\0'};
		const char *id = id_of(name);
		bool wanted = single < 128 && strchr(ID_CHARACTERS, single) != NULL;

		if(id != NULL) kept++;
		if((id != NULL) != wanted || (id != NULL && strcmp(id, name) != 0)) right = false;
	}
	check(right && kept == 62, "of the bytes from 1 to 255 exactly the 62 letters and digits of ASCII make a client id");

	memset(out, GUARD, sizeof(out));
	check(mqtt_sub_connect(out, MQTT_SUB_OUT_MAX, "", 30) == 0 && mqtt_sub_connect(out, MQTT_SUB_OUT_MAX, "-_ ./\xC3\xA4", 30) == 0 &&
	      all_guard(out, sizeof(out)), "a name without a letter or digit gives no packet, and nothing is written");
	check(mqtt_sub_connect(out, MQTT_SUB_OUT_MAX, NULL, 30) == 0 && all_guard(out, sizeof(out)), "no name gives no packet");
	check(mqtt_sub_connect(NULL, MQTT_SUB_OUT_MAX, NAME, 30) == 0, "CONNECT without a room gives no packet");
	check(mqtt_sub_connect(out, 29, NAME, 30) == 0 && mqtt_sub_connect(out, 0, NAME, 30) == 0 && mqtt_sub_connect(out, 2, NAME, 30) == 0 &&
	      all_guard(out, sizeof(out)), "a CONNECT that does not fit is not built, and nothing is written");
	check(mqtt_sub_connect(out, 30, NAME, 30) == 30 && memcmp(out, expected, 30) == 0 && all_guard(out + 30, sizeof(out) - 30),
	      "a CONNECT that fits exactly is built");
}

static void test_subscribe(void)
{
	static const char longest[] = "123456789/123456789/123456789/123456789/123456789/1234567";
	static const char too_long[] = "123456789/123456789/123456789/123456789/123456789/12345678";
	uint8_t expected[MQTT_SUB_OUT_MAX];
	uint8_t out[MQTT_SUB_OUT_MAX + 16];
	size_t expected_length = 0;
	size_t length;
	client_packet_t read;

	check(read_hex("fixtures/mqtt_subscribe.txt", expected, sizeof(expected), &expected_length) && expected_length == 24, "fixture of a SUBSCRIBE");
	memset(out, GUARD, sizeof(out));
	length = mqtt_sub_subscribe(out, MQTT_SUB_OUT_MAX, 1, TOPIC);
	check(length == expected_length && memcmp(out, expected, expected_length) == 0 && all_guard(out + length, sizeof(out) - length),
	      "SUBSCRIBE to display/gx/values with the packet identifier 1: the 24 bytes of the fixture, and nothing behind them");
	check(length == 24 && out[0] == 0x82 && out[1] == 22, "SUBSCRIBE begins with 0x82 and its length without the first two bytes");
	read = broker_reads(out, length);
	check(read.type == 8 && read.text_length == 17 && memcmp(read.text, TOPIC, 17) == 0 && read.qos == 0 && out[4] == 0 && out[5] == 17 && out[23] == 0,
	      "SUBSCRIBE names one topic with QoS 0");

	check(mqtt_sub_subscribe(out, MQTT_SUB_OUT_MAX, 0x1234, TOPIC) == 24 && out[2] == 0x12 && out[3] == 0x34,
	      "the packet identifier is written with its high byte first");
	check(mqtt_sub_subscribe(out, MQTT_SUB_OUT_MAX, 65535, TOPIC) == 24 && out[2] == 0xFF && out[3] == 0xFF &&
	      mqtt_sub_subscribe(out, MQTT_SUB_OUT_MAX, 256, TOPIC) == 24 && out[2] == 1 && out[3] == 0, "the packet identifiers 65535 and 256");
	memset(out, GUARD, sizeof(out));
	check(mqtt_sub_subscribe(out, MQTT_SUB_OUT_MAX, 0, TOPIC) == 0 && all_guard(out, sizeof(out)), "the packet identifier 0 gives no packet");
	check(mqtt_sub_subscribe(out, MQTT_SUB_OUT_MAX, 1, "") == 0 && mqtt_sub_subscribe(out, MQTT_SUB_OUT_MAX, 1, NULL) == 0 && all_guard(out, sizeof(out)),
	      "no topic and an empty topic give no packet");
	check(mqtt_sub_subscribe(NULL, MQTT_SUB_OUT_MAX, 1, TOPIC) == 0, "SUBSCRIBE without a room gives no packet");
	check(mqtt_sub_subscribe(out, MQTT_SUB_OUT_MAX, 1, "display/#") == 0 && mqtt_sub_subscribe(out, MQTT_SUB_OUT_MAX, 1, "#") == 0 &&
	      mqtt_sub_subscribe(out, MQTT_SUB_OUT_MAX, 1, "#/gx") == 0 && all_guard(out, sizeof(out)), "a topic with # gives no packet: no pattern is subscribed");
	check(mqtt_sub_subscribe(out, MQTT_SUB_OUT_MAX, 1, "display/+/values") == 0 && mqtt_sub_subscribe(out, MQTT_SUB_OUT_MAX, 1, "+") == 0 &&
	      mqtt_sub_subscribe(out, MQTT_SUB_OUT_MAX, 1, "display/gx/+") == 0 && all_guard(out, sizeof(out)), "a topic with + gives no packet: no pattern is subscribed");
	check(mqtt_sub_subscribe(out, MQTT_SUB_OUT_MAX, 1, "a") == 8 && out[1] == 6 && out[5] == 1 && out[6] == 'a' && out[7] == 0, "a topic of one byte");
	check(mqtt_sub_subscribe(out, MQTT_SUB_OUT_MAX, 1, "\xC3\x84rger/\xFF\x80") == 16 && memcmp(out + 6, "\xC3\x84rger/\xFF\x80", 9) == 0 && out[15] == 0,
	      "the bytes of a topic are copied as they are, for both kinds of char");

	memset(out, GUARD, sizeof(out));
	check(sizeof(longest) - 1 == 57 && mqtt_sub_subscribe(out, MQTT_SUB_OUT_MAX, 1, longest) == 64 && out[1] == 62 && out[5] == 57 && out[63] == 0 &&
	      all_guard(out + 64, sizeof(out) - 64), "a topic of 57 bytes makes the longest SUBSCRIBE: 64 bytes");
	memset(out, GUARD, sizeof(out));
	check(sizeof(too_long) - 1 == 58 && mqtt_sub_subscribe(out, MQTT_SUB_OUT_MAX, 1, too_long) == 0 && mqtt_sub_subscribe(out, sizeof(out), 1, too_long) == 0 &&
	      all_guard(out, sizeof(out)), "a topic of 58 bytes gives no packet, however large the room is");
	check(mqtt_sub_subscribe(out, 23, 1, TOPIC) == 0 && mqtt_sub_subscribe(out, 0, 1, TOPIC) == 0 && mqtt_sub_subscribe(out, 6, 1, TOPIC) == 0 &&
	      all_guard(out, sizeof(out)), "a SUBSCRIBE that does not fit is not built, and nothing is written");
	check(mqtt_sub_subscribe(out, 24, 1, TOPIC) == 24 && memcmp(out, expected, 24) == 0 && all_guard(out + 24, sizeof(out) - 24),
	      "a SUBSCRIBE that fits exactly is built");
}

static void test_ping(void)
{
	uint8_t expected[8];
	uint8_t out[16];
	size_t expected_length = 0;

	check(read_hex("fixtures/mqtt_pingreq.txt", expected, sizeof(expected), &expected_length) && expected_length == 2, "fixture of a PINGREQ");
	memset(out, GUARD, sizeof(out));
	check(mqtt_sub_ping(out, MQTT_SUB_OUT_MAX) == 2 && out[0] == 0xC0 && out[1] == 0 && memcmp(out, expected, 2) == 0 && all_guard(out + 2, sizeof(out) - 2),
	      "PINGREQ: the two bytes 0xC0 0x00 of the fixture, and nothing behind them");
	memset(out, GUARD, sizeof(out));
	check(mqtt_sub_ping(out, 1) == 0 && mqtt_sub_ping(out, 0) == 0 && all_guard(out, sizeof(out)), "a PINGREQ that does not fit is not built, and nothing is written");
	check(mqtt_sub_ping(out, 2) == 2 && all_guard(out + 2, sizeof(out) - 2), "a PINGREQ that fits exactly is built");
	check(mqtt_sub_ping(NULL, MQTT_SUB_OUT_MAX) == 0, "PINGREQ without a room gives no packet");
}

/*
 * The display must not be able to send anything but CONNECT, SUBSCRIBE and PINGREQ. The three builders are
 * run over every keep alive time and every packet identifier, over every byte as a name and as a topic, over
 * random names and topics and over every size of the room; every packet that comes out is read as a broker
 * reads it.
 */
typedef struct
{
	long connects, subscribes, pings, none, wrong;
} built_t;

// Larger than any size the builders are told of: a builder that does not keep to its room is seen, not stopped
static uint8_t built_out[256];

static void built(size_t length, size_t size, built_t *result)
{
	client_packet_t read;
	size_t i;

	if(length == 0)
	{
		result->none++;
		return;
	}
	if(length > size || length > MQTT_SUB_OUT_MAX)
	{
		result->wrong++;
		return;
	}
	read = broker_reads(built_out, length);
	if(built_out[0] == 0x10)
	{
		bool right = read.type == 1 && read.flags == 0x02 && read.text_length >= 1 && read.text_length <= MQTT_SUB_ID_MAX;

		for(i = 0; i < read.text_length; i++)
		{
			if(read.text[i] <= 0 || strchr(ID_CHARACTERS, read.text[i]) == NULL) right = false;
		}
		if(right) result->connects++;
		else result->wrong++;
	}
	else if(built_out[0] == 0x82)
	{
		bool right = read.type == 8 && read.qos == 0 && read.id != 0 && read.text_length >= 1;

		for(i = 0; i < read.text_length; i++)
		{
			if(read.text[i] == '#' || read.text[i] == '+' || read.text[i] == '\0') right = false;
		}
		if(right) result->subscribes++;
		else result->wrong++;
	}
	else if(built_out[0] == 0xC0 && read.type == 12) result->pings++;
	else result->wrong++;
}

// A text of up to 69 random bytes, none of them zero
static void random_text(char *text)
{
	size_t length = roll(70);
	size_t i;

	for(i = 0; i < length; i++)
	{
		uint32_t kind = roll(4);

		// Mostly what a name or a topic is made of, so that many of them are taken
		if(kind == 0) text[i] = (char)(1 + roll(255));
		else text[i] = "abcXYZ019/-_ "[roll(13)];
	}
	text[length] = '\0';
}

static void test_built(void)
{
	static const uint16_t numbers[] = {0, 1, 30, 255, 256, 0x1234, 0x8000, 65535};
	static char header[32768];
	built_t result;
	char text[80];
	const char *found;
	uint32_t number;
	size_t size, i;
	int single, builders = 0;

	memset(&result, 0, sizeof(result));
	for(number = 0; number < 65536; number++)
	{
		built(mqtt_sub_connect(built_out, MQTT_SUB_OUT_MAX, NAME, (uint16_t)number), MQTT_SUB_OUT_MAX, &result);
		built(mqtt_sub_subscribe(built_out, MQTT_SUB_OUT_MAX, (uint16_t)number, TOPIC), MQTT_SUB_OUT_MAX, &result);
	}
	for(single = 1; single < 256; single++)
	{
		text[0] = (char)single;
		text[1] = '\0';
		for(i = 0; i < COUNT(numbers); i++)
		{
			built(mqtt_sub_connect(built_out, MQTT_SUB_OUT_MAX, text, numbers[i]), MQTT_SUB_OUT_MAX, &result);
			built(mqtt_sub_subscribe(built_out, MQTT_SUB_OUT_MAX, numbers[i], text), MQTT_SUB_OUT_MAX, &result);
		}
	}
	random_state = 20261010;
	for(number = 0; number < 20000; number++)
	{
		uint16_t chosen;

		random_text(text);
		chosen = numbers[roll(COUNT(numbers))];
		size = roll(MQTT_SUB_OUT_MAX + 9);
		built(mqtt_sub_connect(built_out, MQTT_SUB_OUT_MAX, text, chosen), MQTT_SUB_OUT_MAX, &result);
		built(mqtt_sub_subscribe(built_out, MQTT_SUB_OUT_MAX, chosen, text), MQTT_SUB_OUT_MAX, &result);
		built(mqtt_sub_connect(built_out, size, text, chosen), size, &result);
		built(mqtt_sub_subscribe(built_out, size, chosen, text), size, &result);
	}
	for(size = 0; size <= MQTT_SUB_OUT_MAX + 8; size++)
	{
		built(mqtt_sub_connect(built_out, size, NAME, 30), size, &result);
		built(mqtt_sub_subscribe(built_out, size, 1, TOPIC), size, &result);
		built(mqtt_sub_ping(built_out, size), size, &result);
	}
	printf("  built: %ld CONNECT, %ld SUBSCRIBE, %ld PINGREQ, %ld times no packet, %ld times something else\n",
	       result.connects, result.subscribes, result.pings, result.none, result.wrong);
	check(result.wrong == 0, "every packet that is built begins with 0x10, 0x82 or 0xC0 and is a CONNECT without will, name and password, "
	      "a SUBSCRIBE to one topic with QoS 0, or a PINGREQ: 65536 keep alive times and packet identifiers, every byte as name and topic, "
	      "20000 random names and topics, every size of the room");
	check(result.connects > 80000 && result.subscribes > 80000 && result.pings > 60 && result.none > 30000,
	      "the builders were run in numbers: packets of each kind, and calls without a packet");

	// The three builders above are all there are: a fourth one would have to be added to this test
	check(read_fixture("../components/core/mqtt_sub.h", header, sizeof(header)), "mqtt_sub.h is read");
	for(found = header; (found = strstr(found, "(uint8_t *out, size_t size")) != NULL; found++) builders++;
	check(builders == 3 && strstr(header, "size_t mqtt_sub_connect(uint8_t *out, size_t size") != NULL &&
	      strstr(header, "size_t mqtt_sub_subscribe(uint8_t *out, size_t size") != NULL && strstr(header, "size_t mqtt_sub_ping(uint8_t *out, size_t size") != NULL,
	      "mqtt_sub.h declares three functions that write a packet: connect, subscribe and ping");
}

static void test_packets(void)
{
	uint8_t bytes[128];
	uint8_t stream[512];
	uint8_t long_topic[300];
	size_t length = 0, left = 99;

	start();
	check(read_hex("fixtures/mqtt_connack.txt", bytes, sizeof(bytes), &length) && length == 4, "fixture of a CONNACK");
	check(take(bytes, length, &left) == MQTT_SUB_CONNACK && left == 0 && packet.code == 0 && packet.id == 0 && no_message(), "CONNACK: accepted");
	check(read_hex("fixtures/mqtt_connack_refused.txt", bytes, sizeof(bytes), &length) && length == 4, "fixture of a CONNACK that refuses");
	check(take(bytes, length, &left) == MQTT_SUB_CONNACK && left == 0 && packet.code == 5 && packet.id == 0 && no_message(), "CONNACK: the return code comes out");
	check(take_hex("200200FF", &left) == MQTT_SUB_CONNACK && left == 0 && packet.code == 255, "CONNACK: the return code 255 comes out as it is");

	check(read_hex("fixtures/mqtt_suback.txt", bytes, sizeof(bytes), &length) && length == 5, "fixture of a SUBACK");
	check(take(bytes, length, &left) == MQTT_SUB_SUBACK && left == 0 && packet.id == 1 && packet.code == 0 && no_message(), "SUBACK: subscribed, with the packet identifier");
	check(read_hex("fixtures/mqtt_suback_refused.txt", bytes, sizeof(bytes), &length) && length == 5, "fixture of a SUBACK that refuses");
	check(take(bytes, length, &left) == MQTT_SUB_SUBACK && left == 0 && packet.id == 1 && packet.code == 0x80 && no_message(), "SUBACK: the return code comes out");
	check(take_hex("9003123402", &left) == MQTT_SUB_SUBACK && packet.id == 0x1234 && packet.code == 2, "SUBACK: the packet identifier has its high byte first");
	check(take_hex("900300FF01", &left) == MQTT_SUB_SUBACK && packet.id == 255 && packet.code == 1 && take_hex("9003FF0000", &left) == MQTT_SUB_SUBACK && packet.id == 0xFF00,
	      "SUBACK: both bytes of the packet identifier count");

	check(read_hex("fixtures/mqtt_pingresp.txt", bytes, sizeof(bytes), &length) && length == 2, "fixture of a PINGRESP");
	mark_packet();
	check(take(bytes, length, &left) == MQTT_SUB_PINGRESP && left == 0 && packet.code == 0 && packet.id == 0 && no_message(), "PINGRESP");

	check(read_hex("fixtures/mqtt_publish.txt", bytes, sizeof(bytes), &length) && length == 66, "fixture of a PUBLISH");
	check(take(bytes, length, &left) == MQTT_SUB_PUBLISH && left == 0 && topic_is(TOPIC) && payload_is("{\"v\":1,\"boot\":7,\"t\":1000,\"values\":{\"GX_A\":1}}", 45),
	      "PUBLISH: topic and payload come out");
	check(!packet.retained && packet.code == 0 && packet.id == 0, "PUBLISH: not retained, and nothing else is named");
	check(packet.topic == reader->room + 2 && packet.payload == reader->room + 19, "PUBLISH: topic and payload point into the room of the reader");
	check(take_hex("D000", &left) == MQTT_SUB_PINGRESP && no_message() && take_hex("20020003", &left) == MQTT_SUB_CONNACK && no_message() && packet.code == 3,
	      "what a kind does not name is empty, whatever the packet before was");

	check(read_hex("fixtures/mqtt_publish_retained.txt", bytes, sizeof(bytes), &length) && length == 28, "fixture of a retained PUBLISH");
	check(take(bytes, length, &left) == MQTT_SUB_PUBLISH && left == 0 && packet.retained && topic_is(TOPIC) && payload_is("{\"v\":1}", 7),
	      "the retained flag comes out");
	check(take_hex("30050001616263", &left) == MQTT_SUB_PUBLISH && !packet.retained && topic_is("a") && payload_is("bc", 2) &&
	      take_hex("31050001616263", &left) == MQTT_SUB_PUBLISH && packet.retained, "the retained flag is bit 0 of the first byte");
	check(take_hex("300300017A", &left) == MQTT_SUB_PUBLISH && topic_is("z") && packet.payload != NULL && packet.payload_length == 0,
	      "a PUBLISH without payload is a PUBLISH");
	check(take_hex("3006000300FF80AA", &left) == MQTT_SUB_PUBLISH && packet.topic_length == 3 && memcmp(packet.topic, "\0\xFF\x80", 3) == 0 && payload_is("\xAA", 1),
	      "the bytes of a topic come out as they are, a zero among them");

	// A topic longer than one byte can count
	memset(long_topic, 'k', sizeof(long_topic));
	length = broker_publish_head(stream, 0x30, long_topic, sizeof(long_topic), 4, 1);
	memcpy(stream + length, "data", 4);
	check(length == 305 && stream[3] == 1 && stream[4] == 44 && take(stream, length + 4, &left) == MQTT_SUB_PUBLISH && left == 0 && packet.topic_length == 300 &&
	      memcmp(packet.topic, long_topic, 300) == 0 && payload_is("data", 4), "a topic of 300 bytes comes out: both bytes of its length count");
	check(guards_intact(), "the reader wrote nothing outside of itself");
}

static void test_pieces(void)
{
	static const char *const files[] = {
		"fixtures/mqtt_connack.txt", "fixtures/mqtt_connack_refused.txt", "fixtures/mqtt_suback.txt", "fixtures/mqtt_suback_refused.txt",
		"fixtures/mqtt_pingresp.txt", "fixtures/mqtt_publish.txt", "fixtures/mqtt_publish_retained.txt", "fixtures/mqtt_stream.txt",
	};
	uint8_t bytes[256];
	size_t length = 0, left = 99;
	bool every = true;
	tally_t tally;
	size_t i;

	for(i = 0; i < COUNT(files); i++)
	{
		if(!read_hex(files[i], bytes, sizeof(bytes), &length) || length < 2 || !plays_every_way(bytes, length)) every = false;
	}
	check(every, "a packet cut at every byte reads as in one piece: every fixture in one piece, byte by byte and with a cut at every position");

	check(read_hex("fixtures/mqtt_stream.txt", bytes, sizeof(bytes), &length) && length == 47, "fixture of a stream of five packets");
	memset(&tally, 0, sizeof(tally));
	check(plays(bytes, length, piece_whole, &tally) && tally.kinds[MQTT_SUB_CONNACK] == 1 && tally.kinds[MQTT_SUB_SUBACK] == 1 &&
	      tally.kinds[MQTT_SUB_PUBLISH] == 2 && tally.kinds[MQTT_SUB_PINGRESP] == 1 && tally.kinds[MQTT_SUB_NONE] == 1,
	      "five packets in one piece come out one after the other, and then the bytes have run out");

	start();
	check(take(bytes, length, &left) == MQTT_SUB_CONNACK && left == 43, "two packets in one piece: the first comes out, and the bytes of the others are left");
	check(take(bytes + 4, 43, &left) == MQTT_SUB_SUBACK && left == 38 && packet.id == 1, "two packets in one piece: the second comes out with the next call");
	check(take(bytes + 9, 38, &left) == MQTT_SUB_PUBLISH && left == 9 && topic_is(TOPIC) && payload_is("first\x80\0\xFF", 8), "the third packet: its payload with every byte");
	check(take(bytes + 38, 9, &left) == MQTT_SUB_PINGRESP && left == 7, "the fourth packet");
	check(take(bytes + 40, 7, &left) == MQTT_SUB_PUBLISH && left == 0 && packet.retained && topic_is("a/b") && packet.payload_length == 0, "the fifth packet");
	mark_packet();
	check(take(bytes + 47, 0, &left) == MQTT_SUB_NONE && left == 0 && packet_marked(), "when the bytes ran out there is no packet, and none is written");
	check(take(NULL, 0, &left) == MQTT_SUB_NONE && left == 0, "a call without bytes takes nothing");

	// A packet in three pieces, with calls without bytes in between
	start();
	check(take(bytes + 9, 1, &left) == MQTT_SUB_NONE && left == 0 && take(bytes, 0, &left) == MQTT_SUB_NONE && take(bytes + 10, 20, &left) == MQTT_SUB_NONE && left == 0 &&
	      packet_marked() && take(bytes, 0, &left) == MQTT_SUB_NONE && take(bytes + 30, 8, &left) == MQTT_SUB_PUBLISH && left == 0 && payload_is("first\x80\0\xFF", 8),
	      "a packet in three pieces with pauses between them: nothing comes out before its last byte");
	check(in_step, "after every call so far *data and *length named the bytes that were left");
}

// A PUBLISH of QoS 0 on the topic "t" with `payload_length` bytes of payload counted up from `seed`. Returns
// its length; the Remaining Length is payload_length + 3.
static size_t publish_of(uint8_t *out, size_t payload_length, uint8_t seed)
{
	size_t count = broker_publish_head(out, 0x30, "t", 1, payload_length, 1);
	size_t i;

	for(i = 0; i < payload_length; i++) out[count + i] = (uint8_t)(seed + i);
	return count + payload_length;
}

static bool room_is(uint8_t value)
{
	size_t i;

	for(i = 0; i < MQTT_SUB_PACKET_MAX; i++)
	{
		if(reader->room[i] != value) return false;
	}
	return true;
}

static void test_too_long(void)
{
	static uint8_t stream[4096];
	static uint8_t filler[4096];
	uint8_t head[16];
	size_t length, left = 99, tail, sent, i;
	uint32_t pieces = 0;
	bool none = true;

	// The longest packet that is kept
	start();
	length = publish_of(stream, MQTT_SUB_PACKET_MAX - 3, 1);
	check(length == 3 + MQTT_SUB_PACKET_MAX && stream[1] == 0x80 && stream[2] == 0x0C, "a PUBLISH with a Remaining Length of 1536: the length is the two bytes 0x80 0x0C");
	check(take(stream, length, &left) == MQTT_SUB_PUBLISH && left == 0 && topic_is("t") && payload_is(stream + 6, MQTT_SUB_PACKET_MAX - 3) &&
	      packet.payload + packet.payload_length == reader->room + MQTT_SUB_PACKET_MAX, "a PUBLISH with a Remaining Length of 1536 is kept: it fills the room to its last byte");
	check(guards_intact(), "the longest PUBLISH that is kept writes nothing behind the room");

	// One byte more
	start();
	memset(reader->room, 0xEE, sizeof(reader->room));
	length = publish_of(stream, MQTT_SUB_PACKET_MAX - 2, 1);
	tail = broker_suback(stream + length, 0x0102, 0);
	check(stream[1] == 0x81 && stream[2] == 0x0C && take(stream, length - 1, &left) == MQTT_SUB_NONE && left == 0 && packet_marked(),
	      "a PUBLISH with a Remaining Length of 1537: nothing comes out before its last byte");
	check(take(stream + length - 1, 1 + tail, &left) == MQTT_SUB_SKIPPED && left == tail && packet.code == 0 && packet.id == 0 && no_message(),
	      "a PUBLISH with a Remaining Length of 1537 is too long: it goes by, and nothing of it comes out");
	check(room_is(0xEE) && guards_intact(), "of a PUBLISH that goes by not a byte is kept");
	check(take(stream + length, tail, &left) == MQTT_SUB_SUBACK && left == 0 && packet.id == 0x0102, "the packet behind a PUBLISH that went by is right");

	// 100000 bytes in a thousand pieces, a SUBACK and a PUBLISH behind them
	start();
	memset(reader->room, 0xEE, sizeof(reader->room));
	for(i = 0; i < sizeof(filler); i++) filler[i] = (uint8_t)(0x30 + i % 7);
	length = broker_publish_head(head, 0x31, "long", 4, 100000 - 6, 1);
	check(length == 10 && head[1] == 0xA0 && head[2] == 0x8D && head[3] == 0x06, "a PUBLISH with a Remaining Length of 100000: the length is the three bytes 0xA0 0x8D 0x06");
	if(take(head, length, &left) != MQTT_SUB_NONE) none = false;
	for(sent = 6; sent < 100000 - 100; sent += 100, pieces++)
	{
		if(take(filler + sent % 1000, 100, &left) != MQTT_SUB_NONE || left != 0) none = false;
		// A pause in the middle
		if(sent == 50006 && (take(filler, 0, &left) != MQTT_SUB_NONE || take(NULL, 0, &left) != MQTT_SUB_NONE)) none = false;
	}
	check(none && pieces == 999 && sent == 99906 && packet_marked(), "a PUBLISH of 100000 bytes in a thousand pieces, with a pause in the middle: nothing comes out while it goes by");
	memcpy(stream, filler, 94);
	tail = 94 + broker_suback(stream + 94, 7, 0);
	tail += publish_of(stream + tail, 5, 0x41);
	check(take(stream, tail, &left) == MQTT_SUB_SKIPPED && left == tail - 94 && no_message(), "with its last byte the PUBLISH of 100000 bytes has gone by");
	check(take(stream + 94, tail - 94, &left) == MQTT_SUB_SUBACK && left == 10 && packet.id == 7 && take(stream + 99, 10, &left) == MQTT_SUB_PUBLISH && left == 0 &&
	      topic_is("t") && payload_is("ABCDE", 5), "a PUBLISH longer than the room goes by, and the packets behind it are right - also when it came in a thousand pieces");
	check(guards_intact(), "a PUBLISH that goes by writes nothing outside the reader");

	// A megabyte, as much as a segment brings at a time
	start();
	memset(reader->room, 0xEE, sizeof(reader->room));
	length = broker_publish_head(head, 0x30, "big", 3, 1048576 - 5, 1);
	none = length == 9 && head[1] == 0x80 && head[2] == 0x80 && head[3] == 0x40 && take(head, length, &left) == MQTT_SUB_NONE;
	for(sent = 5; sent + 1440 < 1048576; sent += 1440)
	{
		if(take(filler, 1440, &left) != MQTT_SUB_NONE || left != 0) none = false;
	}
	memcpy(stream, filler, 1048576 - sent);
	tail = broker_pingresp(stream + (1048576 - sent));
	check(none && take(stream, 1048576 - sent + tail, &left) == MQTT_SUB_SKIPPED && left == tail && take(stream + (1048576 - sent), tail, &left) == MQTT_SUB_PINGRESP &&
	      left == 0 && room_is(0xEE) && guards_intact(), "a PUBLISH of a megabyte goes by, nothing of it is kept, and the PINGRESP behind it is read");

	// A retained one and one without a sensible topic go by all the same: nothing of them is looked at
	start();
	length = broker_publish_head(stream, 0x31, "", 0, 2000, 1);
	memset(stream + length, 0, 2000);
	tail = broker_pingresp(stream + length + 2000);
	check(take(stream, length + 2000 + tail, &left) == MQTT_SUB_SKIPPED && left == tail && no_message(), "a PUBLISH that goes by is not looked at: its topic may be anything");
	check(in_step, "after every call so far *data and *length named the bytes that were left");
}

static void test_length(void)
{
	static uint8_t filler[4096];
	uint8_t stream[2048];
	uint8_t head[16];
	size_t length, left = 99, sent;
	bool none;

	memset(filler, 0x55, sizeof(filler));

	// The limits of one, two and three bytes
	start();
	length = publish_of(stream, 124, 1);
	check(length == 129 && stream[1] == 0x7F && take(stream, length, &left) == MQTT_SUB_PUBLISH && left == 0 && packet.payload_length == 124,
	      "a Remaining Length of 127 is one byte");
	length = publish_of(stream, 125, 1);
	check(length == 131 && stream[1] == 0x80 && stream[2] == 0x01 && take(stream, length, &left) == MQTT_SUB_PUBLISH && left == 0 && packet.payload_length == 125 &&
	      payload_is(stream + 6, 125), "a Remaining Length of 128 is the two bytes 0x80 0x01: the lowest seven bits come first");
	length = publish_of(stream, 318, 1);
	check(length == 324 && stream[1] == 0xC1 && stream[2] == 0x02 && take(stream, length, &left) == MQTT_SUB_PUBLISH && left == 0 && packet.payload_length == 318,
	      "a Remaining Length of 321 is the two bytes 0xC1 0x02");

	length = broker_publish_head(head, 0x30, "t", 1, 16383 - 3, 1);
	none = length == 6 && head[1] == 0xFF && head[2] == 0x7F && take(head, length, &left) == MQTT_SUB_NONE;
	for(sent = 3; sent + 4096 <= 16383; sent += 4096)
	{
		if(take(filler, 4096, &left) != MQTT_SUB_NONE) none = false;
	}
	check(none && sent == 12291 && take(filler, 4092 + 5, &left) == MQTT_SUB_SKIPPED && left == 5, "a Remaining Length of 16383 is the two bytes 0xFF 0x7F: exactly as many bytes go by");
	length = broker_publish_head(head, 0x30, "t", 1, 16384 - 3, 1);
	none = length == 7 && head[1] == 0x80 && head[2] == 0x80 && head[3] == 0x01 && take(head, length, &left) == MQTT_SUB_NONE;
	for(sent = 3; sent + 4096 <= 16384; sent += 4096)
	{
		if(take(filler, 4096, &left) != MQTT_SUB_NONE) none = false;
	}
	check(none && sent == 12291 && take(filler, 4092, &left) == MQTT_SUB_NONE && take(filler, 1 + 5, &left) == MQTT_SUB_SKIPPED && left == 5,
	      "a Remaining Length of 16384 is the three bytes 0x80 0x80 0x01: exactly as many bytes go by");

	// Four bytes
	length = broker_publish_head(head, 0x30, "t", 1, 2097152 - 3, 1);
	none = length == 8 && head[1] == 0x80 && head[2] == 0x80 && head[3] == 0x80 && head[4] == 0x01 && take(head, length, &left) == MQTT_SUB_NONE;
	for(sent = 3; sent + 4096 <= 2097152; sent += 4096)
	{
		if(take(filler, 4096, &left) != MQTT_SUB_NONE) none = false;
	}
	check(none && sent == 2097152 - 4093 && take(filler, 4092, &left) == MQTT_SUB_NONE && take(filler, 1 + 9, &left) == MQTT_SUB_SKIPPED && left == 9,
	      "a Remaining Length of 2097152 is the four bytes 0x80 0x80 0x80 0x01: exactly as many bytes go by");
	start();
	check(take_hex("30FFFFFF7F", &left) == MQTT_SUB_NONE && left == 0, "the largest Remaining Length, 0xFF 0xFF 0xFF 0x7F, is taken");
	none = true;
	for(sent = 0; sent + 4096 <= 268435455; sent += 4096)
	{
		if(take(filler, 4096, &left) != MQTT_SUB_NONE) none = false;
	}
	check(none && sent == 268435455 - 4095 && take(filler, 4094, &left) == MQTT_SUB_NONE && take(filler, 1 + 2, &left) == MQTT_SUB_SKIPPED && left == 2,
	      "a PUBLISH of 268435455 bytes goes by: exactly as many bytes, and not one more");

	// More bytes than the number needs
	start();
	check(take_hex("90830000010020820000", &left) == MQTT_SUB_SUBACK && left == 4 && packet.id == 1, "a Remaining Length of 3 written with two bytes counts as 3");
	check(take_hex("D08000", &left) == MQTT_SUB_PINGRESP && left == 0 && take_hex("D080808000D000", &left) == MQTT_SUB_PINGRESP && left == 2,
	      "a Remaining Length of 0 written with two and with four bytes counts as 0");
	check(take_hex("208280800000042002", &left) == MQTT_SUB_CONNACK && left == 2 && packet.code == 4, "a Remaining Length of 2 written with four bytes counts as 2");
	check(take_hex("30848000000161626300", &left) == MQTT_SUB_PUBLISH && left == 2 && topic_is("a") && payload_is("b", 1), "a Remaining Length of 4 written with three bytes counts as 4");
	check(in_step && guards_intact(), "after every call so far *data and *length named the bytes that were left, and nothing was written outside the reader");
}

static void test_broken(void)
{
	static const uint8_t known[] = {0x20, 0x30, 0x31, 0x90, 0xD0};
	static const struct
	{
		const char *digits;
		size_t left;
		const char *what;
	}
	wrong[] = {
		{"32040001616263", 6, "a PUBLISH with QoS 1 is broken, with its first byte"},
		{"33040001616263", 6, "a retained PUBLISH with QoS 1 is broken"},
		{"34040001616263", 6, "a PUBLISH with QoS 2 is broken"},
		{"36040001616263", 6, "a PUBLISH with both QoS bits is broken"},
		{"38040001616263", 6, "a PUBLISH of QoS 0 with the DUP flag is broken"},
		{"3A0600016100016263", 8, "a PUBLISH with QoS 1 and the DUP flag is broken"},
		{"101000044D5154540402001E000461626364", 17, "a CONNECT is broken: a subscriber does not get one"},
		{"820600010001610000", 8, "a SUBSCRIBE is broken: a subscriber does not get one"},
		{"C000", 1, "a PINGREQ is broken: a subscriber does not get one"},
		{"E000", 1, "a DISCONNECT is broken: a subscriber does not get one"},
		{"A2050001000161", 6, "an UNSUBSCRIBE is broken: a subscriber does not get one"},
		{"B0020001", 3, "an UNSUBACK is broken: nothing was unsubscribed"},
		{"40020001", 3, "a PUBACK is broken: nothing was published"},
		{"50020001", 3, "a PUBREC is broken"},
		{"62020001", 3, "a PUBREL is broken"},
		{"70020001", 3, "a PUBCOMP is broken"},
		{"0000", 1, "the packet type 0 is broken"},
		{"F000", 1, "the packet type 15 is broken"},
		{"21020000", 3, "a CONNACK with a flag in its first byte is broken"},
		{"28020000", 3, "a CONNACK with another flag in its first byte is broken"},
		{"9203000100", 4, "a SUBACK with a flag in its first byte is broken"},
		{"D100", 1, "a PINGRESP with a flag in its first byte is broken"},
		{"2001006F", 2, "a CONNACK with a Remaining Length of 1 is broken, with the last byte of the length"},
		{"200300000000", 4, "a CONNACK with a Remaining Length of 3 is broken"},
		{"2000D000", 2, "a CONNACK with a Remaining Length of 0 is broken"},
		{"208110", 0, "a CONNACK with a Remaining Length of 2049 is broken: only a PUBLISH may be longer than the room"},
		{"208201", 0, "a CONNACK with a Remaining Length of 130 is broken: the second byte of the length counts"},
		{"9002000100", 3, "a SUBACK with a Remaining Length of 2 is broken"},
		{"900400010000", 4, "a SUBACK with a Remaining Length of 4 is broken: one topic was subscribed"},
		{"90830100", 1, "a SUBACK with a Remaining Length of 131 is broken"},
		{"D00100", 1, "a PINGRESP with a Remaining Length of 1 is broken"},
		{"D0FF7F", 0, "a PINGRESP with a Remaining Length of 16383 is broken"},
		{"3000D000", 2, "a PUBLISH with a Remaining Length of 0 is broken: it has no room for a topic"},
		{"300161", 1, "a PUBLISH with a Remaining Length of 1 is broken"},
		{"30020001", 2, "a PUBLISH with a Remaining Length of 2 is broken: a topic has at least one byte"},
		{"3080808080", 0, "a Remaining Length of five bytes is broken, with its fourth byte"},
		{"30FFFFFFFF7F", 1, "a Remaining Length of five bytes is broken, whatever number it would be"},
		{"D08080808000", 1, "a Remaining Length of 0 in five bytes is broken"},
		{"3080808080808080", 3, "a Remaining Length of eight bytes is broken, with its fourth byte"},
		{"20020100D000", 2, "a CONNACK that tells of a session the broker kept is broken: a clean session was asked for"},
		{"20028000", 0, "a CONNACK with a reserved acknowledge flag is broken"},
		{"20020205", 0, "a CONNACK with another reserved acknowledge flag is broken"},
		{"2002FF00", 0, "a CONNACK with every acknowledge flag is broken"},
		{"30030000619000", 2, "a PUBLISH with a topic of no byte is broken"},
		{"3005000461626320", 1, "a topic that reaches one byte beyond the packet is broken"},
		{"30050100616263", 0, "a topic that reaches far beyond the packet is broken: the high byte of its length counts"},
		{"3004FFFF6162", 0, "a topic of 65535 bytes in a packet of 4 is broken"},
		{"30050101616263", 0, "a topic of 257 bytes in a packet of 5 is broken: not only the low byte of its length counts"},
		{"31030002FF", 0, "a retained PUBLISH whose topic reaches beyond the packet is broken"},
	};
	uint8_t stream[64];
	size_t length, left = 99, i;
	int first, taken = 0;
	bool right = true, final = true;

	// Every byte as the first one of a packet, with a byte behind it
	for(first = 0; first < 256; first++)
	{
		bool wanted = memchr(known, first, sizeof(known)) != NULL;
		mqtt_sub_kind_t kind;

		start();
		stream[0] = (uint8_t)first;
		stream[1] = 0x80;
		kind = take(stream, 2, &left);
		if(kind == MQTT_SUB_NONE && left == 0) taken++;
		if(wanted ? kind != MQTT_SUB_NONE || left != 0 : kind != MQTT_SUB_BROKEN || left != 1) right = false;
		if(!packet_marked()) right = false;
	}
	check(right && taken == 5, "of the 256 first bytes five begin a packet - 0x20, 0x30, 0x31, 0x90, 0xD0 - and every other is broken at once, the byte behind it not taken");

	for(i = 0; i < COUNT(wrong); i++)
	{
		mqtt_sub_kind_t kind;
		const uint8_t *data = stream;

		start();
		kind = take_hex(wrong[i].digits, &left);
		check(kind == MQTT_SUB_BROKEN && left == wrong[i].left && packet_marked(), wrong[i].what);

		// And it stays broken: a good packet is not read, not a byte of it taken, nothing written
		length = broker_pingresp(stream);
		left = length;
		if(mqtt_sub_read(reader, &data, &left, &packet) != MQTT_SUB_BROKEN || data != stream || left != length || !packet_marked()) final = false;
		left = 0;
		if(mqtt_sub_read(reader, &data, &left, &packet) != MQTT_SUB_BROKEN || data != stream || left != 0 || !guards_intact()) final = false;
	}
	check(final, "after broken nothing more is read: every further call says broken, takes no byte and writes no packet");

	// When it is decided
	start();
	check(take_hex("30808080", &left) == MQTT_SUB_NONE && left == 0 && take_hex("80", &left) == MQTT_SUB_BROKEN,
	      "three bytes of a length that announce another are no error yet, the fourth is");
	start();
	check(take_hex("3080808001", &left) == MQTT_SUB_NONE && left == 0, "a Remaining Length of four bytes is not broken");
	start();
	check(take_hex("20", &left) == MQTT_SUB_NONE && take_hex("03", &left) == MQTT_SUB_BROKEN, "a wrong length is broken with the byte that ends it, before anything follows");
	start();
	check(take_hex("200201", &left) == MQTT_SUB_NONE && left == 0 && packet_marked() && take_hex("00", &left) == MQTT_SUB_BROKEN,
	      "what is wrong inside a CONNACK is broken when the CONNACK is complete");
	start();
	check(take_hex("3005000461", &left) == MQTT_SUB_NONE && take_hex("62", &left) == MQTT_SUB_NONE && left == 0 && take_hex("63", &left) == MQTT_SUB_BROKEN,
	      "a topic that reaches beyond the packet is broken when the packet is complete");
	start();
	check(take_hex("300500036162632002", &left) == MQTT_SUB_PUBLISH && left == 2 && topic_is("abc") && packet.payload_length == 0,
	      "a topic that reaches exactly to the end of the packet is a PUBLISH without payload");
	start();
	check(take_hex("3006000461626364", &left) == MQTT_SUB_PUBLISH && topic_is("abcd") && take_hex("300700046162636465", &left) == MQTT_SUB_PUBLISH && payload_is("e", 1),
	      "a topic one byte short of the end leaves a payload of one byte");

	// A new connection
	start();
	check(take_hex("E000", &left) == MQTT_SUB_BROKEN && take_hex("D000", &left) == MQTT_SUB_BROKEN, "a reader that is broken stays broken");
	mqtt_sub_begin(reader);
	check(take_hex("D000", &left) == MQTT_SUB_PINGRESP && left == 0, "a reader that begins anew reads again");
	start();
	check(take_hex("30400011", &left) == MQTT_SUB_NONE, "a reader in the middle of a packet ...");
	mqtt_sub_begin(reader);
	check(take_hex("9003000700", &left) == MQTT_SUB_SUBACK && left == 0 && packet.id == 7, "... that begins anew forgets the packet it was in");
	start();
	check(take_hex("3081", &left) == MQTT_SUB_NONE, "a reader in the middle of a length ...");
	mqtt_sub_begin(reader);
	check(take_hex("300400017879D000", &left) == MQTT_SUB_PUBLISH && left == 2 && topic_is("x") && payload_is("y", 1), "... that begins anew forgets the length it was in");
	check(in_step, "after every call so far *data and *length named the bytes that were left");
}

/* ---------------------------------------------------------------------------------------------------
 * Random streams. What a broker sends, right and wrong, is written one packet behind the other; then the
 * stream goes through the reader in one piece, byte by byte, in random pieces and with single cuts, and
 * plays() compares every call with the rules written once more.
 */

#define WALKS           1500
#define WALK_ROOM       200000
#define WALK_CUTS       24

enum
{
	WRONG_FIRST,
	WRONG_FIVE,
	WRONG_LENGTH,
	WRONG_SHORT,
	WRONG_FLAGS,
	WRONG_NO_TOPIC,
	WRONG_TOPIC,
	WRONGS,
};

typedef struct
{
	long streams, different, bytes, plays;
	long written[MQTT_SUB_BROKEN + 1];  // packets written into the streams, by what they are
	long wrong[WRONGS];
	long padded;                        // lengths written with more bytes than needed
	long full, over;                    // PUBLISH that fill the room exactly, and that are one byte too long
	long open;                          // streams that end in the middle of a packet
	tally_t tally;
} walk_result_t;

static uint8_t walk_stream[WALK_ROOM];

static void fill_random(uint8_t *out, size_t length)
{
	size_t i;

	for(i = 0; i < length; i++) out[i] = (uint8_t)roll(256);
}

// One packet a broker may send. Returns its length.
static size_t walk_right(uint8_t *out, walk_result_t *result)
{
	static const uint8_t codes[] = {0, 0, 0, 1, 2, 5, 0x80, 0xFF};
	uint32_t what = roll(100);
	uint32_t padding = roll(8) == 0 ? 1 + roll(4) : 1;
	size_t topic_length, payload_length, count;
	uint8_t topic[400];
	uint8_t code, first;
	uint16_t id;

	if(what < 12)
	{
		code = codes[roll(COUNT(codes))];
		result->written[MQTT_SUB_CONNACK]++;
		return broker_connack(out, 0, code);
	}
	if(what < 24)
	{
		code = codes[roll(COUNT(codes))];
		id = (uint16_t)roll(65536);
		result->written[MQTT_SUB_SUBACK]++;
		return broker_suback(out, id, code);
	}
	if(what < 36)
	{
		result->written[MQTT_SUB_PINGRESP]++;
		out[0] = 0xD0;
		if(padding > 1) result->padded++;
		return 1 + broker_length(out + 1, 0, (int)padding);
	}

	topic_length = roll(10) == 0 ? 1 + roll(sizeof(topic)) : 1 + roll(24);
	fill_random(topic, topic_length);
	first = (uint8_t)(0x30 + roll(2));
	if(what < 88)
	{
		uint32_t size = roll(10);

		if(size < 5) payload_length = roll(64);
		else if(size < 8) payload_length = 700 + roll(200);
		else if(size == 8) payload_length = roll(MQTT_SUB_PACKET_MAX - 2 - (uint32_t)topic_length + 1);
		else payload_length = MQTT_SUB_PACKET_MAX - 2 - topic_length;
		if(2 + topic_length + payload_length == MQTT_SUB_PACKET_MAX) result->full++;
		result->written[MQTT_SUB_PUBLISH]++;
	}
	else
	{
		uint32_t size = roll(10);

		if(size < 4) payload_length = MQTT_SUB_PACKET_MAX - 2 - topic_length + 1;
		else if(size < 8) payload_length = MQTT_SUB_PACKET_MAX + roll(3000);
		else payload_length = 15000 + roll(20000);
		if(2 + topic_length + payload_length == MQTT_SUB_PACKET_MAX + 1) result->over++;
		result->written[MQTT_SUB_SKIPPED]++;
	}
	if(padding > 1) result->padded++;
	count = broker_publish_head(out, first, topic, topic_length, payload_length, (int)padding);
	fill_random(out + count, payload_length);
	return count + payload_length;
}

// Something a broker must not send. Returns its length; nothing behind it is read.
static size_t walk_wrong(uint8_t *out, walk_result_t *result)
{
	static const uint8_t firsts[] = {0x20, 0x90, 0xD0, 0x30, 0x31};
	static const uint8_t lengths[] = {0, 1, 2, 3, 4, 5, 127};
	uint32_t what = roll(WRONGS);
	size_t count = 0;
	uint8_t first, wanted, length;
	uint32_t pick, number;
	int bytes;

	result->wrong[what]++;
	result->written[MQTT_SUB_BROKEN]++;
	switch(what)
	{
		case WRONG_FIRST:
			do
			{
				first = (uint8_t)roll(256);
			}
			while(memchr(firsts, first, sizeof(firsts)) != NULL);
			out[0] = first;
			return 1;
		case WRONG_FIVE:
			out[0] = firsts[roll(COUNT(firsts))];
			fill_random(out + 1, 4);
			out[1] |= 0x80;
			out[2] |= 0x80;
			out[3] |= 0x80;
			out[4] |= 0x80;
			out[5] = (uint8_t)roll(128);
			return 6;
		case WRONG_LENGTH:
			pick = roll(3);
			first = firsts[pick];
			wanted = pick == 0 ? 2 : pick == 1 ? 3 : 0;
			do
			{
				length = lengths[roll(COUNT(lengths))];
			}
			while(length == wanted);
			// Sometimes longer than the room: only a PUBLISH may be
			number = roll(6) == 0 ? 2000u + length : length;
			bytes = 1 + (int)roll(2);
			out[0] = first;
			return 1 + broker_length(out + 1, number, bytes);
		case WRONG_SHORT:
			out[0] = (uint8_t)(0x30 + roll(2));
			number = roll(3);
			bytes = 1 + (int)roll(3);
			return 1 + broker_length(out + 1, number, bytes);
		case WRONG_FLAGS:
			// `first` are the acknowledge flags here, `wanted` the return code
			first = (uint8_t)(1 + roll(255));
			wanted = (uint8_t)roll(6);
			return broker_connack(out, first, wanted);
		case WRONG_NO_TOPIC:
			length = (uint8_t)(1 + roll(40));
			count = broker_publish_head(out, (uint8_t)(0x30 + roll(2)), "", 0, length, 1);
			fill_random(out + count, length);
			return count + length;
		default:
			// The topic is said to be longer than everything behind its length
			length = (uint8_t)(1 + roll(40));
			pick = (uint32_t)length + 1 + (roll(2) == 0 ? 0 : roll(60000));
			out[0] = (uint8_t)(0x30 + roll(2));
			count = 1 + broker_length(out + 1, 2u + length, 1);
			out[count++] = (uint8_t)(pick / 256);
			out[count++] = (uint8_t)(pick % 256);
			fill_random(out + count, length);
			return count + length;
	}
}

static bool walk(uint32_t seed, walk_result_t *result)
{
	size_t length = 0;
	uint32_t packets, cuts, i;
	bool ends_wrong;

	random_state = seed;
	packets = 1 + roll(30);
	ends_wrong = roll(3) == 0;
	for(i = 0; i < packets && length < WALK_ROOM - 80000; i++) length += walk_right(walk_stream + length, result);
	if(ends_wrong)
	{
		length += walk_wrong(walk_stream + length, result);
		// What follows is never read
		fill_random(walk_stream + length, 40);
		length += roll(40);
	}
	else if(roll(3) == 0)
	{
		// The connection ends in the middle of a packet
		uint32_t shorter = 1 + roll(30);

		length = length > shorter ? length - shorter : 1;
		result->open++;
	}
	result->streams++;
	result->bytes += (long)length;

	if(!plays(walk_stream, length, piece_whole, &result->tally)) return false;
	if(!plays(walk_stream, length, piece_byte, &result->tally)) return false;
	if(!plays(walk_stream, length, piece_random, &result->tally)) return false;
	if(!plays(walk_stream, length, piece_random, &result->tally)) return false;
	result->plays += 4;

	// One cut at a time: at every position of a short stream, at some positions of a long one
	cuts = length <= 300 ? (uint32_t)length - 1 : WALK_CUTS;
	for(i = 0; i < cuts; i++)
	{
		cut_position = length <= 300 ? i + 1 : 1 + roll((uint32_t)length - 1);
		if(!plays(walk_stream, length, piece_cut, &result->tally)) return false;
		result->plays++;
	}
	return true;
}

static void test_walk(void)
{
	walk_result_t result;
	int ends[2];
	int status = -1;
	bool complete = false;
	pid_t child;

	memset(&result, 0, sizeof(result));
	fflush(stdout);
	if(pipe(ends) != 0)
	{
		check(false, "the walk has a pipe for its result");
		return;
	}

	// In a child process: a crash or a hang of the module is then a failed check here, not the end of the test
	alarm(240);
	child = fork();
	if(child == 0)
	{
		uint32_t seed;

		close(ends[0]);
		alarm(120);
		for(seed = 1; seed <= WALKS; seed++)
		{
			if(!walk(seed, &result))
			{
				if(result.different == 0) printf("  walk %lu: the reader and the rules written once more differ\n", (unsigned long)seed);
				result.different++;
			}
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

	printf("  walk: %ld streams of %ld bytes, played %ld times; written %ld CONNACK, %ld SUBACK, %ld PINGRESP, %ld PUBLISH that are kept (%ld fill the room), "
	       "%ld that go by (%ld are one byte too long), %ld lengths with more bytes than needed, %ld streams that end inside a packet\n",
	       result.streams, result.bytes, result.plays, result.written[MQTT_SUB_CONNACK], result.written[MQTT_SUB_SUBACK], result.written[MQTT_SUB_PINGRESP],
	       result.written[MQTT_SUB_PUBLISH], result.full, result.written[MQTT_SUB_SKIPPED], result.over, result.padded, result.open);
	printf("  walk: broken by %ld first bytes, %ld lengths of five bytes, %ld wrong lengths, %ld PUBLISH too short, %ld CONNACK flags, %ld empty topics, %ld topics beyond the packet\n",
	       result.wrong[WRONG_FIRST], result.wrong[WRONG_FIVE], result.wrong[WRONG_LENGTH], result.wrong[WRONG_SHORT], result.wrong[WRONG_FLAGS],
	       result.wrong[WRONG_NO_TOPIC], result.wrong[WRONG_TOPIC]);
	printf("  walk: returned %ld none, %ld CONNACK, %ld SUBACK, %ld PINGRESP, %ld PUBLISH, %ld gone by, %ld broken; pieces ended %ld, %ld, %ld, %ld and %ld or more bytes "
	       "into a packet, %ld inside one that goes by\n",
	       result.tally.kinds[MQTT_SUB_NONE], result.tally.kinds[MQTT_SUB_CONNACK], result.tally.kinds[MQTT_SUB_SUBACK], result.tally.kinds[MQTT_SUB_PINGRESP],
	       result.tally.kinds[MQTT_SUB_PUBLISH], result.tally.kinds[MQTT_SUB_SKIPPED], result.tally.kinds[MQTT_SUB_BROKEN],
	       result.tally.cuts[0], result.tally.cuts[1], result.tally.cuts[2], result.tally.cuts[3], result.tally.cuts[4], result.tally.cuts[5]);

	check(complete && status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "1500 random streams: no crash and no hang");
	check(complete && result.different == 0 && result.streams == WALKS,
	      "1500 random streams in one piece, byte by byte, in random pieces and with single cuts: the reader tells what the rules written once more tell, with every call");
	check(complete && result.written[MQTT_SUB_CONNACK] > 1000 && result.written[MQTT_SUB_SUBACK] > 1000 && result.written[MQTT_SUB_PINGRESP] > 1000 &&
	      result.written[MQTT_SUB_PUBLISH] > 5000 && result.written[MQTT_SUB_SKIPPED] > 1000 && result.full > 300 && result.over > 300 && result.padded > 500 &&
	      result.open > 100, "the streams hold every kind of packet in numbers, also PUBLISH at both sides of the limit, padded lengths and open ends");
	check(complete && result.wrong[WRONG_FIRST] > 30 && result.wrong[WRONG_FIVE] > 30 && result.wrong[WRONG_LENGTH] > 30 && result.wrong[WRONG_SHORT] > 30 &&
	      result.wrong[WRONG_FLAGS] > 30 && result.wrong[WRONG_NO_TOPIC] > 30 && result.wrong[WRONG_TOPIC] > 30, "the streams end with every kind of broken packet in numbers");
	check(complete && result.tally.cuts[0] > 5000 && result.tally.cuts[1] > 5000 && result.tally.cuts[2] > 5000 && result.tally.cuts[3] > 5000 &&
	      result.tally.cuts[4] > 100000 && result.tally.cuts[5] > 100000, "the pieces end at every place of a packet in numbers");
}

int main(void)
{
	test_constants();
	test_notation();
	test_connect();
	test_subscribe();
	test_ping();
	test_built();
	test_packets();
	test_pieces();
	test_too_long();
	test_length();
	test_broken();
	test_walk();
	return test_end();
}
