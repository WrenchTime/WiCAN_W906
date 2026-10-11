/*
 * The device of the tests of the app: the harness of test_app.c and test_app_web.c. It drives app.h exactly
 * as the platform will drive it and stands for everything below it:
 *   - a clock in steps of 20 ms; the time of the display starts at 0 with every start of the app
 *   - the task of the screen: a reading of the switch with every step, app_tick() every 200 ms,
 *     app_platform() every second
 *   - the task of the network: link_next() carried out against a WiFi of the test, poll_prepare() answered by
 *     an adapter of the test (tools/w906/API.md written once more, as in test_poll.c), app_net() where app.h
 *     asks for it
 *   - the task of the second source: cerbo_next() carried out against a broker of the test that stands for
 *     the Cerbo GX (MQTT 3.1.1 as mqtt_harness.h writes it for a broker), as app.h says the platform does it.
 *     A display that has no address of a Cerbo gets no order from it: the stories that set none run as if
 *     there were no such task
 *   - after every step app_take_events() carried out against a flash of the test, so that the app can be
 *     started again with what was stored
 * What the driver would see is app_scene() as the text of scene_dump(), compared with hand-written files
 * fixtures/app_<name>.txt, and app_backlight().
 *
 * Most stories start in drive(): the display has the network "Werkstatt" stored with the address of the
 * adapter, is bound to it, and the adapter runs since 100 s with the ignition on and the engine off (W906
 * profile, the values of tools/w906/fixtures/autopid_data_ignition_on.json). The first round is answered at
 * 0, the next ones every second. A read gets the number 42, the clear behind it 43.
 *
 * The release of the web interface: every story begins with a display whose owner switched "Dauerhaft frei"
 * off (factory() stores WEB_TIMED, the settings with web_open false), unless it says otherwise. That is the
 * display every story was written for - a change through the web needs the release of ten minutes, given at
 * the knob - and the stories go on telling of it, each as it did. The display as it comes, released for
 * good, is the one of the stories that call as_delivered() behind factory() or garage().
 *
 * A header with code, for one test program each: the Makefile does not name it, so a test is built anew
 * with "rm -f test_app test_app_web" after a change here.
 */
#ifndef __APP_HARNESS_H__
#define __APP_HARNESS_H__

#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "test.h"
// The packets a broker sends and what it reads. The header names a type told_t for the tests of the reader,
// and test_app_web.c has one of that name of its own: here it goes by another name.
#define told_t      mqtt_told_t
#include "mqtt_harness.h"
#undef told_t
#include "app.h"

// Other than 0: before every call into the display the stack below is filled with this byte (dirty_stack()
// in test.h). Two tellings of one story over two different bytes part where the display keeps a byte that
// only lay on the stack.
static unsigned char stack_dirt;

static void dirt(void)
{
	if(stack_dirt != 0) dirty_stack(stack_dirt);
}

// Every call of the display from a test goes through here. The harness lays the fill itself before the
// calls it makes into the parts of the display (step()).
#define app_init(...)           (dirt(), app_init(__VA_ARGS__))
#define app_platform(...)       (dirt(), app_platform(__VA_ARGS__))
#define app_temperature(...)    (dirt(), app_temperature(__VA_ARGS__))
#define app_net(...)            (dirt(), app_net(__VA_ARGS__))
#define app_host(...)           (dirt(), app_host(__VA_ARGS__))
#define app_encoder(...)        (dirt(), app_encoder(__VA_ARGS__))
#define app_button(...)         (dirt(), app_button(__VA_ARGS__))
#define app_tap(...)            (dirt(), app_tap(__VA_ARGS__))
#define app_swipe(...)          (dirt(), app_swipe(__VA_ARGS__))
#define app_tick(...)           (dirt(), app_tick(__VA_ARGS__))
#define app_do(...)             (dirt(), app_do(__VA_ARGS__))
#define app_choose_layout(...)  (dirt(), app_choose_layout(__VA_ARGS__))
#define app_take_events(...)    (dirt(), app_take_events(__VA_ARGS__))
#define app_world(...)          (dirt(), app_world(__VA_ARGS__))
#define app_scene(...)          (dirt(), app_scene(__VA_ARGS__))
#define app_backlight(...)      (dirt(), app_backlight(__VA_ARGS__))
#define app_busy(...)           (dirt(), app_busy(__VA_ARGS__))

#define COUNT(a)    ((int)(sizeof(a) / sizeof((a)[0])))
#define STEP_MS     20u
#define FILL        0xAA
#define GUARD       64

#define OWN         "a1b2c3d4e5f6"
#define OTHER       "0123456789ab"
#define OWN_AP      "WiCAN_a1b2c3d4e5f6"    // the access point of the adapter
#define BOOT        1234567890u
#define VERSION     "0.1.0"
#define GIT         "display-v0.1.0-3-g1a2b3c4"

#define PICKUP_MS   300u    // an accepted request waits this long for the AutoPID task
#define SCAN_MS     3000u   // from there to the end of the scan
#define PASS_MS     400u    // one polling pass
#define SCAN_STEPS  3u

#define ALL_STORES  (APP_EVENT_STORE_SETTINGS | APP_EVENT_STORE_WIFI | APP_EVENT_STORE_BOUND | APP_EVENT_STORE_LAYOUT | \
                     APP_EVENT_ERASE_LAYOUT | APP_EVENT_STORE_CATALOG | APP_EVENT_STORE_OLD)

#define GX_HOST     "192.168.88.2"      // where the broker of the Cerbo listens
#define GX_ELSE     "192.168.88.9"      // an address at which nothing listens
#define GX_BOOT     1836452071u         // the number of the run of its flow
#define GX_AGE_MS   86400000u           // the flow started a day before the world of the test begins
#define GX_WIRE     8192
// What a story adds to the settings it stores, so that its display is the one of the stories: the web
// interface not released for good (see the head)
#define WEB_OFF     ",\"web_open\":false}"
// The settings of a display on which nothing was set but that
#define WEB_TIMED   "{\"brightness\":80,\"night\":25,\"night_mode\":false,\"reverse\":false,\"standby_s\":60" WEB_OFF
// The settings of a display that was given the address of the Cerbo and nothing else
#define GX_SETTINGS "{\"brightness\":80,\"night\":25,\"night_mode\":false,\"reverse\":false,\"standby_s\":60,\"cerbo\":\"" GX_HOST "\"" WEB_OFF
// What the flow of the stories publishes: a few of the values of the camper, numbers and switches
#define GX_VALUES   "\"GX_BATT_SOC\":87.5,\"GX_BATT_V\":26.41,\"GX_BATT_I\":-6.2,\"GX_SW_B2B\":\"on\",\"GX_SW_VALVE\":\"off\",\"GX_TANK_FRESH\":77"

/* ---------------------------------------------------------------------------------------------------
 * The device of the test
 */

// The app under test lies between bytes it is not told of
static struct
{
	unsigned char front[GUARD];
	app_t app;
	unsigned char behind[GUARD];
} box;
static app_t *const app = &box.app;
static json_token_t work[LAYOUT_TOKENS];

// What is kept over a restart (components/store/store.h)
typedef struct
{
	bool has_settings;
	char settings[SETTINGS_JSON_SIZE];
	bool has_wifi;
	net_profile_t profiles[NET_PROFILES_MAX];
	int profile_count;
	bool has_bound;
	char bound[33];
	bool has_layout;
	char layout[LAYOUT_TEXT_MAX + 1];
	bool has_layout_prev;
	char layout_prev[LAYOUT_TEXT_MAX + 1];
	bool has_catalog;
	char catalog[16384];
	bool has_old;
	char old[POLL_TEXT_SIZE];
} flash_t;

