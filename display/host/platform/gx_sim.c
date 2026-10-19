/*
 * Runs display/main/gx.c on a PC: the file is included as it is, and its task runs against the real core
 * and a world made of stand-ins - the socket calls, and behind them a broker with a flow that publishes.
 * "make" in this directory builds and runs it; redproof.py breaks gx.c in one place at a time
 * (mutations/gx.py) and expects a check to fail.
 *
 * What is watched, as a check each: no socket call is made with the lock held, and no call into the core
 * without it; every report to the core carries a time that was read for it, before the lock was taken;
 * every order of the core is carried out once; there is never more than one socket, and each one is closed
 * exactly once; nothing is sent but the bytes the core handed out, and every packet begins with 0x10, 0x82
 * or 0xC0; no read takes more than CERBO_READ_MAX bytes, and between two reads that brought bytes
 * CERBO_TURN_MS pass; cerbo_down() follows a failed attempt, a short send, a read error and a broker that
 * closes, with the socket closed before it; an order to close is followed by no report.
 * That the task is not created without an address of the Cerbo, nor in safe mode, is net.c's and is checked
 * in net_sim.c; here a task that runs all the same opens no socket then.
 *
 * What the world does, and what that rests on (ESP-IDF v5.5.2 and its lwIP, espressif/esp-lwip at fd432e4e,
 * read on 2026-10-11, not run; the head of gx.c names the places):
 *   the sockets     one at a time, as gx.c needs. connect() on a socket that does not block returns -1 with
 *                   EINPROGRESS; the socket is writable when the attempt is over, however it ended, and
 *                   SO_ERROR says how, once. A broker that closed makes it readable and recv() return 0, an
 *                   error makes it readable and recv() return -1. The switches of `net` make single calls
 *                   fail the ways a real one can: no socket, a flag that is not taken, a send that takes
 *                   one byte less.
 *   the broker      listens at one address. It accepts an attempt after a moment, refuses it, never
 *                   answers it, or is not reachable at all. It answers CONNECT, SUBSCRIBE and PINGREQ as
 *                   MQTT 3.1.1 says and compares each with what the display has to send, byte for byte.
 *                   It can fall silent (a pause of the radio link), close, hand its bytes out in pieces,
 *                   and send a message of megabytes.
 *   the flow        publishes a bundle of the contract (cerbo.h) every second to a subscriber, or a hundred
 *   FreeRTOS        one task, that of gx.c. Time passes where it waits, a millisecond at a time, and the
 *                   world moves with it. A delay of n ticks is the shortest FreeRTOS may make it: it ends
 *                   with the n-th tick, and the first one may be a moment away. The task is left at one of
 *                   its waits when the time of a step of the scenario is over - before it waits for the
 *                   socket, or behind a delay - and entered again from its top for the next step: gx.c keeps
 *                   nothing in the variables of the task function.
 *   the lock        a counter. Taking it twice is a failed check.
 *   the core        the real one, called through wrappers that look at the lock, the time and what is
 *                   handed over. Two of them can hand gx.c what the core never would: an address that is
 *                   none, a packet without bytes or with too many.
 *
 * What this cannot see: nothing of lwIP runs, so every "the socket does" above is the model. No radio, no
 * real time, no second task: the lock is never waited for. display/host/gx_live runs the same gx.c against
 * the sockets of the PC and a broker that is one.
 */

// The types and the numbers of the socket calls are those of this PC: idf/sys/socket.h steps aside
#define SIM_PC_SOCKETS
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "sim.h"
#include "platform.h"

// gx.c calls these of the PC and of the core; here it calls the stand-ins and wrappers further down instead
static int sim_socket(int domain, int type, int protocol);
static int sim_fcntl(int descriptor, int command, int argument);
static int sim_connect(int descriptor, const struct sockaddr *address, socklen_t length);
static int sim_select(int count, fd_set *reading, fd_set *writing, fd_set *failing, struct timeval *limit);
static int sim_getsockopt(int descriptor, int level, int name, void *value, socklen_t *size);
static ssize_t sim_recv(int descriptor, void *data, size_t size, int flags);
static ssize_t sim_send(int descriptor, const void *data, size_t size, int flags);
static int sim_close(int descriptor);
static int sim_inet_pton(int family, const char *text, void *address);
static cerbo_do_t sim_cerbo_next(cerbo_t *cerbo, uint64_t now_ms);
static const char *sim_cerbo_host(const cerbo_t *cerbo);
static const uint8_t *sim_cerbo_out(const cerbo_t *cerbo, size_t *length);
static void sim_cerbo_opened(cerbo_t *cerbo, uint64_t now_ms);
static void sim_cerbo_bytes(cerbo_t *cerbo, const uint8_t *data, size_t length, uint64_t now_ms, json_token_t *work, int work_count);
static void sim_cerbo_down(cerbo_t *cerbo, uint64_t now_ms);

// A header of the PC may have made a macro of one of them
#undef socket
#undef fcntl
#undef connect
#undef select
#undef getsockopt
#undef recv
#undef send
#undef close
#undef inet_pton
#define socket sim_socket
#define fcntl sim_fcntl
#define connect sim_connect
#define select sim_select
#define getsockopt sim_getsockopt
#define recv sim_recv
#define send sim_send
#define close sim_close
#define inet_pton sim_inet_pton
#define cerbo_next sim_cerbo_next
#define cerbo_host sim_cerbo_host
#define cerbo_out sim_cerbo_out
#define cerbo_opened sim_cerbo_opened
#define cerbo_bytes sim_cerbo_bytes
#define cerbo_down sim_cerbo_down
#include "gx.c"
#undef socket
#undef fcntl
#undef connect
#undef select
#undef getsockopt
#undef recv
#undef send
#undef close
#undef inet_pton
#undef cerbo_next
#undef cerbo_host
#undef cerbo_out
#undef cerbo_opened
#undef cerbo_bytes
#undef cerbo_down

#define TICK_MS         (1000 / configTICK_RATE_HZ)
#define FD_FIRST        5           // the descriptors the world hands out: this one and the three behind it
#define SPIN_ROUNDS     50          // rounds of the task without any time passing: it does not wait
#define RX_ROOM         65536
#define ADDRESS         "192.168.88.2"
#define NAME            "WiCAN-Display-C9A8"
#define CLIENT_ID       "WiCANDisplayC9A8"      // what mqtt_sub.h makes of that name
#define SETTINGS        "{\"cerbo\":\"" ADDRESS "\"}"

#define FAILED(what)    sim_check(false, what, __func__, __LINE__)

/* The rest of the platform ----------------------------------------------------------------------------- */

app_t *platform_app;
platform_info_t platform_info;

