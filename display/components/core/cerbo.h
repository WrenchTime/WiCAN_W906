/*
 * Standalone display for the WiCAN W906 fork.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef __CERBO_H__
#define __CERBO_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "json.h"
#include "values.h"
#include "mqtt_sub.h"

/*
 * The second source of values: the Victron Cerbo GX of the camper. A flow in its Node-RED publishes one
 * small message about once a second on its MQTT broker, the bundle, and the display subscribes to it
 * (mqtt_sub.h). The display publishes nothing, ever: it cannot build the packet.
 *
 * Pure logic, as link.h: the caller owns a socket, asks cerbo_next() what to do with it and reports what
 * happened. This is the counterpart of poll.h and conn.h for the Cerbo, next to them and not in them: the
 * values of this source lie in a store of their own, so nothing a broker sends can reach the values the
 * fault memory functions read (dtc_flow.h takes those of poll.h).
 *
 *   no connection -> opening -> greeting (CONNECT, CONNACK) -> subscribing (SUBSCRIBE, SUBACK) -> session
 *
 * The bundle, version 1 of the contract:
 *   {"v":1,"boot":1836452071,"t":86400123,"values":{"GX_BATT_SOC":99.7,"GX_SW_B2B":"on", ...}}
 *   v        the version of the contract
 *   boot     1 to 2^31-1, chosen when the flow starts: another number is another run of the flow, and its
 *            clock begins anew
 *   t        milliseconds since that start, 0 to 2^53-1; never runs backwards within one boot
 *   values   a flat object: a number, "on" or "off" for each key, as GET /autopid_data of the WiCAN
 * A bundle is a complete snapshot: a key that is missing has no value at the Cerbo right now.
 *
 * Times are milliseconds of the display. The calls come from more than one task, each with its own reading
 * of the time: a time before the latest one seen lets no time pass.
 */

#define CERBO_PREFIX            "GX_"       // every key of this source begins with it, and no key of the WiCAN
#define CERBO_TOPIC             "display/gx/values"
#define CERBO_VERSION           1
#define CERBO_PORT              1883        // for the caller: the port of the broker
#define CERBO_HOST_SIZE         16          // "255.255.255.255" and its zero
#define CERBO_NAME_SIZE         33          // the name the client id is made of, up to 32 bytes
#define CERBO_CLIENT            "WiCANDisplay"  // the client id of a display whose name gives none
#define CERBO_PACKET_ID         1           // of the one SUBSCRIBE of a connection
#define CERBO_TOKENS            256         // tokens the caller provides: a bundle of 64 values needs 137
#define CERBO_BUNDLE_MAX        1400        // bytes of a bundle; with its topic and header it is one TCP segment
#define CERBO_BOOT_MAX          2147483647          // 2^31-1
#define CERBO_T_MAX             9007199254740991    // 2^53-1: up to here JavaScript counts in whole numbers
#define CERBO_READ_MAX          1024        // for the caller: bytes of one report
#define CERBO_TURN_MS           20u         // for the caller: it lets this long pass between two reads
#define CERBO_KEEPALIVE_S       30          // what CONNECT tells the broker
#define CERBO_PING_MS           10000u      // a PINGREQ this long after the one before
#define CERBO_PONG_MS           20000u      // a PINGREQ without its PINGRESP for this long: the session is dead
#define CERBO_SESSION_MS        15000u      // from the order to open to the SUBACK at most
#define CERBO_RETRY_MS          {2000u, 5000u, 10000u, 30000u}  // waits after a failed attempt, as LINK_SCAN_WAITS_MS
#define CERBO_GRACE_MS          15000u      // without a session for this long: "not reachable", before: "waiting"
#define CERBO_FRESH_MS          5000u       // a value seen less than this ago is shown normally, ...
#define CERBO_KEPT_MS           60000u      // ... less than this ago dimmed, and then as a dash
#define CERBO_LIVE_MS           10000u      // a bundle taken less than this ago: the source delivers
#define CERBO_SILENT_MS         20000u      // a session that answers and brings no bundle for this long
#define CERBO_YIELD_PER_MILLE   100         // what the base of the lag gives way: 100 ms in every second
#define CERBO_LAG_MAX_MS        45000       // a lag above this is none: the clock of the flow is not believed