// What the boot loader and the memory that outlasts a restart know
typedef struct
{
	bool safe_mode;
	bool update_pending;
	bool previous_firmware;
	bool rolled_back;
	bool no_builtin;        // the built-in layout is damaged
} machine_t;

// What the platform was asked to carry out since the counters were cleared
typedef struct
{
	int settings, wifi, bound, layout, erase, catalog, old;
	int valid, reboot, reset, previous, install;
	int starts;
	uint32_t last;          // the events of the last step that had any
} done_t;

typedef struct
{
	char in_range[LINK_SEEN_MAX][NET_SSID_SIZE];    // what a scan finds
	int in_range_count;
	const char *gateway;    // of every network of the test
	const char *found;      // what a query for the service finds, NULL: nothing
	bool join_fails;
	bool ap_on;             // as last ordered
	bool joined;
	int profile;            // the profile the display is in
	int scans, joins, leaves, finds, aps_on, aps_off;
} wifi_t;

typedef struct
{
	char id[33];
	bool api;               // false: a firmware without the API, 404 for every path below /api/
	bool dead;              // no answer to anything
	uint32_t boot;
	uint64_t boot_ms;       // the time of the world at which the adapter started
	wican_autopid_t autopid;
	bool supported;
	bool ignition;
	int rpm;
	int32_t batt_mv;        // a multiple of 100, or -1
	int32_t sleep_in_s;
	uint32_t memory;        // trouble codes the vehicle has stored
	bool manual;            // a scan goes on only as the story says (adapter_step, adapter_done)
	int refuse;             // the status for the next POST, 0: by the rules
	const char *refuse_reason;
	bool swallow;           // the next POST arrives, but its answer gets lost
	bool lose;              // the next POST does not arrive
	bool no_result;         // the result the state names is not handed out: 204
	const char *config;     // the profile
	const char *values;     // the values while the ignition is on, NULL: those of the W906 with `rpm`
	const char *read_text;  // the result of a read as a story wants it, NULL: made from `memory`
	const char *clear_text;
	const char *fw;         // the version text of the state
	const char *mqtt;
	uint32_t pass_every_ms; // the pass counter moves this often while the adapter polls
	uint32_t scan_ms;       // from the pickup of a request to the end of its scan
	uint32_t steps;         // control units of a scan
	const char *const *names;   // of the steps, [0] is the engine check

	uint32_t pass;
	uint64_t pass_ms;       // when the counter moved last
	bool valid;             // a polling pass has ended since the ignition came on

	wican_dtc_phase_t phase;
	bool clear;
	bool http;
	uint32_t seq, next_seq;
	uint32_t step;
	uint64_t accepted_ms, ended_ms;
	char reason[32];
	uint32_t result_seq, result_count;
	bool result_clear;
	char result[POLL_TEXT_SIZE];
} adapter_t;

// The Cerbo GX: its broker, the flow that publishes, and the socket of the task that talks to it
typedef struct
{
	bool there;             // a broker listens at GX_HOST: a connection to it comes about
	bool answers;           // it answers CONNECT, SUBSCRIBE and PINGREQ as MQTT says
	bool flow;              // the flow runs: it publishes a bundle every `every_ms`
	uint32_t every_ms;
	int version;            // the version of the contract its bundles name: 1, or that of a flow that is newer
	uint32_t boot;
	uint64_t boot_ms;       // the time of the world at which the flow started: its `t` counts from there
	const char *values;     // the members of `values` of its bundles
	size_t read_max;        // bytes of one read of the task, at most CERBO_READ_MAX
	uint64_t held_until_ms; // the radio link stands until this time of the world: nothing arrives, the bytes pile up
	bool noisy;             // the script of the fault memory stories runs: see gx_script()
	int script;             // where that script is

	// The connection of the task
	bool socket;            // it has a socket: connecting or connected
	bool connecting;        // ... and the connection is not known to stand yet
	bool reachable;         // ... and it will come about
	bool subscribed;        // the broker took the SUBSCRIBE of this connection: the bundles go to it
	uint64_t bundle_ms;     // the time of the world of the next bundle
	uint8_t wire[GX_WIRE];  // the bytes on their way to the display
	size_t wire_length;

	// What the task did and saw since the counters were cleared
	int opens, closes, sends, unreachable, lost;
	int connects, subscribes, pings;    // packets of the display the broker read: CONNECT, SUBSCRIBE, PINGREQ
	char client[80];        // the client id of the last CONNECT (the room of client_packet_t for it)
	char opened[CERBO_HOST_SIZE];   // the address of the last order to open
	int strange;            // ... and what is none of the three, or was sent without a connection: there has to be none
	int published;          // bundles of the flow
	int overflow;           // bytes that had no room on the wire of the test
} gx_t;

// What the second source took and refused in all starts of the display, for the stories that run with it
typedef struct
{
	long taken, refused, sessions, closed;
	long during_request;    // bundles taken while a fault memory request of the display was under way
	long during_dialog;     // ... while the clear dialog showed
} gx_seen_t;

static flash_t flash;
static machine_t machine;
static gx_t gx;
static gx_seen_t gx_seen;
static json_token_t gx_work[CERBO_TOKENS];
// Every story begins with a display that has the address of a Cerbo that sends bundles, rubbish and breaks off
// (gx_script()): for the stories of the fault memory, which run a second time that way
static bool gx_everywhere;
static done_t done;
static wifi_t wifi;
static adapter_t wican;
static app_platform_t platform;

static const char *firmware_version = VERSION;

static uint64_t now;            // the time of the display
static uint64_t epoch;          // the time of the world at which the display started
static bool switch_pressed, switch_ok;

// The request to the adapter that is under way, and when its answer arrives or is given up
static poll_request_t request;
static bool flying;
static uint64_t landing_ms;
static uint32_t latency_ms;     // of every answer
static int sent[POLL_DTC_CLEAR + 1];    // requests handed out, by kind
static int dropped;             // requests that were handed out while the address of the adapter was not known
static uint64_t clear_sent_ms;  // when the last clear was handed out
static char clear_path[POLL_PATH_SIZE]; // and its path

// Hand-written texts
static char builtin_text[LAYOUT_TEXT_MAX + 1];
static char w906_config[4096];
static char w906_values[2048];
static char other_config[512];
static char no_motor_config[4096];
static char other_values[256];
static char result_read[POLL_TEXT_SIZE];
static char result_clear[POLL_TEXT_SIZE];
static char stored_layout[2048];
static char stored_catalog[8192];
static char wanted[SCENE_DUMP_SIZE + 16];
static char what[512];

static char body_room[POLL_BODY_SIZE];
static char header_room[24];

static scene_t scene;
static char dumped[SCENE_DUMP_SIZE];

// The random runs watch what happens: before the reading of a step, behind it, behind a tick and at the end
// of the step; before and behind counts of the encoder, a tap and a swipe; with every request that is handed
// out and every answer that is applied; and with all events that are carried out
typedef enum
{
	TOUCH_COUNTS,       // a: the counts
	TOUCH_TAP,          // a: the row
	TOUCH_SWIPE,        // a: dx, b: dy
} touch_t;