static int locked;
static uint64_t sim_ms = 1000;
static uint64_t time_read;      // what platform_now_ms() said last
static bool time_fresh;         // ... and no report has used it yet

static void world(void);

static void pass(uint64_t ms)
{
	for(uint64_t i = 0; i < ms; i++)
	{
		sim_ms++;
		world();
	}
}

void platform_lock(void)
{
	CHECK(locked == 0);
	locked = 1;
}

void platform_unlock(void)
{
	CHECK(locked == 1);
	locked = 0;
}

uint64_t platform_now_ms(void)
{
	// Read before the lock is waited for: a time read under it would be late by that wait
	CHECK(locked == 0);
	time_read = sim_ms;
	time_fresh = true;
	return sim_ms;
}

// The second source raises no event and stores nothing: its task has no business with either
void platform_events(void)
{
	FAILED("platform_events() called by the task of the second source");
}

void platform_stored(void)
{
	FAILED("platform_stored() called by the task of the second source");
}

const char *esp_err_to_name(esp_err_t code)
{
	(void)code;
	return "ERR";
}

static bool heap_fails;
static int rooms;

void *heap_caps_malloc(size_t size, uint32_t caps)
{
	// The room of gx.c need not lie in the internal RAM, which is what the display is short of
	CHECK(caps == MALLOC_CAP_SPIRAM);
	if(heap_fails)
	{
		return NULL;
	}
	rooms++;
	// The room is the whole of gx_room_t: sizeof of the struct, not of the pointer to it. What is handed out is
	// never less than the struct, so that a room that was asked too small is this failed check and not a write
	// behind 8 bytes, which only stops the simulation without naming a rule.
	CHECK(size == sizeof(gx_room_t));
	return sim_alloc(size > sizeof(gx_room_t) ? size : sizeof(gx_room_t));
}

/* FreeRTOS */

static jmp_buf out;
static uint64_t run_until_ms;
static TaskFunction_t the_task;
static bool task_fails;
static int tasks;
static int delays;

// Where the task waits: the step of the scenario may be over
static void leave_if_over(void)
{
	if(sim_ms >= run_until_ms)
	{
		longjmp(out, 1);
	}
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t code, const char *const name, const uint32_t stack, void *const arg,
                                   UBaseType_t priority, TaskHandle_t *const created, const BaseType_t core)
{
	(void)name;
	(void)arg;
	(void)created;
	// At least the stack of the network task, which makes the same kinds of calls (sim.h; net_sim.c checks the
	// number). What this task needs was not measured: a stack that is too small is a crash on the board, and no
	// simulation sees it. With the 6144 bytes it ran on the device for three hours (read 2026-10-12: one session,
	// 9284 bundles in 11160 s, none refused) - enough for that, and nothing was read of how much less would be.
	CHECK(stack >= SIM_NET_STACK_BYTES);
	// Its room first: the task uses it from its first round on
	if(!CHECK(room != NULL))
	{
		return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
	}
	// Below the network task and the web server (5), above the main task (3); and on core 0, where the WiFi
	// driver runs: the other one is the panel's, and LVGL has this priority there
	CHECK(priority == 4 && core == 0);
	if(task_fails)
	{
		return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
	}
	tasks++;
	the_task = code;
	return pdPASS;
}

void vTaskDelay(const TickType_t ticks)
{
	CHECK(locked == 0);
	delays++;
	// The shortest such a delay may be: it ends with a tick, and the first tick may be a moment away
	if(ticks > 0)
	{
		pass((uint64_t)(ticks - 1) * TICK_MS + 1);
	}
	leave_if_over();
}

/* The world: sockets ----------------------------------------------------------------------------------- */

typedef enum
{
	ACCEPTS,        // the attempt ends as a connection after accept_ms
	REFUSES,        // ... with ECONNREFUSED after accept_ms
	STALLS,         // ... never: no answer to the SYN
	UNREACHABLE,    // connect() fails at once
	AT_ONCE,        // connect() returns 0, as a PC may for a broker on itself
} attempt_t;

static struct
{
	int fd;                 // the socket that is open, -1 = none
	bool nonblocking;
	bool attempting;        // connect() was called, and nobody has asked SO_ERROR how it ended
	bool attempt_over;
	bool attempt_ends;
	uint64_t attempt_done_ms;
	int attempt_error;      // SO_ERROR
	bool connected;
	bool peer_closed;       // the broker has closed: readable, and recv() returns 0 behind what is left
	int error;              // the connection is dead: readable, and recv() returns -1 with this
	int spurious;           // so many times readable with nothing to read
	size_t piece;           // one recv() hands out this many bytes at most, 0 = all there are
	uint8_t delivered[CERBO_READ_MAX];  // what the last recv() handed out
	size_t delivered_length;
	uint64_t bytes_ms;      // when a recv() last brought bytes
	uint64_t second;        // the second that is counted in, ...
	size_t second_bytes;    // ... the bytes read in it, ...
	size_t most_a_second;   // ... and the most of any second
	// Calls that fail once
	bool no_socket, high, getfl_fails, setfl_fails, sockopt_fails, send_short, send_fails;
	int select_error;
	// Counted since the start
	int calls, socket_calls, made, closed, fcntls, connects, selects, recvs, reads, sends;
} net = { .fd = -1 };

static struct
{
	const char *address;    // where it listens
	attempt_t attempt;
	uint32_t accept_ms;
	bool mute;              // it answers nothing and publishes nothing: a pause of the radio link
	bool subscribed;
	bool flow;              // the flow publishes
	uint32_t flow_ms;       // ... this often
	uint64_t flow_since_ms; // its clock began here
	uint64_t bundle_ms;     // the next bundle is due
	size_t junk_left;       // bytes of a huge message that are still to come
	int connects, subscribes, pings, published;
} broker = { .address = ADDRESS, .accept_ms = 30, .flow = true, .flow_ms = 1000 };

static uint8_t rx[RX_ROOM];         // what the broker sent and the display has not read
static size_t rx_count;
static uint8_t later[RX_ROOM];      // what the broker sends behind a huge message that is not through yet
static size_t later_count;

// What the core handed out with its last order, and what happened since
static struct
{
	cerbo_do_t order;
	bool had_socket;
	int sockets, connects, sends, closes, downs;
} acts;

static char handed_host[64];
static uint8_t handed[MQTT_SUB_OUT_MAX + 1];
static size_t handed_length;
static uint8_t long_packet[MQTT_SUB_OUT_MAX + 1];
static const char *host_other;      // handed out once instead of the address of the core
static bool out_none, out_long;     // the next packet handed out has no bytes, or one more than there is room for
static int rounds, openeds, downs, reports;
static uint64_t round_ms;
static int rounds_same_ms;

