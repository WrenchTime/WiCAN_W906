"""Mutations of display/main/gx.c, see ../redproof.py. The simulation is ../gx_sim.c.

Each one leaves a source gcc builds without a warning: a variable that would be set and never used is kept in
use with (void), a function nobody would call any more is still named - also a function of the core, which
the simulation replaces by a wrapper of its own that must not be left without a caller."""

F = "main/gx.c"
S = "gx_sim"

# gx_close(), gx_lost()
CLOSE = "\tif(fd >= 0)\n\t{\n\t\tclose(fd);\n\t}\n\tfd = -1;"
CLOSE_FORGETS = "\t\tclose(fd);\n\t}\n\tfd = -1;\n"
LOST_CLOSES = "\tuint64_t now;\n\n\tgx_close();\n\tESP_LOGI(TAG, \"%s (errno %d)\", what, error);"
LOST_TIME = "\tnow = platform_now_ms();\n\tplatform_lock();\n\tcerbo_down("
LOST_REPORTS = "\tcerbo_down(&platform_app->cerbo, now);"

# gx_open()
OPEN_CLOSES = "\t// and lwIP has ten for the whole display (web.c).\n\tgx_close();\n"
PORT = "htons(CERBO_PORT)"
ADDRESS = "inet_pton(AF_INET, room->host, &address.sin_addr) != 1"
NO_SOCKET = "\tif(fd < 0)\n\t{\n\t\tgx_lost(\"not connected: no socket\", errno);\n\t\treturn;\n\t}\n"
BEYOND = "\tif(fd >= FD_SETSIZE)\n\t{\n\t\tgx_lost(\"not connected: a socket select() cannot wait for\", 0);\n\t\treturn;\n\t}\n"
FLAGS_READ = "\tif(flags < 0 || fcntl("
FLAG_SET = "flags | O_NONBLOCK) != 0)"
CONNECT = "\tif(connect(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 && errno != EINPROGRESS)\n" \
          "\t{\n\t\tgx_lost(\"not connected\", errno);\n\t\treturn;\n\t}\n"
IN_PROGRESS = "errno != EINPROGRESS)"

# gx_send()
NO_PACKET = "\tif(length == 0)\n\t{\n\t\tgx_lost(\"not sent: no packet\", 0);\n\t\treturn;\n\t}\n"
SENT_ALL = "\tif(sent < 0 || (size_t)sent != length)"

# gx_wait()
SLEEPS = "\tif(fd < 0)\n\t{\n\t\tvTaskDelay(pdMS_TO_TICKS(GX_WAIT_MS));\n\t\treturn;\n\t}"
SELECT = "ready = connecting ? select(fd + 1, NULL, &set, NULL, &limit) : select(fd + 1, &set, NULL, NULL, &limit);"
WAIT_MS = "#define GX_WAIT_MS          100u "
NOTHING = "\tif(ready == 0 || (ready < 0 && errno == EINTR))"
SELECT_FAILED = "\t\tgx_lost(\"lost: select()\", errno);\n\t\treturn;"
SO_ERROR = "\t\tif(error != 0)\n\t\t{\n\t\t\tgx_lost(\"not connected\", error);"
SO_ERROR_UNREAD = "\t\t\terror = errno != 0 ? errno : -1;"
CONNECTED = "\t\tconnecting = false;\n"
OPENED = "\t\tcerbo_opened(&platform_app->cerbo, now);"
BYTES = "\t\tcerbo_bytes(&platform_app->cerbo, room->in, (size_t)received, now, room->work, CERBO_TOKENS);"
BYTES_TIME = "\t\tnow = platform_now_ms();\n\t\tplatform_lock();\n\t\tcerbo_bytes("
BYTES_LOCK = "\t\tplatform_lock();\n\t\tcerbo_bytes("
TURN = "\t\tvTaskDelay(pdMS_TO_TICKS(CERBO_TURN_MS) + 1);"
CLOSED_BY_BROKER = "\telse if(received == 0)\n\t{\n\t\tgx_lost(\"lost: closed by the broker\", 0);\n\t}\n" \
                   "\telse if(errno != EWOULDBLOCK && errno != EINTR)"
READ_FAILED = "\telse if(errno != EWOULDBLOCK && errno != EINTR)\n\t{\n\t\tgx_lost(\"lost\", errno);\n\t}"
WOULD_BLOCK = "errno != EWOULDBLOCK && errno != EINTR"