static void (*watch_step)(void);
static void (*watch_button)(void);
static void (*watch_tick)(void);
static void (*watch_after)(void);
static void (*watch_touch)(touch_t kind, int a, int b);
static void (*watch_touched)(void);
static void (*on_request)(const poll_request_t *asked);
static void (*on_answer)(const poll_request_t *asked, int status, const char *body, bool waited);
static void (*watch_events)(uint32_t events);
// What a step adds to the time: the readings come every 20 ms, or less often while a story only waits
static uint64_t stride = STEP_MS;
// How far the times the two tasks read lie behind the time of the display: each reads it before it waits
// for the lock
static uint64_t net_skew, screen_skew;
// ... and the one of the task of the second source
static uint64_t gx_skew;
// Around every round of that task: before its order is asked for (false), and behind its last report (true)
static void (*watch_gx)(bool behind);
// At the end of every step, behind watch_after. Not cleared by factory(): it watches a story from outside,
// from its very first step on.
static void (*watch_trace)(void);
// The latest time the harness gave the app
static uint64_t given_ms;

static uint64_t world(void)
{
	return epoch + now;
}

// A time that is passed to the app
static uint64_t given(uint64_t time_ms)
{
	if(time_ms > given_ms) given_ms = time_ms;
	return time_ms;
}

// The time the task of the screen read before it got the lock
static uint64_t screen_time(void)
{
	return now > screen_skew ? now - screen_skew : 0;
}

static bool all_bytes(const void *memory, size_t size, unsigned char byte)
{
	const unsigned char *bytes = memory;

	for(size_t i = 0; i < size; i++)
	{
		if(bytes[i] != byte) return false;
	}
	return true;
}

/* The adapter ------------------------------------------------------------------------------------------ */

// The control units of the stories, and the 18 of the W906 in the order of its profile
static const char *const UNITS[] = {"", "N3/28 Motorelektronik (CDID3)", "N2/14 Rückhaltesystem (SRS)", "N10 SAM"};
static const char *const UNITS_W906[] = {
	"", "N73 Elektronisches Zündschloss (EZS)", "N3/28 Motorelektronik (CDID3)", "Y3/8n4 Getriebesteuerung (NAG2)", "N15/5 Wählhebelmodul (EWM)", "N30/4 ESP",
	"N10 SAM", "N118/5 Kraftstoffpumpe (FSCU)", "N28/4 Anhängererkennung (AHE)", "N80 Mantelrohrmodul (MRM)", "N70 Dachbedieneinheit (DBE)",
	"N72/1 Oberes Bedienfeld (OBF)", "B162 Collision Prevention Assist", "N87/8 Radio", "A2/30 Navigationsmodul", "A1 Kombiinstrument", "S98 Klimaanlage",
	"N2/14 Rückhaltesystem (SRS)", "N69/1 Fahrertür (TSG)",
};

static void adapter_init(adapter_t *a)
{
	memset(a, 0, sizeof(*a));
	a->fw = "4.21";
	a->mqtt = "off";
	a->pass_every_ms = PASS_MS;
	a->scan_ms = SCAN_MS;
	a->steps = SCAN_STEPS;
	a->names = UNITS;
	strcpy(a->id, OWN);
	a->api = true;
	a->boot = BOOT;
	a->boot_ms = world() - 100000;
	a->autopid = WICAN_AUTOPID_RUN;
	a->supported = true;
	a->ignition = true;
	a->batt_mv = 12400;
	a->sleep_in_s = -1;
	a->memory = 3;
	a->config = w906_config;
	a->pass_ms = a->boot_ms;
	a->next_seq = 42;
}

// The adapter restarts: new boot number, the numbers of the requests begin anew, the result is gone
static void adapter_restart(adapter_t *a, uint32_t boot, uint32_t first_seq)
{
	a->boot = boot;
	a->boot_ms = world();
	a->pass = 0;
	a->pass_ms = world();
	a->valid = false;
	a->phase = WICAN_DTC_IDLE;
	a->clear = false;
	a->http = false;
	a->seq = 0;
	a->step = 0;
	a->next_seq = first_seq;
	a->reason[0] = '\0';
	a->result_seq = 0;
	a->result_count = 0;
	a->result_clear = false;
}

static void adapter_end(adapter_t *a, wican_dtc_phase_t phase, const char *reason, uint64_t at_ms)
{
	a->phase = phase;
	strcpy(a->reason, reason);
	a->ended_ms = at_ms;
}

static void adapter_result(adapter_t *a)
{
	const char *text = a->clear ? a->clear_text : a->read_text;
	int length;

	// One code is back at once after a clear: its cause is still there
	if(a->clear && a->memory > 1) a->memory = 1;
	a->result_seq = a->seq;
	a->result_clear = a->clear;
	a->result_count = a->memory;
	if(text != NULL)
	{
		strcpy(a->result, text);
		return;
	}
	length = snprintf(a->result, sizeof(a->result), "{\"state\":\"done\",\"action\":\"%s\",\"duration_ms\":%u,\"dtc_count\":%u,\"ecus\":[{\"name\":\"N10 SAM\","
	                  "\"id\":\"662\",\"protocol\":\"KWP\",%s\"status\":\"ok\",\"dtcs\":[", a->clear ? "clear" : "read", (unsigned)(30000 + a->seq % 10000),
	                  (unsigned)a->memory, a->clear ? "\"cleared\":true," : "");
	for(uint32_t i = 0; i < a->memory; i++)
	{
		length += snprintf(a->result + length, sizeof(a->result) - (size_t)length, "%s{\"code\":\"930%u\",\"status\":\"60\"}", i > 0 ? "," : "", (unsigned)(i + 1));
	}
	snprintf(a->result + length, sizeof(a->result) - (size_t)length, "]}]}");
}

// The story lets the scan go on: the control unit `step` is being asked (0: the engine check)
static void adapter_step(uint32_t step)
{
	wican.phase = WICAN_DTC_RUNNING;
	wican.step = step;
}

static void adapter_done(void)
{
	wican.step = wican.steps;
	adapter_end(&wican, WICAN_DTC_DONE, "", world());
	adapter_result(&wican);
}

static void adapter_failed(const char *reason)
{
	wican.step = 0;
	adapter_end(&wican, WICAN_DTC_ERROR, reason, world());
}

// What happened in the adapter until now
static void adapter_catch_up(adapter_t *a, uint64_t at_ms)
{
	uint64_t picked = a->accepted_ms + PICKUP_MS;

	if(!a->manual)
	{
		if(a->phase == WICAN_DTC_QUEUED && at_ms >= picked)
		{
			if(!a->supported) adapter_end(a, WICAN_DTC_ERROR, "not_supported", picked);
			else a->phase = WICAN_DTC_RUNNING;
		}
		if(a->phase == WICAN_DTC_RUNNING)
		{
			if(!a->ignition) adapter_end(a, WICAN_DTC_ERROR, "ecu_offline", at_ms);
			else if(a->clear && a->rpm >= 50) adapter_end(a, WICAN_DTC_ERROR, "engine_running", at_ms);
			else if(at_ms >= picked + a->scan_ms)
			{
				a->step = a->steps;
				adapter_end(a, WICAN_DTC_DONE, "", picked + a->scan_ms);
				adapter_result(a);
			}
			else
			{
				// Step 0 is the engine check
				a->step = (uint32_t)((at_ms - picked) * (a->steps + 1) / a->scan_ms);
			}
		}
	}

	// A scan pauses the polling, and so does the ignition
	if(a->autopid != WICAN_AUTOPID_RUN || !a->ignition)
	{
		a->valid = false;
		a->pass_ms = at_ms;
	}
	else if(a->phase == WICAN_DTC_RUNNING || at_ms < a->pass_ms)
	{
		a->pass_ms = at_ms;
	}
	else if(at_ms - a->pass_ms >= a->pass_every_ms)
	{
		uint64_t passes = (at_ms - a->pass_ms) / a->pass_every_ms;

		a->pass += (uint32_t)passes;
		a->pass_ms += passes * a->pass_every_ms;
		a->valid = true;
	}
}