static void connection_gone(void)
{
	net.nonblocking = false;
	net.attempting = false;
	net.attempt_over = false;
	net.attempt_error = 0;
	net.connected = false;
	net.peer_closed = false;
	net.error = 0;
	net.spurious = 0;
	rx_count = 0;
	later_count = 0;
	broker.junk_left = 0;
	broker.subscribed = false;
}

static void push(const uint8_t *bytes, size_t length)
{
	// Behind a huge message that is not through yet
	if(broker.junk_left > 0)
	{
		NEED(later_count + length <= sizeof(later));
		memcpy(later + later_count, bytes, length);
		later_count += length;
		return;
	}
	NEED(rx_count + length <= sizeof(rx));
	memcpy(rx + rx_count, bytes, length);
	rx_count += length;
}

// A PUBLISH with QoS 0 on `topic` (MQTT 3.1.1, 3.3): the broker can build one, the display cannot
static void publish(const char *topic, const char *payload, size_t payload_length)
{
	uint8_t head[8];
	size_t topic_length = strlen(topic);
	size_t remaining = 2 + topic_length + payload_length;
	size_t at = 0;

	head[at++] = 0x30;
	do
	{
		uint8_t byte = (uint8_t)(remaining % 128);

		remaining /= 128;
		head[at++] = remaining > 0 ? (uint8_t)(byte | 0x80) : byte;
	}
	while(remaining > 0);
	head[at++] = (uint8_t)(topic_length >> 8);
	head[at++] = (uint8_t)(topic_length & 0xFF);
	push(head, at);
	push((const uint8_t *)topic, topic_length);
	push((const uint8_t *)payload, payload_length);
}

// One bundle of the contract, with the clock of the flow
static void bundle(void)
{
	char text[256];
	int length = snprintf(text, sizeof(text),
	                      "{\"v\":1,\"boot\":4711,\"t\":%llu,\"values\":{\"GX_BATT_SOC\":%d.5,\"GX_BATT_V\":26.41,\"GX_SW_B2B\":\"on\"}}",
	                      (unsigned long long)(sim_ms - broker.flow_since_ms), 50 + broker.published % 40);

	NEED(length > 0 && (size_t)length < sizeof(text));
	publish(CERBO_TOPIC, text, (size_t)length);
	broker.published++;
}

// What happens by itself as time passes
static void world(void)
{
	if(net.fd < 0)
	{
		return;
	}
	if(net.attempting && !net.attempt_over && net.attempt_ends && sim_ms >= net.attempt_done_ms)
	{
		net.attempt_over = true;
		if(broker.attempt == REFUSES)
		{
			net.attempt_error = ECONNREFUSED;
		}
		else
		{
			net.connected = true;
		}
	}
	if(broker.subscribed && broker.flow && !broker.mute && sim_ms >= broker.bundle_ms)
	{
		bundle();
		broker.bundle_ms += broker.flow_ms;
	}
	// The huge message arrives as fast as it is read
	while(broker.junk_left > 0 && rx_count < 4096)
	{
		rx[rx_count++] = 'x';
		broker.junk_left--;
		if(broker.junk_left == 0)
		{
			size_t count = later_count;

			later_count = 0;
			push(later, count);
		}
	}
}

static bool readable(void)
{
	return rx_count > 0 || net.peer_closed || net.error != 0 || net.spurious > 0;
}

static int sim_socket(int domain, int type, int protocol)
{
	net.calls++;
	net.socket_calls++;
	acts.sockets++;
	CHECK(locked == 0);
	CHECK(domain == AF_INET && type == SOCK_STREAM && protocol == 0);
	// One socket at a time: lwIP has ten for the whole display, and one that is forgotten is gone for good
	CHECK(net.fd < 0);
	if(net.no_socket)
	{
		net.no_socket = false;
		errno = ENFILE;
		return -1;
	}
	connection_gone();
	// Another number with each: one that was closed is not taken for the one that is open
	net.fd = net.high ? FD_SETSIZE : FD_FIRST + net.made % 4;
	net.high = false;
	net.made++;
	return net.fd;
}

static int sim_fcntl(int descriptor, int command, int argument)
{
	net.calls++;
	net.fcntls++;
	CHECK(locked == 0);
	// Also: no descriptor select() could not wait for gets this far
	if(!CHECK(descriptor >= 0 && descriptor < FD_SETSIZE && descriptor == net.fd))
	{
		errno = EBADF;
		return -1;
	}
	if(command == F_GETFL)
	{
		if(net.getfl_fails)
		{
			net.getfl_fails = false;
			errno = EBADF;
			return -1;
		}
		return O_RDWR | (net.nonblocking ? O_NONBLOCK : 0);
	}
	// lwIP takes O_NONBLOCK and the access modes, and refuses every other flag (lwip_fcntl())
	if(!CHECK(command == F_SETFL && (argument & ~(O_NONBLOCK | O_RDONLY | O_WRONLY | O_RDWR)) == 0))
	{
		errno = ENOSYS;
		return -1;
	}
	if(net.setfl_fails)
	{
		net.setfl_fails = false;
		errno = ENOSYS;
		return -1;
	}
	net.nonblocking = (argument & O_NONBLOCK) != 0;
	return 0;
}

static int sim_inet_pton(int family, const char *text, void *address)
{
	// The address of the order, copied while the lock was held: a text that ends within its room
	if(!CHECK(family == AF_INET && text != NULL && memchr(text, '\0', CERBO_HOST_SIZE) != NULL) ||
	   !CHECK(strcmp(text, handed_host) == 0))
	{
		return 0;
	}
	return inet_pton(family, text, address);
}

static int sim_connect(int descriptor, const struct sockaddr *address, socklen_t length)
{
	char text[INET_ADDRSTRLEN] = "";
	struct sockaddr_in in;
	bool listening;

	net.calls++;
	net.connects++;
	acts.connects++;
	CHECK(locked == 0);
	if(!CHECK(descriptor >= 0 && descriptor < FD_SETSIZE && descriptor == net.fd))
	{
		errno = EBADF;
		return -1;
	}
	// A socket that blocks would hold the task in this call for as long as the stack tries
	CHECK(net.nonblocking);
	CHECK(!net.attempting && !net.connected);
	if(CHECK(address != NULL && length == sizeof(in)))
	{
		memcpy(&in, address, sizeof(in));
		// The address the core named with its order, the port of the broker, and no name that is looked up
		CHECK(in.sin_family == AF_INET && in.sin_port == htons(CERBO_PORT));
		NEED(inet_ntop(AF_INET, &in.sin_addr, text, sizeof(text)) != NULL);
		CHECK(strcmp(text, handed_host) == 0);
	}
	listening = strcmp(text, broker.address) == 0;
	if(listening && broker.attempt == UNREACHABLE)
	{
		errno = EHOSTUNREACH;
		return -1;
	}
	net.attempting = true;
	if(listening && broker.attempt == AT_ONCE)
	{
		net.attempt_over = true;
		net.connected = true;
		return 0;
	}
	// Where nothing listens, nothing answers
	net.attempt_ends = listening && broker.attempt != STALLS;
	net.attempt_done_ms = sim_ms + broker.accept_ms;
	errno = EINPROGRESS;
	return -1;
}

