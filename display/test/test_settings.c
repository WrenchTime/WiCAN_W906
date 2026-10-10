/*
 * Host test for display/components/core/settings.c. Run "make test_settings && ./test_settings" in
 * display/test. redproof.py removes or weakens every rule once (mutations/settings.py) and expects this
 * test to fail.
 */
#include <stdint.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test.h"
#include "settings.h"

#define GUARD   0xA5
#define UNTOLD  "untouched"

// The settings lie between guard bytes: what is written next to them lands there and is seen, where the
// address sanitizer would only stop the program without naming a rule.
typedef struct
{
	unsigned char before[32];
	settings_t settings;
	unsigned char after[32];
} box_t;

static box_t box, snapshot;

// Two tokens more than the reader is told about show whether it writes behind its room
static json_token_t work[SETTINGS_TOKENS + 2];
static json_token_t large_work[256];
static json_token_t huge_work[1200];
static char bad[32];
static char text[1024];
static char long_text[8192];
static char huge_text[70000];

static bool guards_intact(void)
{
	size_t i;

	for(i = 0; i < sizeof(box.before); i++)
	{
		if(box.before[i] != GUARD) return false;
	}
	for(i = 0; i < sizeof(box.after); i++)
	{
		if(box.after[i] != GUARD) return false;
	}
	return true;
}

// The field of the address as the test last put it there, byte for byte: what a call that names no address
// has to leave
static char held_address[SETTINGS_CERBO_SIZE];

// The field of the address holds this text and nothing but zeros behind it
static void address(const char *text)
{
	memset(held_address, 0, sizeof(held_address));
	strcpy(held_address, text);
	memcpy(box.settings.cerbo, held_address, sizeof(held_address));
}

// Settings without an address of a Cerbo: the display of before
static void set(int brightness, int night, bool night_mode, bool reverse, int standby_s)
{
	memset(&box, GUARD, sizeof(box));
	box.settings.brightness = (uint8_t)brightness;
	box.settings.night = (uint8_t)night;
	box.settings.night_mode = night_mode;
	box.settings.reverse = reverse;
	box.settings.standby_s = (uint16_t)standby_s;
	address("");
}

// Settings in which every member differs from the defaults and the two booleans from each other
static void other(void)
{
	set(33, 7, true, false, 1234);
	address("10.20.30.40");
}

// The five members are these, and the address is the one the test put there, byte for byte
static bool is(int brightness, int night, bool night_mode, bool reverse, int standby_s)
{
	return box.settings.brightness == brightness && box.settings.night == night && box.settings.night_mode == night_mode &&
	       box.settings.reverse == reverse && box.settings.standby_s == standby_s &&
	       memcmp(box.settings.cerbo, held_address, sizeof(held_address)) == 0 && guards_intact();
}

// The address is this text, with nothing but zeros behind it in its field
static bool address_is(const char *text)
{
	char whole[SETTINGS_CERBO_SIZE] = "";

	strcpy(whole, text);
	return memcmp(box.settings.cerbo, whole, sizeof(whole)) == 0 && guards_intact();
}

static void remember(void)
{
	memcpy(&snapshot, &box, sizeof(box));
}

static bool unchanged(void)
{
	return memcmp(&snapshot, &box, sizeof(box)) == 0;
}

// Reads a text with the token room of the header; `bad` holds something else before
static bool take(const char *json)
{
	strcpy(bad, UNTOLD);
	return settings_from_json(&box.settings, json, strlen(json), bad, sizeof(bad), work, SETTINGS_TOKENS);
}

static bool take_large(const char *json)
{
	strcpy(bad, UNTOLD);
	return settings_from_json(&box.settings, json, strlen(json), bad, sizeof(bad), large_work, 256);
}

// From the other settings: the text is taken, the settings are these, no member is named
static bool taken(const char *json, int brightness, int night, bool night_mode, bool reverse, int standby_s)
{
	other();
	return take(json) && is(brightness, night, night_mode, reverse, standby_s) && bad[0] == '\0';
}

// From the other settings: the text is refused, nothing changed, this member is named
static bool refused(const char *json, const char *name)
{
	other();
	remember();
	return !take(json) && unchanged() && strcmp(bad, name) == 0;
}

// Runs checks in a child process: a crash or a hang of the module is then a failed check here, not the end
// of the test. The checks of the child are printed like all others. Returns false if the child did not come
// to its end.
static bool in_child(void (*checks)(void), const char *what)
{
	int status = 0;
	pid_t child;

	fflush(stdout);
	child = fork();
	if(child == 0)
	{
		// A module that never returns ends the child here
		alarm(120);
		test_failures = 0;
		checks();
		fflush(stdout);
		_exit(test_failures ? 10 : 0);
	}
	if(child < 0 || waitpid(child, &status, 0) != child) status = -1;
	check(status != -1 && WIFEXITED(status) && (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 10), what);
	if(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 10) test_failures++;
	return status != -1 && WIFEXITED(status) && (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 10);
}

// Every function once with ordinary input, before everything else and in a child: a module that reads or writes
// outside of its memory there, or never returns, ends the child. That is a failed check, and the test ends with it -
// what such a module does with the other inputs says nothing any more.
static void test_ordinary(void)
{
	char out[SETTINGS_JSON_SIZE];

	set(255, 255, true, true, 65535);
	settings_defaults(&box.settings);
	check(is(80, 25, false, false, 60), "ordinary input: defaults");
	check(settings_to_json(&box.settings, out, sizeof(out)) == 78 &&
	      strcmp(out, "{\"brightness\":80,\"night\":25,\"night_mode\":false,\"reverse\":false,\"standby_s\":60}") == 0, "ordinary input: the defaults as text");
	set(100, 100, true, true, 3600);
	check(settings_to_json(&box.settings, out, sizeof(out)) == 80 &&
	      strcmp(out, "{\"brightness\":100,\"night\":100,\"night_mode\":true,\"reverse\":true,\"standby_s\":3600}") == 0, "ordinary input: the largest settings as text");
	check(settings_to_json(&box.settings, out, 40) == -1 && out[0] == '\0', "ordinary input: a room that is too small for the text");
	other();
	check(take("{\"later\":{\"a\":[1,2]},\"brightness\":40,\"night\":10,\"night_mode\":false,\"reverse\":true,\"standby_s\":300,\"language\":\"de\"}") &&
	      is(40, 10, false, true, 300) && bad[0] == '\0', "ordinary input: a text with every member and two members of a later firmware");
	check(refused("{\"brightness\":40,\"standby_s\":-1}", "standby_s") && refused("{\"reverse\":1}", "reverse") && refused("[]", ""),
	      "ordinary input: texts that are refused");
	set(80, 25, false, false, 60);
	check(settings_backlight(&box.settings, true, 0) == 80 && settings_backlight(&box.settings, false, 60000) == 0, "ordinary input: backlight");
	check(settings_is_address("192.168.88.2") && !settings_is_address("192.168.88") && !settings_is_address("") && !settings_is_address(NULL),
	      "ordinary input: an address, three numbers, no text and no pointer");
	other();
	check(take("{\"cerbo\":\"192.168.88.2\"}") && address_is("192.168.88.2") && settings_to_json(&box.settings, out, sizeof(out)) == 101 &&
	      strcmp(out, "{\"brightness\":33,\"night\":7,\"night_mode\":true,\"reverse\":false,\"standby_s\":1234,\"cerbo\":\"192.168.88.2\"}") == 0,
	      "ordinary input: the address of a Cerbo is read and written");
}

static void test_constants(void)
{
	check(SETTINGS_BRIGHTNESS_MIN == 5 && SETTINGS_BRIGHTNESS_MAX == 100, "the brightness goes from 5 to 100 percent");
	check(SETTINGS_STANDBY_MAX == 3600, "the standby time goes up to 3600 s");
	check(SETTINGS_JSON_SIZE == 128 && SETTINGS_TOKENS == 32, "128 bytes for the text, 32 tokens for the reader");
	check(SETTINGS_CERBO_SIZE == 16 && sizeof(box.settings.cerbo) == 16 && strlen("255.255.255.255") + 1 == SETTINGS_CERBO_SIZE,
	      "16 bytes for the address of the Cerbo: the longest address and its zero");
}

static void test_defaults(void)
{
	static const int brightness[4] = {0, 80, 33, 255};
	static const int night[4] = {0, 25, 7, 255};
	static const int standby[4] = {0, 60, 1234, 65535};
	bool all;
	int b, n, s, flags;

	// Whatever was in the settings before
	set(255, 255, true, true, 65535);
	settings_defaults(&box.settings);
	check(box.settings.brightness == 80, "defaults: brightness 80");
	check(box.settings.night == 25, "defaults: night 25");
	check(!box.settings.night_mode, "defaults: night mode off");
	check(!box.settings.reverse, "defaults: knob not reversed");
	check(box.settings.standby_s == 60, "defaults: standby after 60 s");
	check(guards_intact(), "defaults: nothing written outside the settings");

	set(0, 0, false, false, 0);
	settings_defaults(&box.settings);
	check(is(80, 25, false, false, 60), "defaults: the same from settings that were zero");

	// Nothing of what was there before counts, the defaults themselves included
	all = true;
	for(b = 0; b < 4; b++)
	{
		for(n = 0; n < 4; n++)
		{
			for(s = 0; s < 4; s++)
			{
				for(flags = 0; flags < 4; flags++)
				{
					set(brightness[b], night[n], (flags & 1) != 0, (flags & 2) != 0, standby[s]);
					settings_defaults(&box.settings);
					if(!is(80, 25, false, false, 60)) all = false;
				}
			}
		}
	}
	check(all, "defaults: the same from 256 settings in which every member is 0, its default, another value or its largest");

	// No address of a Cerbo, whatever the field held
	other();
	settings_defaults(&box.settings);
	check(address_is(""), "defaults: no address of a Cerbo, from settings that held one: the whole field is zero");
	set(255, 255, true, true, 65535);
	memset(box.settings.cerbo, GUARD, sizeof(box.settings.cerbo));
	settings_defaults(&box.settings);
	check(address_is("") && guards_intact(), "defaults: no address of a Cerbo, from a field without a zero in it");
	set(0, 0, false, false, 0);
	memcpy(box.settings.cerbo, "\0" "92.168.188.100", SETTINGS_CERBO_SIZE);
	settings_defaults(&box.settings);
	check(address_is(""), "defaults: nothing is left behind the zero of a field that began with one");
}

// A text of settings written a second time, with printf
static void expected_text(char *to, size_t size, int brightness, int night, bool night_mode, bool reverse, int standby_s)
{
	snprintf(to, size, "{\"brightness\":%d,\"night\":%d,\"night_mode\":%s,\"reverse\":%s,\"standby_s\":%d}",
	         brightness, night, night_mode ? "true" : "false", reverse ? "true" : "false", standby_s);
}

