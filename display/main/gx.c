/*
 * Standalone display for the WiCAN W906 fork.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "platform.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

/*
 * The task that talks to the broker of the Cerbo GX: one task, one socket. It decides nothing. In every
 * round it asks cerbo_next() what to do with the socket (cerbo.h), carries that out, waits for the socket
 * and reports what happened. When to connect, what the bytes mean, when a connection is dead: all of that is
 * the core's, where it is tested on the host. The display publishes nothing: the bytes that leave here are
 * the ones the core built, and it can build a CONNECT, a SUBSCRIBE and a PINGREQ and nothing else
 * (mqtt_sub.h). This file calls no function of an MQTT client; tools/check_build.py looks at the map of the
 * build for one.
 *
 * Written without the board and without a build for it: every call was read in the sources of ESP-IDF
 * v5.5.2 and of its lwIP (espressif/esp-lwip at fd432e4e), 2026-10-11. What only the board can decide is
 * marked CHECK. The file is run on a PC as it is: against stand-ins that play a broker
 * (display/host/platform/gx_sim.c) and against the sockets of the PC (display/host/gx_live).
 *
 * Where the calls come from in ESP-IDF v5.5.2:
 *   socket() connect() send() recv() getsockopt() inet_pton()
 *       <sys/socket.h> (components/lwip/port/esp32xx/include) is lwip/sockets.h of ESP-IDF
 *       (components/lwip/include/lwip), which has each as an inline function that calls lwip_socket() and
 *       so on
 *   fcntl() close() select()
 *       not in that header: LWIP_POSIX_SOCKETS_IO_NAMES is 0 (components/lwip/port/include/lwipopts.h).
 *       They are the ones of newlib - <fcntl.h>, <unistd.h>, <sys/select.h> - and reach lwIP through the
 *       VFS: vfs_lwip.c registers lwip_fcntl(), lwip_close() and lwip_select() for the descriptors of lwIP
 *   <netinet/in.h> <arpa/inet.h>
 *       components/lwip/port/include; named for the PC, which has struct sockaddr_in, htons() and
 *       inet_pton() there
 *
 * A connect() that does not wait, as that lwIP has it (api_msg.c, sockets.c):
 * - On a socket with O_NONBLOCK connect() begins the handshake and returns -1 with EINPROGRESS
 *   (lwip_netconn_do_connect(): ERR_INPROGRESS) - or -1 with another error at once, when there is no way
 *   to the address.
 * - The socket is not writable before it is connected (alloc_socket(): sendevent is 0 for a TCP socket that
 *   was not accepted). It becomes writable when the handshake is done (lwip_netconn_do_connected():
 *   NETCONN_EVT_SENDPLUS) and also when it failed (err_tcp(): NETCONN_EVT_ERROR, then RCVPLUS and SENDPLUS,
 *   "to release selects pending on read or write"). select() for writing ends either way.
 * - getsockopt(SO_ERROR) tells the two apart: it is err_to_errno(netconn_err()), 0 after the handshake and
 *   the error err_tcp() kept after a failure - ECONNRESET for a broker that refuses, ECONNABORTED when the
 *   stack gave the SYN up. Reading it takes it away.
 * - A peer that closes makes the socket readable, and recv() returns 0 (recv_tcp() without data: RCVPLUS;
 *   lwip_recv_tcp(): ERR_CLSD is 0). An error makes it readable as well, and recv() returns -1. So does
 *   the loss of the address of the display: lwIP aborts every connection that had it (tcp.c,
 *   tcp_netif_ip_addr_changed()). Whether ESP-IDF takes the address away when the network is lost was not
 *   read; if it does not, the core ends the session by its PINGREQ, or by its own order when the link
 *   reports the loss.
 * - send() on such a socket never waits: it takes what the send buffer has room for and returns that
 *   number, or -1 with EWOULDBLOCK if there is no room at all (lwip_netconn_do_writemore()). The packets of
 *   the core have 2 to MQTT_SUB_OUT_MAX bytes, the buffer has 11520 (sdkconfig.defaults).
 * A PC does the same in all of this, but for the numbers: a refusal is ECONNREFUSED there.
 * ESP-IDF connects this way itself: esp_tls.c (esp_tls_set_socket_non_blocking(), tcp_connect()) makes the
 * same calls in the same order, with the same headers.
 * CHECK: the log never says "the socket would block". That would be fcntl() refusing the flag, which lwIP
 * does with every flag it does not know (lwip_fcntl()): O_NONBLOCK is newlib's number in this file, and has
 * to be the same number where lwIP was built.
 * CHECK: with the address of a PC in the same network that runs no broker (tools/mock_flow.py stopped), the
 * log says "not connected" with an errno about every 30 s and the info page "nicht erreichbar"; with the
 * address of nothing at all (192.168.88.250) it says nothing - the core gives the attempt up after
 * CERBO_SESSION_MS, before lwIP does - and the info page says the same.
 *
 * Where the task waits: in select(), GX_WAIT_MS at most; in vTaskDelay(), when it has no socket and behind
 * every read. No call is made with the lock held, and no other call waits: the socket never blocks. One
 * exception that was read and not seen: close() waits for lwIP while that has no memory for the last
 * segment of a connection (lwip_netconn_do_close_internal(), up to 20 s).
 * CHECK: the time under the lock for one bundle (cerbo_bytes() with 41 values; reckoned below a
 * millisecond): esp_timer_get_time() around it, for a minute, the largest value.
 * CHECK: the ring of the clear dialog never completes before three seconds of holding, also while bundles
 * arrive, and the knob answers as before (the screen task takes the same lock for every reading).
 * CHECK: after a restart of the Cerbo the display connects again by itself: "connects" of GET /api/info
 * goes up by one, and the view is "live" again within a minute.
 * CHECK: the chip temperature on the info page stays below the 75 degrees where the heat rule begins.
 *
 * The memory. What this task takes of the internal RAM, reckoned and not measured:
 *   its stack                                                       6144 bytes, for good
 *   the task itself, the socket, the connection in lwIP             about 1000 bytes, for good
 *   what lwIP has received and the task has not read yet            up to the window of 11520 bytes
 *                                                                   (CONFIG_LWIP_TCP_WND_DEFAULT; eight
 *                                                                   frames of the WiFi driver), for moments
 * and of the external RAM: the room below, 5200 bytes, and cerbo_t in app_t (5456 bytes as a PC compiles it).
 * Measured WITHOUT this source on the device (2026-10-10 and 2026-10-11, commit df427d0 and the build before
 * it, GET /api/info): heap_malloc about 70 KB after the start and whenever nobody asks the web server;
 * heap_block never below 53 KB then; heap_malloc_min 50 KB after 25 minutes of driving.
 * What takes the low mark down is the web server, not the display by itself (measured 2026-10-11 at 17:57
 * with reading requests only, the vehicle display, commit df427d0): one load of the page leaves heap_malloc
 * at 65 KB; four loads at once take it to 21 KB and heap_block to 12 KB for that moment, and the low mark
 * stood at 4996 bytes afterwards. A browser that opens the page asks for several large answers at once (the
 * low mark fell from 50 to 21 KB in one step when the owner did). lwIP keeps what it sent until the client
 * has confirmed it, up to CONFIG_LWIP_TCP_SND_BUF_DEFAULT (11520 bytes) for each connection. The display went
 * on as before: what malloc() does not find in the internal RAM it takes from the external, and the reserve
 * of 32 KB is not part of these numbers.
 * So the low mark alone says nothing about this source. CHECK, with the source running - the numbers to stop
 * at, each against the same load WITHOUT the source:
 *   while nobody asks the web server (an hour of rest in the vehicle, a read of the fault memory, a pause of
 *   the link of 30 s while tools/mock_flow.py goes on publishing): heap_malloc as GET /api/info reads it
 *   stays above 50 KB, and heap_block is never below 16 KB;
 *   one load of the page in a browser: the low mark lies at most 14 KB below the one without the source;
 *   a firmware upload goes through.
 * If one of them is missed: first the stack, by its measured low mark; this source has no buffer in the
 * internal RAM. The 14 KB were set when the window was 5760 bytes; with 11520 a backlog that arrives at once
 * can take 6 KB more for the moment it takes to read it (CERBO_READ_MAX bytes every CERBO_TURN_MS).
 * If the task cannot be created because the web server holds the internal RAM at that moment, net.c says so
 * and tries again with every round.
 */