static int sim_select(int count, fd_set *reading, fd_set *writing, fd_set *failing, struct timeval *limit)
{
	fd_set *set = reading != NULL ? reading : writing;
	uint32_t limit_ms = GX_WAIT_MS;
	bool sane;

	// Nothing has happened yet in this wait: the task can be left here and entered again from its top
	leave_if_over();
	net.calls++;
	net.selects++;
	CHECK(locked == 0);
	// The one socket, in one set
	sane = CHECK(net.fd >= 0 && net.fd < FD_SETSIZE && failing == NULL && (reading == NULL) != (writing == NULL) &&
	             FD_ISSET(net.fd, set) && count == net.fd + 1);
	// A wait with an end: a tenth of a second at most, so that the core is asked again
	if(CHECK(limit != NULL && limit->tv_sec == 0 && limit->tv_usec > 0 && limit->tv_usec <= 100000))
	{
		limit_ms = (uint32_t)(limit->tv_usec / 1000);
	}
	// While an attempt is under way its end shows as "writable"; a connection is waited for as "readable"
	sane = CHECK((writing != NULL) == net.attempting) && sane;
	if(net.select_error != 0)
	{
		int error = net.select_error;

		net.select_error = 0;
		pass(1);
		errno = error;
		return -1;
	}
	for(uint32_t waited = 0; ; waited++)
	{
		if(sane && (net.attempting ? net.attempt_over : readable()))
		{
			return 1;
		}
		if(waited >= limit_ms)
		{
			break;
		}
		pass(1);
	}
	// Nothing happened: the sets come back empty
	if(set != NULL)
	{
		FD_ZERO(set);
	}
	return 0;
}

static int sim_getsockopt(int descriptor, int level, int name, void *value, socklen_t *size)
{
	int error = net.attempt_error;

	net.calls++;
	CHECK(locked == 0);
	if(!CHECK(descriptor >= 0 && descriptor == net.fd && level == SOL_SOCKET && name == SO_ERROR && value != NULL &&
	          size != NULL && *size == sizeof(int)))
	{
		errno = EINVAL;
		return -1;
	}
	// Asked when select() has said that the attempt is over, and not before
	CHECK(net.attempting && net.attempt_over);
	// Reading it takes it away
	net.attempting = false;
	net.attempt_error = 0;
	if(net.sockopt_fails)
	{
		net.sockopt_fails = false;
		errno = EBADF;
		return -1;
	}
	memcpy(value, &error, sizeof(error));
	return 0;
}

static ssize_t sim_recv(int descriptor, void *data, size_t size, int flags)
{
	size_t count = rx_count;

	net.calls++;
	net.recvs++;
	CHECK(locked == 0);
	if(!CHECK(descriptor >= 0 && descriptor == net.fd && data != NULL))
	{
		errno = EBADF;
		return -1;
	}
	// Never more than the core takes in one report
	if(!CHECK(size >= 1 && size <= CERBO_READ_MAX && flags == 0))
	{
		errno = EINVAL;
		return -1;
	}
	// Of a connection that stands, and that select() was asked about
	CHECK(net.connected && !net.attempting);
	if(net.error != 0)
	{
		errno = net.error;
		return -1;
	}
	if(count == 0)
	{
		if(net.spurious > 0)
		{
			net.spurious--;
		}
		else if(net.peer_closed)
		{
			return 0;
		}
		errno = EWOULDBLOCK;
		return -1;
	}
	if(count > size)
	{
		count = size;
	}
	if(net.piece != 0 && count > net.piece)
	{
		count = net.piece;
	}
	// CERBO_TURN_MS between two reads that brought bytes: what a broker can make the display read is limited
	CHECK(net.reads == 0 || sim_ms - net.bytes_ms >= CERBO_TURN_MS);
	net.reads++;
	net.bytes_ms = sim_ms;
	if(sim_ms / 1000 != net.second)
	{
		net.second = sim_ms / 1000;
		net.second_bytes = 0;
	}
	net.second_bytes += count;
	if(net.second_bytes > net.most_a_second)
	{
		net.most_a_second = net.second_bytes;
	}
	memcpy(data, rx, count);
	memcpy(net.delivered, rx, count);
	net.delivered_length = count;
	memmove(rx, rx + count, rx_count - count);
	rx_count -= count;
	return (ssize_t)count;
}

// What the display sent, as the broker reads it: one whole packet with each send(), and each as it has to be
static void broker_takes(const uint8_t *data, size_t size)
{
	static const uint8_t connack[] = { 0x20, 0x02, 0x00, 0x00 };
	static const uint8_t suback[] = { 0x90, 0x03, 0x00, CERBO_PACKET_ID, 0x00 };
	static const uint8_t pingresp[] = { 0xD0, 0x00 };
	// MQTT 3.1.1, 3.1: the protocol, level 4, a clean session and nothing else, the keep-alive, the client id
	static const uint8_t connect_head[] = { 0x10, 12 + sizeof(CLIENT_ID) - 1, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x02,
	                                        0x00, CERBO_KEEPALIVE_S, 0x00, sizeof(CLIENT_ID) - 1 };
	// 3.8: the packet identifier, one topic, QoS 0
	static const uint8_t subscribe_head[] = { 0x82, 5 + sizeof(CERBO_TOPIC) - 1, 0x00, CERBO_PACKET_ID, 0x00,
	                                          sizeof(CERBO_TOPIC) - 1 };

	if(data[0] == 0x10)
	{
		broker.connects++;
		CHECK(size == sizeof(connect_head) + sizeof(CLIENT_ID) - 1 && memcmp(data, connect_head, sizeof(connect_head)) == 0 &&
		      memcmp(data + sizeof(connect_head), CLIENT_ID, sizeof(CLIENT_ID) - 1) == 0);
		if(!broker.mute)
		{
			push(connack, sizeof(connack));
		}
	}
	else if(data[0] == 0x82)
	{
		broker.subscribes++;
		CHECK(size == sizeof(subscribe_head) + sizeof(CERBO_TOPIC) && memcmp(data, subscribe_head, sizeof(subscribe_head)) == 0 &&
		      memcmp(data + sizeof(subscribe_head), CERBO_TOPIC, sizeof(CERBO_TOPIC) - 1) == 0 && data[size - 1] == 0x00);
		if(!broker.mute)
		{
			push(suback, sizeof(suback));
			broker.subscribed = true;
			broker.bundle_ms = sim_ms + broker.flow_ms;
		}
	}
	else if(data[0] == 0xC0)
	{
		broker.pings++;
		CHECK(size == 2 && data[1] == 0x00);
		if(!broker.mute)
		{
			push(pingresp, sizeof(pingresp));
		}
	}
}