static void test_to_json(void)
{
	static const char stored[] = "{\"brightness\":80,\"night\":25,\"night_mode\":false,\"reverse\":false,\"standby_s\":60}";
	static const char longest[] = "{\"brightness\":255,\"night\":255,\"night_mode\":false,\"reverse\":false,\"standby_s\":65535}";
	// The text between guard bytes, 16 of them behind the largest room that is tried
	unsigned char room[SETTINGS_JSON_SIZE + 16];
	char out[SETTINGS_JSON_SIZE];
	char expected[SETTINGS_JSON_SIZE];
	char fixture[256];
	bool all, fits, empty, behind;
	size_t size, i;
	int value, count;

	settings_defaults(&box.settings);
	check(settings_to_json(&box.settings, out, sizeof(out)) == 78 && strcmp(out, stored) == 0,
	      "to_json: the defaults give the text of the header, 78 bytes");
	check(read_fixture("fixtures/settings_defaults.json", fixture, sizeof(fixture)) && strcmp(out, fixture) == 0,
	      "to_json: the defaults give the fixture, byte for byte");

	set(5, 100, true, true, 0);
	remember();
	check(settings_to_json(&box.settings, out, sizeof(out)) == 75 && read_fixture("fixtures/settings_night.json", fixture, sizeof(fixture)) &&
	      strcmp(out, fixture) == 0, "to_json: the smallest brightness, night mode, reversed knob, no standby give the fixture");
	check(unchanged(), "to_json: the settings are only read");

	set(80, 25, true, false, 60);
	check(settings_to_json(&box.settings, out, sizeof(out)) == 77 &&
	      strcmp(out, "{\"brightness\":80,\"night\":25,\"night_mode\":true,\"reverse\":false,\"standby_s\":60}") == 0,
	      "to_json: night mode alone is true under its own name");
	set(80, 25, false, true, 60);
	check(settings_to_json(&box.settings, out, sizeof(out)) == 77 &&
	      strcmp(out, "{\"brightness\":80,\"night\":25,\"night_mode\":false,\"reverse\":true,\"standby_s\":60}") == 0,
	      "to_json: the reversed knob alone is true under its own name");
	set(41, 9, false, false, 3600);
	check(settings_to_json(&box.settings, out, sizeof(out)) == 79 &&
	      strcmp(out, "{\"brightness\":41,\"night\":9,\"night_mode\":false,\"reverse\":false,\"standby_s\":3600}") == 0,
	      "to_json: brightness and night stand under their own names, 3600 s are written completely");
	set(100, 100, false, false, 1000);
	check(settings_to_json(&box.settings, out, sizeof(out)) == 82 &&
	      strcmp(out, "{\"brightness\":100,\"night\":100,\"night_mode\":false,\"reverse\":false,\"standby_s\":1000}") == 0,
	      "to_json: numbers that end in zeros keep them");

	// What a settings_t can hold at most
	set(255, 255, false, false, 65535);
	check(strlen(longest) == 83 && read_fixture("fixtures/settings_longest.json", fixture, sizeof(fixture)) && strcmp(longest, fixture) == 0,
	      "the longest text has 83 bytes and is the fixture");
	check(settings_to_json(&box.settings, out, sizeof(out)) == 83 && strcmp(out, longest) == 0,
	      "to_json: the largest numbers and the longer of the two words give the longest text");
	check(SETTINGS_JSON_SIZE >= 83 + 1, "SETTINGS_JSON_SIZE is enough for the longest text and its zero");

	// Every room from none to more than enough
	fits = true;
	empty = true;
	behind = true;
	for(size = 0; size <= SETTINGS_JSON_SIZE; size++)
	{
		int length;

		memset(room, GUARD, sizeof(room));
		length = settings_to_json(&box.settings, (char *)room, size);
		if(size >= 84)
		{
			if(length != 83 || memcmp(room, longest, 84) != 0) fits = false;
		}
		else if(length != -1 || (size > 0 && room[0] != '\0')) empty = false;

		for(i = size; i < sizeof(room); i++)
		{
			if(room[i] != GUARD) behind = false;
		}
	}
	check(fits, "to_json: with 84 bytes and more the text of 83 bytes is written with its zero and its length returned");
	check(empty, "to_json: with every room below 84 bytes the answer is -1 and an empty string");
	check(behind, "to_json: nothing is written behind the room, with size 0 nothing at all");
	memset(room, GUARD, sizeof(room));
	check(settings_to_json(&box.settings, (char *)room, 83) == -1 && room[0] == '\0' && room[83] == GUARD,
	      "to_json: one byte too few for the zero is too few");
	memset(long_text, GUARD, sizeof(long_text));
	check(settings_to_json(&box.settings, long_text, sizeof(long_text)) == 83 && memcmp(long_text, longest, 84) == 0,
	      "to_json: a room of 8192 bytes holds the same text with its zero");
	memset(huge_text, GUARD, sizeof(huge_text));
	check(settings_to_json(&box.settings, huge_text, 65536) == 83 && memcmp(huge_text, longest, 84) == 0 && (unsigned char)huge_text[65536] == GUARD,
	      "to_json: a room of 65536 bytes holds the same text with its zero");
	memset(huge_text, GUARD, sizeof(huge_text));
	check(settings_to_json(&box.settings, huge_text, 69999) == 83 && memcmp(huge_text, longest, 84) == 0 && (unsigned char)huge_text[69999] == GUARD,
	      "to_json: a room of 69999 bytes holds the same text with its zero");

	// Every number a member can hold
	all = true;
	for(value = 0; value <= 255; value++)
	{
		set(value, 255 - value, false, true, 60);
		expected_text(expected, sizeof(expected), value, 255 - value, false, true, 60);
		if(settings_to_json(&box.settings, out, sizeof(out)) != (int)strlen(expected) || strcmp(out, expected) != 0) all = false;
	}
	check(all, "to_json: every brightness and night from 0 to 255 is written as printf writes it");
	all = true;
	for(value = 0; value <= 255; value++)
	{
		int second;

		for(second = 0; second <= 255; second++)
		{
			set(value, second, (value + second) % 2 == 0, (value + second) % 3 == 0, 3600 - value - second);
			expected_text(expected, sizeof(expected), value, second, (value + second) % 2 == 0, (value + second) % 3 == 0, 3600 - value - second);
			if(settings_to_json(&box.settings, out, sizeof(out)) != (int)strlen(expected) || strcmp(out, expected) != 0) all = false;
		}
	}
	check(all, "to_json: every pair of brightness and night from 0 to 255, with changing other members, is written as printf writes it");
	all = true;
	for(value = 0; value <= 65535; value++)
	{
		set(80, 25, true, false, value);
		expected_text(expected, sizeof(expected), 80, 25, true, false, value);
		if(settings_to_json(&box.settings, out, sizeof(out)) != (int)strlen(expected) || strcmp(out, expected) != 0) all = false;
	}
	check(all, "to_json: every standby time from 0 to 65535 is written as printf writes it");

	// The reader of the project agrees that it is JSON
	settings_defaults(&box.settings);
	settings_to_json(&box.settings, out, sizeof(out));
	count = json_parse(out, strlen(out), large_work, 256);
	check(count == 11 && large_work[0].type == JSON_OBJECT && large_work[0].size == 5, "to_json: the text is a JSON object of five members, 11 tokens");
	check(strchr(out, ' ') == NULL && strchr(out, '\n') == NULL, "to_json: the text has no whitespace");
}

// Runs before any other text is read: a reader that goes on with the tokens of the text before finds them
// in the same memory here, and the check names it. Later texts lie elsewhere.
static void test_stale_tokens(void)
{
	static char same[] = "{\"brightness\":60}";
	static char two[] = "{\"night\":50,\"brightness\":60}";

	other();
	check(take(same) && is(60, 7, true, false, 1234), "from_json: a text is read before the same memory is used again");
	other();
	remember();
	strcpy(bad, UNTOLD);
	check(!settings_from_json(&box.settings, same, 0, bad, sizeof(bad), work, SETTINGS_TOKENS) && unchanged() && bad[0] == '\0',
	      "from_json: length 0 in the same memory: refused, the tokens of the text before are not used");
	same[0] = ' ';
	check(!settings_from_json(&box.settings, same, 1, bad, sizeof(bad), work, SETTINGS_TOKENS) && unchanged() && bad[0] == '\0',
	      "from_json: a space in the same memory: refused, the tokens of the text before are not used");

	other();
	check(take(two) && is(60, 50, true, false, 1234), "from_json: a text with two members is read before its memory is used again");
	// The same memory now holds {"night":51} followed by "brightness":60}
	memcpy(two, "{\"night\":51}", 12);
	other();
	strcpy(bad, UNTOLD);
	check(settings_from_json(&box.settings, two, 12, bad, sizeof(bad), work, SETTINGS_TOKENS) && is(33, 51, true, false, 1234),
	      "from_json: a text with one member in the same memory: the second member of the text before is not read again from its old tokens");
}

static void test_from_json_members(void)
{
	char fixture[256];

	check(taken("{\"brightness\":40,\"night\":10,\"night_mode\":false,\"reverse\":true,\"standby_s\":300}", 40, 10, false, true, 300),
	      "from_json: a text with every member sets every member");
	other();
	check(read_fixture("fixtures/settings_defaults.json", fixture, sizeof(fixture)) && take(fixture) && is(80, 25, false, false, 60),
	      "from_json: the stored text of the header gives the defaults");
	other();
	check(read_fixture("fixtures/settings_night.json", fixture, sizeof(fixture)) && take(fixture) && is(5, 100, true, true, 0),
	      "from_json: the fixture with night mode is read");
	settings_defaults(&box.settings);
	// The defaults hold no address, and the text names none
	address("");
	check(read_fixture("fixtures/settings_from_browser.json", fixture, sizeof(fixture)) && take(fixture) && is(40, 10, true, true, 300) && bad[0] == '\0',
	      "from_json: a text with whitespace, line breaks and a member of a later firmware is read");

	check(taken("{}", 33, 7, true, false, 1234), "from_json: {} is taken and changes nothing");
	check(taken(" { } ", 33, 7, true, false, 1234) && taken("{                                                                }", 33, 7, true, false, 1234) &&
	      taken("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n{}\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n", 33, 7, true, false, 1234),
	      "from_json: an empty object with whitespace in it and around it is taken and changes nothing");
	check(taken("{\"brightness\":50}", 50, 7, true, false, 1234), "from_json: brightness alone changes only the brightness");
	check(taken("{\"night\":50}", 33, 50, true, false, 1234), "from_json: night alone changes only the night brightness");
	check(taken("{\"night_mode\":false}", 33, 7, false, false, 1234), "from_json: night_mode alone changes only the night mode");
	check(taken("{\"reverse\":true}", 33, 7, true, true, 1234), "from_json: reverse alone changes only the direction");
	check(taken("{\"standby_s\":50}", 33, 7, true, false, 50), "from_json: standby_s alone changes only the standby time");
	check(taken("{\"standby_s\":50,\"reverse\":true,\"night_mode\":false,\"night\":51,\"brightness\":52}", 52, 51, false, true, 50),
	      "from_json: the order of the members does not matter");
	set(255, 0, false, false, 65535);
	check(take("{\"reverse\":true}") && is(255, 0, false, true, 65535),
	      "from_json: members that are missing keep their value, also one outside the range that was there before");
	set(101, 4, false, false, 3601);
	check(take("{}") && is(101, 4, false, false, 3601), "from_json: {} leaves values just outside the ranges as they are");
	check(taken(" {\t\"brightness\" :\n50 ,\r\n \"night_mode\" : false } ", 50, 7, false, false, 1234), "from_json: whitespace between the parts is allowed");

	// Members the format does not know
	check(taken("{\"volume\":11,\"brightness\":50,\"theme\":\"dark\",\"x\":null,\"y\":[1,2,{\"night\":4}],\"z\":{\"brightness\":0,\"reverse\":5}}",
	            50, 7, true, false, 1234), "from_json: unknown members of every type are ignored, also when they contain known names");
	check(taken("{\"z\":{\"standby_s\":9},\"standby_s\":8}", 33, 7, true, false, 8), "from_json: the member behind an unknown object is read");
	// 29 tokens
	check(taken("{\"later\":{\"a\":1,\"b\":[2,3,[4]],\"c\":{\"d\":{\"e\":5}},\"f\":null},\"l\":[[1,2],{\"m\":5}],\"reverse\":true}",
	            33, 7, true, true, 1234), "from_json: the member behind large unknown objects and lists is read");
	check(taken("{\"Brightness\":0,\"brightnes\":0,\"brightness \":0,\"brightness2\":0,\"BRIGHTNESS\":0,\"\":0}", 33, 7, true, false, 1234),
	      "from_json: names that only look like brightness are unknown");
	check(taken("{\"nigh\":0,\"night_\":0,\"Night\":0,\"night_mod\":5,\"night_mode2\":5,\"nightmode\":5,\"n\":0}", 33, 7, true, false, 1234),
	      "from_json: names that only look like night or night_mode are unknown");
	check(taken("{\"revers\":5,\"reversed\":5,\"Reverse\":5,\"standby\":-1,\"standby_\":-1,\"standby_s2\":-1,\"STANDBY_S\":-1}", 33, 7, true, false, 1234),
	      "from_json: names that only look like reverse or standby_s are unknown");

	// Names count with their escapes resolved
	check(taken("{\"\\u0062rightness\":50,\"nigh\\u0074\":9}", 50, 9, true, false, 1234), "from_json: a known name written with escapes is that member");
	check(refused("{\"\\u006eight\":0}", "night"), "from_json: a refused member written with escapes is named without them");

	// The same name twice
	check(taken("{\"brightness\":50,\"brightness\":60}", 60, 7, true, false, 1234), "from_json: of two brightness members the last counts");
	check(taken("{\"night\":50,\"night\":60}", 33, 60, true, false, 1234), "from_json: of two night members the last counts");
	check(taken("{\"night_mode\":false,\"night_mode\":true}", 33, 7, true, false, 1234) &&
	      taken("{\"night_mode\":true,\"night_mode\":false}", 33, 7, false, false, 1234), "from_json: of two night_mode members the last counts");
	check(taken("{\"reverse\":false,\"reverse\":true}", 33, 7, true, true, 1234) &&
	      taken("{\"reverse\":true,\"reverse\":false}", 33, 7, true, false, 1234), "from_json: of two reverse members the last counts");
	check(taken("{\"standby_s\":10,\"night\":9,\"standby_s\":20}", 33, 9, true, false, 20), "from_json: of two standby_s members the last counts");
	check(refused("{\"brightness\":500,\"brightness\":50}", "brightness"), "from_json: a bad member is not healed by a good one of the same name behind it");
	check(refused("{\"brightness\":50,\"brightness\":500}", "brightness"), "from_json: a good member followed by a bad one of the same name is refused");
}

