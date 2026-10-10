/*
 * Standalone display for the WiCAN W906 fork.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <string.h>
#include "mqtt_sub.h"

// The first byte of a packet: its type in the upper four bits, its flags in the lower four (2.2.1, 2.2.2)
#define FIRST_CONNECT       0x10u
#define FIRST_CONNACK       0x20u
#define FIRST_PUBLISH       0x30u
#define FIRST_SUBSCRIBE     0x82u
#define FIRST_SUBACK        0x90u
#define FIRST_PINGREQ       0xC0u
#define FIRST_PINGRESP      0xD0u
#define PUBLISH_RETAIN      0x01u   // the one flag a PUBLISH of QoS 0 may carry

#define CLEAN_SESSION       0x02u   // bit 1 of the connect flags
#define CONNECT_HEAD        14      // bytes of a CONNECT before its client id
#define SUBSCRIBE_AROUND    7       // bytes of a SUBSCRIBE around its topic
#define LENGTH_LAST_SCALE   (128u * 128u * 128u)    // what the fourth byte of a Remaining Length counts

enum
{
	READ_FIRST,     // the next byte begins a packet
	READ_LENGTH,
	READ_BODY,      // the bytes behind the fixed header go into the room
	READ_SKIP,      // they go by
	READ_BROKEN,
};

// A letter or a digit of ASCII, whatever the kind of char and the locale say: isalnum() follows both
static bool id_character(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

size_t mqtt_sub_connect(uint8_t *out, size_t size, const char *name, uint16_t keepalive_s)
{
	uint8_t id[MQTT_SUB_ID_MAX];
	size_t count = 0;

	if(out == NULL || name == NULL) return 0;

	for(; *name != '\0' && count < MQTT_SUB_ID_MAX; name++)
	{
		if(id_character(*name)) id[count++] = (uint8_t)*name;
	}
	if(count == 0 || CONNECT_HEAD + count > size) return 0;

	out[0] = FIRST_CONNECT;
	out[1] = (uint8_t)(CONNECT_HEAD - 2 + count);
	out[2] = 0;
	out[3] = 4;
	out[4] = 'M';
	out[5] = 'Q';
	out[6] = 'T';
	out[7] = 'T';
	out[8] = 4;
	out[9] = CLEAN_SESSION;
	out[10] = (uint8_t)(keepalive_s >> 8);
	out[11] = (uint8_t)(keepalive_s & 0xFFu);
	out[12] = 0;
	out[13] = (uint8_t)count;
	memcpy(out + CONNECT_HEAD, id, count);
	return CONNECT_HEAD + count;
}

size_t mqtt_sub_subscribe(uint8_t *out, size_t size, uint16_t id, const char *topic)
{
	size_t length = 0;

	if(out == NULL || topic == NULL || id == 0) return 0;

	for(; topic[length] != '\0'; length++)
	{
		if(length >= MQTT_SUB_OUT_MAX - SUBSCRIBE_AROUND) return 0;
		// A pattern would bring topics nobody asked for
		if(topic[length] == '#' || topic[length] == '+') return 0;
	}
	if(length == 0 || SUBSCRIBE_AROUND + length > size) return 0;

	out[0] = FIRST_SUBSCRIBE;
	out[1] = (uint8_t)(SUBSCRIBE_AROUND - 2 + length);
	out[2] = (uint8_t)(id >> 8);
	out[3] = (uint8_t)(id & 0xFFu);
	out[4] = 0;
	out[5] = (uint8_t)length;
	memcpy(out + 6, topic, length);
	// The requested QoS: the display can acknowledge nothing
	out[6 + length] = 0;
	return SUBSCRIBE_AROUND + length;
}

size_t mqtt_sub_ping(uint8_t *out, size_t size)
{
	if(out == NULL || size < 2) return 0;

	out[0] = FIRST_PINGREQ;
	out[1] = 0;
	return 2;
}

void mqtt_sub_begin(mqtt_sub_reader_t *reader)
{
	// Everything else is set with the first byte of a packet
	reader->state = READ_FIRST;
}

static mqtt_sub_kind_t broken(mqtt_sub_reader_t *reader)
{
	reader->state = READ_BROKEN;
	return MQTT_SUB_BROKEN;
}

// A packet a subscriber of QoS 0 gets begins with one of these
static bool first_known(uint8_t first)
{
	if(first == FIRST_CONNACK || first == FIRST_SUBACK || first == FIRST_PINGRESP) return true;
	return (first & ~PUBLISH_RETAIN) == FIRST_PUBLISH;
}

// What the Remaining Length of a packet that began with `first` may be
static bool length_fits(uint8_t first, uint32_t remaining)
{
	if(first == FIRST_CONNACK) return remaining == 2;
	if(first == FIRST_SUBACK) return remaining == 3;
	if(first == FIRST_PINGRESP) return remaining == 0;
	// The two bytes of the length of the topic, and a topic of one byte
	return remaining >= 3;
}

// A packet is handed out: what its kind does not name is empty
static mqtt_sub_kind_t hand_out(mqtt_sub_reader_t *reader, mqtt_sub_packet_t *packet, mqtt_sub_kind_t kind)
{
	reader->state = READ_FIRST;
	packet->code = 0;
	packet->id = 0;
	packet->retained = false;
	packet->topic = NULL;
	packet->topic_length = 0;
	packet->payload = NULL;
	packet->payload_length = 0;
	return kind;
}

// Every byte behind the fixed header is in the room
static mqtt_sub_kind_t complete(mqtt_sub_reader_t *reader, mqtt_sub_packet_t *packet)
{
	const uint8_t *room = reader->room;
	uint32_t topic_length;

	if(reader->first == FIRST_PINGRESP) return hand_out(reader, packet, MQTT_SUB_PINGRESP);
	if(reader->first == FIRST_CONNACK)
	{
		// A session the broker kept, or a reserved bit: no answer to the CONNECT that is built here
		if(room[0] != 0) return broken(reader);
		hand_out(reader, packet, MQTT_SUB_CONNACK);
		packet->code = room[1];
		return MQTT_SUB_CONNACK;
	}
	if(reader->first == FIRST_SUBACK)
	{
		hand_out(reader, packet, MQTT_SUB_SUBACK);
		packet->id = (uint16_t)(((uint32_t)room[0] << 8) | room[1]);
		packet->code = room[2];
		return MQTT_SUB_SUBACK;
	}

	topic_length = ((uint32_t)room[0] << 8) | room[1];
	if(topic_length == 0 || topic_length > reader->remaining - 2) return broken(reader);

	hand_out(reader, packet, MQTT_SUB_PUBLISH);
	packet->retained = (reader->first & PUBLISH_RETAIN) != 0;
	packet->topic = room + 2;
	packet->topic_length = topic_length;
	packet->payload = room + 2 + topic_length;
	packet->payload_length = reader->remaining - 2 - topic_length;
	return MQTT_SUB_PUBLISH;
}

mqtt_sub_kind_t mqtt_sub_read(mqtt_sub_reader_t *reader, const uint8_t **data, size_t *length, mqtt_sub_packet_t *packet)
{
	if(reader->state == READ_BROKEN) return MQTT_SUB_BROKEN;

	while(*length > 0)
	{
		uint8_t byte;

		if(reader->state == READ_BODY || reader->state == READ_SKIP)
		{
			uint32_t missing = reader->remaining - reader->have;
			size_t take = *length < missing ? *length : missing;

			if(reader->state == READ_BODY) memcpy(reader->room + reader->have, *data, take);
			reader->have += (uint32_t)take;
			*data += take;
			*length -= take;
			if(reader->have < reader->remaining) continue;
			if(reader->state == READ_SKIP) return hand_out(reader, packet, MQTT_SUB_SKIPPED);
			return complete(reader, packet);
		}

		byte = **data;
		*data += 1;
		*length -= 1;
		if(reader->state == READ_FIRST)
		{
			if(!first_known(byte)) return broken(reader);
			reader->first = byte;
			reader->scale = 1;
			reader->remaining = 0;
			reader->have = 0;
			reader->state = READ_LENGTH;
			continue;
		}

		reader->remaining += (uint32_t)(byte & 0x7Fu) * reader->scale;
		if((byte & 0x80u) != 0)
		{
			// Another byte follows, and a length has at most four
			if(reader->scale == LENGTH_LAST_SCALE) return broken(reader);
			reader->scale *= 128u;
			continue;
		}
		if(!length_fits(reader->first, reader->remaining)) return broken(reader);
		if(reader->remaining == 0) return complete(reader, packet);
		// Only a PUBLISH can be longer than the room: every other length was refused above
		reader->state = reader->remaining > MQTT_SUB_PACKET_MAX ? READ_SKIP : READ_BODY;
	}
	return MQTT_SUB_NONE;
}