#define TAG "gx"

// CHECK: uxTaskGetStackHighWaterMark() of this task after a day. On it: the JSON reader and the reckoning
// of the core, the socket calls of lwIP, and the log. None of them was measured, so it has what the
// network task has, which makes the same kinds of calls: a stack that is too small is a crash, and the
// 2 KB above the 4096 the design reckoned with are what a measurement may give back.
#define GX_STACK_BYTES      6144
#define GX_PRIORITY         4       // below the network task (5) and the web server (5), above the main task (3)
#define GX_CORE             0       // not the core of the panel: LVGL has priority 4 there (screen.c)
#define GX_WAIT_MS          100u    // a round waits this long for the socket at most, or sleeps it away without one

// What is too large for the stack and need not lie in the internal RAM, allocated once in the external RAM
typedef struct
{
	uint8_t in[CERBO_READ_MAX];         // what one recv() brought
	uint8_t out[MQTT_SUB_OUT_MAX];      // the packet of the last CERBO_DO_SEND, copied under the lock
	char host[CERBO_HOST_SIZE];         // the address of the last CERBO_DO_OPEN, copied under the lock
	json_token_t work[CERBO_TOKENS];    // for cerbo_bytes(): not the tokens of the app, which other tasks use
} gx_room_t;

static gx_room_t *room;
static bool running;        // the task was created: there is never a second one
static int fd = -1;         // the one socket, -1 = none
static bool connecting;     // of that socket: connect() was called on it and has not ended yet