static void test_from_json_ranges(void)
{
	static const struct
	{
		const char *json;
		const char *name;
		const char *what;
	} cases[] = {
		{"{\"brightness\":4}", "brightness", "brightness 4, one below the smallest: refused"},
		{"{\"brightness\":101}", "brightness", "brightness 101, one above the largest: refused"},
		{"{\"brightness\":0}", "brightness", "brightness 0: refused"},
		{"{\"brightness\":-0}", "brightness", "brightness -0: refused"},
		{"{\"brightness\":-50}", "brightness", "brightness -50: refused"},
		{"{\"brightness\":256}", "brightness", "brightness 256, which is 0 in 8 bit: refused"},
		{"{\"brightness\":261}", "brightness", "brightness 261, which is 5 in 8 bit: refused"},
		{"{\"brightness\":336}", "brightness", "brightness 336, which is 80 in 8 bit: refused"},
		{"{\"brightness\":65616}", "brightness", "brightness 65616, which is 80 in 16 bit: refused"},
		{"{\"brightness\":4294967376}", "brightness", "brightness 2^32 + 80: refused"},
		{"{\"brightness\":18446744073709551696}", "brightness", "brightness 2^64 + 80, which json_integer() refuses: refused"},
		{"{\"brightness\":-9223372036854775808}", "brightness", "brightness -2^63: refused"},
		{"{\"brightness\":9223372036854775807}", "brightness", "brightness 2^63 - 1: refused"},
		{"{\"brightness\":80.0}", "brightness", "brightness with a fraction that is zero: refused"},
		{"{\"brightness\":80.5}", "brightness", "brightness with a fraction: refused"},
		{"{\"brightness\":8e1}", "brightness", "brightness with an exponent: refused"},
		{"{\"brightness\":\"80\"}", "brightness", "brightness as a text: refused"},
		{"{\"brightness\":true}", "brightness", "brightness true: refused"},
		{"{\"brightness\":false}", "brightness", "brightness false: refused"},
		{"{\"brightness\":null}", "brightness", "brightness null: refused"},
		{"{\"brightness\":[80]}", "brightness", "brightness as an array: refused"},
		{"{\"brightness\":{\"v\":80}}", "brightness", "brightness as an object: refused"},
		{"{\"night\":4}", "night", "night 4, one below the smallest: refused"},
		{"{\"night\":101}", "night", "night 101, one above the largest: refused"},
		{"{\"night\":0}", "night", "night 0: refused"},
		{"{\"night\":-25}", "night", "night -25: refused"},
		{"{\"night\":281}", "night", "night 281, which is 25 in 8 bit: refused"},
		{"{\"night\":25.0}", "night", "night with a fraction: refused"},
		{"{\"night\":2e1}", "night", "night with an exponent: refused"},
		{"{\"night\":\"25\"}", "night", "night as a text: refused"},
		{"{\"night\":true}", "night", "night true: refused"},
		{"{\"night\":null}", "night", "night null: refused"},
		{"{\"night\":[25]}", "night", "night as an array: refused"},
		{"{\"standby_s\":-1}", "standby_s", "standby_s -1: refused"},
		{"{\"standby_s\":3601}", "standby_s", "standby_s 3601, one above the largest: refused"},
		{"{\"standby_s\":65536}", "standby_s", "standby_s 65536, which is 0 in 16 bit: refused"},
		{"{\"standby_s\":65596}", "standby_s", "standby_s 65596, which is 60 in 16 bit: refused"},
		{"{\"standby_s\":4294967356}", "standby_s", "standby_s 2^32 + 60: refused"},
		{"{\"standby_s\":18446744073709551676}", "standby_s", "standby_s 2^64 + 60: refused"},
		{"{\"standby_s\":60.0}", "standby_s", "standby_s with a fraction: refused"},
		{"{\"standby_s\":6e1}", "standby_s", "standby_s with an exponent: refused"},
		{"{\"standby_s\":\"60\"}", "standby_s", "standby_s as a text: refused"},
		{"{\"standby_s\":true}", "standby_s", "standby_s true: refused"},
		{"{\"standby_s\":false}", "standby_s", "standby_s false: refused"},
		{"{\"standby_s\":null}", "standby_s", "standby_s null: refused"},
		{"{\"standby_s\":[60]}", "standby_s", "standby_s as an array: refused"},
		{"{\"night_mode\":0}", "night_mode", "night_mode 0: refused"},
		{"{\"night_mode\":1}", "night_mode", "night_mode 1: refused"},
		{"{\"night_mode\":\"true\"}", "night_mode", "night_mode as the text true: refused"},
		{"{\"night_mode\":\"false\"}", "night_mode", "night_mode as the text false: refused"},
		{"{\"night_mode\":null}", "night_mode", "night_mode null: refused"},
		{"{\"night_mode\":[true]}", "night_mode", "night_mode as an array: refused"},
		{"{\"night_mode\":{}}", "night_mode", "night_mode as an object: refused"},
		{"{\"reverse\":0}", "reverse", "reverse 0: refused"},
		{"{\"reverse\":1}", "reverse", "reverse 1: refused"},
		{"{\"reverse\":\"true\"}", "reverse", "reverse as the text true: refused"},
		{"{\"reverse\":\"false\"}", "reverse", "reverse as the text false: refused"},
		{"{\"reverse\":null}", "reverse", "reverse null: refused"},
		{"{\"reverse\":[false]}", "reverse", "reverse as an array: refused"},
		{"{\"reverse\":{}}", "reverse", "reverse as an object: refused"},

		// The first one in the order of the text is named
		{"{\"night\":4,\"brightness\":4}", "night", "night and brightness bad: night, the first in the text, is named"},
		{"{\"brightness\":4,\"night\":4}", "brightness", "brightness and night bad: brightness, the first in the text, is named"},
		{"{\"standby_s\":-1,\"reverse\":0,\"night_mode\":0}", "standby_s", "standby_s, reverse and night_mode bad: standby_s is named"},
		{"{\"reverse\":0,\"standby_s\":-1}", "reverse", "reverse and standby_s bad: reverse is named"},
		{"{\"night_mode\":0,\"reverse\":0}", "night_mode", "night_mode and reverse bad: night_mode is named"},
		{"{\"brightness\":50,\"night\":10,\"night_mode\":false,\"reverse\":true,\"standby_s\":99999}", "standby_s",
		 "four good members and a bad one at the end: refused, none of the good ones is applied"},
		{"{\"brightness\":50,\"night\":0,\"reverse\":true}", "night", "a bad member between good ones: refused, nothing applied"},
		{"{\"volume\":\"x\",\"reverse\":1}", "reverse", "an unknown member before a bad one: the bad one is named"},
		{"{\"x\":{\"brightness\":0},\"night\":0}", "night", "a bad value inside an unknown object does not count, the bad member behind it does"},
	};
	size_t i;

	check(taken("{\"brightness\":5}", 5, 7, true, false, 1234), "brightness 5, the smallest: taken");
	check(taken("{\"brightness\":6}", 6, 7, true, false, 1234), "brightness 6: taken");
	check(taken("{\"brightness\":99}", 99, 7, true, false, 1234), "brightness 99: taken");
	check(taken("{\"brightness\":100}", 100, 7, true, false, 1234), "brightness 100, the largest: taken");
	check(taken("{\"night\":5}", 33, 5, true, false, 1234), "night 5, the smallest: taken");
	check(taken("{\"night\":6}", 33, 6, true, false, 1234), "night 6: taken");
	check(taken("{\"night\":99}", 33, 99, true, false, 1234), "night 99: taken");
	check(taken("{\"night\":100}", 33, 100, true, false, 1234), "night 100, the largest: taken");
	check(taken("{\"standby_s\":0}", 33, 7, true, false, 0), "standby_s 0, never: taken");
	check(taken("{\"standby_s\":-0}", 33, 7, true, false, 0), "standby_s -0 is the integer 0: taken");
	check(taken("{\"standby_s\":1}", 33, 7, true, false, 1), "standby_s 1: taken");
	check(taken("{\"standby_s\":255}", 33, 7, true, false, 255) && taken("{\"standby_s\":256}", 33, 7, true, false, 256),
	      "standby_s 255 and 256: taken, more than 8 bit are kept");
	check(taken("{\"standby_s\":3599}", 33, 7, true, false, 3599), "standby_s 3599: taken");
	check(taken("{\"standby_s\":3600}", 33, 7, true, false, 3600), "standby_s 3600, the largest: taken");
	check(taken("{\"night_mode\":true}", 33, 7, true, false, 1234) && taken("{\"night_mode\":false}", 33, 7, false, false, 1234),
	      "night_mode true and false: taken");
	check(taken("{\"reverse\":true}", 33, 7, true, true, 1234) && taken("{\"reverse\":false}", 33, 7, true, false, 1234),
	      "reverse true and false: taken");

	for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) check(refused(cases[i].json, cases[i].name), cases[i].what);

	check(refused("{\"night\":-9223372036854775808}", "night") && refused("{\"night\":9223372036854775807}", "night"),
	      "night -2^63 and 2^63 - 1: refused");
	check(refused("{\"standby_s\":-9223372036854775808}", "standby_s") && refused("{\"standby_s\":9223372036854775807}", "standby_s"),
	      "standby_s -2^63 and 2^63 - 1: refused");
	check(refused("{\"standby_s\":-3600}", "standby_s") && refused("{\"standby_s\":-60}", "standby_s"), "standby_s -3600 and -60: refused");
}

// Every integer far beyond the ranges on both sides, for each of the three numbers alone
static void test_from_json_every_number(void)
{
	bool brightness = true, night = true, standby = true;
	char json[48];
	int value;

	for(value = -70000; value <= 70000; value++)
	{
		bool percent = value >= 5 && value <= 100;
		bool seconds = value >= 0 && value <= 3600;

		other();
		remember();
		snprintf(json, sizeof(json), "{\"brightness\":%d}", value);
		if(take(json) != percent || strcmp(bad, percent ? "" : "brightness") != 0) brightness = false;
		if(percent ? !is(value, 7, true, false, 1234) : !unchanged()) brightness = false;

		other();
		snprintf(json, sizeof(json), "{\"night\":%d}", value);
		if(take(json) != percent || strcmp(bad, percent ? "" : "night") != 0) night = false;
		if(percent ? !is(33, value, true, false, 1234) : !unchanged()) night = false;

		other();
		snprintf(json, sizeof(json), "{\"standby_s\":%d}", value);
		if(take(json) != seconds || strcmp(bad, seconds ? "" : "standby_s") != 0) standby = false;
		if(seconds ? !is(33, 7, true, false, value) : !unchanged()) standby = false;
	}
	check(brightness, "every brightness from -70000 to 70000: taken as written from 5 to 100, else refused, nothing else changed");
	check(night, "every night from -70000 to 70000: taken as written from 5 to 100, else refused, nothing else changed");
	check(standby, "every standby_s from -70000 to 70000: taken as written from 0 to 3600, else refused, nothing else changed");
}

// Members together: no value of one changes what is read of another
static void test_from_json_together(void)
{
	char json[128];
	bool all = true;
	int b, n, s;

	for(b = 5; b <= 100; b++)
	{
		for(n = 5; n <= 100; n++)
		{
			bool night_mode = (b + n) % 2 == 0, reverse = (b + n / 2) % 2 == 0;
			int standby = (b * 97 + n * 31) % 3601;

			set(b == 50 ? 51 : 50, n == 50 ? 51 : 50, !night_mode, !reverse, standby + 1);
			snprintf(json, sizeof(json), "{\"brightness\":%d,\"night\":%d,\"night_mode\":%s,\"reverse\":%s,\"standby_s\":%d}",
			         b, n, night_mode ? "true" : "false", reverse ? "true" : "false", standby);
			if(!take(json) || !is(b, n, night_mode, reverse, standby)) all = false;
		}
	}
	check(all, "every pair of brightness and night with changing booleans and standby times: all five members are read as written");

	all = true;
	for(s = 0; s <= 3600; s++)
	{
		bool night_mode = s % 2 == 0, reverse = s / 2 % 2 == 0;

		b = 5 + s % 96;
		n = 5 + s / 7 % 96;
		set(b == 50 ? 51 : 50, n == 50 ? 51 : 50, !night_mode, !reverse, s + 1);
		snprintf(json, sizeof(json), "{\"standby_s\":%d,\"reverse\":%s,\"night_mode\":%s,\"night\":%d,\"brightness\":%d}",
		         s, reverse ? "true" : "false", night_mode ? "true" : "false", n, b);
		if(!take(json) || !is(b, n, night_mode, reverse, s)) all = false;
	}
	check(all, "every standby time with changing other members, in the reverse order: all five members are read as written");

	// The booleans in every combination, read into every combination: no member changes what is read of another
	all = true;
	for(s = 0; s <= 3600; s++)
	{
		int flags;

		for(flags = 0; flags < 16; flags++)
		{
			bool night_mode = (flags & 1) != 0, reverse = (flags & 2) != 0;

			b = 5 + (s * 7 + flags) % 96;
			n = 5 + (s * 11 + flags) % 96;
			set(n, b, (flags & 4) != 0, (flags & 8) != 0, 3600 - s);
			snprintf(json, sizeof(json), "{\"brightness\":%d,\"night\":%d,\"night_mode\":%s,\"reverse\":%s,\"standby_s\":%d}",
			         b, n, night_mode ? "true" : "false", reverse ? "true" : "false", s);
			if(!take(json) || !is(b, n, night_mode, reverse, s)) all = false;
		}
	}
	check(all, "every standby time with both booleans in every combination, read into settings with every combination: all five members are read as written");
	all = true;
	for(b = 5; b <= 100; b++)
	{
		int flags;

		for(n = 5; n <= 100; n++)
		{
			for(flags = 0; flags < 16; flags++)
			{
				bool night_mode = (flags & 1) != 0, reverse = (flags & 2) != 0;

				s = (b * 37 + n * 101 + flags) % 3601;
				set(n, b, (flags & 4) != 0, (flags & 8) != 0, 3600 - s);
				snprintf(json, sizeof(json), "{\"reverse\":%s,\"brightness\":%d,\"standby_s\":%d,\"night_mode\":%s,\"night\":%d}",
				         reverse ? "true" : "false", b, s, night_mode ? "true" : "false", n);
				if(!take(json) || !is(b, n, night_mode, reverse, s)) all = false;
			}
		}
	}
	check(all, "every pair of brightness and night with both booleans in every combination, read into settings with every combination: all five members are read as written");

	// Every subset of the five members, with every standby time and many values of the others: the members that are
	// there are set, the others keep what they had
	all = true;
	for(s = 0; s <= 3600; s++)
	{
		int subset;

		for(subset = 0; subset < 32; subset++)
		{
			bool night_mode = (s + subset) % 2 == 0, reverse = (s / 2 + subset) % 2 == 0;
			size_t at = 0;

			b = 5 + (s * 13 + subset) % 96;
			n = 5 + (s * 17 + subset) % 96;
			// What was there before differs in every member
			set(b == 100 ? 5 : b + 1, n == 5 ? 100 : n - 1, !night_mode, !reverse, s == 3600 ? 0 : s + 1);
			at += (size_t)snprintf(json + at, sizeof(json) - at, "{\"later\":%d", subset);
			if(subset & 1) at += (size_t)snprintf(json + at, sizeof(json) - at, ",\"brightness\":%d", b);
			if(subset & 2) at += (size_t)snprintf(json + at, sizeof(json) - at, ",\"night\":%d", n);
			if(subset & 4) at += (size_t)snprintf(json + at, sizeof(json) - at, ",\"night_mode\":%s", night_mode ? "true" : "false");
			if(subset & 8) at += (size_t)snprintf(json + at, sizeof(json) - at, ",\"reverse\":%s", reverse ? "true" : "false");
			if(subset & 16) at += (size_t)snprintf(json + at, sizeof(json) - at, ",\"standby_s\":%d", s);
			snprintf(json + at, sizeof(json) - at, "}");
			if(!take(json) || !is(subset & 1 ? b : (b == 100 ? 5 : b + 1), subset & 2 ? n : (n == 5 ? 100 : n - 1), subset & 4 ? night_mode : !night_mode,
			                     subset & 8 ? reverse : !reverse, subset & 16 ? s : (s == 3600 ? 0 : s + 1))) all = false;
		}
	}
	check(all, "every subset of the five members with every standby time: the members that are there are set, the others keep their value");
}

