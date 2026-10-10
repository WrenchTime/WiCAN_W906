/*
 * Standalone display for the WiCAN W906 fork.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef __MQTT_SUB_H__
#define __MQTT_SUB_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * MQTT 3.1.1 for one who only subscribes: three packets are built here, CONNECT, SUBSCRIBE and PINGREQ,
 * and a stream of bytes is read into the four packets a broker sends to a subscriber of QoS 0 - CONNACK,
 * SUBACK, PINGRESP and PUBLISH. Pure functions and a reader with a state; no time, and no decision about
 * the connection (cerbo.h).
 *
 * There is no function here that builds a PUBLISH, an UNSUBSCRIBE or a DISCONNECT, and the CONNECT that
 * is built carries no will: on the broker of a Cerbo GX a text published below W/ switches relays. What
 * cannot be built cannot be sent. Every packet built here begins with 0x10, 0x82 or 0xC0.
 *
 * The sections and the numbers in brackets name "MQTT Version 3.1.1", OASIS Standard of 29 October 2014
 * (docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html), read on 2026-10-10.
 */

#define MQTT_SUB_PACKET_MAX     1536    // bytes of one packet the reader keeps: what follows the fixed header
#define MQTT_SUB_OUT_MAX        64      // room for every packet that is built here
#define MQTT_SUB_ID_MAX         23      // bytes of a client id every broker has to take (3.1.3.1, [MQTT-3.1.3-5])

/*
 * The builders write one packet to `out` and return its length. They return 0 and write nothing if there
 * is no packet: `out` is NULL, the packet has more than `size` bytes, or the reason named with each. No
 * packet has more than MQTT_SUB_OUT_MAX bytes, so its Remaining Length is one byte (2.2.3: up to 127).
 */

// CONNECT (3.1) for MQTT 3.1.1: the protocol name "MQTT" (3.1.2.1) and the level 4 (3.1.2.2); of the
// connect flags (3.1.2.3) Clean Session alone, so the broker keeps nothing of an earlier session and
// nothing for a later one (3.1.2.4, [MQTT-3.1.2-6]) - no will (3.1.2.5), no user name and no password
// (3.1.2.8, 3.1.2.9), and so the payload is the client id and nothing else (3.1.3, [MQTT-3.1.2-11],
// [MQTT-3.1.2-18], [MQTT-3.1.2-20]). keepalive_s as it is, the high byte first (3.1.2.10): the broker
// closes after one and a half times that long without a packet from here ([MQTT-3.1.2-24]).
// The client id (3.1.3.1) is made of the letters a-z, A-Z and the digits of `name`, in their order, at
// most MQTT_SUB_ID_MAX of them: what every broker has to take ([MQTT-3.1.3-5]). "WiCAN-Display-C9A8"
// becomes WiCANDisplayC9A8. A name that is NULL or has no such character gives no packet. A broker ends
// the connection of another client with the same id ([MQTT-3.1.4-2]): the name has to tell two displays
// apart.
size_t mqtt_sub_connect(uint8_t *out, size_t size, const char *name, uint16_t keepalive_s);

// SUBSCRIBE (3.8) with the packet identifier `id` (3.8.2) and one topic filter with the requested QoS 0
// (3.8.3). The first byte is 0x82 ([MQTT-3.8.1-1]). No packet if `id` is 0 ([MQTT-2.3.1-1]), if `topic`
// is NULL or empty ([MQTT-4.7.3-1]), if it contains '#' or '+' (4.7.1: the display subscribes to topics,
// never to a pattern), or if it has more than MQTT_SUB_OUT_MAX - 7 bytes.
size_t mqtt_sub_subscribe(uint8_t *out, size_t size, uint16_t id, const char *topic);

// PINGREQ (3.12): the two bytes 0xC0 0x00. The broker answers with a PINGRESP ([MQTT-3.12.4-1]).
size_t mqtt_sub_ping(uint8_t *out, size_t size);

typedef enum
{
	MQTT_SUB_NONE,          // the bytes ran out before a packet was complete
	MQTT_SUB_CONNACK,
	MQTT_SUB_SUBACK,
	MQTT_SUB_PINGRESP,
	MQTT_SUB_PUBLISH,
	MQTT_SUB_SKIPPED,       // a PUBLISH too long for the room has gone by
	MQTT_SUB_BROKEN,        // not what a broker sends to a subscriber of QoS 0: the caller closes
} mqtt_sub_kind_t;

// What a packet carries. A member its kind does not name is 0, false or NULL.
typedef struct
{
	uint8_t code;           // CONNACK: the return code, 0 = accepted (3.2.2.3). SUBACK: the return code,
	                        // 0 = subscribed with QoS 0, 0x80 = refused (3.9.3)
	uint16_t id;            // SUBACK: the packet identifier of the SUBSCRIBE it answers (3.9.2, [MQTT-3.8.4-2])
	bool retained;          // PUBLISH: the RETAIN flag (3.3.1.3): the message was kept by the broker and is as
	                        // old as nobody knows ([MQTT-3.3.1-8], [MQTT-3.3.1-9])
	const uint8_t *topic;   // PUBLISH: the topic name (3.3.2.1), its bytes as they came - any bytes, without a
	size_t topic_length;    // zero behind them
	const uint8_t *payload; // PUBLISH: what follows the topic (3.3.3); NULL only for another kind
	size_t payload_length;  // may be 0
} mqtt_sub_packet_t;

