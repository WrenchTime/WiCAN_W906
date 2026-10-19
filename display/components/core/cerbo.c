/*
 * Standalone display for the WiCAN W906 fork.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <math.h>
#include <string.h>
#include "cerbo.h"

static const uint32_t RETRY_WAITS[] = CERBO_RETRY_MS;

#define RETRY_WAIT_COUNT    ((int)(sizeof(RETRY_WAITS) / sizeof(RETRY_WAITS[0])))
#define PREFIX_LENGTH       (sizeof(CERBO_PREFIX) - 1)
#define TOPIC_LENGTH        (sizeof(CERBO_TOPIC) - 1)

// Whoever makes room among the names counts on it: one bundle never brings more keys than there are names
_Static_assert(CERBO_NAMES_MAX >= VALUES_MAX, "every key of the store has room among the names");

static bool has_text(const value_t *item);

// The calls come from more than one task, each with its own reading of the time: one that lies before the
// latest seen lets no time pass.
static void advance(cerbo_t *cerbo, uint64_t now_ms)
{
	if(now_ms > cerbo->clock_ms) cerbo->clock_ms = now_ms;
}

// The time a question is asked for: every time kept here lies at or before it
static uint64_t at(const cerbo_t *cerbo, uint64_t now_ms)
{
	return now_ms > cerbo->clock_ms ? now_ms : cerbo->clock_ms;
}

// The address `text` names, empty if it names none: NULL, or without an end within CERBO_HOST_SIZE bytes
static void read_host(char *host, const char *text)
{
	size_t length = 0;

	host[0] = '\0';
	if(text == NULL) return;
	while(length < CERBO_HOST_SIZE && text[length] != '\0') length++;
	if(length == CERBO_HOST_SIZE) return;

	memcpy(host, text, length + 1);
}

// An attempt is due at once, the waits start from the beginning, and so does the time of "waiting"
static void at_once(cerbo_t *cerbo)
{
	cerbo->wait_step = 0;
	cerbo->wait_ms = 0;
	cerbo->grace_since_ms = cerbo->clock_ms;
}

// The connection is gone, by a report of the caller or by an order to close it
static void ended(cerbo_t *cerbo)
{
	// "Waiting" begins anew behind a session, not behind an attempt that made none
	if(cerbo->phase == CERBO_SESSION) cerbo->grace_since_ms = cerbo->clock_ms;
	cerbo->phase = CERBO_IDLE;
	cerbo->send = false;
	cerbo->failed = false;
	if(cerbo->moved)
	{
		// The new address is tried at once, as cerbo_set_host() said
		cerbo->moved = false;
		return;
	}
	cerbo->wait_since_ms = cerbo->clock_ms;
	cerbo->wait_ms = RETRY_WAITS[cerbo->wait_step];
	if(cerbo->wait_step < RETRY_WAIT_COUNT - 1) cerbo->wait_step++;
}

void cerbo_init(cerbo_t *cerbo, const char *host, bool safe_mode, const char *name, uint64_t now_ms)
{
	uint8_t probe[MQTT_SUB_OUT_MAX];
	size_t length = 0;

	memset(cerbo, 0, sizeof(*cerbo));
	cerbo->clock_ms = now_ms;
	cerbo->safe_mode = safe_mode;
	read_host(cerbo->host, host);
	if(name != NULL)
	{
		while(length < CERBO_NAME_SIZE - 1 && name[length] != '\0') length++;
		memcpy(cerbo->name, name, length);
	}
	// The source must not stay without a connection for a name mqtt_sub.h makes no client id of
	if(mqtt_sub_connect(probe, sizeof(probe), cerbo->name, CERBO_KEEPALIVE_S) == 0) strcpy(cerbo->name, CERBO_CLIENT);
}

bool cerbo_on(const cerbo_t *cerbo)
{
	return cerbo->host[0] != '\0' && !cerbo->safe_mode;
}

void cerbo_set_host(cerbo_t *cerbo, const char *host, uint64_t now_ms)
{
	char taken[CERBO_HOST_SIZE];

	advance(cerbo, now_ms);
	read_host(taken, host);
	if(strcmp(taken, cerbo->host) == 0) return;

	strcpy(cerbo->host, taken);
	// What another broker sent says nothing about this one
	values_clear(&cerbo->values);
	cerbo->has_bundle = false;
	cerbo->newer = false;
	cerbo->has_boot = false;
	cerbo->lag_ms = 0;
	// ... and its keys are not the keys of this one
	cerbo->name_count = 0;
	if(cerbo->phase != CERBO_IDLE) cerbo->moved = true;
	at_once(cerbo);
}

void cerbo_network(cerbo_t *cerbo, bool up, uint64_t now_ms)
{
	advance(cerbo, now_ms);
	if(up == cerbo->network) return;

	cerbo->network = up;
	if(up) at_once(cerbo);
}

cerbo_do_t cerbo_next(cerbo_t *cerbo, uint64_t now_ms)
{
	advance(cerbo, now_ms);

	if(cerbo->phase == CERBO_IDLE)
	{
		if(!cerbo_on(cerbo) || !cerbo->network) return CERBO_DO_NOTHING;
		if(cerbo->clock_ms - cerbo->wait_since_ms < cerbo->wait_ms) return CERBO_DO_NOTHING;

		cerbo->phase = CERBO_OPENING;
		cerbo->open_ms = cerbo->clock_ms;
		return CERBO_DO_OPEN;
	}

	// A source that was switched off has another address, none: `moved` tells of it as well
	if(!cerbo->network || cerbo->moved || cerbo->failed)
	{
		ended(cerbo);
		return CERBO_DO_CLOSE;
	}
	if(cerbo->phase != CERBO_SESSION)
	{
		if(cerbo->clock_ms - cerbo->open_ms >= CERBO_SESSION_MS)
		{
			ended(cerbo);
			return CERBO_DO_CLOSE;
		}
		if(!cerbo->send) return CERBO_DO_NOTHING;

		cerbo->send = false;
		return CERBO_DO_SEND;
	}

	if(cerbo->pings_open > 0 && cerbo->clock_ms - cerbo->ping_open_ms >= CERBO_PONG_MS)
	{
		ended(cerbo);
		return CERBO_DO_CLOSE;
	}
	if(cerbo->clock_ms - cerbo->ping_ms < CERBO_PING_MS) return CERBO_DO_NOTHING;

	cerbo->out_length = mqtt_sub_ping(cerbo->out, sizeof(cerbo->out));
	if(cerbo->pings_open == 0) cerbo->ping_open_ms = cerbo->clock_ms;
	cerbo->pings_open++;
	cerbo->ping_ms = cerbo->clock_ms;
	return CERBO_DO_SEND;
}

const char *cerbo_host(const cerbo_t *cerbo)
{
	return cerbo->host;
}

const uint8_t *cerbo_out(const cerbo_t *cerbo, size_t *length)
{
	*length = cerbo->out_length;
	return cerbo->out;
}

void cerbo_opened(cerbo_t *cerbo, uint64_t now_ms)
{
	advance(cerbo, now_ms);
	if(cerbo->phase != CERBO_OPENING) return;

	cerbo->phase = CERBO_GREETING;
	mqtt_sub_begin(&cerbo->reader);
	// The first bundle of a connection has no lag: the broker kept nothing for a clean session
	cerbo->has_clock = false;
	cerbo->out_length = mqtt_sub_connect(cerbo->out, sizeof(cerbo->out), cerbo->name, CERBO_KEEPALIVE_S);
	cerbo->send = true;
}

void cerbo_down(cerbo_t *cerbo, uint64_t now_ms)
{
	advance(cerbo, now_ms);
	if(cerbo->phase != CERBO_IDLE) ended(cerbo);
}

static void connack(cerbo_t *cerbo, const mqtt_sub_packet_t *packet)
{
	// `send` while greeting: the CONNECT this would answer was not handed out yet
	if(cerbo->phase != CERBO_GREETING || cerbo->send || packet->code != 0)
	{
		cerbo->failed = true;
		return;
	}
	cerbo->phase = CERBO_SUBSCRIBING;
	cerbo->out_length = mqtt_sub_subscribe(cerbo->out, sizeof(cerbo->out), CERBO_PACKET_ID, CERBO_TOPIC);
	cerbo->send = true;
}

static void suback(cerbo_t *cerbo, const mqtt_sub_packet_t *packet)
{
	if(cerbo->phase != CERBO_SUBSCRIBING || cerbo->send || packet->id != CERBO_PACKET_ID || packet->code != 0)
	{
		cerbo->failed = true;
		return;
	}
	cerbo->phase = CERBO_SESSION;
	cerbo->sessions++;
	cerbo->wait_step = 0;
	cerbo->ping_ms = cerbo->clock_ms;
	cerbo->pings_open = 0;
	cerbo->quiet_since_ms = cerbo->clock_ms;
}

static void pingresp(cerbo_t *cerbo)
{
	if(cerbo->phase != CERBO_SESSION || cerbo->pings_open == 0)
	{
		cerbo->failed = true;
		return;
	}
	cerbo->pings_open--;
	// The one that is still open is the last one handed out: cerbo_next() lets no third one wait
	if(cerbo->pings_open > 0) cerbo->ping_open_ms = cerbo->ping_ms;
	cerbo->pong_ms = cerbo->clock_ms;
}

// The lag of the bundle with this boot and t that arrives now, by the rule of cerbo.h
static void reckon(cerbo_t *cerbo, uint32_t boot, int64_t t_ms)
{
	uint64_t lag = 0;

	if(!cerbo->has_boot || cerbo->boot != boot)
	{
		// Another run of the flow: its clock began anew
		cerbo->boots++;
		cerbo->has_clock = false;
	}
	if(cerbo->has_clock)
	{
		uint64_t elapsed = cerbo->clock_ms - cerbo->bundle_ms;
		// In two parts, so that no time of the display is too large to be multiplied
		uint64_t part = (elapsed % 1000) * CERBO_YIELD_PER_MILLE + cerbo->yield_rest;
		uint64_t yielded = (elapsed / 1000) * CERBO_YIELD_PER_MILLE + part / 1000;
		// Neither sum can wrap: the yield takes a tenth of `elapsed`, a lag is at most CERBO_LAG_MAX_MS, and
		// t steps by less than 2^53
		uint64_t behind = cerbo->lag_ms + (elapsed - yielded);
		int64_t step = t_ms - cerbo->t_ms;

		cerbo->yield_rest = (uint32_t)(part % 1000);
		if(step < 0) lag = behind + (uint64_t)(-step);
		else if(behind > (uint64_t)step) lag = behind - (uint64_t)step;
	}
	if(lag > CERBO_LAG_MAX_MS)
	{
		cerbo->distrusted++;
		cerbo->has_clock = false;
		lag = 0;
	}
	// A new base: the yield begins without a rest
	if(!cerbo->has_clock) cerbo->yield_rest = 0;
	cerbo->has_boot = true;
	cerbo->boot = boot;
	cerbo->has_clock = true;
	cerbo->t_ms = t_ms;
	cerbo->lag_ms = (uint32_t)lag;
}

// CERBO_PREFIX, then 1 to 29 of A-Z, 0-9 and _; the length was limited by the room the key was read into
static bool key_fits(const char *key)
{
	const char *c = key + PREFIX_LENGTH;

	if(!cerbo_key(key) || *c == '\0') return false;
	for(; *c != '\0'; c++)
	{
		if(!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) return false;
	}
	return true;
}

static value_t *find(values_t *values, const char *name)
{
	value_t *item = values->items;

	for(int i = 0; i < values->count; i++, item++)
	{
		if(strcmp(item->name, name) == 0) return item;
	}
	return NULL;
}

// Empties the store and fills it from the object at `object`. values_apply() cannot do it: it takes every
// key and keeps what an answer does not name.
static void fill(cerbo_t *cerbo, const char *json, const json_token_t *work, int object, uint64_t seen_ms)
{
	values_t *values = &cerbo->values;
	int key = object + 1;

	values_clear(values);
	for(int i = 0; i < work[object].size; i++)
	{
		const json_token_t *name_token = &work[key];
		const json_token_t *member = &work[key + 1];
		char name[VALUE_NAME_SIZE];
		value_kind_t kind;
		double number = 0;
		value_t *item;

		key += 1 + member->skip;

		if(!json_text(json, name_token, name, sizeof(name)) || !key_fits(name))
		{
			cerbo->skipped++;
			continue;
		}
		if(json_number(json, member, &number)) kind = VALUE_NUMBER;
		else if(json_text_is(json, member, "on")) kind = VALUE_ON;
		else if(json_text_is(json, member, "off")) kind = VALUE_OFF;
		else
		{
			cerbo->skipped++;
			continue;
		}

		item = find(values, name);
		if(item == NULL)
		{
			if(values->count >= VALUES_MAX)
			{
				cerbo->skipped++;
				continue;
			}
			item = values->items + values->count++;
			strcpy(item->name, name);
		}
		item->kind = kind;
		item->number = number;
		item->seen_ms = seen_ms;
	}
}

// The keys of the store for the editor (cerbo.h, "The names"): each that has a text and is not among the names
// yet comes behind them. Called with the store a bundle has just filled.
static void note_names(cerbo_t *cerbo)
{
	for(int i = 0; i < cerbo->values.count; i++)
	{
		const value_t *item = &cerbo->values.items[i];
		int tried = 0;

		// A number without end stands in no answer, and neither does its key
		if(!has_text(item)) continue;
		// Among all the names, beginning with the one at the place the key has in the store: a flow sends its
		// keys in the same order every second, and then that one is it
		while(tried < cerbo->name_count && strcmp(cerbo->names[(i + tried) % cerbo->name_count], item->name) != 0) tried++;
		if(tried < cerbo->name_count) continue;

		if(cerbo->name_count == CERBO_NAMES_MAX)
		{
			// No room: the oldest name this bundle did not bring gives way, and the names behind it move up. There
			// is one - the store holds no more keys than there are names, and this key of it is none of them -, so
			// the search never reaches the last name without having to take it.
			char (*old)[VALUE_NAME_SIZE] = cerbo->names;
			char (*last)[VALUE_NAME_SIZE] = cerbo->names + CERBO_NAMES_MAX - 1;

			while(old < last && find(&cerbo->values, *old) != NULL) old++;
			memmove(old, old + 1, (size_t)(last - old) * sizeof(*old));
			cerbo->name_count--;
		}
		strcpy(cerbo->names[cerbo->name_count], item->name);
		cerbo->name_count++;
	}
}

// The member `name` of the bundle as a whole number from `least` to `most`. false if there is none, if it is
// anything else, or if it lies outside.
static bool whole(const char *json, const json_token_t *work, const char *name, int64_t least, int64_t most, int64_t *number)
{
	int member = json_member(json, work, 0, name);

	return member >= 0 && json_integer(json, &work[member], number) && *number >= least && *number <= most;
}

// A PUBLISH on some topic: a bundle if it passes the rules of cerbo.h
static void bundle(cerbo_t *cerbo, const mqtt_sub_packet_t *packet, json_token_t *work, int work_count)
{
	const char *json = (const char *)packet->payload;
	int64_t version = 0, boot = 0, t_ms = 0;
	int values;

	if(packet->topic_length != TOPIC_LENGTH || memcmp(packet->topic, CERBO_TOPIC, TOPIC_LENGTH) != 0)
	{
		cerbo->refused_topic++;
		return;
	}
	if(packet->retained)
	{
		cerbo->refused_retained++;
		return;
	}
	if(packet->payload_length > CERBO_BUNDLE_MAX)
	{
		cerbo->refused_long++;
		return;
	}

	// What could not be read has no tokens to look at, and what is no object has no member
	if(json_parse(json, packet->payload_length, work, work_count) < 0 || !whole(json, work, "v", 1, INT64_MAX, &version))
	{
		cerbo->refused_invalid++;
		return;
	}
	if(version > CERBO_VERSION)
	{
		cerbo->refused_version++;
		cerbo->newer = true;
		return;
	}
	values = json_member(json, work, 0, "values");
	if(!whole(json, work, "boot", 1, CERBO_BOOT_MAX, &boot) || !whole(json, work, "t", 0, CERBO_T_MAX, &t_ms) || values < 0 || work[values].type != JSON_OBJECT)
	{
		cerbo->refused_invalid++;
		return;
	}

	reckon(cerbo, (uint32_t)boot, t_ms);
	fill(cerbo, json, work, values, cerbo->clock_ms > cerbo->lag_ms ? cerbo->clock_ms - cerbo->lag_ms : 0);
	note_names(cerbo);
	cerbo->newer = false;
	cerbo->has_bundle = true;
	cerbo->bundle_ms = cerbo->clock_ms;
	cerbo->quiet_since_ms = cerbo->clock_ms;
	cerbo->bundles++;
}

void cerbo_bytes(cerbo_t *cerbo, const uint8_t *data, size_t length, uint64_t now_ms, json_token_t *work, int work_count)
{
	mqtt_sub_packet_t packet;

	advance(cerbo, now_ms);
	// Without a connection that stands there are no bytes to read; and what an address that is not set any
	// more sends, or a connection that is to be closed, is not read
	if(cerbo->phase == CERBO_IDLE || cerbo->phase == CERBO_OPENING || cerbo->moved || cerbo->failed) return;

	for(;;)
	{
		mqtt_sub_kind_t kind = mqtt_sub_read(&cerbo->reader, &data, &length, &packet);

		if(kind == MQTT_SUB_NONE) return;
		if(kind == MQTT_SUB_BROKEN)
		{
			cerbo->failed = true;
			return;
		}
		if(kind == MQTT_SUB_CONNACK) connack(cerbo, &packet);
		else if(kind == MQTT_SUB_SUBACK) suback(cerbo, &packet);
		else if(kind == MQTT_SUB_PINGRESP) pingresp(cerbo);
		// A message before the display subscribed is none it asked for
		else if(cerbo->phase == CERBO_GREETING || cerbo->send) cerbo->failed = true;
		else if(kind == MQTT_SUB_SKIPPED) cerbo->refused_long++;
		else bundle(cerbo, &packet, work, work_count);

		// What follows a packet that gets the connection closed is not read
		if(cerbo->failed) return;
	}
}

cerbo_view_t cerbo_view(const cerbo_t *cerbo, uint64_t now_ms)
{
	uint64_t now = at(cerbo, now_ms);

	if(!cerbo_on(cerbo)) return CERBO_VIEW_OFF;
	if(!cerbo->network) return CERBO_VIEW_NO_WIFI;
	if(cerbo->phase != CERBO_SESSION)
	{
		return now - cerbo->grace_since_ms < CERBO_GRACE_MS ? CERBO_VIEW_CONNECTING : CERBO_VIEW_UNREACHABLE;
	}
	if(cerbo->newer) return CERBO_VIEW_NEWER;
	if(cerbo->has_bundle && now - cerbo->bundle_ms < CERBO_LIVE_MS) return CERBO_VIEW_LIVE;
	// A PINGRESP from before the last bundle, or of a session before, says nothing about the silence of this one
	if(cerbo->pong_ms >= cerbo->quiet_since_ms && cerbo->pong_ms - cerbo->quiet_since_ms >= CERBO_SILENT_MS) return CERBO_VIEW_SILENT;
	return CERBO_VIEW_QUIET;
}

bool cerbo_showing(const cerbo_t *cerbo, uint64_t now_ms)
{
	const value_t *item = cerbo->values.items;

	if(cerbo_view(cerbo, now_ms) == CERBO_VIEW_LIVE) return true;
	for(int i = 0; i < cerbo->values.count; i++, item++)
	{
		if(cerbo_age(item, now_ms) != VALUE_AGE_GONE) return true;
	}
	return false;
}

value_age_t cerbo_age(const value_t *value, uint64_t now_ms)
{
	uint64_t age;

	if(value == NULL) return VALUE_AGE_GONE;

	// A clock that stepped back must not turn a value into an old one
	age = now_ms > value->seen_ms ? now_ms - value->seen_ms : 0;
	if(age < CERBO_FRESH_MS) return VALUE_AGE_FRESH;
	if(age < CERBO_KEPT_MS) return VALUE_AGE_OLD;
	return VALUE_AGE_GONE;
}

bool cerbo_key(const char *key)
{
	return key != NULL && strncmp(key, CERBO_PREFIX, PREFIX_LENGTH) == 0;
}

uint32_t cerbo_refused(const cerbo_t *cerbo)
{
	// In 64 bit: five counters of 32 bit do not add up in one
	uint64_t sum = (uint64_t)cerbo->refused_invalid + cerbo->refused_version + cerbo->refused_retained + cerbo->refused_topic + cerbo->refused_long;

	return sum > UINT32_MAX ? UINT32_MAX : (uint32_t)sum;
}

// Whether GET /api/values has a text for a value of the store (web_json.h): a number that is finite, "on" or
// "off". A number beyond the range of a double is taken into the store as infinite, as the store of the
// adapter takes one (values.h), and has none.
static bool has_text(const value_t *item)
{
	if(item->kind == VALUE_NUMBER) return isfinite(item->number);
	return item->kind == VALUE_ON || item->kind == VALUE_OFF;
}

int cerbo_shown(const cerbo_t *cerbo, uint64_t now_ms)
{
	int shown = 0;

	for(int i = 0; i < cerbo->values.count; i++)
	{
		// The number of values GET /api/values names: that answer leaves out what has no text
		if(!has_text(&cerbo->values.items[i])) continue;
		if(cerbo_age(&cerbo->values.items[i], now_ms) != VALUE_AGE_GONE) shown++;
	}
	return shown;
}