static void test_from_json_whole(void)
{
	static const struct
	{
		const char *json;
		const char *what;
	} cases[] = {
		// First a text whose tokens are all there: a reader that goes on does it in an orderly way
		{"{\"brightness\":50}x", "refused as a whole: text behind the object"},
		{"", "refused as a whole: an empty text"},
		{" ", "refused as a whole: a space"},
		{"5", "refused as a whole: a number"},
		{"\"brightness\"", "refused as a whole: a text"},
		{"null", "refused as a whole: null"},
		{"true", "refused as a whole: true"},
		{"[]", "refused as a whole: an empty array"},
		{"[{\"brightness\":50}]", "refused as a whole: an object inside an array"},
		{"[\"brightness\",50]", "refused as a whole: an array of a name and a value"},
		{"{", "refused as a whole: only a brace"},
		{"{\"brightness\":50", "refused as a whole: an object that is not closed"},
		{"{\"brightness\":50,}", "refused as a whole: a comma before the end"},
		{"{\"brightness\":}", "refused as a whole: a member without value"},
		{"{\"brightness\":050}", "refused as a whole: a number with a leading zero"},
		{"{\"brightness\":+50}", "refused as a whole: a number with a plus sign"},
		{"{'brightness':50}", "refused as a whole: a name in single quotes"},
		{"{\"brightness\":50,\"x\":[[[[[[[[1]]]]]]]]}", "refused as a whole: nested deeper than the reader follows"},
		{"{\"brightness\":4,}", "refused as a whole: broken JSON behind a bad member names no member"},
		{"{\"brightness\":4", "refused as a whole: a bad member in an object that is not closed names no member"},
	};
	size_t i;

	for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) check(refused(cases[i].json, ""), cases[i].what);

	// Only `length` bytes belong to the text
	other();
	strcpy(bad, UNTOLD);
	check(settings_from_json(&box.settings, "{\"brightness\":50}x", 17, bad, sizeof(bad), work, SETTINGS_TOKENS) && is(50, 7, true, false, 1234),
	      "from_json: bytes behind the given length are not read");
	other();
	remember();
	strcpy(bad, UNTOLD);
	check(!settings_from_json(&box.settings, "{\"brightness\":50}", 16, bad, sizeof(bad), work, SETTINGS_TOKENS) && unchanged() && bad[0] == '\0',
	      "from_json: a length that ends before the brace: refused as a whole");
}

static void test_bad(void)
{
	static const struct
	{
		const char *json;
		const char *name;
		const char *what;
	} cases[] = {
		{"{\"brightness\":0}", "brightness", "bad: brightness is cut to every room from 0 to 16 bytes, nothing written behind the room"},
		{"{\"night\":0}", "night", "bad: night is cut to every room from 0 to 16 bytes, nothing written behind the room"},
		{"{\"night_mode\":0}", "night_mode", "bad: night_mode is cut to every room from 0 to 16 bytes, nothing written behind the room"},
		{"{\"reverse\":0}", "reverse", "bad: reverse is cut to every room from 0 to 16 bytes, nothing written behind the room"},
		{"{\"standby_s\":-1}", "standby_s", "bad: standby_s is cut to every room from 0 to 16 bytes, nothing written behind the room"},
		{"{\"cerbo\":\"192.168.88\"}", "cerbo", "bad: cerbo is cut to every room from 0 to 16 bytes, nothing written behind the room"},
	};
	static unsigned char large_room[4096];
	unsigned char room[32];
	char what[96];
	size_t i, size, at;
	bool all;

	for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		all = true;
		for(size = 0; size <= 16; size++)
		{
			char expected[16];
			size_t length = strlen(cases[i].name);

			// What a room of `size` bytes can hold of the name
			if(size > 0 && length > size - 1) length = size - 1;
			memcpy(expected, cases[i].name, length);
			expected[length] = '\0';

			other();
			remember();
			memset(room, GUARD, sizeof(room));
			if(settings_from_json(&box.settings, cases[i].json, strlen(cases[i].json), (char *)room, size, work, SETTINGS_TOKENS)) all = false;
			if(!unchanged()) all = false;
			if(size > 0 && strcmp((char *)room, expected) != 0) all = false;
			for(at = size; at < sizeof(room); at++)
			{
				if(room[at] != GUARD) all = false;
			}
		}
		check(all, cases[i].what);
	}

	memset(room, GUARD, sizeof(room));
	other();
	check(!settings_from_json(&box.settings, "{\"brightness\":0}", 16, (char *)room, 11, work, SETTINGS_TOKENS) && strcmp((char *)room, "brightness") == 0,
	      "bad: 11 bytes hold brightness completely");
	memset(room, GUARD, sizeof(room));
	check(!settings_from_json(&box.settings, "{\"brightness\":0}", 16, (char *)room, 10, work, SETTINGS_TOKENS) && strcmp((char *)room, "brightnes") == 0,
	      "bad: 10 bytes hold brightnes and the zero");
	memset(room, GUARD, sizeof(room));
	check(!settings_from_json(&box.settings, "{\"brightness\":0}", 16, (char *)room, 1, work, SETTINGS_TOKENS) && room[0] == '\0' && room[1] == GUARD,
	      "bad: 1 byte holds the zero alone");

	// A room far larger than any name
	for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		all = true;
		for(size = 17; size <= sizeof(large_room) - 16; size += size < 300 ? 1 : 97)
		{
			other();
			memset(large_room, GUARD, sizeof(large_room));
			if(settings_from_json(&box.settings, cases[i].json, strlen(cases[i].json), (char *)large_room, size, work, SETTINGS_TOKENS)) all = false;
			if(strcmp((char *)large_room, cases[i].name) != 0) all = false;
			for(at = size; at < sizeof(large_room); at++)
			{
				if(large_room[at] != GUARD) all = false;
			}
		}
		snprintf(what, sizeof(what), "bad: %s is written completely into every room from 17 bytes on, up to 4080 bytes", cases[i].name);
		check(all, what);
	}
	memset(huge_text, GUARD, sizeof(huge_text));
	other();
	check(!settings_from_json(&box.settings, "{\"standby_s\":-1}", 16, huge_text, 65536, work, SETTINGS_TOKENS) && strcmp(huge_text, "standby_s") == 0 &&
	      (unsigned char)huge_text[65536] == GUARD, "bad: a room of 65536 bytes holds the name");
	memset(huge_text, GUARD, sizeof(huge_text));
	check(!settings_from_json(&box.settings, "{\"night_mode\":1}", 16, huge_text, 69999, work, SETTINGS_TOKENS) && strcmp(huge_text, "night_mode") == 0 &&
	      (unsigned char)huge_text[69999] == GUARD, "bad: a room of 69999 bytes holds the name");
	memset(large_room, GUARD, sizeof(large_room));
	other();
	check(settings_from_json(&box.settings, "{\"brightness\":50}", 17, (char *)large_room, 4000, work, SETTINGS_TOKENS) && large_room[0] == '\0',
	      "bad: a text that is taken leaves an empty text in a room of 4000 bytes");
	memset(large_room, GUARD, sizeof(large_room));
	check(!settings_from_json(&box.settings, "[1]", 3, (char *)large_room, 4000, work, SETTINGS_TOKENS) && large_room[0] == '\0',
	      "bad: a text refused as a whole leaves an empty text in a room of 4000 bytes");

	// A text that is taken or refused as a whole leaves an empty text
	all = true;
	for(size = 0; size <= 16; size++)
	{
		other();
		memset(room, GUARD, sizeof(room));
		if(!settings_from_json(&box.settings, "{\"brightness\":50}", 17, (char *)room, size, work, SETTINGS_TOKENS)) all = false;
		if(size > 0 && room[0] != '\0') all = false;
		for(at = size; at < sizeof(room); at++)
		{
			if(room[at] != GUARD) all = false;
		}
	}
	check(all, "bad: a text that is taken leaves an empty text in every room from 1 to 16 bytes and nothing in none");
	all = true;
	for(size = 0; size <= 16; size++)
	{
		other();
		memset(room, GUARD, sizeof(room));
		if(settings_from_json(&box.settings, "[1]", 3, (char *)room, size, work, SETTINGS_TOKENS)) all = false;
		if(size > 0 && room[0] != '\0') all = false;
		for(at = size; at < sizeof(room); at++)
		{
			if(room[at] != GUARD) all = false;
		}
	}
	check(all, "bad: a text refused as a whole leaves an empty text in every room from 1 to 16 bytes and nothing in none");
}

// Nobody has to listen, and nobody has to give a buffer where no room is
static void test_null(void)
{
	other();
	check(settings_from_json(&box.settings, "{\"brightness\":50}", 17, NULL, 0, work, SETTINGS_TOKENS) && is(50, 7, true, false, 1234),
	      "bad may be NULL: a text is taken");
	other();
	check(settings_from_json(&box.settings, "{\"night\":50}", 12, NULL, 16, work, SETTINGS_TOKENS) && is(33, 50, true, false, 1234),
	      "bad may be NULL with a size that is not 0: a text is taken");
	other();
	remember();
	check(!settings_from_json(&box.settings, "{\"brightness\":0}", 16, NULL, 0, work, SETTINGS_TOKENS) && unchanged(), "bad may be NULL: a bad member is refused");
	check(!settings_from_json(&box.settings, "{\"reverse\":0}", 13, NULL, 16, work, SETTINGS_TOKENS) && unchanged(),
	      "bad may be NULL with a size that is not 0: a bad member is refused");
	check(!settings_from_json(&box.settings, "[", 1, NULL, 16, work, SETTINGS_TOKENS) && unchanged(), "bad may be NULL: broken JSON is refused");

	other();
	remember();
	strcpy(bad, UNTOLD);
	check(!settings_from_json(&box.settings, "{\"brightness\":50}", 17, bad, sizeof(bad), NULL, 0) && unchanged() && bad[0] == '\0',
	      "no room for tokens and no array for them: refused as a whole");
	strcpy(bad, UNTOLD);
	check(!settings_from_json(&box.settings, NULL, 0, bad, sizeof(bad), work, SETTINGS_TOKENS) && unchanged() && bad[0] == '\0',
	      "a text of length 0 that is NULL: refused as a whole");

	set(255, 255, false, false, 65535);
	check(settings_to_json(&box.settings, NULL, 0) == -1, "to_json: no room and no buffer: -1");
	check(!settings_is_address(NULL), "no text (NULL) is no address");
	other();
	remember();
	check(!settings_from_json(&box.settings, "{\"cerbo\":7}", 11, NULL, 0, work, SETTINGS_TOKENS) && unchanged(), "bad may be NULL: a bad address is refused");
}

static void test_tokens(void)
{
	static const char full[] = "{\"brightness\":40,\"night\":10,\"night_mode\":false,\"reverse\":true,\"standby_s\":300}";
	// The five members, nine members of a later firmware and one with a list: 1 + 2 * 14 + 3 = 32 tokens
	static const char tokens32[] = "{\"a\":1,\"b\":2,\"c\":3,\"d\":4,\"e\":5,\"f\":6,\"g\":7,\"h\":8,\"i\":9,\"j\":[1],"
	                               "\"brightness\":40,\"night\":10,\"night_mode\":false,\"reverse\":true,\"standby_s\":300}";
	// One member more instead of the list: 1 + 2 * 16 = 33 tokens
	static const char tokens33[] = "{\"a\":1,\"b\":2,\"c\":3,\"d\":4,\"e\":5,\"f\":6,\"g\":7,\"h\":8,\"i\":9,\"j\":1,\"k\":2,"
	                               "\"brightness\":40,\"night\":10,\"night_mode\":false,\"reverse\":true,\"standby_s\":300}";

	check(json_parse(full, strlen(full), large_work, 256) == 11 && json_parse(tokens32, strlen(tokens32), large_work, 256) == 32 &&
	      json_parse(tokens33, strlen(tokens33), large_work, 256) == 33, "the test texts have 11, 32 and 33 tokens");

	other();
	check(take(tokens32) && is(40, 10, false, true, 300), "a text of 32 tokens is read with SETTINGS_TOKENS tokens");
	check(refused(tokens33, ""), "a text of 33 tokens is refused as a whole with SETTINGS_TOKENS tokens");
	other();
	check(take_large(tokens33) && is(40, 10, false, true, 300), "a text of 33 tokens is read with a larger room");

	other();
	strcpy(bad, UNTOLD);
	check(settings_from_json(&box.settings, full, strlen(full), bad, sizeof(bad), work, 11) && is(40, 10, false, true, 300),
	      "exactly as many tokens as the text needs: taken");
	other();
	remember();
	memset(&work[10], 0x5A, 2 * sizeof(work[0]));
	strcpy(bad, UNTOLD);
	check(!settings_from_json(&box.settings, full, strlen(full), bad, sizeof(bad), work, 10) && unchanged() && bad[0] == '\0',
	      "one token less than the text needs: refused as a whole, nothing changed");
	check(work[10].start == 0x5A5A5A5A && work[11].start == 0x5A5A5A5A, "one token less than the text needs: nothing written behind the room of the reader");
	memset(&work[0], 0x5A, 2 * sizeof(work[0]));
	strcpy(bad, UNTOLD);
	check(!settings_from_json(&box.settings, "{}", 2, bad, sizeof(bad), work, 0) && unchanged() && bad[0] == '\0' && work[0].start == 0x5A5A5A5A,
	      "no room for tokens at all: refused, nothing written");
	strcpy(bad, UNTOLD);
	check(!settings_from_json(&box.settings, "{}", 2, bad, sizeof(bad), work, -1) && unchanged() && bad[0] == '\0' && work[0].start == 0x5A5A5A5A,
	      "a negative room for tokens: refused, nothing written");
}