static ssize_t sim_send(int descriptor, const void *data, size_t size, int flags)
{
	const uint8_t *bytes = data;

	net.calls++;
	net.sends++;
	acts.sends++;
	CHECK(locked == 0);
	if(!CHECK(descriptor >= 0 && descriptor == net.fd && data != NULL))
	{
		errno = EBADF;
		return -1;
	}
	CHECK(net.connected && !net.attempting && net.nonblocking && flags == 0);
	// A packet, and one the display may send: CONNECT, SUBSCRIBE or PINGREQ. Never a PUBLISH (0x30 to 0x3F).
	if(!CHECK(size >= 2 && size <= MQTT_SUB_OUT_MAX) || !CHECK(bytes[0] == 0x10 || bytes[0] == 0x82 || bytes[0] == 0xC0))
	{
		errno = EINVAL;
		return -1;
	}
	// The bytes the core handed out with this order, all of them and nothing else
	CHECK(acts.order == CERBO_DO_SEND && size == handed_length && memcmp(bytes, handed, size) == 0);
	if(net.send_fails)
	{
		net.send_fails = false;
		errno = ECONNRESET;
		return -1;
	}
	if(net.send_short)
	{
		net.send_short = false;
		return (ssize_t)size - 1;
	}
	broker_takes(bytes, size);
	return (ssize_t)size;
}

static int sim_close(int descriptor)
{
	net.calls++;
	CHECK(locked == 0);
	// Every socket is closed once, and nothing else ever is
	if(CHECK(descriptor >= 0 && descriptor == net.fd))
	{
		net.fd = -1;
		net.closed++;
		acts.closes++;
		connection_gone();
	}
	return 0;
}

/* What gx.c hands to the core -------------------------------------------------------------------------- */

// Every call into the core: under the lock, on the source of the app
static void in_core(const cerbo_t *cerbo)
{
	CHECK(locked == 1);
	CHECK(cerbo == &platform_app->cerbo);
}

// ... and with a time: one that was read for this call, before the lock was taken
static void timed(uint64_t now_ms)
{
	CHECK(time_fresh && now_ms == time_read);
	time_fresh = false;
}

// The order before this round was carried out, once
static void order_done(void)
{
	switch(acts.order)
	{
		case CERBO_DO_OPEN:
			// A socket and the beginning of a connection, or the report that there is none
			CHECK(acts.sockets <= 1 && acts.connects <= 1 && acts.sends == 0 && (acts.connects == 1 || acts.downs == 1));
			break;
		case CERBO_DO_SEND:
			// One send(), or the report that nothing could be sent
			CHECK(acts.sends <= 1 && acts.sockets == 0 && (acts.sends == 1 || acts.downs == 1));
			break;
		case CERBO_DO_CLOSE:
			// The socket is closed, and nothing is reported: the core has given the connection up itself
			CHECK(acts.closes == (acts.had_socket ? 1 : 0) && acts.downs == 0 && acts.sockets == 0 && acts.sends == 0);
			break;
		default:
			CHECK(acts.sockets == 0 && acts.connects == 0 && acts.sends == 0);
			break;
	}
}

static cerbo_do_t sim_cerbo_next(cerbo_t *cerbo, uint64_t now_ms)
{
	in_core(cerbo);
	timed(now_ms);
	order_done();
	rounds++;
	// A task that never waits starves everything below it, and here no time would ever pass
	if(sim_ms != round_ms)
	{
		round_ms = sim_ms;
		rounds_same_ms = 0;
	}
	else if(++rounds_same_ms > SPIN_ROUNDS)
	{
		FAILED("the task turns without waiting");
		rounds_same_ms = 0;
		locked = 0;
		memset(&acts, 0, sizeof(acts));
		// As if it had waited: the scenario goes on
		pass(GX_WAIT_MS);
		longjmp(out, 1);
	}
	memset(&acts, 0, sizeof(acts));
	acts.order = cerbo_next(cerbo, now_ms);
	acts.had_socket = net.fd >= 0;
	return acts.order;
}

static const char *sim_cerbo_host(const cerbo_t *cerbo)
{
	const char *host = cerbo_host(cerbo);

	in_core(cerbo);
	if(host_other != NULL)
	{
		host = host_other;
		host_other = NULL;
	}
	snprintf(handed_host, sizeof(handed_host), "%s", host);
	return host;
}

static const uint8_t *sim_cerbo_out(const cerbo_t *cerbo, size_t *length)
{
	const uint8_t *bytes = cerbo_out(cerbo, length);

	in_core(cerbo);
	if(out_none)
	{
		out_none = false;
		*length = 0;
	}
	else if(out_long)
	{
		out_long = false;
		memset(long_packet, 0xC0, sizeof(long_packet));
		bytes = long_packet;
		*length = sizeof(long_packet);
	}
	handed_length = *length;
	memcpy(handed, bytes, handed_length <= sizeof(handed) ? handed_length : sizeof(handed));
	return bytes;
}

static void sim_cerbo_opened(cerbo_t *cerbo, uint64_t now_ms)
{
	in_core(cerbo);
	timed(now_ms);
	// Only a connection that stands is reported as one
	CHECK(net.fd >= 0 && net.connected && !net.attempting);
	openeds++;
	cerbo_opened(cerbo, now_ms);
}

static void sim_cerbo_bytes(cerbo_t *cerbo, const uint8_t *data, size_t length, uint64_t now_ms, json_token_t *work, int work_count)
{
	in_core(cerbo);
	timed(now_ms);
	// What the last recv() brought, all of it and nothing else, and a room for the tokens that is this task's
	if(CHECK(data != NULL && length >= 1 && length <= CERBO_READ_MAX && length == net.delivered_length) &&
	   CHECK(memcmp(data, net.delivered, length) == 0) && CHECK(work != NULL && work != platform_app->work && work_count == CERBO_TOKENS))
	{
		reports++;
		cerbo_bytes(cerbo, data, length, now_ms, work, work_count);
	}
	net.delivered_length = 0;
}

static void sim_cerbo_down(cerbo_t *cerbo, uint64_t now_ms)
{
	in_core(cerbo);
	timed(now_ms);
	// The socket is closed before the core hears of it: its next order may be to open another
	CHECK(net.fd < 0);
	acts.downs++;
	downs++;
	cerbo_down(cerbo, now_ms);
}