// The socket is closed, whoever asked for it. Nothing is reported here.
static void gx_close(void)
{
	if(fd >= 0)
	{
		close(fd);
	}
	fd = -1;
}

// The connection is lost, or never came about: the socket is closed first, then the core is told
// (cerbo_down()). The time is read before the lock is waited for.
static void gx_lost(const char *what, int error)
{
	uint64_t now;

	gx_close();
	ESP_LOGI(TAG, "%s (errno %d)", what, error);

	now = platform_now_ms();
	platform_lock();
	cerbo_down(&platform_app->cerbo, now);
	platform_unlock();
}

// CERBO_DO_OPEN: a socket that never blocks, and the beginning of a connection to the address the core named.
// How it ends is seen by gx_wait(); the core gives it up by itself after CERBO_SESSION_MS (CERBO_DO_CLOSE).
static void gx_open(void)
{
	struct sockaddr_in address;
	int flags;

	// The core orders this only without a connection. A socket that is still there would be lost for good,
	// and lwIP has ten for the whole display (web.c).
	gx_close();

	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(CERBO_PORT);
	// An address in dotted form, as settings.h lets one through; no name is ever looked up
	if(inet_pton(AF_INET, room->host, &address.sin_addr) != 1)
	{
		gx_lost("not connected: no address", 0);
		return;
	}
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if(fd < 0)
	{
		gx_lost("not connected: no socket", errno);
		return;
	}
	// select() takes no descriptor beyond its sets. lwIP hands out none (lwipopts.h: they begin at
	// FD_SETSIZE - CONFIG_LWIP_MAX_SOCKETS); a PC may.
	if(fd >= FD_SETSIZE)
	{
		gx_lost("not connected: a socket select() cannot wait for", 0);
		return;
	}
	flags = fcntl(fd, F_GETFL, 0);
	if(flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
	{
		// A socket that blocks would hold the task in connect() for as long as lwIP tries
		gx_lost("not connected: the socket would block", errno);
		return;
	}
	if(connect(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 && errno != EINPROGRESS)
	{
		gx_lost("not connected", errno);
		return;
	}
	// Also when connect() says that the connection stands already: select() and SO_ERROR say it once more
	connecting = true;
}

// CERBO_DO_SEND: `length` bytes of room->out, all of them at once or the connection is given up. Nothing is
// sent a second time, and nothing but what the core handed out.
static void gx_send(size_t length)
{
	ssize_t sent;

	// An order without a packet cannot be carried out, and that is a send that failed (gx_round() says when
	// there is none)
	if(length == 0)
	{
		gx_lost("not sent: no packet", 0);
		return;
	}
	sent = send(fd, room->out, length, 0);
	if(sent < 0 || (size_t)sent != length)
	{
		gx_lost("not sent", sent < 0 ? errno : 0);
	}
}

// Waits for the socket, GX_WAIT_MS at most, and reports what came of it. Without a socket the time is slept.
static void gx_wait(void)
{
	struct timeval limit = { .tv_sec = 0, .tv_usec = (int)(GX_WAIT_MS * 1000u) };
	fd_set set;
	ssize_t received;
	uint64_t now;
	int ready;

	if(fd < 0)
	{
		vTaskDelay(pdMS_TO_TICKS(GX_WAIT_MS));
		return;
	}

	FD_ZERO(&set);
	FD_SET(fd, &set);
	// While connecting for "writable", which is how both ends of an attempt show (see the head of this file)
	ready = connecting ? select(fd + 1, NULL, &set, NULL, &limit) : select(fd + 1, &set, NULL, NULL, &limit);
	if(ready == 0 || (ready < 0 && errno == EINTR))
	{
		// Nothing happened in this time; a signal that ended the wait is nothing either (a PC has them)
		return;
	}
	if(ready < 0)
	{
		// The socket is of no use any more. Closed, so that the next round does not come back here at once.
		gx_lost("lost: select()", errno);
		return;
	}

	if(connecting)
	{
		int error = 0;
		socklen_t size = sizeof(error);

		if(getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0)
		{
			error = errno != 0 ? errno : -1;
		}
		if(error != 0)
		{
			gx_lost("not connected", error);
			return;
		}
		connecting = false;
		ESP_LOGI(TAG, "connected to %s", room->host);

		now = platform_now_ms();
		platform_lock();
		cerbo_opened(&platform_app->cerbo, now);
		platform_unlock();
		return;
	}

	// One read with each round, and never more than the core takes in one report
	received = recv(fd, room->in, sizeof(room->in), 0);
	if(received > 0)
	{
		now = platform_now_ms();
		platform_lock();
		cerbo_bytes(&platform_app->cerbo, room->in, (size_t)received, now, room->work, CERBO_TOKENS);
		platform_unlock();
		// CERBO_TURN_MS between two reads: a broker cannot make the display read more than CERBO_READ_MAX
		// bytes in that time (51 KB a second), and the lock is free for the screen task in between. One tick
		// more than that: a wait of FreeRTOS ends with a tick, and the first one may be a moment away.
		vTaskDelay(pdMS_TO_TICKS(CERBO_TURN_MS) + 1);
	}
	else if(received == 0)
	{
		gx_lost("lost: closed by the broker", 0);
	}
	else if(errno != EWOULDBLOCK && errno != EINTR)
	{
		gx_lost("lost", errno);
	}
	// else: readable, and then nothing to read (EWOULDBLOCK is what lwIP says then, and EAGAIN is the same
	// number there and on a PC). No error: the next round waits again.
}

// One round: the order of the core, taken under the lock with what it names and carried out without it,
// then the wait
static void gx_round(void)
{
	uint64_t now = platform_now_ms();
	size_t length = 0;
	cerbo_do_t order;

	platform_lock();
	order = cerbo_next(&platform_app->cerbo, now);
	// What the order names lies in the app and changes with it: copied before the lock is given back
	if(order == CERBO_DO_OPEN)
	{
		strlcpy(room->host, cerbo_host(&platform_app->cerbo), sizeof(room->host));
	}
	else if(order == CERBO_DO_SEND)
	{
		const uint8_t *bytes = cerbo_out(&platform_app->cerbo, &length);

		// A packet without room here is not cut short: none is sent (gx_send())
		if(length > sizeof(room->out))
		{
			length = 0;
		}
		memcpy(room->out, bytes, length);
	}
	platform_unlock();

	switch(order)
	{
		case CERBO_DO_OPEN:     gx_open(); break;
		case CERBO_DO_SEND:     gx_send(length); break;
		// The core has given the connection up already: nothing is reported
		case CERBO_DO_CLOSE:    gx_close(); break;
		default:                break;
	}
	gx_wait();
}

static void gx_task(void *arg)
{
	(void)arg;
	for(;;)
	{
		gx_round();
	}
}

esp_err_t gx_start(void)
{
	if(running)
	{
		return ESP_ERR_INVALID_STATE;
	}
	// Kept if the task cannot be created: the next call asks for no second room
	if(room == NULL)
	{
		room = heap_caps_malloc(sizeof(*room), MALLOC_CAP_SPIRAM);
	}
	if(room == NULL)
	{
		return ESP_ERR_NO_MEM;
	}
	// On the core of the WiFi driver; the picture has the other one (main.c)
	if(xTaskCreatePinnedToCore(gx_task, "gx", GX_STACK_BYTES, NULL, GX_PRIORITY, NULL, GX_CORE) != pdPASS)
	{
		return ESP_ERR_NO_MEM;
	}
	running = true;
	return ESP_OK;
}