// The state of the reader between two calls. Nothing in it is for the caller.
typedef struct
{
	uint8_t state;
	uint8_t first;          // the first byte of the packet under way
	uint32_t scale;         // what the next byte of its Remaining Length counts
	uint32_t remaining;     // its Remaining Length
	uint32_t have;          // bytes behind its fixed header that arrived
	uint8_t room[MQTT_SUB_PACKET_MAX];
} mqtt_sub_reader_t;

// The reader begins anew, with the first byte of a packet: for every new connection
void mqtt_sub_begin(mqtt_sub_reader_t *reader);

/*
 * Takes bytes from *data (*length of them) until a packet is complete, and returns its kind; *data and
 * *length then name the bytes that are left, for the next call. MQTT_SUB_NONE when the bytes ran out
 * (*length is 0 then). The state stays between the calls: a packet may arrive cut at any byte, and in any
 * number of pieces. *packet is written when a packet is handed out, and only then; topic and payload point
 * into the room of the reader and hold until the next call with it.
 *
 * - The first byte (2.2.1, 2.2.2) is 0x20 CONNACK, 0x90 SUBACK, 0xD0 PINGRESP, or 0x30 or 0x31 PUBLISH with
 *   QoS 0, the RETAIN flag in bit 0. Any other byte is BROKEN at once: a packet a subscriber does not get
 *   (CONNECT, SUBSCRIBE, UNSUBSCRIBE, PINGREQ, DISCONNECT; PUBACK, PUBREC, PUBREL, PUBCOMP and UNSUBACK
 *   answer what is never sent from here; 0 and 15 are forbidden), flags that have to be 0
 *   ([MQTT-2.2.2-1], [MQTT-2.2.2-2]), a PUBLISH with QoS 1 or 2 - the subscription asked for 0
 *   ([MQTT-3.8.4-6]), and nothing here can acknowledge one - or with both QoS bits ([MQTT-3.3.1-4]) or with
 *   DUP ([MQTT-3.3.1-2]).
 * - The Remaining Length (2.2.3) has one to four bytes, seven bits each, the lowest first, bit 7 saying
 *   that another byte follows. A fourth byte that announces a fifth is BROKEN. The number counts, also when
 *   it is written with more bytes than it needs. (Table 2.4 has lengths of four bytes up to 268435455; the
 *   decoding algorithm printed below it, taken by the letter, raises its error with every fourth byte. The
 *   table is followed here.)
 * - The length is judged with its last byte: 2 for a CONNACK (3.2.1), 3 for a SUBACK - the packet
 *   identifier and one return code for the one topic ([MQTT-3.8.4-5]) -, 0 for a PINGRESP (3.13.1), and at
 *   least 3 for a PUBLISH: the two bytes of the length of the topic and a topic of one byte
 *   ([MQTT-4.7.3-1]). Anything else is BROKEN.
 * - A PUBLISH whose Remaining Length is more than MQTT_SUB_PACKET_MAX: its bytes are counted, not kept and
 *   not looked at; with the last of them MQTT_SUB_SKIPPED. A pause in the middle changes nothing - the
 *   reader knows how many bytes are missing - and the packet behind it is read as any other.
 * - What is inside a packet is judged when it is complete:
 *   CONNACK   the acknowledge flags (3.2.2.1) have to be 0, else BROKEN: bits 7 to 1 are reserved, and bit
 *             0, Session Present, is 0 for a clean session ([MQTT-3.2.2-1]) and with a refusal
 *             ([MQTT-3.2.2-4]). The return code (3.2.2.3) is handed out as it is.
 *   SUBACK    the packet identifier, the high byte first, and the return code are handed out as they are.
 *   PUBLISH   the length of the topic (1.5.3: two bytes, the high one first) is at least 1 and reaches no
 *             further than the packet, else BROKEN. A PUBLISH of QoS 0 has no packet identifier (3.3.2.2,
 *             [MQTT-2.3.1-5]): everything behind the topic is payload. The bytes of the topic are not
 *             checked ([MQTT-1.5.3-1], [MQTT-3.3.2-2]): the caller compares them with the one it expects.
 * - After BROKEN the reader takes nothing more: every further call returns MQTT_SUB_BROKEN and leaves *data
 *   and *length as they are, until mqtt_sub_begin().
 */
mqtt_sub_kind_t mqtt_sub_read(mqtt_sub_reader_t *reader, const uint8_t **data, size_t *length, mqtt_sub_packet_t *packet);

#endif