/* The scenarios ---------------------------------------------------------------------------------------- */

static void state(void)
{
	printf("   %llu ms: socket %d attempting %d connected %d, made %d closed %d sent %d, opened %d down %d, phase %d order %d, "
	       "bundles %u of %d, sessions %u\n",
	       (unsigned long long)sim_ms, net.fd, net.attempting, net.connected, net.made, net.closed, net.sends, openeds, downs,
	       (int)platform_app->cerbo.phase, (int)acts.order, (unsigned)platform_app->cerbo.bundles, broker.published,
	       (unsigned)platform_app->cerbo.sessions);
}

// Lets the task run for `ms` of the time of the world
static void run(uint64_t ms)
{
	run_until_ms = sim_ms + ms;
	if(setjmp(out) == 0)
	{
		if(CHECK(the_task != NULL))
		{
			the_task(NULL);
			// A task function of FreeRTOS never returns
			FAILED("the task function returned");
		}
		else
		{
			pass(ms);
		}
	}
	CHECK(locked == 0);
	locked = 0;
}

// A start of the app with these settings. The app is new; gx.c is as it was.
static void app_start(const char *settings, bool safe_mode)
{
	static json_token_t work[LAYOUT_TOKENS];
	app_boot_t boot;

	if(platform_app == NULL)
	{
		platform_app = sim_alloc(sizeof(app_t));
	}
	memset(&boot, 0, sizeof(boot));
	boot.version = "0.1.0";
	boot.git = "sim";
	boot.safe_mode = safe_mode;
	boot.settings_json = settings;
	boot.ap_ssid = NAME;
	boot.work = work;
	boot.work_count = LAYOUT_TOKENS;
	app_init(platform_app, &boot, sim_ms);
}

// What the network task reports to the source through app_net(): the display is in a network or not
static void network(bool up)
{
	platform_lock();
	cerbo_network(&platform_app->cerbo, up, sim_ms);
	platform_unlock();
}

static void set_host(const char *host)
{
	platform_lock();
	cerbo_set_host(&platform_app->cerbo, host, sim_ms);
	platform_unlock();
}

static bool in_session(void)
{
	return platform_app->cerbo.phase == CERBO_SESSION && net.fd >= 0 && net.connected;
}

static cerbo_view_t view(void)
{
	return cerbo_view(&platform_app->cerbo, sim_ms);
}

// The network goes and comes back, with rounds of the task in between: whatever the source waited for, an
// attempt is due at once (cerbo.h)
static void again(void)
{
	network(false);
	run(300);
	network(true);
}

static void test_start(void)
{
	app_start(NULL, false);
	sim_state = state;

	// No room: no task, and the next call asks again
	heap_fails = true;
	CHECK(gx_start() == ESP_ERR_NO_MEM && rooms == 0 && tasks == 0);
	heap_fails = false;
	// A room and no task: the room is kept, the next call asks for no second one
	task_fails = true;
	CHECK(gx_start() == ESP_ERR_NO_MEM && rooms == 1 && tasks == 0);
	task_fails = false;
	CHECK(gx_start() == ESP_OK && rooms == 1 && tasks == 1);
	// There is never a second task
	CHECK(gx_start() == ESP_ERR_INVALID_STATE && rooms == 1 && tasks == 1);
}

// A task that runs although the source is off: it opens nothing
static void test_off(void)
{
	int before;

	// No address, in a network
	network(true);
	before = rounds;
	run(3000);
	CHECK(net.calls == 0 && downs == 0);
	// It waits: about ten rounds a second
	CHECK(rounds - before >= 28 && rounds - before <= 36 && delays == rounds - before);

	// An address, and the safe mode
	app_start(SETTINGS, true);
	network(true);
	run(3000);
	CHECK(net.calls == 0 && view() == CERBO_VIEW_OFF);

	// An address, and no network
	app_start(SETTINGS, false);
	run(3000);
	CHECK(net.calls == 0 && view() == CERBO_VIEW_NO_WIFI);
}

static void test_session(void)
{
	int selects;

	network(true);
	run(500);
	// One socket that does not block, a connection to the broker, CONNECT and SUBSCRIBE, each once
	CHECK(net.made == 1 && net.socket_calls == 1 && net.connects == 1 && net.nonblocking && openeds == 1 && net.sends == 2);
	CHECK(broker.connects == 1 && broker.subscribes == 1 && in_session() && platform_app->cerbo.sessions == 1);
	CHECK(strcmp(handed_host, ADDRESS) == 0);

	selects = net.selects;
	run(60000);
	CHECK(in_session() && view() == CERBO_VIEW_LIVE && net.made == 1 && net.closed == 0 && downs == 0);
	// Every bundle the flow published was read, one a second
	CHECK(broker.published >= 59 && broker.published <= 61 && platform_app->cerbo.bundles == (uint32_t)broker.published);
	CHECK(platform_app->cerbo.values.count == 3 && cerbo_refused(&platform_app->cerbo) == 0 && platform_app->cerbo.lag_ms == 0);
	// A PINGREQ every ten seconds, each answered
	CHECK(broker.pings >= 5 && broker.pings <= 6 && platform_app->cerbo.pings_open == 0 && net.sends == 2 + broker.pings);
	// It waits for the socket, a tenth of a second at a time
	CHECK(net.selects - selects >= 600 && net.selects - selects <= 760);
	// One report for each read that brought bytes, and a pause behind each
	CHECK(reports == net.reads);
}

static void test_pieces(void)
{
	uint32_t before = platform_app->cerbo.bundles;
	int published;

	// A byte at a time: a bundle arrives cut at every byte, and is read all the same
	net.piece = 1;
	run(5000);
	CHECK(platform_app->cerbo.bundles >= before + 1 && in_session());
	// In sevens, and then what piled up meanwhile in one go
	net.piece = 7;
	run(3000);
	net.piece = 0;
	run(3000);
	CHECK(platform_app->cerbo.bundles == (uint32_t)broker.published && cerbo_refused(&platform_app->cerbo) == 0);
	CHECK(view() == CERBO_VIEW_LIVE && net.closed == 0);

	// Two bundles in one segment
	published = broker.published;
	bundle();
	bundle();
	run(200);
	CHECK(broker.published >= published + 2 && platform_app->cerbo.bundles == (uint32_t)broker.published);
}

// The broker closes, and the display finds its way back
static void test_broker_closes(void)
{
	int made = net.made;

	net.peer_closed = true;
	run(300);
	// Closed once and reported once
	CHECK(net.closed == 1 && downs == 1 && net.fd < 0 && platform_app->cerbo.phase == CERBO_IDLE);
	// After a session that brought bundles the next attempt follows after two seconds, not before
	run(1500);
	CHECK(net.made == made);
	run(1000);
	CHECK(net.made == made + 1 && in_session() && platform_app->cerbo.sessions == 2 && broker.connects == 2);
	run(3000);
	CHECK(view() == CERBO_VIEW_LIVE);
}

