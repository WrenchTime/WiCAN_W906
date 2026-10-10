/*
 * The broker of the tests of mqtt_sub.h and cerbo.h (test_mqtt_sub.c, test_cerbo.c): everything the display
 * cannot do itself, written for the tests alone.
 *   - fixtures of bytes, written by hand in the notation below (fixtures/mqtt_*.txt)
 *   - the packets a broker sends, built from their parts: the core has no function that builds them
 *   - what a broker reads in a packet the display built (MQTT 3.1.1, sections 3.1, 3.8 and 3.12)
 *   - the rules of the reader of mqtt_sub.h written once more, in another shape: with the whole stream at
 *     hand instead of byte by byte, and with type and flags of the first byte taken apart
 *
 * A header with code, for one test program each, as app_harness.h.
 */
#ifndef __MQTT_HARNESS_H__
#define __MQTT_HARNESS_H__

#include <stdint.h>
#include <stdlib.h>
#include "test.h"
#include "mqtt_sub.h"

/* ---------------------------------------------------------------------------------------------------
 * Fixtures of bytes. The notation:
 *   c0 00           bytes, two hex digits each, separated by blanks or line ends
 *   'text'          the bytes of the text between the quotes, on one line
 *   # ...           a comment, to the end of the line
 */

static inline int hex_digit(char c)
{
	if(c >= '0' && c <= '9') return c - '0';
	if(c >= 'a' && c <= 'f') return c - 'a' + 10;
	if(c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// false if the file is missing, is not written in the notation or has more than `size` bytes
static inline bool read_hex(const char *path, uint8_t *bytes, size_t size, size_t *length)
{
	static char text[16384];
	const char *p;
	size_t count = 0;

	*length = 0;
	if(!read_fixture(path, text, sizeof(text))) return false;

	for(p = text; *p != '\0'; p++)
	{
		int high, low;

		if(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') continue;
		if(*p == '#')
		{
			while(p[1] != '\0' && p[1] != '\n') p++;
			continue;
		}
		if(*p == '\'')
		{
			for(p++; *p != '\''; p++)
			{
				if(*p == '\0' || *p == '\n' || count >= size) return false;
				bytes[count++] = (uint8_t)*p;
			}
			continue;
		}
		high = hex_digit(p[0]);
		low = high < 0 ? -1 : hex_digit(p[1]);
		if(low < 0 || count >= size) return false;
		bytes[count++] = (uint8_t)(high * 16 + low);
		p++;
		// Two digits are one byte: a third one would be a slip of the hand
		if(hex_digit(p[1]) >= 0) return false;
	}
	*length = count;
	return true;
}

/* ---------------------------------------------------------------------------------------------------
 * What a broker sends
 */

// The Remaining Length `value` (MQTT 3.1.1, 2.2.3) in `bytes` bytes, or in as few as it takes if that is
// more. Returns the number of bytes written.
static inline size_t broker_length(uint8_t *out, uint32_t value, int bytes)
{
	size_t count = 0;

	do
	{
		uint8_t digit = (uint8_t)(value % 128);

		value /= 128;
		bytes--;
		if(value > 0 || bytes > 0) digit |= 128;
		out[count++] = digit;
	}
	while(value > 0 || bytes > 0);
	return count;
}

static inline size_t broker_connack(uint8_t *out, uint8_t flags, uint8_t code)
{
	out[0] = 0x20;
	out[1] = 2;
	out[2] = flags;
	out[3] = code;
	return 4;
}

static inline size_t broker_suback(uint8_t *out, uint16_t id, uint8_t code)
{
	out[0] = 0x90;
	out[1] = 3;
	out[2] = (uint8_t)(id / 256);
	out[3] = (uint8_t)(id % 256);
	out[4] = code;
	return 5;
}

static inline size_t broker_pingresp(uint8_t *out)
{
	out[0] = 0xD0;
	out[1] = 0;
	return 2;
}

// The fixed header and the topic of a PUBLISH that begins with `first` and has `payload_length` bytes of
// payload behind them: the caller appends the payload. length_bytes as for broker_length().
static inline size_t broker_publish_head(uint8_t *out, uint8_t first, const void *topic, size_t topic_length, size_t payload_length, int length_bytes)
{
	size_t count = 1;

	out[0] = first;
	count += broker_length(out + count, (uint32_t)(2 + topic_length + payload_length), length_bytes);
	out[count++] = (uint8_t)(topic_length / 256);
	out[count++] = (uint8_t)(topic_length % 256);
	if(topic_length > 0) memcpy(out + count, topic, topic_length);
	return count + topic_length;
}

// A whole PUBLISH of QoS 0
static inline size_t broker_publish(uint8_t *out, bool retained, const char *topic, const void *payload, size_t payload_length)
{
	size_t count = broker_publish_head(out, retained ? 0x31 : 0x30, topic, strlen(topic), payload_length, 1);

	if(payload_length > 0) memcpy(out + count, payload, payload_length);
	return count + payload_length;
}

/* ---------------------------------------------------------------------------------------------------
 * What a broker reads
 */

typedef struct
{
	int type;               // 1 CONNECT, 8 SUBSCRIBE, 12 PINGREQ; 0 if the bytes are no packet a broker takes
	int flags;              // of a CONNECT: the connect flags
	unsigned keepalive_s;
	unsigned id;            // of a SUBSCRIBE: the packet identifier
	unsigned qos;           // and the requested QoS
	char text[80];          // the client id of a CONNECT, the topic filter of a SUBSCRIBE
	size_t text_length;
} client_packet_t;

// Reads the bytes as the one packet of a client they have to be: a CONNECT without will, name and password, a
// SUBSCRIBE with one topic filter, or a PINGREQ - with nothing missing and nothing behind it. type 0 for
// everything else.
static inline client_packet_t broker_reads(const uint8_t *bytes, size_t length)
{
	client_packet_t read;
	size_t text_length;

	memset(&read, 0, sizeof(read));
	// Every packet of the display is short: one byte of Remaining Length, and it names all that follows
	if(length < 2 || bytes[1] >= 128 || (size_t)bytes[1] + 2 != length) return read;

	if(bytes[0] == 0xC0)
	{
		if(length == 2) read.type = 12;
		return read;
	}
	if(bytes[0] == 0x10)
	{
		if(length < 14 || memcmp(bytes + 2, "\0\4MQTT\4", 7) != 0) return read;
		text_length = (size_t)bytes[12] * 256 + bytes[13];
		// Behind the client id there would be a will, a user name or a password
		if(14 + text_length != length || text_length >= sizeof(read.text)) return read;
		read.type = 1;
		read.flags = bytes[9];
		read.keepalive_s = (unsigned)bytes[10] * 256 + bytes[11];
		memcpy(read.text, bytes + 14, text_length);
		read.text_length = text_length;
		return read;
	}
	if(bytes[0] == 0x82)
	{
		if(length < 7) return read;
		text_length = (size_t)bytes[4] * 256 + bytes[5];
		// One topic filter and its requested QoS, no second one
		if(7 + text_length != length || text_length >= sizeof(read.text)) return read;
		read.type = 8;
		read.id = (unsigned)bytes[2] * 256 + bytes[3];
		memcpy(read.text, bytes + 6, text_length);
		read.text_length = text_length;
		read.qos = bytes[6 + text_length];
		return read;
	}
	return read;
}

/* ---------------------------------------------------------------------------------------------------
 * The reader once more
 */

typedef struct
{
	mqtt_sub_kind_t kind;
	uint8_t code;
	uint16_t id;
	bool retained;
	size_t topic_at, topic_length;      // of a PUBLISH: where its topic and its payload are in the stream
	size_t payload_at, payload_length;
} told_t;

// The packet that begins at stream[*at]: what mqtt_sub_read() has to tell of it once all `length` bytes of the
// stream went in. *at moves behind the packet. MQTT_SUB_NONE if the stream ends before the packet does, or
// before it is decided that it is broken; *at stays then.
static inline mqtt_sub_kind_t told_next(const uint8_t *stream, size_t length, size_t *at, told_t *told)
{
	size_t start = *at;
	size_t have = length - start;
	size_t head = 0;
	uint32_t remaining = 0;
	const uint8_t *body;
	unsigned type, flags;
	bool right;
	int i;

	memset(told, 0, sizeof(*told));
	told->kind = MQTT_SUB_NONE;
	if(have == 0) return MQTT_SUB_NONE;

	type = stream[start] / 16;
	flags = stream[start] % 16;
	// CONNACK, SUBACK and PINGRESP without a flag; a PUBLISH with nothing but RETAIN
	right = type == 3 ? flags <= 1 : (type == 2 || type == 9 || type == 13) && flags == 0;
	if(!right)
	{
		*at = start + 1;
		return told->kind = MQTT_SUB_BROKEN;
	}

	// The length ends with the first of four bytes that is below 128
	for(i = 1; head == 0; i++)
	{
		if((size_t)i >= have) return MQTT_SUB_NONE;
		remaining |= (uint32_t)(stream[start + (size_t)i] % 128) << (7 * (i - 1));
		if(stream[start + (size_t)i] < 128) head = (size_t)i + 1;
		else if(i == 4)
		{
			*at = start + 5;
			return told->kind = MQTT_SUB_BROKEN;
		}
	}

	if(type == 2) right = remaining == 2;
	else if(type == 9) right = remaining == 3;
	else if(type == 13) right = remaining == 0;
	else right = remaining > 2;
	if(!right)
	{
		*at = start + head;
		return told->kind = MQTT_SUB_BROKEN;
	}

	if(have - head < remaining) return MQTT_SUB_NONE;
	*at = start + head + remaining;
	body = stream + start + head;

	if(type == 13) return told->kind = MQTT_SUB_PINGRESP;
	if(type == 2)
	{
		if(body[0] != 0) return told->kind = MQTT_SUB_BROKEN;
		told->code = body[1];
		return told->kind = MQTT_SUB_CONNACK;
	}
	if(type == 9)
	{
		told->id = (uint16_t)(body[0] * 256 + body[1]);
		told->code = body[2];
		return told->kind = MQTT_SUB_SUBACK;
	}
	if(remaining > MQTT_SUB_PACKET_MAX) return told->kind = MQTT_SUB_SKIPPED;

	told->topic_length = (size_t)body[0] * 256 + body[1];
	if(told->topic_length < 1 || told->topic_length + 2 > remaining)
	{
		told->topic_length = 0;
		return told->kind = MQTT_SUB_BROKEN;
	}
	told->retained = flags == 1;
	told->topic_at = start + head + 2;
	told->payload_at = told->topic_at + told->topic_length;
	told->payload_length = remaining - 2 - told->topic_length;
	return told->kind = MQTT_SUB_PUBLISH;
}

static inline void mqtt_harness_unused(void)
{
	(void)read_hex;
	(void)broker_connack;
	(void)broker_suback;
	(void)broker_pingresp;
	(void)broker_publish;
	(void)broker_reads;
	(void)told_next;
	test_unused();
}

#endif