// Texts that are larger than those of this firmware: longer, with more members, with larger members
static void test_from_json_large(void)
{
	static const char full[] = "{\"brightness\":40,\"night\":10,\"night_mode\":false,\"reverse\":true,\"standby_s\":300}";
	size_t length = 0;
	bool all = true;
	int i, count;

	// 60 members of a later firmware in front of the five
	length += (size_t)snprintf(long_text + length, sizeof(long_text) - length, "{");
	for(i = 0; i < 60; i++) length += (size_t)snprintf(long_text + length, sizeof(long_text) - length, "\"later%d\":%d,", i, i);
	length += (size_t)snprintf(long_text + length, sizeof(long_text) - length, "%s", full + 1);
	other();
	strcpy(bad, UNTOLD);
	check(json_parse(long_text, length, huge_work, 1200) == 131 &&
	      settings_from_json(&box.settings, long_text, length, bad, sizeof(bad), huge_work, 1200) && is(40, 10, false, true, 300) && bad[0] == '\0',
	      "from_json: the five members behind 60 unknown ones are read, 131 tokens");
	other();
	remember();
	strcpy(bad, UNTOLD);
	memcpy(strstr(long_text, "\"standby_s\":300"), "\"standby_s\":-30", 15);
	check(!settings_from_json(&box.settings, long_text, length, bad, sizeof(bad), huge_work, 1200) && unchanged() && strcmp(bad, "standby_s") == 0,
	      "from_json: a bad member behind 64 others is refused and named");

	// One unknown member with a list of 200 numbers and one with an object of 100 members in front of the five
	length = (size_t)snprintf(long_text, sizeof(long_text), "{\"list\":[");
	for(i = 0; i < 200; i++) length += (size_t)snprintf(long_text + length, sizeof(long_text) - length, "%s%d", i ? "," : "", i);
	length += (size_t)snprintf(long_text + length, sizeof(long_text) - length, "],\"map\":{");
	for(i = 0; i < 100; i++) length += (size_t)snprintf(long_text + length, sizeof(long_text) - length, "%s\"night\":%d", i ? "," : "", i);
	length += (size_t)snprintf(long_text + length, sizeof(long_text) - length, "},%s", full + 1);
	other();
	strcpy(bad, UNTOLD);
	check(json_parse(long_text, length, huge_work, 1200) == 415 &&
	      settings_from_json(&box.settings, long_text, length, bad, sizeof(bad), huge_work, 1200) && is(40, 10, false, true, 300) && bad[0] == '\0',
	      "from_json: the five members behind an unknown list of 200 numbers and an unknown object of 100 members are read, 415 tokens");

	// 8000 bytes of whitespace in front of the object, between its parts and behind it
	memset(long_text, ' ', 8000);
	length = 3000 + (size_t)snprintf(long_text + 3000, sizeof(long_text) - 3000, "{\"brightness\":40,");
	long_text[length] = ' ';
	length = 7900 + (size_t)snprintf(long_text + 7900, sizeof(long_text) - 7900, "\"standby_s\":300}");
	long_text[length] = '\n';
	length = 8000;
	other();
	strcpy(bad, UNTOLD);
	check(settings_from_json(&box.settings, long_text, length, bad, sizeof(bad), work, SETTINGS_TOKENS) && is(40, 7, true, false, 300) && bad[0] == '\0',
	      "from_json: a text of 8000 bytes, most of them whitespace, is read up to its last member at byte 7900");
	other();
	remember();
	strcpy(bad, UNTOLD);
	memcpy(long_text + 7900, "\"standby_s\":-30}", 16);
	check(!settings_from_json(&box.settings, long_text, length, bad, sizeof(bad), work, SETTINGS_TOKENS) && unchanged() && strcmp(bad, "standby_s") == 0,
	      "from_json: a bad member at byte 7900 of a text is refused and named");

	// An unknown member with a list of 1187 numbers in front of the five: 1200 tokens, as many as the largest room here has
	length = (size_t)snprintf(long_text, sizeof(long_text), "{\"list\":[");
	for(i = 0; i < 1187; i++) length += (size_t)snprintf(long_text + length, sizeof(long_text) - length, "%s%d", i ? "," : "", i % 10);
	length += (size_t)snprintf(long_text + length, sizeof(long_text) - length, "],%s", full + 1);
	other();
	strcpy(bad, UNTOLD);
	check(json_parse(long_text, length, huge_work, 1200) == 1200 &&
	      settings_from_json(&box.settings, long_text, length, bad, sizeof(bad), huge_work, 1200) && is(40, 10, false, true, 300) && bad[0] == '\0',
	      "from_json: the five members behind an unknown list of 1187 numbers are read with a room of 1200 tokens, 1200 tokens");
	other();
	remember();
	strcpy(bad, UNTOLD);
	check(!settings_from_json(&box.settings, long_text, length, bad, sizeof(bad), huge_work, 1199) && unchanged() && bad[0] == '\0',
	      "from_json: the text of 1200 tokens is refused as a whole with a room of 1199");

	// 300 members of a later firmware in front of the five: 611 tokens
	length = (size_t)snprintf(long_text, sizeof(long_text), "{");
	for(i = 0; i < 300; i++) length += (size_t)snprintf(long_text + length, sizeof(long_text) - length, "\"later%d\":%d,", i, i);
	length += (size_t)snprintf(long_text + length, sizeof(long_text) - length, "%s", full + 1);
	other();
	strcpy(bad, UNTOLD);
	check(json_parse(long_text, length, huge_work, 1200) == 611 &&
	      settings_from_json(&box.settings, long_text, length, bad, sizeof(bad), huge_work, 1200) && is(40, 10, false, true, 300) && bad[0] == '\0',
	      "from_json: the five members behind 300 unknown ones are read, 611 tokens");

	// A text longer than 16 bit can count: 69900 bytes of whitespace in front of the object
	memset(huge_text, ' ', sizeof(huge_text));
	memcpy(huge_text + 69900, full, strlen(full));
	other();
	strcpy(bad, UNTOLD);
	check(settings_from_json(&box.settings, huge_text, 69900 + strlen(full), bad, sizeof(bad), work, SETTINGS_TOKENS) && is(40, 10, false, true, 300) && bad[0] == '\0',
	      "from_json: a text of 69978 bytes is read up to its end");
	other();
	remember();
	strcpy(bad, UNTOLD);
	memcpy(huge_text + 69900, "{\"brightness\":4 ,", 17);
	check(!settings_from_json(&box.settings, huge_text, 69900 + strlen(full), bad, sizeof(bad), work, SETTINGS_TOKENS) && unchanged() && strcmp(bad, "brightness") == 0,
	      "from_json: a bad member behind 69900 bytes is refused and named");

	// The room for the tokens may have any size from what the text needs
	for(count = -2; count <= 1200; count++)
	{
		other();
		remember();
		strcpy(bad, UNTOLD);
		if(settings_from_json(&box.settings, full, strlen(full), bad, sizeof(bad), huge_work, count) != (count >= 11) || bad[0] != '\0') all = false;
		if(count >= 11 ? !is(40, 10, false, true, 300) : !unchanged()) all = false;
	}
	check(all, "from_json: with every room from -2 to 1200 tokens the text of 11 tokens is taken from 11 on and refused as a whole below");
}

// What to_json wrote is read back as the same settings
static void test_round_trip(void)
{
	static const int percents[] = {5, 6, 25, 80, 99, 100};
	static const int standbys[] = {0, 1, 59, 60, 255, 256, 3599, 3600};
	char out[SETTINGS_JSON_SIZE];
	bool all = true;
	size_t b, n, s;
	int flags;

	for(b = 0; b < sizeof(percents) / sizeof(percents[0]); b++)
	{
		for(n = 0; n < sizeof(percents) / sizeof(percents[0]); n++)
		{
			for(s = 0; s < sizeof(standbys) / sizeof(standbys[0]); s++)
			{
				for(flags = 0; flags < 4; flags++)
				{
					set(percents[b], percents[n], (flags & 1) != 0, (flags & 2) != 0, standbys[s]);
					if(settings_to_json(&box.settings, out, sizeof(out)) < 0) all = false;
					// Read into settings that differ in every member
					set(percents[b] == 50 ? 51 : 50, percents[n] == 50 ? 51 : 50, (flags & 1) == 0, (flags & 2) == 0, standbys[s] + 7);
					if(!take(out) || !is(percents[b], percents[n], (flags & 1) != 0, (flags & 2) != 0, standbys[s])) all = false;
				}
			}
		}
	}
	check(all, "round trip: 1152 settings within the ranges are written and read back unchanged");
}

/* The address of the Cerbo ------------------------------------------------------------------------------ */

// The rule of the header written a second time, in another shape: the text is cut at its dots, and each
// piece is looked at by itself
static bool model_address(const char *text)
{
	const char *piece = text;
	int pieces = 0;

	for(;;)
	{
		size_t length = strcspn(piece, ".");

		// One to three digits that write a number up to 255, and no zero in front of another digit
		if(length < 1 || length > 3 || strspn(piece, "0123456789") != length) return false;
		if(length > 1 && piece[0] == '0') return false;
		if(atoi(piece) > 255) return false;

		pieces++;
		if(piece[length] == '\0') return pieces == 4;
		piece += length + 1;
	}
}

static void test_is_address(void)
{
	static const char *const good[] = {
		"192.168.88.2", "0.0.0.0", "255.255.255.255", "1.2.3.4", "10.0.0.1", "100.200.30.4", "9.99.199.249", "250.251.252.253", "127.0.0.1", "192.168.188.100",
	};
	static const struct
	{
		const char *text;
		const char *what;
	} bad[] = {
		{"", "address: the empty text is none"},
		{" ", "address: a blank is none"},
		{"192.168.88", "address: three numbers are none"},
		{"192.168.88.2.1", "address: five numbers are none"},
		{"192.168.88.", "address: a dot without a fourth number behind it is none"},
		{"192.168.88.2.", "address: a dot behind the fourth number is none"},
		{".192.168.88.2", "address: a dot in front is none"},
		{"192..168.88", "address: two dots in a row are none"},
		{"192.168..2", "address: an empty third number is none"},
		{"192.168.88.2 ", "address: a blank behind it is none"},
		{" 192.168.88.2", "address: a blank in front of it is none"},
		{"192.168.88.2\n", "address: a line end behind it is none"},
		{"192.168. 88.2", "address: a blank inside is none"},
		{"192.168.88.256", "address: 256 as the fourth number is none"},
		{"256.168.88.2", "address: 256 as the first number is none"},
		{"192.256.88.2", "address: 256 as the second number is none"},
		{"192.168.256.2", "address: 256 as the third number is none"},
		{"192.168.88.300", "address: 300 is no number of an address"},
		{"192.168.88.999", "address: 999 is no number of an address"},
		{"1000.168.88.2", "address: four digits are no number of an address"},
		{"192.168.88.2000", "address: four digits at the end are none"},
		{"192.168.88.1234", "address: 1234 at the end is none, although 123 would be"},
		{"01.2.3.4", "address: a leading zero in the first number is none"},
		{"1.02.3.4", "address: a leading zero in the second number is none"},
		{"1.2.03.4", "address: a leading zero in the third number is none"},
		{"1.2.3.04", "address: a leading zero in the fourth number is none"},
		{"00.0.0.0", "address: two zeros are no number of an address"},
		{"192.168.088.2", "address: 088 is no number of an address"},
		{"000.0.0.0", "address: three zeros are no number of an address"},
		{"-1.2.3.4", "address: a minus sign is none"},
		{"+1.2.3.4", "address: a plus sign is none"},
		{"1.2.3.-4", "address: a minus sign in the last number is none"},
		{"1,2,3,4", "address: commas are no dots"},
		{"1.2.3,4", "address: one comma is no dot"},
		{"1:2:3:4", "address: colons are no dots"},
		{"1.2.3.4a", "address: a letter behind it is none"},
		{"a1.2.3.4", "address: a letter in front of it is none"},
		{"1.2.3.a", "address: a letter as a number is none"},
		{"a.b.c.d", "address: letters are none"},
		{"einstein", "address: a name is none"},
		{"cerbo.local", "address: a name with a dot is none"},
		{"venus.local.lan.de", "address: a name with three dots is none"},
		{"192.168.88.2:1883", "address: a port behind it is none"},
		{"192.168.88.2/24", "address: a mask behind it is none"},
		{"0x7f.0.0.1", "address: a hexadecimal number is none"},
		{"1e1.2.3.4", "address: a number with an exponent is none"},
		{"1.5", "address: a fraction is none"},
		{"3232258050", "address: one large number is none"},
		{"\xEF\xBC\x91.2.3.4", "address: a digit that is no ASCII digit is none"},
		{"192.168.88.2\t", "address: a tab behind it is none"},
		{"::1", "address: an IPv6 address is none"},
		{"::ffff:192.168.88.2", "address: an IPv4 address inside an IPv6 one is none"},
	};
	static const char base[] = "192.168.88.2";
	char text[64];
	bool all;
	size_t i;
	int number, place, at, byte, taken = 0, compared = 0;

	all = true;
	for(i = 0; i < sizeof(good) / sizeof(good[0]); i++)
	{
		if(!settings_is_address(good[i]) || !model_address(good[i]))
		{
			printf("  %s is not taken\n", good[i]);
			all = false;
		}
	}
	check(all, "address: ten addresses in dotted form are addresses, among them 0.0.0.0, 255.255.255.255 and the longest, of 15 bytes");
	check(settings_is_address("0.0.0.0") && settings_is_address("10.0.0.1") && settings_is_address("100.200.30.4"),
	      "address: 0 is a number of an address, and so is a number that has a zero behind its first digit");
	for(i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) check(!settings_is_address(bad[i].text) && !model_address(bad[i].text), bad[i].what);

	// Every number from 0 to 999 at each of the four places, as printf writes it, and with a zero in front
	all = true;
	for(place = 0; place < 4; place++)
	{
		for(number = 0; number <= 999; number++)
		{
			int numbers[4] = {192, 168, 88, 2};

			numbers[place] = number;
			snprintf(text, sizeof(text), "%d.%d.%d.%d", numbers[0], numbers[1], numbers[2], numbers[3]);
			if(settings_is_address(text) != (number <= 255)) all = false;
			if(number <= 255) taken++;

			snprintf(text, sizeof(text), "%s%d.%s%d.%s%d.%s%d", place == 0 ? "0" : "", numbers[0], place == 1 ? "0" : "", numbers[1], place == 2 ? "0" : "",
			         numbers[2], place == 3 ? "0" : "", numbers[3]);
			if(settings_is_address(text)) all = false;
		}
	}
	check(all && taken == 4 * 256, "address: at each of the four places every number from 0 to 255 is taken and every one from 256 to 999 is not, and none with a zero in front of it");

	// One byte of an address replaced by every other byte, one byte put in at every place, one byte left out
	all = true;
	taken = 0;
	for(at = 0; at <= (int)strlen(base); at++)
	{
		for(byte = 1; byte <= 255; byte++)
		{
			// Replaced
			if(at < (int)strlen(base))
			{
				strcpy(text, base);
				text[at] = (char)byte;
				if(settings_is_address(text) != model_address(text)) all = false;
				if(model_address(text)) taken++;
				compared++;
			}
			// Put in
			memcpy(text, base, (size_t)at);
			text[at] = (char)byte;
			strcpy(text + at + 1, base + at);
			if(settings_is_address(text) != model_address(text)) all = false;
			if(model_address(text)) taken++;
			compared++;
		}
		// Left out
		if(at < (int)strlen(base))
		{
			memcpy(text, base, (size_t)at);
			strcpy(text + at, base + at + 1);
			if(settings_is_address(text) != model_address(text)) all = false;
			if(model_address(text)) taken++;
			compared++;
		}
	}
	printf("  %d texts one byte away from 192.168.88.2, %d of them addresses\n", compared, taken);
	check(all && compared == 6387 && taken == 102, "address: of the 6387 texts that are one byte away from 192.168.88.2 - replaced, put in, left out - exactly the 102 are taken that the rule written a second time takes");
}

