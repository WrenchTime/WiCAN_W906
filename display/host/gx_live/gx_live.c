/*
 * Runs display/main/gx.c on a PC against a real MQTT broker: the file is built as it is, with the core
 * (cerbo.c, mqtt_sub.c) and the sockets of the PC, and this file is the rest of the platform around it - a
 * lock, a clock, a second thread for its task. So the whole way of a bundle runs before it comes to the
 * board: TCP as the PC has it, the broker as it is, the packets as it sends them.
 *
 * The program can connect, subscribe to CERBO_TOPIC and ping, and nothing else: it is the display's own code,
 * and that has no function that builds another packet (mqtt_sub.h). It may listen to the broker of a Cerbo.
 *
 *   build/gx_live <address> [seconds]      address: of the broker, IPv4 in dotted form; port CERBO_PORT.
 *                                          seconds: how long to listen, 60 if not given
 *
 * What it writes: a line whenever the view of the source changes (cerbo_view(), the words of GET /api/info),
 * a line every ten seconds with the counters, and at the end what was seen - sessions, bundles taken and
 * refused by reason, the gaps between two bundles as the core saw them arrive, the largest lag, and the keys
 * of the last bundle. The log of gx.c is written as it comes.
 *
 * It names itself to the broker as "WiCANDisplayLive", not as a display does: a broker ends the connection
 * of another client with the same name (mqtt_sub.h), and a display must not lose its session to this.
 *
 * What this cannot see: lwIP, the radio link, the tasks of the device and their priorities, the memory.
 */
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "platform.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define NAME        "WiCAN-Display-Live"
#define LOOK_MS     20u         // the source is looked at this often
#define LINE_MS     10000u      // a line with the counters this often

app_t *platform_app;
platform_info_t platform_info;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec began;

void platform_lock(void)
{
	pthread_mutex_lock(&lock);
}

void platform_unlock(void)
{
	pthread_mutex_unlock(&lock);
}

// Milliseconds since the start of the program, from a clock that never runs backwards
uint64_t platform_now_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)((int64_t)(now.tv_sec - began.tv_sec) * 1000 + (now.tv_nsec - began.tv_nsec) / 1000000);
}

static void sleep_ms(uint32_t ms)
{
	struct timespec time = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };

	nanosleep(&time, NULL);
}

// What gx.c logs, with the time of the program in front
void sim_log(char level, const char *tag, const char *format, ...)
{
	char line[256];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	printf("%8.1f s  %c %s: %s\n", (double)platform_now_ms() / 1000, level, tag, line);
	fflush(stdout);
}

// The PC has one kind of memory
void *heap_caps_malloc(size_t size, uint32_t caps)
{
	(void)caps;
	return malloc(size);
}

void vTaskDelay(const TickType_t ticks)
{
	sleep_ms((uint32_t)ticks * (1000 / configTICK_RATE_HZ));
}

static TaskFunction_t task;

static void *thread_of_the_task(void *arg)
{
	task(arg);
	return NULL;
}

// The task of gx.c is a second thread; stack, priority and core are the board's business
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t code, const char *const name, const uint32_t stack, void *const arg,
                                   UBaseType_t priority, TaskHandle_t *const created, const BaseType_t core)
{
	pthread_t thread;

	(void)name;
	(void)stack;
	(void)priority;
	(void)created;
	(void)core;
	task = code;
	return pthread_create(&thread, NULL, thread_of_the_task, arg) == 0 ? pdPASS : errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
}

static const char *view_word(cerbo_view_t view)
{
	switch(view)
	{
		case CERBO_VIEW_OFF: return "off";
		case CERBO_VIEW_NO_WIFI: return "no_wifi";
		case CERBO_VIEW_CONNECTING: return "connecting";
		case CERBO_VIEW_UNREACHABLE: return "unreachable";
		case CERBO_VIEW_QUIET: return "quiet";
		case CERBO_VIEW_SILENT: return "silent";
		case CERBO_VIEW_NEWER: return "newer";
		case CERBO_VIEW_LIVE: return "live";
		default: return "?";
	}
}