// A request for a scan with the rules of main/dtc_state.h. Returns the status of API.md.
static int adapter_request(adapter_t *a, bool clear, uint32_t seq, uint64_t at_ms, uint32_t *number, const char **reason)
{
	*number = a->seq;
	if(a->autopid != WICAN_AUTOPID_RUN || a->sleep_in_s == 0)
	{
		*number = 0;
		*reason = "not_ready";
		return 503;
	}
	*reason = NULL;
	if(a->phase == WICAN_DTC_QUEUED || a->phase == WICAN_DTC_RUNNING) *reason = "busy";
	else if(clear)
	{
		if(a->phase != WICAN_DTC_DONE || a->clear || at_ms - a->ended_ms > 600000) *reason = "read_required";
		else if(seq != a->seq) *reason = "stale_seq";
		else if(a->result_count == 0) *reason = "nothing_to_clear";
	}
	if(*reason != NULL) return 409;

	a->seq = a->next_seq;
	a->next_seq = a->next_seq >= 0x7FFFFFFFu ? 1 : a->next_seq + 1;
	a->phase = WICAN_DTC_QUEUED;
	a->clear = clear;
	a->http = true;
	a->step = 0;
	a->reason[0] = '\0';
	a->accepted_ms = at_ms;
	*number = a->seq;
	return 202;
}

// The state as the text of API.md
static size_t adapter_state(const adapter_t *a, uint64_t at_ms, char *out, size_t size)
{
	static const char *const autopid[] = {"off", "starting", "run"};
	static const char *const phases[] = {"idle", "queued", "running", "done", "error"};
	bool running = a->phase == WICAN_DTC_RUNNING;
	bool ended = a->phase == WICAN_DTC_DONE || a->phase == WICAN_DTC_ERROR;
	char volts[24];

	if(a->batt_mv < 0) strcpy(volts, "-1");
	else snprintf(volts, sizeof(volts), "%d.%d", (int)(a->batt_mv / 1000), (int)(a->batt_mv % 1000 / 100));
	return (size_t)snprintf(out, size, "{\"api\":1,\"id\":\"%s\",\"fw\":\"%s\",\"git\":\"w906-v1.4.0-9-g0123abc\",\"boot\":%" PRIu32 ",\"up\":%" PRIu32 ","
	                        "\"autopid\":\"%s\",\"pids\":%d,\"ecu\":\"%s\",\"pass\":%" PRIu32 ",\"rx_age_ms\":%d,\"mqtt\":\"%s\",\"batt_v\":%s,"
	                        "\"sleep_in_s\":%" PRId32 ",\"heap\":61000,\"heap_min\":48000,\"dtc\":{\"supported\":%s,\"state\":\"%s\","
	                        "\"action\":\"%s\",\"src\":\"%s\",\"seq\":%" PRIu32 ",\"ecu\":%" PRIu32 ",\"total\":%" PRIu32 ",\"name\":\"%s\",\"reason\":\"%s\","
	                        "\"age_s\":%" PRIu32 ",\"count\":%" PRIu32 ",\"result_seq\":%" PRIu32 "}}",
	                        a->id, a->fw, a->boot, (uint32_t)((at_ms - a->boot_ms) / 1000), autopid[a->autopid], a->autopid == WICAN_AUTOPID_OFF ? 0 : 35,
	                        a->ignition && a->autopid == WICAN_AUTOPID_RUN ? "online" : "offline", a->pass, a->valid ? 140 : -1, a->mqtt, volts, a->sleep_in_s,
	                        a->supported && a->autopid != WICAN_AUTOPID_OFF ? "true" : "false", phases[a->phase],
	                        a->phase == WICAN_DTC_IDLE ? "" : a->clear ? "clear" : "read", a->phase == WICAN_DTC_IDLE ? "" : a->http ? "http" : "mqtt",
	                        a->seq, running || ended ? a->step : 0, running || ended ? a->steps : 0, running ? a->names[a->step] : "", a->reason,
	                        ended ? (uint32_t)((at_ms - a->ended_ms) / 1000) : 0, a->result_count, a->result_seq);
}

// The values of the W906 fixture with the engine speed of the adapter
static void adapter_values(const adapter_t *a, char *out, size_t size)
{
	static const char head[] = "{\"ENGINE_RPM\":0,";

	snprintf(out, size, "{\"ENGINE_RPM\":%d,%s", a->rpm, w906_values + strlen(head));
}

// The answer of the adapter to one request, by method and path as the display wrote them. Returns the status,
// 0 if there is no answer.
static int adapter_answer(adapter_t *a, const poll_request_t *asked, uint64_t at_ms, const char **body, size_t *length, const char **seq_header)
{
	int status = 404;

	adapter_catch_up(a, at_ms);
	strcpy(body_room, "Nothing matches the given URI");
	*body = body_room;
	*seq_header = NULL;

	if(a->dead)
	{
		status = 0;
		body_room[0] = '\0';
	}
	else if(strncmp(asked->path, "/api/", 5) == 0 && !a->api)
	{
		// The firmware does not know the path
	}
	else if(!asked->post && strcmp(asked->path, "/api/state") == 0)
	{
		status = 200;
		adapter_state(a, at_ms, body_room, sizeof(body_room));
	}
	else if(asked->post && strncmp(asked->path, "/api/dtc?", 9) == 0)
	{
		const char *query = asked->path + 9;
		const char *reason = "bad_request";
		uint32_t number = 0;
		bool clear = strncmp(query, "action=clear&seq=", 17) == 0;

		status = 400;
		if(a->lose)
		{
			a->lose = false;
			status = 0;
			reason = "";
		}
		else if(a->refuse != 0)
		{
			status = a->refuse;
			reason = a->refuse_reason;
			number = a->seq;
			a->refuse = 0;
		}
		else if(strcmp(query, "action=read") == 0) status = adapter_request(a, false, 0, at_ms, &number, &reason);
		else if(clear) status = adapter_request(a, true, (uint32_t)strtoul(query + 17, NULL, 10), at_ms, &number, &reason);

		if(reason == NULL) snprintf(body_room, sizeof(body_room), "{\"accepted\":true,\"seq\":%" PRIu32 "}", number);
		else snprintf(body_room, sizeof(body_room), "{\"accepted\":false,\"reason\":\"%s\",\"seq\":%" PRIu32 "}", reason, number);
		if(a->swallow)
		{
			a->swallow = false;
			status = 0;
		}
		if(status == 0) body_room[0] = '\0';
	}
	else if(!asked->post && strcmp(asked->path, "/api/dtc/result") == 0)
	{
		if(a->autopid != WICAN_AUTOPID_RUN)
		{
			status = 503;
			strcpy(body_room, "{\"accepted\":false,\"reason\":\"not_ready\",\"seq\":0}");
		}
		else if(a->result_seq == 0 || a->no_result)
		{
			status = 204;
			body_room[0] = '\0';
		}
		else
		{
			status = 200;
			strcpy(body_room, a->result);
			snprintf(header_room, sizeof(header_room), "%" PRIu32, a->result_seq);
			*seq_header = header_room;
		}
	}
	else if(!asked->post && strcmp(asked->path, "/autopid_data") == 0)
	{
		status = 200;
		if(a->autopid == WICAN_AUTOPID_OFF) strcpy(body_room, "{\"error\":\"No data available\"}");
		else if(!a->valid) strcpy(body_room, "{}");
		else if(a->values != NULL) strcpy(body_room, a->values);
		else adapter_values(a, body_room, sizeof(body_room));
	}
	else if(!asked->post && strcmp(asked->path, "/load_car_config") == 0)
	{
		if(a->autopid == WICAN_AUTOPID_OFF)
		{
			status = 500;
			strcpy(body_room, "Failed to generate JSON");
		}
		else
		{
			status = 200;
			strcpy(body_room, a->config);
		}
	}
	*length = strlen(body_room);
	return status;
}