// A piece that is no number of an address, in many ways, and the numbers at the edges
static const char *const PIECES[] = {
	"0", "1", "9", "10", "88", "99", "100", "168", "199", "200", "249", "250", "255", "256", "260", "300", "999", "1000", "00", "01", "007", "", " ", "a", "-1", "1 ", "2e1",
};

static void test_address_walk(void)
{
	char text[96];
	bool all = true;
	int taken = 0, refused = 0;
	uint32_t state = 20261010u;
	int round;

	// Up to six pieces between dots, or other marks
	for(round = 0; round < 200000; round++)
	{
		static const char *const marks[] = {".", ".", ".", ".", ".", ".", ".", ".", ".", ".", ".", ".", ".", ".", ".", ".", ".", ".", ",", ""};
		size_t length = 0;
		int count, i;

		state = state * 1664525u + 1013904223u;
		// Mostly four pieces
		count = (state >> 8) % 10 < 7 ? 4 : 1 + (int)((state >> 12) % 6);
		for(i = 0; i < count; i++)
		{
			uint32_t piece, mark;

			state = state * 1664525u + 1013904223u;
			// Mostly one of the 13 pieces that are numbers of an address
			piece = (state >> 8) % 10 < 8 ? (state >> 12) % 13 : (state >> 12) % (sizeof(PIECES) / sizeof(PIECES[0]));
			state = state * 1664525u + 1013904223u;
			mark = (state >> 8) % (sizeof(marks) / sizeof(marks[0]));
			length += (size_t)snprintf(text + length, sizeof(text) - length, "%s%s", i > 0 ? marks[mark] : "", PIECES[piece]);
		}
		if(settings_is_address(text) != model_address(text))
		{
			if(all) printf("  \"%s\": the module says %d, the rule written a second time %d\n", text, settings_is_address(text), model_address(text));
			all = false;
		}
		if(model_address(text)) taken++;
		else refused++;
	}
	printf("  200000 texts of pieces: %d addresses, %d that are none\n", taken, refused);
	check(all, "address: 200000 texts put together of numbers, of pieces that are none, of dots and of other marks are taken exactly as the rule written a second time takes them");
	check(taken > 20000 && refused > 20000, "address: the texts put together hold more than 20000 addresses and more than 20000 texts that are none");
}

// From settings with the address 10.20.30.40: the text is taken, the five members stay, the address is this
static bool address_taken(const char *json, const char *expected)
{
	other();
	return take(json) && bad[0] == '\0' && address_is(expected) && box.settings.brightness == 33 && box.settings.night == 7 && box.settings.night_mode &&
	       !box.settings.reverse && box.settings.standby_s == 1234;
}

static void test_from_json_address(void)
{
	static const struct
	{
		const char *json;
		const char *what;
	} wrong[] = {
		{"{\"cerbo\":5}", "from_json: a number as the address is refused"},
		{"{\"cerbo\":3232258050}", "from_json: the address as one number is refused"},
		{"{\"cerbo\":192.168}", "from_json: a fraction as the address is refused"},
		{"{\"cerbo\":true}", "from_json: true as the address is refused"},
		{"{\"cerbo\":false}", "from_json: false as the address is refused: the source is switched off with the empty text"},
		{"{\"cerbo\":null}", "from_json: null as the address is refused: the source is switched off with the empty text"},
		{"{\"cerbo\":[\"192.168.88.2\"]}", "from_json: an address inside a list is refused"},
		{"{\"cerbo\":{\"host\":\"192.168.88.2\"}}", "from_json: an address inside an object is refused"},
		{"{\"cerbo\":\"192.168.88\"}", "from_json: three numbers as the address are refused"},
		{"{\"cerbo\":\"192.168.88.256\"}", "from_json: 256 in the address is refused"},
		{"{\"cerbo\":\"192.168.088.2\"}", "from_json: a leading zero in the address is refused"},
		{"{\"cerbo\":\"einstein\"}", "from_json: a name as the address is refused"},
		{"{\"cerbo\":\"venus.local\"}", "from_json: a name with a dot as the address is refused"},
		{"{\"cerbo\":\" \"}", "from_json: a blank as the address is refused"},
		{"{\"cerbo\":\"192.168.88.2 \"}", "from_json: an address with a blank behind it is refused"},
		{"{\"cerbo\":\"192.168.88.2:1883\"}", "from_json: an address with its port is refused: 17 bytes, more than the field holds"},
		{"{\"cerbo\":\"255.255.255.2555\"}", "from_json: a text of 16 bytes is refused: it does not fit the field with its zero"},
		{"{\"cerbo\":\"255.255.255.25.5\"}", "from_json: another text of 16 bytes is refused"},
		{"{\"cerbo\":\"192.168.188.1000\"}", "from_json: a text of 16 bytes that begins like an address is refused, not cut"},
		{"{\"cerbo\":\"192.168.88.2\\u0000\"}", "from_json: an address with an escaped zero behind it is refused"},
		{"{\"cerbo\":\"192.168.88.2\\n\"}", "from_json: an address with an escaped line end behind it is refused"},
		{"{\"cerbo\":\"1111111111111111111111111111111111111111111111111111111111111111\"}", "from_json: 64 digits as the address are refused"},
	};
	char fixture[256];
	size_t i;

	check(address_taken("{\"cerbo\":\"192.168.88.2\"}", "192.168.88.2"), "from_json: cerbo alone changes only the address of the Cerbo");
	check(address_taken("{\"cerbo\":\"\"}", ""), "from_json: an empty text takes the address away: the whole field is zero");
	check(address_taken("{\"cerbo\":\"1.2.3.4\"}", "1.2.3.4"),
	      "from_json: a shorter address replaces a longer one, and nothing of the longer one stays behind its zero");
	check(address_taken("{\"cerbo\":\"255.255.255.255\"}", "255.255.255.255"), "from_json: the longest address, of 15 bytes, fills the field up to its zero");
	check(address_taken("{\"cerbo\":\"0.0.0.0\"}", "0.0.0.0"), "from_json: 0.0.0.0 is taken as an address");
	check(address_taken("{}", "10.20.30.40") && address_taken("{\"brightness\":33}", "10.20.30.40") && address_taken("{\"later\":\"192.168.88.2\"}", "10.20.30.40"),
	      "from_json: a text that does not name the address leaves it as it is");
	set(33, 7, true, false, 1234);
	check(take("{\"cerbo\":\"192.168.88.2\"}") && address_is("192.168.88.2"), "from_json: settings without an address get one");
	set(33, 7, true, false, 1234);
	check(take("{\"cerbo\":\"\"}") && address_is(""), "from_json: an empty text leaves settings without an address without one");

	other();
	check(take("{\"brightness\":40,\"night\":10,\"night_mode\":false,\"reverse\":true,\"standby_s\":300,\"cerbo\":\"192.168.88.2\"}") && address_is("192.168.88.2") &&
	      box.settings.brightness == 40 && box.settings.night == 10 && !box.settings.night_mode && box.settings.reverse && box.settings.standby_s == 300 && bad[0] == '\0',
	      "from_json: a text with all six members sets all six");
	other();
	check(read_fixture("fixtures/settings_cerbo.json", fixture, sizeof(fixture)) && take(fixture) && address_is("192.168.88.2") && box.settings.brightness == 80 &&
	      box.settings.night == 25 && !box.settings.night_mode && !box.settings.reverse && box.settings.standby_s == 60,
	      "from_json: the stored text of a display with a Cerbo gives the defaults and its address");
	other();
	check(take("{\"cerbo\":\"192.168.88.2\",\"standby_s\":50,\"brightness\":52}") && address_is("192.168.88.2") && box.settings.brightness == 52 && box.settings.standby_s == 50,
	      "from_json: the address may come before the other members");
	other();
	check(take(" {\n\t\"cerbo\" :\r\n \"192.168.88.2\" } ") && address_is("192.168.88.2"), "from_json: whitespace around the address is allowed, none inside it");

	for(i = 0; i < sizeof(wrong) / sizeof(wrong[0]); i++) check(refused(wrong[i].json, "cerbo"), wrong[i].what);

	// The name counts as a whole, and with its escapes resolved
	check(address_taken("{\"Cerbo\":\"1.2.3.4\",\"cerb\":\"1.2.3.4\",\"cerbo \":\"1.2.3.4\",\"cerbo2\":\"1.2.3.4\",\"CERBO\":\"1.2.3.4\",\"gx\":\"1.2.3.4\",\"\":\"1.2.3.4\"}",
	                    "10.20.30.40"), "from_json: names that only look like cerbo are unknown");
	check(address_taken("{\"Cerbo\":5,\"cerbo2\":true,\"cerb\":null}", "10.20.30.40"), "from_json: under a name that only looks like cerbo anything is passed over");
	check(address_taken("{\"c\\u0065rbo\":\"192.168.88.2\"}", "192.168.88.2"), "from_json: the name cerbo written with an escape is that member");
	check(refused("{\"\\u0063erbo\":5}", "cerbo"), "from_json: a refused address under a name written with an escape is named without it");
	check(address_taken("{\"cerbo\":\"\\u0031\\u0039\\u0032.168.88.2\"}", "192.168.88.2"), "from_json: an address written with escapes is the address they write");
	check(address_taken("{\"z\":{\"cerbo\":\"1.2.3.4\"},\"y\":[{\"cerbo\":5}]}", "10.20.30.40"), "from_json: an address inside a member the format does not know is not read");

	// The same name twice
	check(address_taken("{\"cerbo\":\"1.2.3.4\",\"cerbo\":\"192.168.88.2\"}", "192.168.88.2"), "from_json: of two addresses the last counts");
	check(address_taken("{\"cerbo\":\"192.168.188.100\",\"cerbo\":\"\"}", ""),
	      "from_json: an address followed by the empty text is no address, and nothing of it stays in the field");
	check(address_taken("{\"cerbo\":\"\",\"cerbo\":\"192.168.88.2\"}", "192.168.88.2"), "from_json: the empty text followed by an address is that address");
	check(refused("{\"cerbo\":\"einstein\",\"cerbo\":\"192.168.88.2\"}", "cerbo"), "from_json: a bad address is not healed by a good one behind it");
	check(refused("{\"cerbo\":\"192.168.88.2\",\"cerbo\":7}", "cerbo"), "from_json: a good address followed by a bad one is refused");

	// The first bad member in the order of the text is named, and nothing is taken
	check(refused("{\"cerbo\":\"x\",\"brightness\":0}", "cerbo"), "from_json: a bad address before a bad brightness is the one named");
	check(refused("{\"brightness\":0,\"cerbo\":\"x\"}", "brightness"), "from_json: a bad brightness before a bad address is the one named");
	check(refused("{\"brightness\":50,\"night\":50,\"cerbo\":\"192.168.88.2.\",\"standby_s\":9}", "cerbo"),
	      "from_json: a bad address between good members: nothing of the text is taken");
	check(refused("{\"cerbo\":\"192.168.88.2\",\"standby_s\":3601}", "standby_s"), "from_json: a good address before a bad member: the address is not taken either");
	check(refused("{\"cerbo\":\"192.168.88.2\",}", ""), "from_json: broken JSON behind a good address is refused as a whole");
	check(refused("{\"cerbo\":\"192.168.88\",}", ""), "from_json: broken JSON behind a bad address names no member");
}