typedef enum
{
	CERBO_DO_NOTHING,
	CERBO_DO_OPEN,      // connect to cerbo_host(), port CERBO_PORT; then cerbo_opened() or cerbo_down()
	CERBO_DO_SEND,      // send the bytes of cerbo_out(); if not all of them go out: close, and cerbo_down()
	CERBO_DO_CLOSE,     // close the socket, also one that is still connecting; nothing is reported
} cerbo_do_t;

typedef enum
{
	CERBO_VIEW_OFF,             // no address set, or safe mode
	CERBO_VIEW_NO_WIFI,         // in no network
	CERBO_VIEW_CONNECTING,      // no session, still within the grace time
	CERBO_VIEW_UNREACHABLE,
	CERBO_VIEW_QUIET,           // a session, no bundle for CERBO_LIVE_MS: nothing is known about the reason
	CERBO_VIEW_SILENT,          // a session that answers pings, and no bundle
	CERBO_VIEW_NEWER,           // the bundles are of a newer contract
	CERBO_VIEW_LIVE,
} cerbo_view_t;

typedef enum
{
	CERBO_IDLE,                 // no connection
	CERBO_OPENING,              // CERBO_DO_OPEN was handed out: the caller connects
	CERBO_GREETING,             // the connection stands: CONNECT goes out, the CONNACK is awaited
	CERBO_SUBSCRIBING,          // SUBSCRIBE goes out, the SUBACK is awaited
	CERBO_SESSION,
} cerbo_phase_t;

typedef struct
{
	char host[CERBO_HOST_SIZE];     // the address of the broker as text, empty = none set
	char name[CERBO_NAME_SIZE];     // what the client id is made of
	bool safe_mode;
	bool network;                   // the display is in a network
	uint64_t clock_ms;              // the latest time seen: it follows the calls but never runs backwards

	cerbo_phase_t phase;
	bool moved;                     // the address changed while there was a connection: that one is closed first
	bool failed;                    // broken bytes, a packet out of place or a refusal: the connection is closed
	bool send;                      // `out` waits to be handed out
	uint8_t out[MQTT_SUB_OUT_MAX];  // the packet of the last CERBO_DO_SEND, or the one that waits
	size_t out_length;
	uint64_t open_ms;               // when CERBO_DO_OPEN was handed out
	int wait_step;                  // index into CERBO_RETRY_MS of the wait behind the next connection that ends
	uint32_t wait_ms;               // the wait before the next attempt, 0 = due at once, ...
	uint64_t wait_since_ms;         // ... counted from here
	uint64_t grace_since_ms;        // the network joined, the address set or the last session ended: the latest
	uint64_t ping_ms;               // when the last PINGREQ was handed out; before the first, when the SUBACK came
	uint32_t pings_open;            // PINGREQ handed out and not answered yet
	uint64_t ping_open_ms;          // when the oldest of them was handed out
	uint64_t quiet_since_ms;        // of a session: its SUBACK or the last bundle taken, the later of the two
	uint64_t pong_ms;               // when the last PINGRESP came, in whatever session

	mqtt_sub_reader_t reader;
	values_t values;                // of the last bundle taken; seen_ms is its arrival less its lag

	bool has_bundle;                // a bundle of the broker at `host` was taken, the last one at bundle_ms
	uint64_t bundle_ms;
	bool newer;                     // a bundle with a version above CERBO_VERSION came, and none was taken since
	bool has_boot;                  // boot, t_ms and lag_ms are those of the last bundle taken
	uint32_t boot;
	bool has_clock;                 // a bundle was taken on this connection: the next one of its boot is measured by it
	int64_t t_ms;
	uint32_t lag_ms;                // also for the info page: the lag of the last bundle taken
	uint32_t yield_rest;            // thousandths of a millisecond the yield has not given way yet

	// Counters for the info page, since cerbo_init()
	uint32_t bundles;               // bundles taken
	uint32_t sessions;              // sessions that stood
	uint32_t boots;                 // changes of `boot`, the first bundle counted: a number that runs means two flows
	uint32_t refused_invalid;       // messages not taken: no bundle of the contract, ...
	uint32_t refused_version;       // ... of a newer contract, ...
	uint32_t refused_retained;      // ... kept by the broker, ...
	uint32_t refused_topic;         // ... on another topic, ...
	uint32_t refused_long;          // ... longer than CERBO_BUNDLE_MAX, or than the room of the reader
	uint32_t skipped;               // members of `values` not taken
	uint32_t distrusted;            // lags above CERBO_LAG_MAX_MS
} cerbo_t;