static void test_read_fails(void)
{
	int made = net.made;
	int closed = net.closed;
	int reported = downs;

	// The connection is dead: readable, and the read fails
	net.error = ECONNRESET;
	run(300);
	CHECK(net.closed == closed + 1 && downs == reported + 1 && net.fd < 0);
	run(2500);
	CHECK(net.made == made + 1 && in_session());

	// Readable, and then nothing to read: no error, the connection stays
	net.spurious = 3;
	run(1000);
	CHECK(net.spurious == 0 && net.closed == closed + 1 && downs == reported + 1 && in_session());

	// A wait that a signal ended: nothing happened
	net.select_error = EINTR;
	run(500);
	CHECK(net.select_error == 0 && net.closed == closed + 1 && downs == reported + 1 && in_session());
	// A wait that fails: the socket is of no use any more
	net.select_error = EBADF;
	run(300);
	CHECK(net.closed == closed + 2 && downs == reported + 2 && net.fd < 0);
	run(2500);
	CHECK(in_session());
}

// Attempts that fail, each in its way: the socket is closed once, the core is told once
static void test_attempt_fails(void)
{
	int made, closed, reported, opened, connects, fcntls, asked;
	uint32_t bundles = platform_app->cerbo.bundles;

	// The session that stands is half a second old and has brought no bundle: if it ended now it would be an
	// attempt that failed itself, and the wait behind it the second one, 5 s (cerbo.h, since 2026-10-12). The
	// flow publishes a second after the SUBSCRIBE.
	run(1000);
	CHECK(in_session() && platform_app->cerbo.bundles > bundles);

	// The broker is gone and refuses: after two seconds the first attempt, then after 5, 10 and 30
	net.peer_closed = true;
	broker.attempt = REFUSES;
	run(300);
	made = net.made;
	closed = net.closed;
	reported = downs;
	opened = openeds;
	run(58000);
	CHECK(net.made == made + 4 && net.closed == closed + 4 && downs == reported + 4 && openeds == opened && net.fd < 0);
	CHECK(view() == CERBO_VIEW_UNREACHABLE);

	// No way to the address: connect() fails at once
	broker.attempt = UNREACHABLE;
	again();
	run(300);
	CHECK(net.made == made + 5 && net.closed == closed + 5 && downs == reported + 5 && openeds == opened && net.fd < 0);

	// No socket to be had: nothing to close
	net.no_socket = true;
	asked = net.socket_calls;
	connects = net.connects;
	again();
	run(300);
	CHECK(!net.no_socket && net.socket_calls == asked + 1 && net.made == made + 5 && net.closed == closed + 5 &&
	      downs == reported + 6 && net.connects == connects);

	// A socket select() could not wait for: closed, and nothing is done with it
	net.high = true;
	fcntls = net.fcntls;
	again();
	run(300);
	CHECK(!net.high && net.made == made + 6 && net.closed == closed + 6 && downs == reported + 7 &&
	      net.connects == connects && net.fcntls == fcntls);

	// A socket whose flags cannot be read, and one that would block: no connect() is tried on either
	net.getfl_fails = true;
	again();
	run(300);
	CHECK(!net.getfl_fails && net.made == made + 7 && net.closed == closed + 7 && downs == reported + 8 && net.connects == connects);
	net.setfl_fails = true;
	again();
	run(300);
	CHECK(!net.setfl_fails && net.made == made + 8 && net.closed == closed + 8 && downs == reported + 9 && net.connects == connects);

	// The attempt ends, and how it ended cannot be read: no connection
	broker.attempt = ACCEPTS;
	net.sockopt_fails = true;
	again();
	run(300);
	CHECK(!net.sockopt_fails && net.made == made + 9 && net.closed == closed + 9 && downs == reported + 10 && openeds == opened);

	// An address that is none (the core never hands one out): no socket is made for it, and no name looked up
	host_other = "einstein";
	asked = net.socket_calls;
	again();
	run(300);
	CHECK(host_other == NULL && net.socket_calls == asked && net.made == made + 9 && downs == reported + 11);

	// A PC may say at once that the connection stands: it is asked all the same, and the session follows
	broker.attempt = AT_ONCE;
	again();
	run(500);
	CHECK(net.made == made + 10 && openeds == opened + 1 && in_session() && net.closed == closed + 9);
	broker.attempt = ACCEPTS;
}

// Nothing answers: the core gives up, the task closes and reports nothing
static void test_stalls(void)
{
	int made, closed, reported;

	net.peer_closed = true;
	broker.attempt = STALLS;
	run(300);
	again();
	run(300);
	made = net.made;
	closed = net.closed;
	reported = downs;
	// An attempt that never ends: the socket waits ...
	CHECK(net.fd >= 0 && net.attempting && !net.attempt_over);
	run(14000);
	CHECK(net.fd >= 0 && net.closed == closed);
	// ... until the core orders it closed, CERBO_SESSION_MS after it ordered it opened
	run(1200);
	CHECK(net.fd < 0 && net.closed == closed + 1 && downs == reported && net.made == made);

	// A broker that takes the connection and never says CONNACK: the same
	broker.attempt = ACCEPTS;
	broker.mute = true;
	again();
	run(500);
	CHECK(net.made == made + 1 && net.connected && platform_app->cerbo.phase == CERBO_GREETING);
	run(15000);
	CHECK(net.fd < 0 && net.closed == closed + 2 && downs == reported);
	broker.mute = false;
	again();
	run(500);
	CHECK(in_session());
}

// A pause of the radio link in a session: no bundle, no PINGRESP. The core ends the session.
static void test_pause(void)
{
	int closed = net.closed;
	int reported = downs;
	uint32_t sessions = platform_app->cerbo.sessions;

	run(3000);
	broker.mute = true;
	run(9000);
	CHECK(in_session() && net.closed == closed);
	// Ten seconds to the next PINGREQ at most, twenty for its answer: closed, and nothing reported
	run(22000);
	CHECK(!in_session() && net.closed == closed + 1 && downs == reported);
	broker.mute = false;
	again();
	run(500);
	CHECK(in_session() && platform_app->cerbo.sessions == sessions + 1);
}

// Runs until a switch of the world that works once has worked: the next PINGREQ is ten seconds away at most
static void until_used(const bool *waiting)
{
	for(int i = 0; i < 120 && *waiting; i++)
	{
		run(100);
	}
	CHECK(!*waiting);
}