/* The flash and the platform -------------------------------------------------------------------------- */

// Carries out what the app asks for, storing first
static void carry_out(void)
{
	uint32_t events = app_take_events(app);
	int length;

	if(events == 0) return;
	done.last = events;

	if(events & APP_EVENT_STORE_SETTINGS)
	{
		length = settings_to_json(&app->settings, flash.settings, sizeof(flash.settings));
		flash.has_settings = length > 0;
		done.settings++;
	}
	if(events & APP_EVENT_STORE_WIFI)
	{
		memcpy(flash.profiles, app->profiles, sizeof(flash.profiles));
		flash.profile_count = app->profile_count;
		flash.has_wifi = true;
		done.wifi++;
	}
	if(events & APP_EVENT_STORE_BOUND)
	{
		strcpy(flash.bound, app->poll.bound_id);
		flash.has_bound = true;
		done.bound++;
	}
	if(events & APP_EVENT_STORE_LAYOUT)
	{
		if(flash.has_layout) strcpy(flash.layout_prev, flash.layout);
		flash.has_layout_prev = flash.has_layout;
		memcpy(flash.layout, app->layout_text, app->layout_length);
		flash.layout[app->layout_length] = '\0';
		flash.has_layout = true;
		done.layout++;
	}
	if(events & APP_EVENT_ERASE_LAYOUT)
	{
		flash.has_layout = false;
		done.erase++;
	}
	if(events & APP_EVENT_STORE_CATALOG)
	{
		length = catalog_to_json(&app->poll.catalog, flash.catalog, sizeof(flash.catalog));
		flash.has_catalog = length > 0;
		done.catalog++;
	}
	if(events & APP_EVENT_STORE_OLD)
	{
		strcpy(flash.old, app->poll.old_text);
		flash.has_old = true;
		done.old++;
	}
	if(events & APP_EVENT_MARK_VALID)
	{
		machine.update_pending = false;
		done.valid++;
	}
	if(events & APP_EVENT_REBOOT) done.reboot++;
	if(events & APP_EVENT_FACTORY_RESET) done.reset++;
	if(events & APP_EVENT_PREVIOUS_FIRMWARE) done.previous++;
	if(events & APP_EVENT_INSTALL_FIRMWARE) done.install++;
	if(watch_events != NULL) watch_events(events);
}

// What the platform knows about itself right now
static void tell_platform(void)
{
	static char ssid[NET_SSID_SIZE];

	// The name of the network as the driver knows it, not as the list holds it by now
	strcpy(ssid, wifi.joined ? app->profiles[wifi.profile].ssid : "");
	platform.ssid = ssid;
	platform.ip = wifi.joined ? "192.168.1.77" : "";
	platform.rssi = wifi.joined ? -61 : 0;
	app_platform(app, &platform);
}

// The app starts with what the flash holds. The time of the display begins again, at `at_ms`.
static void start_at(uint64_t at_ms)
{
	app_boot_t boot;

	epoch = world();
	now = at_ms;
	memset(&boot, 0, sizeof(boot));
	boot.version = firmware_version;
	boot.git = GIT;
	boot.safe_mode = machine.safe_mode;
	boot.update_pending = machine.update_pending;
	boot.previous_firmware = machine.previous_firmware;
	boot.rolled_back = machine.rolled_back;
	if(flash.has_settings) boot.settings_json = flash.settings;
	if(flash.has_wifi) boot.profiles = flash.profiles;
	boot.profile_count = flash.profile_count;
	if(flash.has_bound) boot.bound_id = flash.bound;
	if(flash.has_layout)
	{
		boot.layout_text = flash.layout;
		boot.layout_length = strlen(flash.layout);
	}
	if(!machine.no_builtin)
	{
		boot.builtin_layout = builtin_text;
		boot.builtin_length = strlen(builtin_text);
	}
	if(flash.has_catalog)
	{
		boot.catalog_json = flash.catalog;
		boot.catalog_length = strlen(flash.catalog);
	}
	if(flash.has_old)
	{
		boot.old_text = flash.old;
		boot.old_length = strlen(flash.old);
	}
	boot.work = work;
	boot.work_count = LAYOUT_TOKENS;
	boot.ap_ssid = platform.ap_ssid;

	// Whatever stood in the memory before
	memset(&box, FILL, sizeof(box));
	app_init(app, &boot, now);
	given_ms = now;

	wifi.joined = false;
	wifi.ap_on = false;
	wifi.profile = -1;
	// The task of the second source starts anew as well: its socket is gone, and what was on its way
	gx.socket = false;
	gx.connecting = false;
	gx.subscribed = false;
	gx.wire_length = 0;
	flying = false;
	switch_pressed = false;
	switch_ok = true;
	done.starts++;
	tell_platform();
}

static void start(void)
{
	start_at(0);
}

// A restart the app asked for, as the platform and the boot loader carry it out
static void restart_as_asked(void)
{
	uint32_t events = done.last;

	if(events & APP_EVENT_FACTORY_RESET)
	{
		// STORE_CFG: settings, networks, binding. The views, the catalogue and the old list stay.
		flash.has_settings = false;
		flash.has_wifi = false;
		flash.profile_count = 0;
		memset(flash.profiles, 0, sizeof(flash.profiles));
		flash.has_bound = false;
	}
	if(events & APP_EVENT_INSTALL_FIRMWARE)
	{
		// The version that ran until now is the one in the other slot
		machine.update_pending = true;
		machine.previous_firmware = true;
	}
	else if(machine.update_pending)
	{
		// A restart before the update was confirmed: the boot loader takes it back
		machine.update_pending = false;
		machine.rolled_back = true;
	}
	done.last = 0;
	start();
}

/* The task that talks to the network ------------------------------------------------------------------ */

// The time the task of the network read before it got the lock
static uint64_t net_time(void)
{
	return now > net_skew ? now - net_skew : 0;
}

static void link_step(void)
{
	uint64_t at = net_time();

	for(int actions = 0; actions < 8; actions++)
	{
		link_do_t action = link_next(&app->link, at);

		switch(action)
		{
			case LINK_DO_SCAN:
				wifi.scans++;
				link_scanned(&app->link, wifi.in_range, wifi.in_range_count, at);
				break;
			case LINK_DO_JOIN:
				wifi.joins++;
				if(wifi.join_fails)
				{
					link_join_failed(&app->link, at);
				}
				else
				{
					wifi.joined = true;
					wifi.profile = link_profile(&app->link);
					link_joined(&app->link, wifi.gateway, at);
				}
				break;
			case LINK_DO_LEAVE:
				wifi.leaves++;
				wifi.joined = false;
				link_left(&app->link, at);
				break;
			case LINK_DO_FIND:
				wifi.finds++;
				if(wifi.found != NULL) link_found(&app->link, wifi.found, at);
				else link_not_found(&app->link, at);
				break;
			case LINK_DO_AP_ON:
				wifi.aps_on++;
				wifi.ap_on = true;
				break;
			case LINK_DO_AP_OFF:
				wifi.aps_off++;
				wifi.ap_on = false;
				break;
			default:
				return;
		}
		app_net(app, given(at));
	}
}