// Everything empty: no connection, no value, in no network. host: the address of the broker as the
// settings hold it, see cerbo_set_host(). safe_mode: the source is off whatever is stored (guard.h: the
// address is a stored setting, and the safe mode is there to get out of what a stored setting does).
// name: the name of the own access point, of which the client id is made (mqtt_sub_connect()); of a name
// longer than CERBO_NAME_SIZE - 1 bytes the first ones count, and a name that is NULL or gives no client id
// is replaced by CERBO_CLIENT.
void cerbo_init(cerbo_t *cerbo, const char *host, bool safe_mode, const char *name, uint64_t now_ms);

// true if an address is set and it is not the safe mode. Without it there is never CERBO_DO_OPEN.
bool cerbo_on(const cerbo_t *cerbo);

// The address of the broker was set. NULL, and a text without an end within CERBO_HOST_SIZE bytes, count as
// no address (an address cut short would be the address of something else); whether the text is an address
// is the business of settings.h. The same address again changes nothing. Another one, also none: the values
// are forgotten, and with them what was known of the bundles of the other broker - the last bundle, its
// boot, its lag, a newer contract; the counters stay. The next attempt is due at once and the waits start
// from the beginning; if there is a connection, cerbo_next() orders it closed first, and its bytes are not
// read any more.
void cerbo_set_host(cerbo_t *cerbo, const char *host, uint64_t now_ms);

// The display is in a network or not (link.h). The same value again changes nothing. With the network an
// attempt is due at once and the waits start from the beginning. Without it cerbo_next() orders a
// connection closed; the values stay and grow old.
void cerbo_network(cerbo_t *cerbo, bool up, uint64_t now_ms);

// What to do now; each order is handed out once. The first that applies:
//   CLOSE   there is a connection, also one that is still opening, and
//           - the display is in no network, or the address is another one now, or
//           - the reader met broken bytes, a packet came that does not belong where the connection is, or
//             CONNACK or SUBACK refused (cerbo_bytes()), or
//           - CERBO_SESSION_MS after CERBO_DO_OPEN there is no session, or
//           - a PINGREQ is without its PINGRESP for CERBO_PONG_MS.
//           The connection is gone with the order; what follows is as after cerbo_down().
//   SEND    CONNECT after cerbo_opened(); SUBSCRIBE to CERBO_TOPIC after a CONNACK that accepted; in the
//           session a PINGREQ CERBO_PING_MS after the one before, the first one that long after the SUBACK -
//           whether the one before was answered or not. (So at most two are without an answer at a time: a
//           third would be due when the first is CERBO_PONG_MS old, and that closes.)
//   OPEN    the source is on, the display is in a network, there is no connection and the wait is over.
cerbo_do_t cerbo_next(cerbo_t *cerbo, uint64_t now_ms);

// The address for CERBO_DO_OPEN, and the bytes for CERBO_DO_SEND (*length of them): the caller copies them
// before it lets go of its lock. The bytes hold until the next call of cerbo_next(), cerbo_opened() or
// cerbo_bytes().
const char *cerbo_host(const cerbo_t *cerbo);
const uint8_t *cerbo_out(const cerbo_t *cerbo, size_t *length);

// The connection stands: the reader begins anew, CONNECT waits to be sent. Counts only while opening.
void cerbo_opened(cerbo_t *cerbo, uint64_t now_ms);