int main(int argc, char **argv)
{
	// Static: the store of values alone has several kilobytes
	static cerbo_t seen;
	const char *host = argc > 1 ? argv[1] : "";
	long seconds = argc > 2 ? strtol(argv[2], NULL, 10) : 60;
	unsigned a, b, c, d;
	char rest;
	cerbo_view_t view = CERBO_VIEW_OFF;
	uint64_t line_ms = 0;
	uint64_t now;
	// Of the bundles, as the core saw them arrive
	uint32_t bundles = 0;
	uint64_t bundle_ms = 0;
	uint64_t gap_least = UINT64_MAX, gap_most = 0, gap_sum = 0;
	uint32_t gaps = 0, gaps_over_2s = 0;
	uint32_t lag_most = 0;
	int values_least = VALUES_MAX + 1, values_most = 0;

	// Four numbers and nothing behind them: a name would be looked up by nobody (gx.c)
	if(argc < 2 || argc > 3 || seconds < 1 || strlen(host) >= CERBO_HOST_SIZE ||
	   sscanf(host, "%3u.%3u.%3u.%3u%c", &a, &b, &c, &d, &rest) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
	{
		fprintf(stderr, "usage: gx_live <IPv4 address of the broker> [seconds, 60 if not given]\n");
		return 2;
	}

	clock_gettime(CLOCK_MONOTONIC, &began);
	platform_app = calloc(1, sizeof(*platform_app));
	if(platform_app == NULL)
	{
		fprintf(stderr, "no memory\n");
		return 2;
	}
	// What app_init() and app_net() do for the source on the display: its address, its name, a network
	now = platform_now_ms();
	cerbo_init(&platform_app->cerbo, host, false, NAME, now);
	cerbo_network(&platform_app->cerbo, true, now);
	printf("listening to %s:%d, topic %s, for %ld s\n", host, CERBO_PORT, CERBO_TOPIC, seconds);
	if(gx_start() != ESP_OK)
	{
		fprintf(stderr, "the task cannot be started\n");
		return 2;
	}

	for(;;)
	{
		cerbo_view_t view_now;

		sleep_ms(LOOK_MS);
		now = platform_now_ms();
		// As every other task of the display reads the source: under the lock, and a copy to look at
		platform_lock();
		memcpy(&seen, &platform_app->cerbo, sizeof(seen));
		view_now = cerbo_view(&platform_app->cerbo, now);
		platform_unlock();

		if(seen.bundles != bundles)
		{
			// Several may have come since the last look: the gap is the one in front of the last of them
			if(bundles > 0 && seen.bundles == bundles + 1)
			{
				uint64_t gap = seen.bundle_ms - bundle_ms;

				gaps++;
				gap_sum += gap;
				if(gap < gap_least) gap_least = gap;
				if(gap > gap_most) gap_most = gap;
				if(gap > 2000) gaps_over_2s++;
			}
			bundles = seen.bundles;
			bundle_ms = seen.bundle_ms;
			if(seen.lag_ms > lag_most) lag_most = seen.lag_ms;
			if(seen.values.count < values_least) values_least = seen.values.count;
			if(seen.values.count > values_most) values_most = seen.values.count;
		}
		if(view_now != view)
		{
			view = view_now;
			printf("%8.1f s  view %s\n", (double)now / 1000, view_word(view));
			fflush(stdout);
		}
		if(now >= line_ms + LINE_MS)
		{
			line_ms = now;
			printf("%8.1f s  view %s, %" PRIu32 " bundles, %" PRIu32 " refused, %" PRIu32 " sessions, %d values, lag %" PRIu32 " ms\n",
			       (double)now / 1000, view_word(view), seen.bundles, cerbo_refused(&seen), seen.sessions, seen.values.count,
			       seen.lag_ms);
			fflush(stdout);
		}
		if(now >= (uint64_t)seconds * 1000)
		{
			break;
		}
	}

	printf("\nafter %.1f s: view %s\n", (double)now / 1000, view_word(view));
	printf("sessions %" PRIu32 ", bundles taken %" PRIu32 ", runs of the flow seen (boots) %" PRIu32 "\n", seen.sessions, seen.bundles,
	       seen.boots);
	printf("refused %" PRIu32 ": invalid %" PRIu32 ", newer version %" PRIu32 ", retained %" PRIu32 ", other topic %" PRIu32
	       ", too long %" PRIu32 "; members passed over %" PRIu32 "; clocks not believed %" PRIu32 "\n",
	       cerbo_refused(&seen), seen.refused_invalid, seen.refused_version, seen.refused_retained, seen.refused_topic,
	       seen.refused_long, seen.skipped, seen.distrusted);
	if(gaps > 0)
	{
		printf("gaps between two bundles: %" PRIu32 " measured, %" PRIu64 " to %" PRIu64 " ms, %" PRIu64 " ms on average, %" PRIu32
		       " above 2 s\n", gaps, gap_least, gap_most, gap_sum / gaps, gaps_over_2s);
	}
	printf("lag: at most %" PRIu32 " ms, at the end %" PRIu32 " ms\n", lag_most, seen.lag_ms);
	if(values_most > 0)
	{
		printf("values in a bundle: %d to %d; the keys of the last one:", values_least, values_most);
		for(int i = 0; i < seen.values.count; i++)
		{
			printf(" %s", seen.values.items[i].name);
		}
		printf("\n");
	}
	// The process ends with the task in the middle of a round: the socket goes with it, and the broker sees a
	// connection that closed - a DISCONNECT is one of the packets this program cannot build
	fflush(stdout);
	return 0;
}