static void land(void)
{
	const char *body, *seq_header;
	size_t length;
	int status = adapter_answer(&wican, &request, world(), &body, &length, &seq_header);
	// An answer that comes after the network was lost is not waited for any more
	bool waited = app->poll.asking;

	flying = false;
	poll_apply(&app->poll, &request, status, body, length, seq_header, net_time(), work, POLL_TOKENS);
	app_net(app, given(net_time()));
	if(on_answer != NULL) on_answer(&request, status, body, waited);
}

static void poll_step(void)
{
	for(int requests = 0; requests < 8; requests++)
	{
		if(flying)
		{
			if(now < landing_ms) return;
			land();
		}
		// Storing comes before the next request is sent
		carry_out();
		if(!poll_prepare(&app->poll, net_time(), &request)) return;
		app_net(app, given(net_time()));

		if(request.kind <= POLL_DTC_CLEAR) sent[request.kind]++;
		if(request.kind == POLL_DTC_CLEAR)
		{
			clear_sent_ms = now;
			strcpy(clear_path, request.path);
		}
		if(on_request != NULL) on_request(&request);
		// A request for an adapter whose address is not known cannot be sent
		if(app_host(app)[0] == '\0')
		{
			dropped++;
			continue;
		}
		flying = true;
		landing_ms = now + (wican.dead ? POLL_TIMEOUT_MS : latency_ms);
	}
}

// The network is gone
static void lose_wifi(void)
{
	wifi.joined = false;
	wifi.profile = -1;
	link_lost(&app->link, now);
	app_net(app, given(now));
}

/* The Cerbo and the task that talks to its broker ----------------------------------------------------- */

// Bytes of the broker on their way to the display
static void gx_send(const uint8_t *bytes, size_t length)
{
	if(length > sizeof(gx.wire) - gx.wire_length)
	{
		gx.overflow++;
		return;
	}
	memcpy(gx.wire + gx.wire_length, bytes, length);
	gx.wire_length += length;
}

// A message on a topic, as a broker passes it on to its subscriber
static void gx_publish(const char *topic, bool retained, const char *json)
{
	static uint8_t packet[2048];
	size_t length = strlen(json);

	// With one byte of length a packet holds 127 bytes; the bundles of the flow need two
	size_t head = broker_publish_head(packet, retained ? 0x31 : 0x30, topic, strlen(topic), length, 1);

	if(head + length > sizeof(packet))
	{
		gx.overflow++;
		return;
	}
	memcpy(packet + head, json, length);
	gx_send(packet, head + length);
}

// A bundle of version 1 of the contract with these members of `values`, as the flow publishes it right now
static void gx_bundle_of(const char *values)
{
	static char json[CERBO_BUNDLE_MAX + 1];
	int length = snprintf(json, sizeof(json), "{\"v\":%d,\"boot\":%" PRIu32 ",\"t\":%" PRIu64 ",\"values\":{%s}}", gx.version, gx.boot, world() - gx.boot_ms, values);

	if(length < 0 || (size_t)length >= sizeof(json))
	{
		gx.overflow++;
		return;
	}
	gx_publish(CERBO_TOPIC, false, json);
	gx.published++;
}

/*
 * What the Cerbo of the fault memory stories does instead of one bundle a second, in turns of twelve: bundles,
 * and between them everything a broker can do to a subscriber. Among the bundles one that names the engine
 * speed - under the key of the WiCAN, which no bundle may set, and under a key of its own. at: the time the
 * task read for this round.
 */
static void gx_script(uint64_t at)
{
	static const uint8_t qos1[] = {0x32, 0x05, 0x00, 0x01, 'a', 0x00, 0x01};

	switch(gx.script++ % 12)
	{
		case 2:  gx_bundle_of("\"ENGINE_RPM\":3000,\"GX_ENGINE_RPM\":3000,\"@BATT_V\":9.9,\"GX_BATT_SOC\":12.5"); break;
		case 4:  gx_publish(CERBO_TOPIC, false, "kein JSON"); break;
		case 5:  gx_publish(CERBO_TOPIC, true, "{\"v\":1,\"boot\":7,\"t\":1,\"values\":{\"GX_BATT_SOC\":1}}"); break;
		case 6:  gx_publish("display/gx/other", false, "{\"v\":1,\"boot\":7,\"t\":1,\"values\":{\"ENGINE_RPM\":3000}}"); break;
		case 7:  gx_publish(CERBO_TOPIC, false, "{\"v\":2,\"boot\":7,\"t\":1,\"values\":{\"ENGINE_RPM\":3000}}"); break;
		// A packet no subscriber of QoS 0 gets: the display closes, and comes back two seconds later
		case 9:  gx_send(qos1, sizeof(qos1)); break;
		// The broker breaks off: gone for this round, the connection with it
		case 11: gx.socket = false; gx.lost++; cerbo_down(&app->cerbo, at); break;
		default: gx_bundle_of(gx.values); break;
	}
}

// What the broker makes of a packet the display sent
static void gx_hears(const uint8_t *bytes, size_t length)
{
	static uint8_t answer[8];
	client_packet_t packet = broker_reads(bytes, length);

	if(packet.type == 1)
	{
		gx.connects++;
		memcpy(gx.client, packet.text, sizeof(gx.client));
		if(gx.answers) gx_send(answer, broker_connack(answer, 0, 0));
	}
	else if(packet.type == 8 && packet.qos == 0 && packet.text_length == strlen(CERBO_TOPIC) && memcmp(packet.text, CERBO_TOPIC, packet.text_length) == 0)
	{
		gx.subscribes++;
		if(gx.answers)
		{
			gx_send(answer, broker_suback(answer, (uint16_t)packet.id, 0));
			gx.subscribed = true;
			// The flow goes on at its own pace: the next bundle is the next one that is due
			if(gx.bundle_ms < world()) gx.bundle_ms = world();
		}
	}
	else if(packet.type == 12)
	{
		gx.pings++;
		if(gx.answers) gx_send(answer, broker_pingresp(answer));
	}
	// Anything else - a PUBLISH above all - is what the display must never send
	else gx.strange++;
}