/*
 * Bytes arrived (any number of them; a packet may be cut anywhere). Every packet that becomes complete in
 * them is read, in their order; work: CERBO_TOKENS tokens for the JSON reader. Bytes without a connection
 * that stands are ignored, and so are all bytes once the connection is to be closed (cerbo_next(): CLOSE),
 * the rest of this call among them.
 *
 * CONNACK    while greeting, after CONNECT was handed out, with the return code 0: SUBSCRIBE waits to be sent.
 *            With another code: refused, the connection is closed.
 * SUBACK     while subscribing, after SUBSCRIBE was handed out, with CERBO_PACKET_ID and the return code 0: the
 *            session stands, and the waits start from the beginning. With another code - also 1 or 2, a QoS
 *            the display did not ask for and cannot acknowledge -: refused, closed.
 * PINGRESP   in a session, with a PINGREQ waiting for it: noted, with its time.
 * PUBLISH    after SUBSCRIBE was handed out, so also before the SUBACK (MQTT 3.1.1, 3.8.4 lets a broker do
 *            that): the rules below.
 * broken     mqtt_sub.h: closed.
 * A packet that does not belong where the connection is counts as broken: a CONNACK or a SUBACK at any other
 * place, a SUBACK with another packet identifier, a PINGRESP nobody asked for, a PUBLISH before the display
 * subscribed ([MQTT-3.2.0-1]: a CONNACK comes first).
 *
 * A PUBLISH is a bundle if it passes these in their order; what does not is counted and changes nothing:
 * 1. too long for the room of the reader (MQTT_SUB_SKIPPED): refused_long
 * 2. the topic is not CERBO_TOPIC, byte for byte: refused_topic
 * 3. the RETAIN flag is set: refused_retained - nobody knows how old it is
 * 4. the payload has more than CERBO_BUNDLE_MAX bytes: refused_long
 * 5. the payload is not a JSON object (json.h: strict, at most 8 levels, at most work_count tokens), or `v`
 *    is missing or no whole number from 1 on: refused_invalid
 * 6. `v` is more than CERBO_VERSION: refused_version, and cerbo_view() is NEWER until a bundle is taken.
 *    Nothing else of such a bundle is looked at: the display does not guess what a later contract means.
 * 7. `boot` is no whole number from 1 to CERBO_BOOT_MAX, `t` none from 0 to CERBO_T_MAX, or `values` no
 *    object: refused_invalid
 * A whole number is what json_integer() takes: 1.0 and 1e0 are none. Of two members with one of these four
 * names the first counts (json_member()). Members the contract does not know are passed over.
 *
 * The bundle is then taken. Its lag is reckoned (below), and the store of values is emptied and filled anew
 * from `values`, every value seen at the time of arrival less the lag (not before 0):
 * - a member is taken if its key is CERBO_PREFIX and then 1 to 29 of the characters A-Z, 0-9 and _ (the key
 *   as json_text() reads it, at most 32 bytes), and its value is a number json_number() takes, "on" or "off".
 *   A number beyond the range of a double is taken as infinite, as values.h takes one of the adapter: it is
 *   in the store, has no text to show, and no answer names or counts it (cerbo_shown())
 * - every other member is passed over and counted in `skipped`
 * - of two members with the same key the last counts
 * - the 65th key finds no room (VALUES_MAX): counted in `skipped`, not kept
 *
 * The lag. The display must not show a value as newer than it arrived, and after a pause of the radio link
 * TCP delivers everything that piled up at once: `t` tells the bundles apart, as the counter `pass` does for
 * the WiCAN. In milliseconds, signed, 64 bit; d = time of arrival - t (negative for a display that started
 * later than the flow):
 * - the first bundle taken on a connection, and every bundle with another `boot` than the one before:
 *   base = d, the lag is 0. A new session has no backlog - clean session, QoS 0 -, and another boot is
 *   another clock. Every change of boot is counted in `boots`, the first bundle of all among them.
 * - else base = min(d, base + yielded), the lag is d - base. yielded: CERBO_YIELD_PER_MILLE thousandths of
 *   the time since the bundle before was taken, in whole milliseconds; what the division leaves is carried
 *   on to the next bundle (so a step of 990 ms does not yield nothing). An offset that stays is a clock that
 *   was set or runs fast, not a backlog, and has to wear off: 11 s are below CERBO_FRESH_MS after 61 s.
 * - a lag above CERBO_LAG_MAX_MS is not believed: base = d, the lag is 0, counted in `distrusted`. On a
 *   session that lives nothing waits longer: a PINGRESP lies behind everything the broker piled up, and
 *   without it the session ends after CERBO_PING_MS + CERBO_PONG_MS.
 * - with every new base the yield begins without a rest
 * Reckoned here in differences, without d and base: the lag is the lag before + the time since the bundle
 * before - yielded - the step of t, not below 0. That is the same number; test_cerbo.c reckons with d and
 * base and compares after every bundle.
 */