static void test_send_fails(void)
{
	int closed = net.closed;
	int reported = downs;
	int sent = net.sends;

	// The socket takes one byte less than the packet has: nothing is sent a second time, the connection goes
	net.send_short = true;
	until_used(&net.send_short);
	CHECK(net.sends == sent + 1 && net.closed == closed + 1 && downs == reported + 1 && net.fd < 0);
	run(3000);
	CHECK(in_session());

	// ... and a send that fails
	net.send_fails = true;
	sent = net.sends;
	until_used(&net.send_fails);
	CHECK(net.sends == sent + 1 && net.closed == closed + 2 && downs == reported + 2 && net.fd < 0);
	run(3000);
	CHECK(in_session());

	// An order without a packet, and one with a packet too long for any room (the core hands out neither):
	// nothing is sent, least of all a part of it
	out_none = true;
	sent = net.sends;
	until_used(&out_none);
	CHECK(net.sends == sent && net.closed == closed + 3 && downs == reported + 3 && net.fd < 0);
	run(3000);
	CHECK(in_session());
	out_long = true;
	sent = net.sends;
	until_used(&out_long);
	CHECK(net.sends == sent && net.closed == closed + 4 && downs == reported + 4 && net.fd < 0);
	run(3000);
	CHECK(in_session());
}

static void test_flood(void)
{
	int closed = net.closed;
	int reported = downs;
	int published;
	uint32_t refused;

	// A hundred bundles a second: each one is read, the last one counts
	run(1000);
	broker.flow_ms = 10;
	broker.bundle_ms = sim_ms;
	published = broker.published;
	run(10000);
	broker.flow_ms = 1000;
	run(500);
	CHECK(broker.published >= published + 990 && platform_app->cerbo.bundles == (uint32_t)broker.published);
	CHECK(in_session() && view() == CERBO_VIEW_LIVE && net.closed == closed && cerbo_refused(&platform_app->cerbo) == 0);

	// A message of 64 KB goes by, and what follows it is read as before
	refused = platform_app->cerbo.refused_long;
	push((const uint8_t *)"\x30\x80\x80\x04", 4);
	broker.junk_left = 65536;
	run(4000);
	CHECK(broker.junk_left == 0 && platform_app->cerbo.refused_long == refused + 1 && in_session());
	CHECK(platform_app->cerbo.bundles == (uint32_t)broker.published && view() == CERBO_VIEW_LIVE);

	// One of 4 MB: nothing of it is kept, and no bundle and no PINGRESP gets past it. The core ends the
	// session when the answer to a PINGREQ is twenty seconds late.
	push((const uint8_t *)"\x30\x80\x80\x80\x02", 5);
	broker.junk_left = 4u * 1024u * 1024u;
	run(31000);
	CHECK(net.closed == closed + 1 && downs == reported && platform_app->cerbo.refused_long == refused + 1);
	run(3000);
	CHECK(in_session());

	// What the task read in any one second of all this: at most CERBO_READ_MAX bytes every CERBO_TURN_MS
	CHECK(net.most_a_second >= 40000 && net.most_a_second <= CERBO_READ_MAX * (1000 / CERBO_TURN_MS));
}

// The WiFi goes and comes back
static void test_wifi(void)
{
	int made = net.made;
	int closed = net.closed;
	int reported = downs;

	// Between two rounds of the task: the core orders nothing, and the connection lives on if lwIP kept it
	network(false);
	network(true);
	run(3000);
	CHECK(net.closed == closed && net.made == made && in_session() && view() == CERBO_VIEW_LIVE);

	// ... and if lwIP aborted it with the address of the display, the socket says so
	network(false);
	network(true);
	net.error = ECONNABORTED;
	run(300);
	CHECK(net.closed == closed + 1 && downs == reported + 1 && net.fd < 0);
	run(3000);
	CHECK(net.made == made + 1 && in_session());

	// Seen by the core: it orders the connection closed, and no attempt is made without a network
	network(false);
	run(300);
	CHECK(net.closed == closed + 2 && downs == reported + 1 && net.fd < 0 && view() == CERBO_VIEW_NO_WIFI);
	run(5000);
	CHECK(net.made == made + 1);
	// With the network the attempt comes at once
	network(true);
	run(500);
	CHECK(net.made == made + 2 && in_session());
}

static void test_address(void)
{
	int made = net.made;
	int closed = net.closed;
	int reported = downs;

	// Another address: the connection is closed first, then the new address is tried at once. Nothing
	// listens there.
	set_host("192.168.88.9");
	run(300);
	CHECK(net.closed == closed + 1 && downs == reported && net.made == made + 1 && net.attempting);
	CHECK(strcmp(handed_host, "192.168.88.9") == 0);
	// Back to the broker
	set_host(ADDRESS);
	run(500);
	CHECK(net.closed == closed + 2 && net.made == made + 2 && in_session() && strcmp(handed_host, ADDRESS) == 0);

	// The source is switched off: closed, and nothing is opened any more
	set_host("");
	run(300);
	CHECK(net.closed == closed + 3 && downs == reported && net.fd < 0 && view() == CERBO_VIEW_OFF);
	run(5000);
	CHECK(net.made == made + 2);
	set_host(ADDRESS);
	run(500);
	CHECK(net.made == made + 3 && in_session());
}

// The app is new and the task is as it was: an order to open finds the old socket, which must not be lost
static void test_app_anew(void)
{
	int made = net.made;
	int closed = net.closed;
	int reported = downs;

	CHECK(in_session());
	app_start(SETTINGS, false);
	network(true);
	run(500);
	CHECK(net.closed == closed + 1 && net.made == made + 1 && downs == reported && in_session());
	CHECK(platform_app->cerbo.sessions == 1);
}

int main(void)
{
	// One display from its first start on: each scenario finds gx.c and the world as the one before left them
	test_start();
	test_off();
	test_session();
	test_pieces();
	test_broker_closes();
	test_read_fails();
	test_attempt_fails();
	test_stalls();
	test_pause();
	test_send_fails();
	test_flood();
	test_wifi();
	test_address();
	test_app_anew();

	// Every socket that was made is closed but the one that is open
	CHECK(net.made == net.closed + (net.fd >= 0 ? 1 : 0));
	// Nothing but CONNECT, SUBSCRIBE and PINGREQ ever left: every send() brought the broker one of them, but
	// for the two that failed
	CHECK(net.sends == broker.connects + broker.subscribes + broker.pings + 2 && broker.connects >= 10);
	CHECK(delays >= net.reads);
	printf("%d rounds of the task in %llu s: %d sockets, %d packets sent, %d reads, at most %u bytes read in a second\n",
	       rounds, (unsigned long long)(sim_ms / 1000), net.made, net.sends, net.reads, (unsigned)net.most_a_second);
	return sim_end();
}