# gx_round(), gx_task()
ROUND_TIME = "\tuint64_t now = platform_now_ms();\n\tsize_t length = 0;"
ROUND_LOCK = "\tplatform_lock();\n\torder = cerbo_next(&platform_app->cerbo, now);"
ROUND_UNLOCK = "\tplatform_unlock();\n\n\tswitch(order)"
HOST_COPIED = "\t\tstrlcpy(room->host, cerbo_host(&platform_app->cerbo), sizeof(room->host));"
OUT_COPIED = "\t\tmemcpy(room->out, bytes, length);"
OUT_FITS = "\t\tif(length > sizeof(room->out))\n\t\t{\n\t\t\tlength = 0;\n\t\t}"
DO_OPEN = "\t\tcase CERBO_DO_OPEN:     gx_open(); break;"
DO_SEND = "\t\tcase CERBO_DO_SEND:     gx_send(length); break;"
DO_CLOSE = "\t\tcase CERBO_DO_CLOSE:    gx_close(); break;"
FOR_EVER = "\tfor(;;)\n\t{\n\t\tgx_round();\n\t}"

# gx_start()
ONCE = "\tif(running)\n"
ROOM_ONCE = "\tif(room == NULL)\n\t{\n\t\troom = heap_caps_malloc(sizeof(*room), MALLOC_CAP_SPIRAM);\n\t}"
NO_ROOM = "\tif(room == NULL)\n\t{\n\t\treturn ESP_ERR_NO_MEM;\n\t}\n"
NO_TASK = "!= pdPASS)\n\t{\n\t\treturn ESP_ERR_NO_MEM;\n\t}\n\trunning = true;"