void cerbo_bytes(cerbo_t *cerbo, const uint8_t *data, size_t length, uint64_t now_ms, json_token_t *work, int work_count);

// The caller lost the connection and closed the socket: connecting failed, sending failed, reading failed,
// or the broker closed. Ignored without a connection. The next attempt follows after the next wait of
// CERBO_RETRY_MS, counted from here: 2 s, 5 s, 10 s, then 30 s each time; after a session that stood, 2 s.
// The values stay - they grow old.
void cerbo_down(cerbo_t *cerbo, uint64_t now_ms);

// What the display says about this source. The first that applies:
//   safe mode, or no address                                                    OFF
//   in no network                                                               NO_WIFI
//   a session, and a bundle of a newer contract came and none was taken since   NEWER
//   a session, and a bundle was taken less than CERBO_LIVE_MS ago               LIVE
//   a session, and a PINGRESP came CERBO_SILENT_MS or more after the SUBACK
//     and after the last bundle taken (quiet_since_ms)                          SILENT
//   a session                                                                   QUIET
//   no session, and less than CERBO_GRACE_MS since the network was joined,
//     the address was set or the last session ended, the latest of them         CONNECTING
//   else                                                                        UNREACHABLE
// SILENT blames the flow, so it needs the proof that the connection lives: the broker answered a PINGREQ
// after the silence had lasted its time. A pause of the radio link never shows it - no PINGRESP comes then -
// but QUIET, and after the session ended CONNECTING and UNREACHABLE.
// The bundle of LIVE may be one of the session before: the values are those of the broker at `host` either way.
cerbo_view_t cerbo_view(const cerbo_t *cerbo, uint64_t now_ms);

// true if cerbo_view() is LIVE or a value of the store is not gone (cerbo_age()): the values stay dimmed
// for up to CERBO_KEPT_MS after the session ended
bool cerbo_showing(const cerbo_t *cerbo, uint64_t now_ms);

// As values_age(), with CERBO_FRESH_MS and CERBO_KEPT_MS. A time before seen_ms counts as no time passed.
// value may be NULL (gone).
value_age_t cerbo_age(const value_t *value, uint64_t now_ms);

// true if the key begins with CERBO_PREFIX: a value of this source, whatever the stores hold. NULL is none.
bool cerbo_key(const char *key);

// The messages not taken since cerbo_init(), whatever the reason: the five counters refused_* together, and
// 2^32-1 if they are more than that. For the info line and GET /api/info (app.h, app_web.h).
uint32_t cerbo_refused(const cerbo_t *cerbo);

// How many values of the store GET /api/values names right now (web_json.h), 0 to VALUES_MAX: those that are
// not gone (cerbo_age()) and are a finite number, "on" or "off". The two never disagree: `cerbo.values` of
// GET /api/info is the number of keys with CERBO_PREFIX in GET /api/values at the same time.
// A number beyond the range of a double - 1e999 - is taken into the store as infinite, as values.h takes one
// of the adapter, and like there it has no text to show (fmt_number() refuses it) and is left out of GET
// /api/values: it is not counted here. A value the last bundle did not name is not in the store.
int cerbo_shown(const cerbo_t *cerbo, uint64_t now_ms);

#endif