static void test_to_json_address(void)
{
	static const char stored[] = "{\"brightness\":80,\"night\":25,\"night_mode\":false,\"reverse\":false,\"standby_s\":60}";
	static const char with_cerbo[] = "{\"brightness\":80,\"night\":25,\"night_mode\":false,\"reverse\":false,\"standby_s\":60,\"cerbo\":\"192.168.88.2\"}";
	static const char longest[] = "{\"brightness\":255,\"night\":255,\"night_mode\":false,\"reverse\":false,\"standby_s\":65535,\"cerbo\":\"255.255.255.255\"}";
	static const struct
	{
		const char *field;      // the 16 bytes of the field
		const char *what;
	} none[] = {
		{"einstein\0\0\0\0\0\0\0\0", "to_json: a name in the field of the address is not written"},
		{"192.168.88\0\0\0\0\0\0", "to_json: three numbers in the field of the address are not written"},
		{"192.168.88.256\0\0", "to_json: an address with 256 in the field is not written"},
		{"192.168.088.2\0\0\0", "to_json: an address with a leading zero in the field is not written"},
		{"1.2.3.4\"}\0\0\0\0\0\0\0", "to_json: an address with a quote behind it in the field is not written: the text stays JSON"},
		{" \0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", "to_json: a blank in the field of the address is not written"},
		{"1111111111111111", "to_json: sixteen digits without a zero in the field are not written"},
		{"255.255.255.2551", "to_json: an address with a digit where its zero belongs is not written"},
		{"1.1.1.0000000000", "to_json: a field without a zero that ends in zeros is not written"},
		{"................", "to_json: sixteen dots without a zero in the field are not written"},
		{"\0" "92.168.188.100", "to_json: a field that begins with its zero is empty, whatever stands behind it"},
	};
	unsigned char room[SETTINGS_JSON_SIZE + 16];
	char out[SETTINGS_JSON_SIZE];
	char fixture[256];
	bool fits, empty, behind;
	size_t size, i;
	int count;

	settings_defaults(&box.settings);
	address("192.168.88.2");
	remember();
	check(settings_to_json(&box.settings, out, sizeof(out)) == 101 && strcmp(out, with_cerbo) == 0,
	      "to_json: the defaults with the address of a Cerbo give the second text of the header, 101 bytes: the sixth member behind the five");
	check(read_fixture("fixtures/settings_cerbo.json", fixture, sizeof(fixture)) && strcmp(out, fixture) == 0, "to_json: the defaults with an address give the fixture, byte for byte");
	check(unchanged(), "to_json: settings with an address are only read");
	address("");
	check(settings_to_json(&box.settings, out, sizeof(out)) == 78 && strcmp(out, stored) == 0 && strstr(out, "cerbo") == NULL,
	      "to_json: without an address the text is the one of five members, byte for byte: the sixth is not written, not as an empty one either");
	address("1.2.3.4");
	check(settings_to_json(&box.settings, out, sizeof(out)) == 96 &&
	      strcmp(out, "{\"brightness\":80,\"night\":25,\"night_mode\":false,\"reverse\":false,\"standby_s\":60,\"cerbo\":\"1.2.3.4\"}") == 0,
	      "to_json: a short address is written whole and nothing behind it");
	address("0.0.0.0");
	check(settings_to_json(&box.settings, out, sizeof(out)) == 96 && strstr(out, ",\"cerbo\":\"0.0.0.0\"}") != NULL, "to_json: 0.0.0.0 is written as an address");

	// What does not come from settings_from_json(): only an address is ever written
	for(i = 0; i < sizeof(none) / sizeof(none[0]); i++)
	{
		settings_defaults(&box.settings);
		memcpy(box.settings.cerbo, none[i].field, SETTINGS_CERBO_SIZE);
		check(settings_to_json(&box.settings, out, sizeof(out)) == 78 && strcmp(out, stored) == 0 && guards_intact(), none[i].what);
	}

	// The longest text there is
	set(255, 255, false, false, 65535);
	address("255.255.255.255");
	check(strlen(longest) == 109 && read_fixture("fixtures/settings_longest_cerbo.json", fixture, sizeof(fixture)) && strcmp(longest, fixture) == 0,
	      "the longest text with an address has 109 bytes and is the fixture");
	check(settings_to_json(&box.settings, out, sizeof(out)) == 109 && strcmp(out, longest) == 0, "to_json: the largest numbers and the longest address give the longest text");
	check(SETTINGS_JSON_SIZE >= 109 + 1, "SETTINGS_JSON_SIZE is enough for the longest text with an address and its zero");
	fits = true;
	empty = true;
	behind = true;
	for(size = 0; size <= SETTINGS_JSON_SIZE; size++)
	{
		int length;

		memset(room, GUARD, sizeof(room));
		length = settings_to_json(&box.settings, (char *)room, size);
		if(size >= 110)
		{
			if(length != 109 || memcmp(room, longest, 110) != 0) fits = false;
		}
		else if(length != -1 || (size > 0 && room[0] != '\0')) empty = false;

		for(i = size; i < sizeof(room); i++)
		{
			if(room[i] != GUARD) behind = false;
		}
	}
	check(fits, "to_json: with 110 bytes and more the text of 109 bytes is written with its zero and its length returned");
	check(empty, "to_json: with every room below 110 bytes the answer is -1 and an empty string, also where the five members alone would fit");
	check(behind, "to_json: nothing is written behind the room of a text with an address");

	// The reader of the project agrees that it is JSON
	settings_defaults(&box.settings);
	address("192.168.88.2");
	settings_to_json(&box.settings, out, sizeof(out));
	count = json_parse(out, strlen(out), large_work, 256);
	check(count == 13 && large_work[0].type == JSON_OBJECT && large_work[0].size == 6 && json_text_is(out, &large_work[12], "192.168.88.2"),
	      "to_json: the text with an address is a JSON object of six members, 13 tokens, the address the last of them");
	check(strchr(out, ' ') == NULL && strchr(out, '\n') == NULL, "to_json: the text with an address has no whitespace");
}

static void test_round_trip_address(void)
{
	static const char *const addresses[] = {"", "192.168.88.2", "0.0.0.0", "255.255.255.255", "1.2.3.4", "10.0.0.138", "192.168.188.100"};
	static const int percents[] = {5, 80, 100};
	static const int standbys[] = {0, 60, 3600};
	char out[SETTINGS_JSON_SIZE];
	bool all = true;
	size_t a, other_address, b, s;
	int flags;

	for(a = 0; a < sizeof(addresses) / sizeof(addresses[0]); a++)
	{
		for(other_address = 0; other_address < sizeof(addresses) / sizeof(addresses[0]); other_address++)
		{
			for(b = 0; b < sizeof(percents) / sizeof(percents[0]); b++)
			{
				for(s = 0; s < sizeof(standbys) / sizeof(standbys[0]); s++)
				{
					for(flags = 0; flags < 4; flags++)
					{
						set(percents[b], percents[2 - b], (flags & 1) != 0, (flags & 2) != 0, standbys[s]);
						address(addresses[a]);
						if(settings_to_json(&box.settings, out, sizeof(out)) < 0) all = false;
						// Read into settings that differ in every member; a text without an address leaves theirs
						set(50, 51, (flags & 1) == 0, (flags & 2) == 0, standbys[s] + 7);
						address(addresses[other_address]);
						if(!take(out) || box.settings.brightness != percents[b] || box.settings.night != percents[2 - b] ||
						   box.settings.night_mode != ((flags & 1) != 0) || box.settings.reverse != ((flags & 2) != 0) || box.settings.standby_s != standbys[s] ||
						   !address_is(addresses[a][0] != '\0' ? addresses[a] : addresses[other_address])) all = false;
					}
				}
			}
		}
	}
	check(all, "round trip: 1764 settings with and without an address are written and read back unchanged - a text without an address leaves the one that was there");
}

static void test_backlight(void)
{
	static const struct
	{
		int brightness, night;
		bool night_mode;
		int standby_s;
		bool showing;
		uint64_t idle_ms;
		int expected;
		const char *what;
	} cases[] = {
		{80, 25, false, 60, false, 0, 80, "backlight: by day, input just now: the brightness"},
		{80, 25, true, 60, false, 0, 25, "backlight: in night mode, input just now: the night brightness"},
		{80, 25, false, 60, false, 59999, 80, "backlight: nothing to show, idle 1 ms less than the standby time: on"},
		{80, 25, false, 60, false, 60000, 0, "backlight: nothing to show, idle exactly the standby time: dark"},
		{80, 25, false, 60, false, 60001, 0, "backlight: nothing to show, idle 1 ms more than the standby time: dark"},
		{80, 25, true, 60, false, 59999, 25, "backlight: night mode, nothing to show, idle 1 ms less than the standby time: the night brightness"},
		{80, 25, true, 60, false, 60000, 0, "backlight: night mode, nothing to show, idle exactly the standby time: dark"},
		{80, 25, false, 60, true, 60000, 80, "backlight: something to show, idle the standby time: stays on"},
		{80, 25, false, 60, true, UINT64_MAX, 80, "backlight: something to show, idle for the longest time: stays on"},
		{80, 25, true, 60, true, 60000, 25, "backlight: night mode, something to show, idle the standby time: the night brightness"},
		{80, 25, false, 0, false, 0, 80, "backlight: standby 0 means never: on right after an input"},
		{80, 25, false, 0, false, 60000, 80, "backlight: standby 0 means never: on after a minute"},
		{80, 25, false, 0, false, UINT64_MAX, 80, "backlight: standby 0 means never: on after the longest time"},
		{80, 25, true, 0, false, UINT64_MAX, 25, "backlight: standby 0 in night mode: the night brightness after the longest time"},
		{80, 25, false, 1, false, 999, 80, "backlight: standby 1 s, idle 999 ms: on"},
		{80, 25, false, 1, false, 1000, 0, "backlight: standby 1 s, idle 1000 ms: dark"},
		{80, 25, false, 3600, false, 3599999, 80, "backlight: standby 3600 s, idle 1 ms less: on"},
		{80, 25, false, 3600, false, 3600000, 0, "backlight: standby 3600 s, idle exactly that: dark"},
		{80, 25, false, 65535, false, 65534999, 80, "backlight: standby 65535 s, idle 1 ms less: on"},
		{80, 25, false, 65535, false, 65535000, 0, "backlight: standby 65535 s, idle exactly that: dark"},
		{80, 25, false, 60, false, UINT64_MAX, 0, "backlight: nothing to show, idle for the longest time: dark"},
		{80, 25, false, 60, false, ((uint64_t)1 << 32) + 5, 0, "backlight: an idle time beyond 32 bit whose low part is small: dark"},
		{80, 25, false, 60, false, (uint64_t)1 << 32, 0, "backlight: an idle time of exactly 2^32 ms: dark"},
		{80, 25, false, 300, false, 65536 + 5000, 80, "backlight: standby 300 s, idle 70 s: on, the time is not cut to 16 bit"},

		// What a member can hold beyond its range
		{5, 25, false, 60, true, 0, 5, "backlight: brightness 5, the smallest: 5"},
		{4, 25, false, 60, true, 0, 5, "backlight: brightness 4 counts as the smallest, 5"},
		{0, 25, false, 60, true, 0, 5, "backlight: brightness 0 is no dark display, it counts as 5"},
		{6, 25, false, 60, true, 0, 6, "backlight: brightness 6: 6"},
		{99, 25, false, 60, true, 0, 99, "backlight: brightness 99: 99"},
		{100, 25, false, 60, true, 0, 100, "backlight: brightness 100, the largest: 100"},
		{101, 25, false, 60, true, 0, 100, "backlight: brightness 101 counts as the largest, 100"},
		{255, 25, false, 60, true, 0, 100, "backlight: brightness 255 counts as 100"},
		{80, 5, true, 60, true, 0, 5, "backlight: night 5, the smallest: 5"},
		{80, 4, true, 60, true, 0, 5, "backlight: night 4 counts as 5"},
		{80, 0, true, 60, true, 0, 5, "backlight: night 0 is no dark display, it counts as 5"},
		{80, 100, true, 60, true, 0, 100, "backlight: night 100: 100"},
		{80, 101, true, 60, true, 0, 100, "backlight: night 101 counts as 100"},
		{80, 255, true, 60, true, 0, 100, "backlight: night 255 counts as 100"},
		{0, 0, false, 60, false, 60000, 0, "backlight: the standby is dark whatever the brightness is"},
		{255, 255, true, 60, false, 60000, 0, "backlight: the standby is dark also with a brightness beyond the range"},
	};
	static const int percents[] = {0, 4, 5, 6, 50, 99, 100, 101, 255};
	static const int standbys[] = {0, 1, 2, 60, 3600, 3601, 65535};
	static const int64_t offsets[] = {-1000, -1, 0, 1, 1000};
	size_t i, b, n, s, o;
	int flags, value, expected_on;
	bool all = true;

	for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		set(cases[i].brightness, cases[i].night, cases[i].night_mode, false, cases[i].standby_s);
		check(settings_backlight(&box.settings, cases[i].showing, cases[i].idle_ms) == cases[i].expected, cases[i].what);
	}

	set(80, 25, false, true, 60);
	remember();
	check(settings_backlight(&box.settings, false, 0) == 80 && settings_backlight(&box.settings, false, 60000) == 0 && unchanged(),
	      "backlight: the direction of the knob does not matter, the settings are only read");

	// Every combination around the limits, against the rule written down once more: first the time, then the mode
	for(b = 0; b < sizeof(percents) / sizeof(percents[0]); b++)
	{
		for(n = 0; n < sizeof(percents) / sizeof(percents[0]); n++)
		{
			for(s = 0; s < sizeof(standbys) / sizeof(standbys[0]); s++)
			{
				for(o = 0; o < sizeof(offsets) / sizeof(offsets[0]); o++)
				{
					for(flags = 0; flags < 8; flags++)
					{
						bool night_mode = (flags & 1) != 0, showing = (flags & 2) != 0, reverse = (flags & 4) != 0;
						int64_t idle = (int64_t)standbys[s] * 1000 + offsets[o];
						int wanted = night_mode ? percents[n] : percents[b];
						int expected;

						if(idle < 0) continue;
						if(wanted <= 5) wanted = 5;
						if(wanted >= 100) wanted = 100;
						expected = wanted;
						if(!showing && standbys[s] > 0 && idle / 1000 >= standbys[s]) expected = 0;

						set(percents[b], percents[n], night_mode, reverse, standbys[s]);
						if(settings_backlight(&box.settings, showing, (uint64_t)idle) != expected) all = false;
					}
				}
			}
		}
	}
	check(all, "backlight: every combination of 9 brightnesses, 9 night values, 7 standby times, 5 idle times around them, mode, showing and direction");

	// Every value a member can hold
	all = true;
	for(value = 0; value <= 255; value++)
	{
		int expected = value < 5 ? 5 : value > 100 ? 100 : value;

		set(value, 255 - value, false, false, 60);
		if(settings_backlight(&box.settings, false, 59999) != expected || settings_backlight(&box.settings, true, 60000) != expected) all = false;
		if(settings_backlight(&box.settings, false, 60000) != 0) all = false;
		set(255 - value, value, true, false, 60);
		if(settings_backlight(&box.settings, false, 59999) != expected || settings_backlight(&box.settings, true, 60000) != expected) all = false;
		if(settings_backlight(&box.settings, false, 60000) != 0) all = false;
	}
	check(all, "backlight: every brightness and every night value from 0 to 255 is passed on from 5 to 100 and counts as the nearest limit else");
	all = true;
	for(value = 0; value <= 255; value++)
	{
		int second;

		for(second = 0; second <= 255; second++)
		{
			int day = value < 5 ? 5 : value > 100 ? 100 : value;
			int night = second < 5 ? 5 : second > 100 ? 100 : second;

			set(value, second, false, (value + second) % 3 == 0, 60);
			if(settings_backlight(&box.settings, true, 60000) != day || settings_backlight(&box.settings, false, 59999) != day) all = false;
			box.settings.night_mode = true;
			if(settings_backlight(&box.settings, true, 60000) != night || settings_backlight(&box.settings, false, 59999) != night) all = false;
		}
	}
	check(all, "backlight: every pair of brightness and night value: by day only the one counts, in night mode only the other");
	all = true;
	for(value = 0; value <= 255; value++)
	{
		static const int times[] = {0, 1, 61, 3600, 40000, 65535};
		int second;
		size_t t;

		for(second = 0; second <= 255; second++)
		{
			int day = value < 5 ? 5 : value > 100 ? 100 : value;
			int night = second < 5 ? 5 : second > 100 ? 100 : second;

			for(t = 0; t < sizeof(times) / sizeof(times[0]); t++)
			{
				set(value, second, false, (value + second) % 2 == 0, times[t]);
				if(settings_backlight(&box.settings, true, UINT64_MAX) != day || settings_backlight(&box.settings, false, 0) != day) all = false;
				if(settings_backlight(&box.settings, false, UINT64_MAX) != (times[t] == 0 ? day : 0)) all = false;
				box.settings.night_mode = true;
				if(settings_backlight(&box.settings, true, UINT64_MAX) != night || settings_backlight(&box.settings, false, 0) != night) all = false;
				if(settings_backlight(&box.settings, false, UINT64_MAX) != (times[t] == 0 ? night : 0)) all = false;
			}
		}
	}
	check(all, "backlight: every pair of brightness and night value with six standby times: the standby time changes nothing but when the screen goes dark");
	all = true;
	for(value = 0; value < 64; value++)
	{
		static const int times[] = {0, 1, 60, 3600, 65535};
		uint64_t power = (uint64_t)1 << value;
		uint64_t idles[3];
		size_t t, k;

		idles[0] = power - 1;
		idles[1] = power;
		idles[2] = power + 1;
		for(t = 0; t < sizeof(times) / sizeof(times[0]); t++)
		{
			for(k = 0; k < 3; k++)
			{
				bool dark = times[t] != 0 && idles[k] / 1000 >= (uint64_t)times[t];

				set(80, 25, value % 2 == 0, false, times[t]);
				expected_on = value % 2 == 0 ? 25 : 80;
				if(settings_backlight(&box.settings, false, idles[k]) != (dark ? 0 : expected_on) || settings_backlight(&box.settings, true, idles[k]) != expected_on) all = false;
			}
		}
	}
	check(all, "backlight: idle times around every power of two up to 2^63, with five standby times: dark from the standby time on, on while there is something to show");
	all = true;
	for(value = 1; value <= 65535; value++)
	{
		uint64_t limit = (uint64_t)value * 1000;

		set(80, 25, value % 2 == 0, false, value);
		expected_on = value % 2 == 0 ? 25 : 80;
		if(settings_backlight(&box.settings, false, limit - 1) != expected_on || settings_backlight(&box.settings, false, limit) != 0) all = false;
		if(settings_backlight(&box.settings, true, limit) != expected_on || settings_backlight(&box.settings, false, 0) != expected_on) all = false;
		box.settings.reverse = true;
		if(settings_backlight(&box.settings, false, limit - 1) != expected_on || settings_backlight(&box.settings, false, limit) != 0) all = false;
		if(settings_backlight(&box.settings, true, limit) != expected_on || settings_backlight(&box.settings, false, 0) != expected_on) all = false;
	}
	check(all, "backlight: every standby time from 1 to 65535 s, with the knob in both directions: on until 1 ms before it, dark from it on, on while there is something to show");
}