MUTATIONS = [
    # the socket is closed once, and the core is told
    ("gx_closes_nothing", S, F, CLOSE, "\t(void)close;\n\tfd = -1;"),
    ("gx_closed_socket_kept", S, F, CLOSE_FORGETS, "\t\tclose(fd);\n\t}\n"),
    ("gx_lost_connection_not_closed", S, F, LOST_CLOSES, "\tuint64_t now;\n\n\tESP_LOGI(TAG, \"%s (errno %d)\", what, error);"),
    ("gx_lost_connection_reported_without_its_time", S, F, LOST_TIME, "\tnow = 0;\n\tplatform_lock();\n\tcerbo_down("),
    ("gx_lost_connection_not_reported", S, F, LOST_REPORTS, "\t(void)now;\n\t(void)cerbo_down;"),

    # an order to open
    ("gx_open_keeps_the_socket_it_finds", S, F, OPEN_CLOSES, "\t// and lwIP has ten for the whole display (web.c).\n"),
    ("gx_port_in_the_byte_order_of_the_pc", S, F, PORT, "CERBO_PORT"),
    ("gx_takes_a_text_that_is_no_address", S, F, ADDRESS, "inet_pton(AF_INET, room->host, &address.sin_addr) < 0"),
    ("gx_no_socket_goes_unnoticed", S, F, NO_SOCKET, ""),
    ("gx_takes_a_descriptor_beyond_the_sets", S, F, BEYOND, ""),
    ("gx_flags_that_cannot_be_read_are_set", S, F, FLAGS_READ, "\tif(fcntl("),
    ("gx_socket_blocks", S, F, FLAG_SET, "flags) != 0)"),
    ("gx_flag_that_is_not_taken_goes_unnoticed", S, F, FLAG_SET, "flags | O_NONBLOCK) > 0)"),
    ("gx_failed_connect_goes_unnoticed", S, F, CONNECT, "\t(void)connect(fd, (const struct sockaddr *)&address, sizeof(address));\n"),
    ("gx_attempt_under_way_taken_for_a_failure", S, F, IN_PROGRESS, "errno == EINPROGRESS)"),

    # an order to send
    ("gx_sends_a_packet_without_bytes", S, F, NO_PACKET, ""),
    ("gx_short_send_goes_unnoticed", S, F, SENT_ALL, "\tif(sent < 0)"),
    ("gx_failed_send_goes_unnoticed", S, F, SENT_ALL, "\tif(sent >= 0 && (size_t)sent != length)"),

    # the wait, and what it reports
    ("gx_no_sleep_without_a_socket", S, F, SLEEPS, "\tif(fd < 0)\n\t{\n\t\treturn;\n\t}"),
    ("gx_waits_for_readable_while_connecting", S, F, SELECT, "ready = select(fd + 1, &set, NULL, NULL, &limit);"),
    ("gx_waits_for_writable_in_a_connection", S, F, SELECT, "ready = select(fd + 1, NULL, &set, NULL, &limit);"),
    ("gx_waits_a_second", S, F, WAIT_MS, "#define GX_WAIT_MS          1000u"),
    ("gx_end_of_the_wait_taken_for_an_event", S, F, NOTHING, "\tif(ready < 0 && errno == EINTR)"),
    ("gx_signal_taken_for_a_lost_connection", S, F, NOTHING, "\tif(ready == 0)"),
    ("gx_failed_wait_goes_unnoticed", S, F, SELECT_FAILED, "\t\treturn;"),
    ("gx_refusal_taken_for_a_connection", S, F, SO_ERROR, "\t\tif(error < 0)\n\t\t{\n\t\t\tgx_lost(\"not connected\", error);"),
    ("gx_unread_end_of_an_attempt_taken_for_a_connection", S, F, SO_ERROR_UNREAD, "\t\t\terror = 0;"),
    ("gx_goes_on_asking_how_the_attempt_ended", S, F, CONNECTED, ""),
    ("gx_connection_not_reported", S, F, OPENED, "\t\t(void)now;\n\t\t(void)cerbo_opened;"),
    ("gx_bytes_not_reported", S, F, BYTES, "\t\t(void)now;\n\t\t(void)cerbo_bytes;"),
    ("gx_bytes_reported_without_their_time", S, F, BYTES_TIME, "\t\tnow = 0;\n\t\tplatform_lock();\n\t\tcerbo_bytes("),
    ("gx_bytes_reported_without_the_lock", S, F, BYTES_LOCK, "\t\tcerbo_bytes("),
    ("gx_reads_with_the_tokens_of_the_app", S, F, BYTES,
     "\t\tcerbo_bytes(&platform_app->cerbo, room->in, (size_t)received, now, platform_app->work, CERBO_TOKENS);"),
    ("gx_no_pause_behind_a_read", S, F, TURN, ""),
    ("gx_pause_behind_a_read_one_tick_short", S, F, TURN, "\t\tvTaskDelay(pdMS_TO_TICKS(CERBO_TURN_MS));"),
    ("gx_broker_that_closed_goes_unnoticed", S, F, CLOSED_BY_BROKER, "\telse if(received < 0 && errno != EWOULDBLOCK && errno != EINTR)"),
    ("gx_failed_read_goes_unnoticed", S, F, READ_FAILED, ""),
    ("gx_nothing_to_read_taken_for_a_lost_connection", S, F, WOULD_BLOCK, "errno != EINTR"),

    # the round and the task
    ("gx_round_without_its_time", S, F, ROUND_TIME, "\tuint64_t now = 0;\n\tsize_t length = 0;"),
    ("gx_core_asked_without_the_lock", S, F, ROUND_LOCK, "\torder = cerbo_next(&platform_app->cerbo, now);"),
    ("gx_order_carried_out_under_the_lock", S, F, ROUND_UNLOCK, "\n\tswitch(order)"),
    ("gx_address_of_the_order_not_copied", S, F, HOST_COPIED, "\t\t(void)cerbo_host(&platform_app->cerbo);"),
    ("gx_packet_of_the_order_not_copied", S, F, OUT_COPIED, "\t\t(void)bytes;"),
    ("gx_packet_too_long_is_cut_and_sent", S, F, OUT_FITS, "\t\tif(length > sizeof(room->out))\n\t\t{\n\t\t\tlength = sizeof(room->out);\n\t\t}"),
    ("gx_order_to_open_not_carried_out", S, F, DO_OPEN, "\t\tcase CERBO_DO_OPEN:     (void)gx_open; break;"),
    ("gx_order_to_send_not_carried_out", S, F, DO_SEND, "\t\tcase CERBO_DO_SEND:     (void)gx_send; (void)length; break;"),
    ("gx_order_to_close_not_carried_out", S, F, DO_CLOSE, "\t\tcase CERBO_DO_CLOSE:    break;"),
    ("gx_order_to_close_reported", S, F, DO_CLOSE, "\t\tcase CERBO_DO_CLOSE:    gx_lost(\"closed\", 0); break;"),
    ("gx_task_ends_after_a_round", S, F, FOR_EVER, "\tgx_round();"),

    # the start
    ("gx_second_task", S, F, ONCE, "\tif(running && room == NULL)\n"),
    ("gx_second_room", S, F, ROOM_ONCE, "\troom = heap_caps_malloc(sizeof(*room), MALLOC_CAP_SPIRAM);"),
    ("gx_task_without_its_room", S, F, NO_ROOM, ""),
    ("gx_no_task_taken_for_one", S, F, NO_TASK, "!= pdPASS)\n\t{\n\t}\n\trunning = true;"),
    ("gx_room_in_the_internal_ram", S, F, "MALLOC_CAP_SPIRAM", "MALLOC_CAP_INTERNAL"),
    ("gx_on_the_core_of_the_panel", S, F, "#define GX_CORE             0 ", "#define GX_CORE             1 "),
    ("gx_at_the_priority_of_the_network_task", S, F, "#define GX_PRIORITY         4 ", "#define GX_PRIORITY         5 "),
]
