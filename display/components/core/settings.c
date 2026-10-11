/*
 * Standalone display for the WiCAN W906 fork.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <string.h>
#include "settings.h"

// The members every text has, in the order of the stored text
enum
{
	BRIGHTNESS,
	NIGHT,
	NIGHT_MODE,
	REVERSE,
	STANDBY_S,
	WEB_OPEN,
	MEMBERS,
};

static const char *const names[MEMBERS] = {"brightness", "night", "night_mode", "reverse", "standby_s", "web_open"};

// One member is none of them: it is written only for a display that has a second source, behind standby_s
// and before web_open, which stays the last member of every text
#define CERBO_NAME  "cerbo"

typedef struct
{
	char *out;
	size_t size;
	size_t length;      // of the whole text, also when it does not fit
} writer_t;

void settings_defaults(settings_t *settings)
{
	settings->brightness = 80;
	settings->night = 25;
	settings->night_mode = false;
	settings->reverse = false;
	settings->standby_s = 60;
	// The whole field: the settings are compared and stored as bytes
	memset(settings->cerbo, 0, sizeof(settings->cerbo));
	// Open by default, as the owner asked for on 2026-10-10: a stored text from before has no such member
	settings->web_open = true;
}

bool settings_is_address(const char *text)
{
	int numbers = 0;

	if(text == NULL) return false;

	for(;;)
	{
		int digits = 0;
		int number = 0;

		// Three digits at most. So no more than SETTINGS_CERBO_SIZE bytes of a text are ever read, whatever it
		// holds: a field without a zero in it is read to its end and is no address.
		while(digits < 3 && *text >= '0' && *text <= '9')
		{
			number = number * 10 + (*text - '0');
			text++;
			digits++;
		}
		if(digits == 0 || number > 255) return false;
		// "01" is no number of an address: some readers take a leading zero for octal
		if(digits > 1 && text[-digits] == '0') return false;

		numbers++;
		if(numbers == 4) return *text == '\0';
		if(*text != '.') return false;
		text++;
	}
}

// Index into names, MEMBERS for a member the format does not know
static int known_member(const char *json, const json_token_t *name)
{
	int member = 0;

	while(member < MEMBERS && !json_text_is(json, name, names[member])) member++;
	return member;
}

// The range is checked before the number is made smaller: 261 must not pass as 5
static bool read_percent(const char *json, const json_token_t *token, uint8_t *percent)
{
	int64_t number;

	if(!json_integer(json, token, &number)) return false;
	if(number < SETTINGS_BRIGHTNESS_MIN || number > SETTINGS_BRIGHTNESS_MAX) return false;
	*percent = (uint8_t)number;
	return true;
}

static bool read_standby(const char *json, const json_token_t *token, uint16_t *seconds)
{
	int64_t number;

	if(!json_integer(json, token, &number)) return false;
	if(number < 0 || number > SETTINGS_STANDBY_MAX) return false;
	*seconds = (uint16_t)number;
	return true;
}

static bool read_boolean(const json_token_t *token, bool *value)
{
	if(token->type != JSON_TRUE && token->type != JSON_FALSE) return false;
	*value = token->type == JSON_TRUE;
	return true;
}

// The address of the broker, or the empty text that switches the second source off. The field is written
// whole: nothing of a longer address before this one stays behind it.
static bool read_address(const char *json, const json_token_t *token, char *address)
{
	char text[SETTINGS_CERBO_SIZE];

	// A text that does not fit is longer than any address
	if(!json_text(json, token, text, sizeof(text))) return false;
	if(text[0] != '\0' && !settings_is_address(text)) return false;

	memset(address, 0, SETTINGS_CERBO_SIZE);
	strcpy(address, text);
	return true;
}

// As much of the name as there is room for
static void write_bad(char *bad, size_t bad_size, const char *name)
{
	size_t length = strlen(name);

	if(bad == NULL || bad_size == 0) return;

	if(length >= bad_size) length = bad_size - 1;
	memcpy(bad, name, length);
	bad[length] = '\0';
}

bool settings_from_json(settings_t *settings, const char *json, size_t length, char *bad, size_t bad_size,
                        json_token_t *work, int work_count)
{
	// Nothing may change before the whole text is known to be good
	settings_t changed;
	int count = json_parse(json, length, work, work_count);
	int key = 1;

	// As bytes, here and back at the end: the struct has a gap behind its last member, and what lies there in
	// the settings of the caller is to lie there afterwards. Copied member by member the gap of this local
	// would hold what happened to lie on the stack, and carry it into the display (app.h).
	memcpy(&changed, settings, sizeof(changed));
	write_bad(bad, bad_size, "");
	if(count < 0 || work[0].type != JSON_OBJECT) return false;

	for(int i = 0; i < work[0].size; i++)
	{
		const json_token_t *value = &work[key + 1];
		int member = known_member(json, &work[key]);
		bool good;

		key += 1 + value->skip;

		// The member that is not one of `names` (CERBO_NAME); `key` is behind its value by now
		if(member == MEMBERS && json_text_is(json, value - 1, CERBO_NAME))
		{
			if(read_address(json, value, changed.cerbo)) continue;

			write_bad(bad, bad_size, CERBO_NAME);
			return false;
		}

		switch(member)
		{
			case BRIGHTNESS: good = read_percent(json, value, &changed.brightness); break;
			case NIGHT:      good = read_percent(json, value, &changed.night); break;
			case NIGHT_MODE: good = read_boolean(value, &changed.night_mode); break;
			case REVERSE:    good = read_boolean(value, &changed.reverse); break;
			case STANDBY_S:  good = read_standby(json, value, &changed.standby_s); break;
			case WEB_OPEN:   good = read_boolean(value, &changed.web_open); break;
			default:         continue;
		}
		if(!good)
		{
			write_bad(bad, bad_size, names[member]);
			return false;
		}
	}

	memcpy(settings, &changed, sizeof(changed));
	return true;
}

static void put_char(writer_t *writer, char c)
{
	// What does not fit is only counted
	if(writer->length < writer->size) writer->out[writer->length] = c;
	writer->length++;
}

static void put(writer_t *writer, const char *text)
{
	for(; *text != '\0'; text++) put_char(writer, *text);
}

// {"name": for the first member, ,"name": for the others
static void put_name(writer_t *writer, int member)
{
	put_char(writer, member == 0 ? '{' : ',');
	put_char(writer, '"');
	put(writer, names[member]);
	put(writer, "\":");
}

static void put_number(writer_t *writer, uint16_t number)
{
	char digits[5];
	int count = 0;

	do
	{
		digits[count++] = (char)('0' + number % 10);
		number /= 10;
	}
	while(number != 0);
	while(count > 0) put_char(writer, digits[--count]);
}

static void put_boolean(writer_t *writer, bool value)
{
	put(writer, value ? "true" : "false");
}

int settings_to_json(const settings_t *settings, char *out, size_t size)
{
	writer_t writer = {out, size, 0};

	put_name(&writer, BRIGHTNESS);
	put_number(&writer, settings->brightness);
	put_name(&writer, NIGHT);
	put_number(&writer, settings->night);
	put_name(&writer, NIGHT_MODE);
	put_boolean(&writer, settings->night_mode);
	put_name(&writer, REVERSE);
	put_boolean(&writer, settings->reverse);
	put_name(&writer, STANDBY_S);
	put_number(&writer, settings->standby_s);
	// Only for a display that has the address of a Cerbo: every other text is the one without this member
	if(settings_is_address(settings->cerbo))
	{
		put(&writer, ",\"" CERBO_NAME "\":\"");
		put(&writer, settings->cerbo);
		put_char(&writer, '"');
	}
	// In every text, and the last of it
	put_name(&writer, WEB_OPEN);
	put_boolean(&writer, settings->web_open);
	put_char(&writer, '}');

	if(size == 0) return -1;
	if(writer.length >= size)
	{
		out[0] = '\0';
		return -1;
	}
	out[writer.length] = '\0';
	return (int)writer.length;
}

int settings_backlight(const settings_t *settings, bool showing, uint64_t idle_ms)
{
	int percent = settings->night_mode ? settings->night : settings->brightness;

	if(settings->standby_s != 0 && !showing && idle_ms >= (uint64_t)settings->standby_s * 1000) return 0;

	// Settings that did not come through settings_from_json() must neither leave the screen dark for good
	// nor ask for more than there is
	if(percent < SETTINGS_BRIGHTNESS_MIN) return SETTINGS_BRIGHTNESS_MIN;
	if(percent > SETTINGS_BRIGHTNESS_MAX) return SETTINGS_BRIGHTNESS_MAX;
	return percent;
}