// One round of the task, as app.h says the platform makes it: the order asked for and what it needs copied
// under the lock, the order carried out without it, then what the socket has to tell, reported under the lock
static void gx_step(void)
{
	cerbo_t *cerbo = &app->cerbo;
	uint64_t at = now > gx_skew ? now - gx_skew : 0;
	uint32_t taken = cerbo->bundles, refused = cerbo_refused(cerbo), sessions = cerbo->sessions;
	char host[CERBO_HOST_SIZE] = "";
	uint8_t out[MQTT_SUB_OUT_MAX] = {0};
	size_t length = 0;
	cerbo_do_t order;

	if(watch_gx != NULL) watch_gx(false);
	order = cerbo_next(cerbo, at);
	if(order == CERBO_DO_OPEN) strcpy(host, cerbo_host(cerbo));
	if(order == CERBO_DO_SEND)
	{
		const uint8_t *bytes = cerbo_out(cerbo, &length);

		if(length > sizeof(out)) length = 0;
		memcpy(out, bytes, length);
	}

	switch(order)
	{
		case CERBO_DO_OPEN:
			gx.opens++;
			// A second socket next to one that is open
			if(gx.socket) gx.strange++;
			strcpy(gx.opened, host);
			gx.socket = true;
			gx.connecting = true;
			gx.reachable = gx.there && strcmp(host, GX_HOST) == 0;
			gx.subscribed = false;
			gx.wire_length = 0;
			break;
		case CERBO_DO_SEND:
			gx.sends++;
			if(!gx.socket || gx.connecting || length == 0) gx.strange++;
			else if(gx.there) gx_hears(out, length);
			break;
		case CERBO_DO_CLOSE:
			gx.closes++;
			if(!gx.socket) gx.strange++;
			gx.socket = false;
			gx.connecting = false;
			gx.subscribed = false;
			gx.wire_length = 0;
			break;
		default:
			break;
	}

	if(gx.socket && gx.connecting)
	{
		gx.connecting = false;
		if(gx.reachable) cerbo_opened(cerbo, at);
		else
		{
			gx.socket = false;
			gx.unreachable++;
			cerbo_down(cerbo, at);
		}
	}
	else if(gx.socket && !gx.there)
	{
		// The broker is gone, and its end of the connection with it
		gx.socket = false;
		gx.subscribed = false;
		gx.wire_length = 0;
		gx.lost++;
		cerbo_down(cerbo, at);
	}
	else if(gx.socket)
	{
		// The flow publishes at its own pace, whether the radio link stands or not
		while(gx.subscribed && gx.flow && gx.every_ms > 0 && world() >= gx.bundle_ms)
		{
			gx.bundle_ms += gx.every_ms;
			if(gx.noisy) gx_script(at);
			else gx_bundle_of(gx.values);
			if(!gx.socket) break;
		}
		if(gx.socket && gx.wire_length > 0 && world() >= gx.held_until_ms)
		{
			size_t count = gx.wire_length < gx.read_max ? gx.wire_length : gx.read_max;

			cerbo_bytes(cerbo, gx.wire, count, at, gx_work, CERBO_TOKENS);
			memmove(gx.wire, gx.wire + count, gx.wire_length - count);
			gx.wire_length -= count;
		}
	}

	// What became of it, for the stories that count
	if(cerbo->bundles != taken)
	{
		dtc_flow_phase_t flow = app->poll.flow.phase;

		gx_seen.taken += cerbo->bundles - taken;
		if(flow == DTC_FLOW_READ_SENT || flow == DTC_FLOW_READING || flow == DTC_FLOW_CLEAR_SENT || flow == DTC_FLOW_CLEARING) gx_seen.during_request++;
		if(app->nav.screen == NAV_DTC_CONFIRM) gx_seen.during_dialog++;
	}
	gx_seen.refused += cerbo_refused(cerbo) - refused;
	gx_seen.sessions += cerbo->sessions - sessions;
	if(order == CERBO_DO_CLOSE) gx_seen.closed++;
	if(watch_gx != NULL) watch_gx(true);
}

/* The clock and the task of the screen ---------------------------------------------------------------- */

static void step(void)
{
	if(watch_step != NULL) watch_step();
	app_button(app, switch_pressed, switch_ok, given(screen_time()));
	if(watch_button != NULL) watch_button();
	if(now % 200 == 0)
	{
		app_tick(app, given(screen_time()));
		if(watch_tick != NULL) watch_tick();
	}
	dirt();
	if(now % 1000 == 0) tell_platform();
	dirt();
	link_step();
	dirt();
	poll_step();
	dirt();
	gx_step();
	dirt();
	carry_out();
	now += stride;
	if(watch_after != NULL) watch_after();
	if(watch_trace != NULL) watch_trace();
}

static void run(uint64_t ms)
{
	for(uint64_t passed = 0; passed < ms; passed += stride) step();
}

// Runs until the time of the display is `at_ms`
static void run_to(uint64_t at_ms)
{
	while(now < at_ms) step();
}

// The knob pressed for `ms`, then released for 60 ms: the knob reports a short press 40 ms after the release
static void press(uint64_t ms)
{
	switch_pressed = true;
	run(ms);
	switch_pressed = false;
	run(60);
}

static void short_press(void)
{
	press(100);
}

static void long_press(void)
{
	press(900);
}

static void count(int counts)
{
	if(watch_touch != NULL) watch_touch(TOUCH_COUNTS, counts, 0);
	app_encoder(app, counts, given(screen_time()));
	carry_out();
	if(watch_touched != NULL) watch_touched();
}

static void turn(int detents)
{
	count(detents * KNOB_COUNTS_PER_DETENT);
}

static void tap(int row)
{
	if(watch_touch != NULL) watch_touch(TOUCH_TAP, row, 0);
	app_tap(app, row, given(screen_time()));
	carry_out();
	if(watch_touched != NULL) watch_touched();
}

static void swipe(int dx, int dy)
{
	if(watch_touch != NULL) watch_touch(TOUCH_SWIPE, dx, dy);
	app_swipe(app, dx, dy, given(screen_time()));
	carry_out();
	if(watch_touched != NULL) watch_touched();
}

/* What the driver sees --------------------------------------------------------------------------------- */

// The screen right now as the text of scene_dump()
static const char *screen(void)
{
	app_scene(app, &scene, now);
	if(scene_dump(&scene, dumped, sizeof(dumped)) < 0) strcpy(dumped, "no dump\n");
	return dumped;
}

static int light(void)
{
	return app_backlight(app, now);
}

// The screen is the text of fixtures/app_<name>.txt, byte for byte
static bool screen_is(const char *name)
{
	char path[160];

	snprintf(path, sizeof(path), "fixtures/app_%s.txt", name);
	if(!read_fixture(path, wanted, sizeof(wanted) - 1)) return false;
	// Every line of a dump ends with a line break; read_fixture() took the last one away
	strcat(wanted, "\n");

	if(strcmp(screen(), wanted) == 0) return true;
	printf("  %s wants\n%s  the screen is\n%s", path, wanted, dumped);
	return false;
}

static void shows(const char *name, const char *rule)
{
	snprintf(what, sizeof(what), "app_%s.txt: %s", name, rule);
	check(screen_is(name), what);
}

// A line of the screen as the dump writes it, e.g. "row: > action | Lesen |  | enabled"
static bool has_line(const char *line)
{
	const char *found = strstr(screen(), line);
	size_t length = strlen(line);

	while(found != NULL)
	{
		if((found == dumped || found[-1] == '\n') && found[length] == '\n') return true;
		found = strstr(found + 1, line);
	}
	return false;
}

static bool on(nav_screen_t expected)
{
	return app->nav.screen == expected;
}

static conn_view_t view(void)
{
	return conn_view(&app->poll.conn, now);
}

static dtc_flow_phase_t phase(void)
{
	return app->poll.flow.phase;
}

/* The scenes -------------------------------------------------------------------------------------------- */

static void load_fixtures(void)
{
	bool loaded = read_fixture("../layouts/w906_default.json", builtin_text, sizeof(builtin_text)) &&
	              read_fixture("../../tools/w906/fixtures/car_config_w906.json", w906_config, sizeof(w906_config)) &&
	              read_fixture("../../tools/w906/fixtures/autopid_data_ignition_on.json", w906_values, sizeof(w906_values)) &&
	              read_fixture("fixtures/app_config_other.json", other_config, sizeof(other_config)) &&
	              read_fixture("fixtures/app_values_other.json", other_values, sizeof(other_values)) &&
	              read_fixture("fixtures/app_config_no_motor.json", no_motor_config, sizeof(no_motor_config)) &&
	              read_fixture("fixtures/app_result_read.json", result_read, sizeof(result_read)) &&
	              read_fixture("fixtures/app_result_clear.json", result_clear, sizeof(result_clear)) &&
	              read_fixture("fixtures/app_layout_stored.json", stored_layout, sizeof(stored_layout)) &&
	              read_fixture("fixtures/app_catalog_stored.json", stored_catalog, sizeof(stored_catalog));

	check(loaded && strncmp(w906_values, "{\"ENGINE_RPM\":0,", 16) == 0, "the fixtures of the stories are there, and the values of the W906 begin with the engine speed");
}