/*
 * The rules of settings_from_json written a second time, in another shape: the text is made member by
 * member from a list that says of each member what it is, and the outcome is told from the list alone.
 */

#define LIST_MAX 10

typedef struct
{
	int name;           // 0 to 4 the known ones in the order of the header, 5 an unknown one, 6 the address of the Cerbo
	bool good;
	int number;         // of a good number
	bool flag;          // of a good boolean
	char address[SETTINGS_CERBO_SIZE];  // of a good address, empty for the text that takes it away
} listed_t;

static const char *const known[5] = {"brightness", "night", "night_mode", "reverse", "standby_s"};
static uint32_t random_state;

/*
 * Never two rolls among the arguments of one call: C leaves open which argument is worked out first, gcc
 * takes the last and clang the first, and the texts of the CI were others than the ones on a Mac. Where a
 * call needs two, they are rolled before it, in the order gcc had: the last argument first.
 */
static uint32_t rnd(uint32_t below)
{
	random_state = random_state * 1664525u + 1013904223u;
	return (random_state >> 8) % below;
}

// The value of one member as text; the list entry says what the reader has to make of it
static void write_value(char *to, size_t size, listed_t *entry)
{
	static const char *const wrong_number[] = {"\"50\"", "true", "false", "null", "50.0", "5e1", "[50]", "{\"brightness\":50}", "-7", "100000"};
	static const char *const wrong_flag[] = {"0", "1", "\"true\"", "\"false\"", "null", "[true]", "{}"};
	static const char *const anything[] = {"0", "-1", "\"x\"", "true", "null", "[1,[2]]", "{\"night\":0,\"reverse\":3}", "1e9"};
	uint32_t pick = rnd(10);

	entry->good = true;
	entry->number = 0;
	entry->flag = false;
	memset(entry->address, 0, sizeof(entry->address));

	if(entry->name == 5)
	{
		snprintf(to, size, "%s", anything[rnd(8)]);
	}
	else if(entry->name == 6)
	{
		static const char *const wrong_address[] = {"5", "true", "null", "[\"1.2.3.4\"]", "\"einstein\"", "\"1.2.3\"", "\"1.2.3.4.5\"", "\"256.1.1.1\"", "\"01.1.1.1\"",
		                                            "\"1.2.3.4 \"", "\"192.168.188.1000\"", "{\"cerbo\":\"1.2.3.4\"}"};
		// Rolled one after the other: see rnd()
		int first = (int)rnd(256);
		int second = (int)rnd(256);
		int third = (int)rnd(256);
		int fourth = (int)rnd(256);
		// Room for four numbers of any size: the compiler does not know that these have three digits at most
		char made[48];

		entry->good = pick < 7;
		// Two in ten take the address away
		if(pick >= 2 && pick < 7)
		{
			snprintf(made, sizeof(made), "%d.%d.%d.%d", first, second, third, fourth);
			memcpy(entry->address, made, strlen(made));
		}
		if(entry->good) snprintf(to, size, "\"%s\"", entry->address);
		else snprintf(to, size, "%s", wrong_address[rnd(12)]);
	}
	else if(entry->name == 2 || entry->name == 3)
	{
		entry->good = pick < 7;
		entry->flag = rnd(2) == 1;
		snprintf(to, size, "%s", entry->good ? (entry->flag ? "true" : "false") : wrong_flag[rnd(7)]);
	}
	else
	{
		int low = entry->name == 4 ? 0 : 5;
		int high = entry->name == 4 ? 3600 : 100;

		entry->good = pick < 7;
		if(pick < 2) entry->number = low;
		else if(pick < 4) entry->number = high;
		else if(pick < 7) entry->number = low + (int)rnd((uint32_t)(high - low + 1));
		else if(pick == 7) entry->number = low - 1;
		else if(pick == 8) entry->number = high + 1;

		if(pick < 9) snprintf(to, size, "%d", entry->number);
		else snprintf(to, size, "%s", wrong_number[rnd(10)]);
	}
}

static int seen_taken, seen_member, seen_whole, seen_twice, seen_unknown, seen_addresses, seen_no_address, seen_bad_address;

// names: 6 for texts of the five members and unknown ones, 7 for texts that name the address of the Cerbo as well
static bool model_walk(uint32_t seed, int steps, int names)
{
	listed_t list[LIST_MAX];
	settings_t model;
	int step;

	random_state = seed;
	other();
	model = box.settings;

	for(step = 0; step < steps; step++)
	{
		int count = (int)rnd(LIST_MAX + 1);
		bool broken = rnd(12) == 0;
		size_t length = 0;
		const char *expected_bad = "";
		bool expected = !broken;
		bool result;
		int i, name;

		length += (size_t)snprintf(text + length, sizeof(text) - length, "%s{", rnd(4) == 0 ? " " : "");
		for(i = 0; i < count; i++)
		{
			char value[48];

			list[i].name = (int)rnd((uint32_t)names);
			write_value(value, sizeof(value), &list[i]);
			if(list[i].name == 5)
			{
				length += (size_t)snprintf(text + length, sizeof(text) - length, "%s\"u%u\":%s", i ? "," : "", (unsigned)rnd(3), value);
				seen_unknown++;
			}
			else
			{
				const char *behind = rnd(5) == 0 ? " " : "";
				const char *before = rnd(5) == 0 ? "\n" : "";

				length += (size_t)snprintf(text + length, sizeof(text) - length, "%s%s\"%s\"%s:%s", i ? "," : "", before,
				                           list[i].name == 6 ? "cerbo" : known[list[i].name], behind, value);
			}
		}
		// A text that is no JSON object at all, in three ways
		if(broken)
		{
			uint32_t how = rnd(3);

			if(how == 0) length += (size_t)snprintf(text + length, sizeof(text) - length, "}x");
			else if(how == 1) length += (size_t)snprintf(text + length, sizeof(text) - length, ",}");
		}
		else length += (size_t)snprintf(text + length, sizeof(text) - length, "}");

		// The model: the first bad member decides; if there is none, the last one of every name counts
		if(!broken)
		{
			for(i = 0; i < count && expected; i++)
			{
				if(list[i].name != 5 && !list[i].good)
				{
					expected = false;
					expected_bad = list[i].name == 6 ? "cerbo" : known[list[i].name];
					if(list[i].name == 6) seen_bad_address++;
				}
			}
		}
		if(expected)
		{
			for(name = 0; name < 5; name++)
			{
				int found = 0;

				for(i = count - 1; i >= 0; i--)
				{
					if(list[i].name != name) continue;
					if(found++ > 0) continue;
					if(name == 0) model.brightness = (uint8_t)list[i].number;
					if(name == 1) model.night = (uint8_t)list[i].number;
					if(name == 2) model.night_mode = list[i].flag;
					if(name == 3) model.reverse = list[i].flag;
					if(name == 4) model.standby_s = (uint16_t)list[i].number;
				}
				if(found > 1) seen_twice++;
			}
			// The address: the last one named, as a field that is zero behind its text
			for(i = count - 1; i >= 0; i--)
			{
				if(list[i].name != 6) continue;

				memcpy(model.cerbo, list[i].address, sizeof(model.cerbo));
				if(model.cerbo[0] != '\0') seen_addresses++;
				else seen_no_address++;
				break;
			}
			seen_taken++;
		}
		else if(broken) seen_whole++;
		else seen_member++;

		remember();
		strcpy(bad, UNTOLD);
		result = settings_from_json(&box.settings, text, length, bad, sizeof(bad), large_work, 256);
		// The address the settings have to hold now, byte for byte
		memcpy(held_address, model.cerbo, sizeof(held_address));
		if(result != expected || strcmp(bad, expected_bad) != 0 || (!expected && !unchanged()) ||
		   !is(model.brightness, model.night, model.night_mode, model.reverse, model.standby_s))
		{
			printf("  seed %lu, step %d: %s\n  module %d \"%s\" %d %d %d %d %d \"%.15s\"\n  model  %d \"%s\" %d %d %d %d %d \"%.15s\"\n", (unsigned long)seed, step, text,
			       result, bad, box.settings.brightness, box.settings.night, box.settings.night_mode, box.settings.reverse, box.settings.standby_s, box.settings.cerbo,
			       expected, expected_bad, model.brightness, model.night, model.night_mode, model.reverse, model.standby_s, model.cerbo);
			return false;
		}
	}
	return true;
}

static void test_model(void)
{
	char what[160];
	uint32_t seed;

	for(seed = 1; seed <= 40; seed++)
	{
		snprintf(what, sizeof(what), "model: 1000 random texts with seed %lu are taken or refused as the rules say", (unsigned long)seed);
		check(model_walk(seed, 1000, 6), what);
	}
	printf("  taken %d, a member refused %d, refused as a whole %d, a name twice %d, unknown members %d\n",
	       seen_taken, seen_member, seen_whole, seen_twice, seen_unknown);
	check(seen_taken > 1000 && seen_member > 1000 && seen_whole > 1000 && seen_twice > 1000 && seen_unknown > 1000,
	      "model: the walks contain texts that are taken, refused for a member, refused as a whole, names twice and unknown members");
	check(seen_addresses == 0 && seen_no_address == 0 && seen_bad_address == 0, "model: none of these 40 walks names the address of a Cerbo: it stays what it was, byte for byte, in every one of them");

	// The same with the sixth member among the names
	for(seed = 41; seed <= 60; seed++)
	{
		snprintf(what, sizeof(what), "model: 1000 random texts that name the address of the Cerbo as well, seed %lu, are taken or refused as the rules say", (unsigned long)seed);
		check(model_walk(seed, 1000, 7), what);
	}
	printf("  with the address: an address taken %d times, taken away %d times, a bad one the first bad member %d times\n", seen_addresses, seen_no_address, seen_bad_address);
	check(seen_addresses > 1000 && seen_no_address > 300 && seen_bad_address > 1000,
	      "model: these 20 walks contain texts that set an address, that take it away and that are refused for it");
}

int main(void)
{
	memset(&work[SETTINGS_TOKENS], 0x5A, 2 * sizeof(work[0]));

	test_constants();
	if(!in_child(test_ordinary, "every function with ordinary input: no crash, no hang")) return test_end();
	test_defaults();
	test_to_json();
	test_stale_tokens();
	test_from_json_members();
	test_from_json_ranges();
	test_from_json_every_number();
	test_from_json_together();
	test_from_json_whole();
	test_from_json_large();
	test_is_address();
	test_address_walk();
	test_from_json_address();
	test_to_json_address();
	test_round_trip_address();
	in_child(test_bad, "the name of the bad member in every room: no crash");
	in_child(test_null, "calls without a buffer for the name or the text: no crash");
	test_tokens();
	test_round_trip();
	test_backlight();
	in_child(test_model, "model: 60 walks of 1000 random texts each: no crash, no hang");

	check(work[SETTINGS_TOKENS].start == 0x5A5A5A5A && work[SETTINGS_TOKENS + 1].start == 0x5A5A5A5A,
	      "no call wrote behind the SETTINGS_TOKENS tokens it was given");
	return test_end();
}