// A device fresh from the factory in a world without any network: nothing stored, nothing in range
static void factory(void)
{
	memset(&flash, 0, sizeof(flash));
	memset(&machine, 0, sizeof(machine));
	memset(&done, 0, sizeof(done));
	memset(&wifi, 0, sizeof(wifi));
	memset(sent, 0, sizeof(sent));
	memset(&platform, 0, sizeof(platform));
	platform.ap_ssid = "WiCAN-Display";
	platform.ap_password = "geheim1234";
	platform.slot = "ota_0";
	platform.reset = "poweron";
	platform.heap = 182340;
	platform.heap_min = 151200;
	platform.psram = 7340032;
	platform.psram_min = 7100416;
	platform.heap_malloc = 149572;
	platform.heap_malloc_min = 118432;
	platform.heap_block = 94208;
	platform.reconnects = 2;
	wifi.gateway = "192.168.1.1";
	wifi.profile = -1;
	dropped = 0;
	clear_sent_ms = 0;
	clear_path[0] = '\0';
	latency_ms = 0;
	firmware_version = VERSION;
	watch_step = NULL;
	watch_button = NULL;
	watch_tick = NULL;
	watch_after = NULL;
	watch_touch = NULL;
	watch_touched = NULL;
	on_request = NULL;
	on_answer = NULL;
	watch_events = NULL;
	watch_gx = NULL;
	stride = STEP_MS;
	net_skew = 0;
	screen_skew = 0;
	gx_skew = 0;
	// The world is older than the display: the adapter runs since 100 s
	epoch = 1000000;
	now = 0;
	adapter_init(&wican);
	wican.read_text = result_read;
	wican.clear_text = result_clear;
	// The Cerbo: its flow runs since a day, and nothing listens until a story says so
	memset(&gx, 0, sizeof(gx));
	gx.answers = true;
	gx.every_ms = 1000;
	gx.version = CERBO_VERSION;
	gx.boot = GX_BOOT;
	gx.boot_ms = epoch - GX_AGE_MS;
	gx.values = GX_VALUES;
	gx.read_max = CERBO_READ_MAX;
	// The display of the stories: its owner switched "Dauerhaft frei" off, and nothing else is set
	strcpy(flash.settings, WEB_TIMED);
	flash.has_settings = true;
	if(gx_everywhere)
	{
		// The display was given its address, and it sends what gx_script() says, five times a second
		gx.there = true;
		gx.flow = true;
		gx.noisy = true;
		gx.every_ms = 200;
		strcpy(flash.settings, GX_SETTINGS);
		flash.has_settings = true;
	}
}

// The display as it comes: its stored settings do not say whether the web interface is released for good, so
// it is. Called behind factory() or garage(), before the start.
static void as_delivered(void)
{
	char *member = strstr(flash.settings, WEB_OFF);

	if(strcmp(flash.settings, WEB_TIMED) == 0)
	{
		// Nothing was ever set on it: nothing is stored
		memset(flash.settings, 0, sizeof(flash.settings));
		flash.has_settings = false;
	}
	// What else is stored stays - the address of a Cerbo -, without the member: the text of a firmware from
	// before it
	else if(member != NULL) strcpy(member, "}");
}

// A Cerbo whose flow publishes a bundle every second, and a display that was given its address. Called
// behind factory() or garage(), before the start.
static void cerbo_there(void)
{
	gx.there = true;
	gx.flow = true;
	strcpy(flash.settings, GX_SETTINGS);
	flash.has_settings = true;
}

// The network "Werkstatt" is in range and stored with the address of the adapter, the display is bound to it
static void garage(void)
{
	factory();
	strcpy(wifi.in_range[0], "Werkstatt");
	wifi.in_range_count = 1;
	strcpy(flash.profiles[0].ssid, "Werkstatt");
	strcpy(flash.profiles[0].password, "geheim-123");
	strcpy(flash.profiles[0].host, "192.168.1.50");
	flash.profile_count = 1;
	flash.has_wifi = true;
	strcpy(flash.bound, OWN);
	flash.has_bound = true;
}

// ... started, and two seconds later: the adapter has answered three rounds, commands are allowed
static void drive(void)
{
	garage();
	start();
	run(2100);
	memset(&done, 0, sizeof(done));
	memset(sent, 0, sizeof(sent));
}

// A test does not use everything of the harness
static inline void app_harness_unused(void)
{
	(void)box;
	(void)app;
	(void)UNITS;
	(void)UNITS_W906;
	(void)switch_pressed;
	(void)switch_ok;
	(void)net_skew;
	(void)screen_skew;
	(void)gx_skew;
	(void)watch_gx;
	(void)watch_trace;
	(void)stack_dirt;
	(void)gx;
	(void)gx_seen;
	(void)gx_work;
	(void)gx_everywhere;
	(void)gx_send;
	(void)gx_publish;
	(void)gx_bundle_of;
	(void)gx_script;
	(void)gx_hears;
	(void)gx_step;
	(void)cerbo_there;
	(void)as_delivered;
	(void)work;
	(void)flash;
	(void)machine;
	(void)done;
	(void)wifi;
	(void)wican;
	(void)platform;
	(void)firmware_version;
	(void)now;
	(void)epoch;
	(void)request;
	(void)flying;
	(void)landing_ms;
	(void)latency_ms;
	(void)sent;
	(void)dropped;
	(void)clear_sent_ms;
	(void)clear_path;
	(void)builtin_text;
	(void)w906_config;
	(void)w906_values;
	(void)other_config;
	(void)no_motor_config;
	(void)other_values;
	(void)result_read;
	(void)result_clear;
	(void)stored_layout;
	(void)stored_catalog;
	(void)wanted;
	(void)what;
	(void)body_room;
	(void)header_room;
	(void)scene;
	(void)dumped;
	(void)watch_step;
	(void)watch_button;
	(void)watch_tick;
	(void)watch_after;
	(void)watch_touch;
	(void)watch_touched;
	(void)on_request;
	(void)on_answer;
	(void)watch_events;
	(void)stride;
	(void)given_ms;
	(void)world;
	(void)given;
	(void)screen_time;
	(void)all_bytes;
	(void)adapter_init;
	(void)adapter_restart;
	(void)adapter_end;
	(void)adapter_result;
	(void)adapter_step;
	(void)adapter_done;
	(void)adapter_failed;
	(void)adapter_catch_up;
	(void)adapter_request;
	(void)adapter_state;
	(void)adapter_values;
	(void)adapter_answer;
	(void)carry_out;
	(void)tell_platform;
	(void)start_at;
	(void)start;
	(void)restart_as_asked;
	(void)net_time;
	(void)link_step;
	(void)land;
	(void)poll_step;
	(void)lose_wifi;
	(void)step;
	(void)run;
	(void)run_to;
	(void)press;
	(void)short_press;
	(void)long_press;
	(void)count;
	(void)turn;
	(void)tap;
	(void)swipe;
	(void)screen;
	(void)light;
	(void)screen_is;
	(void)shows;
	(void)has_line;
	(void)on;
	(void)view;
	(void)phase;
	(void)load_fixtures;
	(void)factory;
	(void)garage;
	(void)drive;
}

#endif
