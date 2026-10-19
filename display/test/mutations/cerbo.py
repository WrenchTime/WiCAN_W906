"""Mutations of display/components/core/cerbo.c, see ../redproof.py.

A mutation here must leave the source inside its memory and its loops finite: a test that only crashes or
hangs names no rule. Where a rule keeps a write inside the store, the mutation moves the limit to the safe
side; where a rule ends the reading of broken bytes, the mutation keeps the return.
"""

F = "components/core/cerbo.c"
H = "components/core/cerbo.h"
T = "test_cerbo"

ADVANCE = "\tif(now_ms > cerbo->clock_ms) cerbo->clock_ms = now_ms;"
AT = "\treturn now_ms > cerbo->clock_ms ? now_ms : cerbo->clock_ms;"
HOST_NULL = "\tif(text == NULL) return;\n"
HOST_LONG = "\tif(length == CERBO_HOST_SIZE) return;\n"
ONCE_STEP = "\tcerbo->wait_step = 0;\n\tcerbo->wait_ms = 0;\n"
ONCE_WAIT = "\tcerbo->wait_ms = 0;\n\tcerbo->grace_since_ms = cerbo->clock_ms;\n}"
ENDED_GRACE = "\tif(cerbo->phase == CERBO_SESSION) cerbo->grace_since_ms = cerbo->clock_ms;\n\tcerbo->phase = CERBO_IDLE;"
ENDED_FLAGS = "\tcerbo->send = false;\n\tcerbo->failed = false;\n"
ENDED_MOVED = "\t\tcerbo->moved = false;\n\t\treturn;\n\t}\n"
ENDED_WAIT = "\tcerbo->wait_since_ms = cerbo->clock_ms;\n\tcerbo->wait_ms = RETRY_WAITS[cerbo->wait_step];\n"
ENDED_STEP = "\tif(cerbo->wait_step < RETRY_WAIT_COUNT - 1) cerbo->wait_step++;"
INIT_NAME = "\t\twhile(length < CERBO_NAME_SIZE - 1 && name[length] != '\\0') length++;"
INIT_CLIENT = "\tif(mqtt_sub_connect(probe, sizeof(probe), cerbo->name, CERBO_KEEPALIVE_S) == 0) strcpy(cerbo->name, CERBO_CLIENT);\n"
ON = "\treturn cerbo->host[0] != '\\0' && !cerbo->safe_mode;"
SAME_HOST = "\tif(strcmp(taken, cerbo->host) == 0) return;\n"
SET_MOVED = "\tif(cerbo->phase != CERBO_IDLE) cerbo->moved = true;\n\tat_once(cerbo);\n"
SAME_NETWORK = "\tif(up == cerbo->network) return;\n"
JOINED = "\tcerbo->network = up;\n\tif(up) at_once(cerbo);"
IDLE_OFF = "\t\tif(!cerbo_on(cerbo) || !cerbo->network) return CERBO_DO_NOTHING;"
IDLE_WAIT = "\t\tif(cerbo->clock_ms - cerbo->wait_since_ms < cerbo->wait_ms) return CERBO_DO_NOTHING;\n"
OPEN = "\t\tcerbo->phase = CERBO_OPENING;\n\t\tcerbo->open_ms = cerbo->clock_ms;\n\t\treturn CERBO_DO_OPEN;"
CLOSE_IF = "\tif(!cerbo->network || cerbo->moved || cerbo->failed)"
LATE = "\t\tif(cerbo->clock_ms - cerbo->open_ms >= CERBO_SESSION_MS)"
SEND = "\t\tif(!cerbo->send) return CERBO_DO_NOTHING;\n\n\t\tcerbo->send = false;\n\t\treturn CERBO_DO_SEND;"
PONG_LATE = "\tif(cerbo->pings_open > 0 && cerbo->clock_ms - cerbo->ping_open_ms >= CERBO_PONG_MS)"
PING_DUE = "\tif(cerbo->clock_ms - cerbo->ping_ms < CERBO_PING_MS) return CERBO_DO_NOTHING;\n"
PING_BUILT = "\tcerbo->out_length = mqtt_sub_ping(cerbo->out, sizeof(cerbo->out));\n"
PING_OLDEST = "\tif(cerbo->pings_open == 0) cerbo->ping_open_ms = cerbo->clock_ms;\n"
PING_NOTED = "\tcerbo->pings_open++;\n\tcerbo->ping_ms = cerbo->clock_ms;\n\treturn CERBO_DO_SEND;"
OPENED_IF = "\tif(cerbo->phase != CERBO_OPENING) return;\n"
OPENED = "\tcerbo->phase = CERBO_GREETING;\n\tmqtt_sub_begin(&cerbo->reader);\n"
OPENED_CLOCK = "\tcerbo->has_clock = false;\n\tcerbo->out_length = mqtt_sub_connect("
CONNECT = "\tcerbo->out_length = mqtt_sub_connect(cerbo->out, sizeof(cerbo->out), cerbo->name, CERBO_KEEPALIVE_S);\n\tcerbo->send = true;"
DOWN = "\tif(cerbo->phase != CERBO_IDLE) ended(cerbo);"
CONNACK_IF = "\tif(cerbo->phase != CERBO_GREETING || cerbo->send || packet->code != 0)"
CONNACK_FAILS = CONNACK_IF + "\n\t{\n\t\tcerbo->failed = true;\n"
SUBSCRIBE = "\tcerbo->out_length = mqtt_sub_subscribe(cerbo->out, sizeof(cerbo->out), CERBO_PACKET_ID, CERBO_TOPIC);\n\tcerbo->send = true;"
SUBACK_IF = "\tif(cerbo->phase != CERBO_SUBSCRIBING || cerbo->send || packet->id != CERBO_PACKET_ID || packet->code != 0)"
SUBACK_FAILS = SUBACK_IF + "\n\t{\n\t\tcerbo->failed = true;\n"
PINGRESP_IF = "\tif(cerbo->phase != CERBO_SESSION || cerbo->pings_open == 0)"
PINGRESP_FAILS = PINGRESP_IF + "\n\t{\n\t\tcerbo->failed = true;\n"
BOOT_NEW = "\tif(!cerbo->has_boot || cerbo->boot != boot)"
BOOT_COUNTED = "\t\tcerbo->boots++;\n\t\tcerbo->has_clock = false;\n\t}\n\tif(cerbo->has_clock)"
PART = "\t\tuint64_t part = (elapsed % 1000) * CERBO_YIELD_PER_MILLE + cerbo->yield_rest;"
YIELDED = "\t\tuint64_t yielded = (elapsed / 1000) * CERBO_YIELD_PER_MILLE + part / 1000;"
BEHIND = "\t\tuint64_t behind = cerbo->lag_ms + (elapsed - yielded);"
STEP_BACK = "\t\tif(step < 0) lag = behind + (uint64_t)(-step);"
STEP_ON = "\t\telse if(behind > (uint64_t)step) lag = behind - (uint64_t)step;"
DISTRUST = "\tif(lag > CERBO_LAG_MAX_MS)"
DISTRUSTED = "\t\tcerbo->distrusted++;\n\t\tcerbo->has_clock = false;\n\t\tlag = 0;\n"
NEW_BASE = "\tif(!cerbo->has_clock) cerbo->yield_rest = 0;\n"
KEY_HEAD = "\tif(!cerbo_key(key) || *c == '\\0') return false;"
KEY_BODY = "\t\tif(!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) return false;"
FOUND = "\t\tif(strcmp(item->name, name) == 0) return item;"
EMPTIED = "\tvalues_clear(values);\n\tfor(int i = 0; i < work[object].size; i++)"
KEY_READ = "\t\tif(!json_text(json, name_token, name, sizeof(name)) || !key_fits(name))\n\t\t{\n\t\t\tcerbo->skipped++;\n"
KINDS = ("\t\tif(json_number(json, member, &number)) kind = VALUE_NUMBER;\n"
         "\t\telse if(json_text_is(json, member, \"on\")) kind = VALUE_ON;\n"
         "\t\telse if(json_text_is(json, member, \"off\")) kind = VALUE_OFF;\n"
         "\t\telse\n\t\t{\n\t\t\tcerbo->skipped++;\n")
NO_ROOM = "\t\t\tif(values->count >= VALUES_MAX)\n\t\t\t{\n\t\t\t\tcerbo->skipped++;\n"
KEPT = "\t\titem->kind = kind;\n\t\titem->number = number;\n\t\titem->seen_ms = seen_ms;\n"
WHOLE = "\treturn member >= 0 && json_integer(json, &work[member], number) && *number >= least && *number <= most;"
TOPIC = "\tif(packet->topic_length != TOPIC_LENGTH || memcmp(packet->topic, CERBO_TOPIC, TOPIC_LENGTH) != 0)"
TOPIC_COUNTED = TOPIC + "\n\t{\n\t\tcerbo->refused_topic++;\n\t\treturn;\n\t}\n"
RETAINED = "\tif(packet->retained)\n\t{\n\t\tcerbo->refused_retained++;\n\t\treturn;\n\t}\n"
LONG = "\tif(packet->payload_length > CERBO_BUNDLE_MAX)\n\t{\n\t\tcerbo->refused_long++;\n\t\treturn;\n\t}\n"
READ = "\tif(json_parse(json, packet->payload_length, work, work_count) < 0 || !whole(json, work, \"v\", 1, INT64_MAX, &version))\n\t{\n\t\tcerbo->refused_invalid++;\n"
NEWER = "\tif(version > CERBO_VERSION)\n\t{\n\t\tcerbo->refused_version++;\n\t\tcerbo->newer = true;\n\t\treturn;\n\t}\n"
REST = ("\tif(!whole(json, work, \"boot\", 1, CERBO_BOOT_MAX, &boot) || !whole(json, work, \"t\", 0, CERBO_T_MAX, &t_ms) || values < 0 || "
        "work[values].type != JSON_OBJECT)\n\t{\n\t\tcerbo->refused_invalid++;\n")
SEEN = "\tfill(cerbo, json, work, values, cerbo->clock_ms > cerbo->lag_ms ? cerbo->clock_ms - cerbo->lag_ms : 0);"
TAKEN = ("\tcerbo->newer = false;\n\tcerbo->has_bundle = true;\n\tcerbo->bundle_ms = cerbo->clock_ms;\n\tcerbo->quiet_since_ms = cerbo->clock_ms;\n"
         "\tcerbo->bundles++;\n")
READS = "\tif(cerbo->phase == CERBO_IDLE || cerbo->phase == CERBO_OPENING || cerbo->moved || cerbo->failed) return;"
BROKEN = "\t\tif(kind == MQTT_SUB_BROKEN)\n\t\t{\n\t\t\tcerbo->failed = true;\n\t\t\treturn;\n\t\t}\n"
EARLY = "\t\telse if(cerbo->phase == CERBO_GREETING || cerbo->send) cerbo->failed = true;"
GONE_BY = "\t\telse if(kind == MQTT_SUB_SKIPPED) cerbo->refused_long++;\n"
AFTER = "\t\tif(cerbo->failed) return;\n\t}\n}"
VIEW_OFF = "\tif(!cerbo_on(cerbo)) return CERBO_VIEW_OFF;\n"
VIEW_WIFI = "\tif(!cerbo->network) return CERBO_VIEW_NO_WIFI;\n"
VIEW_GRACE = "\t\treturn now - cerbo->grace_since_ms < CERBO_GRACE_MS ? CERBO_VIEW_CONNECTING : CERBO_VIEW_UNREACHABLE;"
VIEW_NEWER = "\tif(cerbo->newer) return CERBO_VIEW_NEWER;\n"
VIEW_LIVE = "\tif(cerbo->has_bundle && now - cerbo->bundle_ms < CERBO_LIVE_MS) return CERBO_VIEW_LIVE;\n"
VIEW_SILENT = "\tif(cerbo->pong_ms >= cerbo->quiet_since_ms && cerbo->pong_ms - cerbo->quiet_since_ms >= CERBO_SILENT_MS) return CERBO_VIEW_SILENT;"
SHOW_LIVE = "\tif(cerbo_view(cerbo, now_ms) == CERBO_VIEW_LIVE) return true;\n"
SHOW_VALUE = "\t\tif(cerbo_age(item, now_ms) != VALUE_AGE_GONE) return true;"
AGE_NULL = "\tif(value == NULL) return VALUE_AGE_GONE;"
AGE = "\tage = now_ms > value->seen_ms ? now_ms - value->seen_ms : 0;"
AGE_FRESH = "\tif(age < CERBO_FRESH_MS) return VALUE_AGE_FRESH;"
AGE_OLD = "\tif(age < CERBO_KEPT_MS) return VALUE_AGE_OLD;"
KEY = "\treturn key != NULL && strncmp(key, CERBO_PREFIX, PREFIX_LENGTH) == 0;"


def define(name, value, other, spaces):
    """A mutation of one #define of cerbo.h; `spaces` separate the name from its value there."""
    return H, "#define %s%s%s" % (name, " " * spaces, value), "#define %s%s%s" % (name, " " * spaces, other)


MUTATIONS = [
    # the constants of the contract and of the timing
    ("cerbo_prefix_changed", T) + define("CERBO_PREFIX", '"GX_"', '"GY_"', 12),
    ("cerbo_topic_changed", T) + define("CERBO_TOPIC", '"display/gx/values"', '"display/gx/value"', 13),
    ("cerbo_version_2_is_read", T) + define("CERBO_VERSION", "1", "2", 11),
    ("cerbo_host_of_16_bytes", T) + define("CERBO_HOST_SIZE", "16", "17", 9),
    ("cerbo_name_of_33_bytes", T) + define("CERBO_NAME_SIZE", "33", "34", 9),
    ("cerbo_client_changed", T) + define("CERBO_CLIENT", '"WiCANDisplay"', '"Display"', 12),
    ("cerbo_packet_id_changed", T) + define("CERBO_PACKET_ID", "1", "2", 9),
    ("cerbo_bundle_of_1401_bytes", T) + define("CERBO_BUNDLE_MAX", "1400", "1401", 8),
    ("cerbo_boot_of_2_31", T) + define("CERBO_BOOT_MAX", "2147483647", "2147483648", 10),
    ("cerbo_t_of_2_53", T) + define("CERBO_T_MAX", "9007199254740991", "9007199254740992", 13),
    ("cerbo_keepalive_changed", T) + define("CERBO_KEEPALIVE_S", "30", "60", 7),
    ("cerbo_ping_every_11_s", T) + define("CERBO_PING_MS", "10000u", "11000u", 11),
    ("cerbo_pong_within_21_s", T) + define("CERBO_PONG_MS", "20000u", "21000u", 11),
    ("cerbo_session_within_16_s", T) + define("CERBO_SESSION_MS", "15000u", "16000u", 8),
    ("cerbo_first_wait_changed", T) + define("CERBO_RETRY_MS", "{2000u, 5000u, 10000u, 30000u}", "{1000u, 5000u, 10000u, 30000u}", 10),
    ("cerbo_last_wait_changed", T) + define("CERBO_RETRY_MS", "{2000u, 5000u, 10000u, 30000u}", "{2000u, 5000u, 10000u, 60000u}", 10),
    ("cerbo_grace_of_16_s", T) + define("CERBO_GRACE_MS", "15000u", "16000u", 10),
    ("cerbo_fresh_for_6_s", T) + define("CERBO_FRESH_MS", "5000u", "6000u", 10),
    ("cerbo_kept_for_61_s", T) + define("CERBO_KEPT_MS", "60000u", "61000u", 11),
    ("cerbo_live_for_11_s", T) + define("CERBO_LIVE_MS", "10000u", "11000u", 11),
    ("cerbo_silent_after_21_s", T) + define("CERBO_SILENT_MS", "20000u", "21000u", 9),
    ("cerbo_yield_halved", T) + define("CERBO_YIELD_PER_MILLE", "100", "50", 3),
    ("cerbo_lag_of_46_s_believed", T) + define("CERBO_LAG_MAX_MS", "45000", "46000", 8),

    # the time
    ("cerbo_time_steps_back_with_the_caller", T, F, ADVANCE, "\tcerbo->clock_ms = now_ms;"),
    ("cerbo_view_for_an_older_time", T, F, AT, "\t(void)cerbo;\n\treturn now_ms;"),
    ("cerbo_view_ignores_the_time_asked_for", T, F, AT, "\t(void)now_ms;\n\treturn cerbo->clock_ms;"),
    ("cerbo_starts_at_time_0", T, F, "\tcerbo->clock_ms = now_ms;\n\tcerbo->safe_mode = safe_mode;", "\t(void)now_ms;\n\tcerbo->safe_mode = safe_mode;"),

    # the address
    ("cerbo_no_text_is_an_address", T, F, HOST_NULL, "\tif(text == NULL) text = \"192.168.88.2\";\n"),
    ("cerbo_long_address_is_cut", T, F, HOST_LONG,
     "\tif(length == CERBO_HOST_SIZE)\n\t{\n\t\tmemcpy(host, text, CERBO_HOST_SIZE - 1);\n\t\thost[CERBO_HOST_SIZE - 1] = '\\0';\n\t\treturn;\n\t}\n"),
    ("cerbo_address_of_15_bytes_refused", T, F, HOST_LONG, "\tif(length >= CERBO_HOST_SIZE - 1) return;\n"),
    ("cerbo_address_not_taken_at_start", T, F, "\tread_host(cerbo->host, host);\n", "\t(void)host;\n"),
    ("cerbo_safe_mode_forgotten", T, F, "\tcerbo->safe_mode = safe_mode;", "\tcerbo->safe_mode = false;\n\t(void)safe_mode;"),
    ("cerbo_on_in_safe_mode", T, F, ON, "\treturn cerbo->host[0] != '\\0';"),
    ("cerbo_on_without_address", T, F, ON, "\treturn !cerbo->safe_mode;"),
    ("cerbo_same_address_starts_over", T, F, SAME_HOST, ""),
    ("cerbo_new_address_not_kept", T, F, "\tstrcpy(cerbo->host, taken);\n", ""),
    ("cerbo_new_address_keeps_the_values", T, F, "\tvalues_clear(&cerbo->values);\n\tcerbo->has_bundle = false;", "\tcerbo->has_bundle = false;"),
    ("cerbo_new_address_keeps_the_last_bundle", T, F, "\tcerbo->has_bundle = false;\n\tcerbo->newer = false;", "\tcerbo->newer = false;"),
    ("cerbo_new_address_keeps_the_newer_contract", T, F, "\tcerbo->newer = false;\n\tcerbo->has_boot = false;", "\tcerbo->has_boot = false;"),
    ("cerbo_new_address_keeps_the_boot", T, F, "\tcerbo->has_boot = false;\n\tcerbo->lag_ms = 0;", "\tcerbo->lag_ms = 0;"),
    ("cerbo_new_address_keeps_the_lag", T, F, "\tcerbo->has_boot = false;\n\tcerbo->lag_ms = 0;", "\tcerbo->has_boot = false;"),
    ("cerbo_new_address_keeps_the_connection", T, F, SET_MOVED, "\tat_once(cerbo);\n"),
    ("cerbo_new_address_closes_what_is_not_there", T, F, SET_MOVED, "\tcerbo->moved = true;\n\tat_once(cerbo);\n"),
    ("cerbo_new_address_waits", T, F, SET_MOVED, "\tif(cerbo->phase != CERBO_IDLE) cerbo->moved = true;\n"),

    # the client id
    ("cerbo_name_of_30_bytes", T, F, INIT_NAME, INIT_NAME.replace("CERBO_NAME_SIZE - 1", "CERBO_NAME_SIZE - 3")),
    ("cerbo_name_not_taken", T, F, "\t\tmemcpy(cerbo->name, name, length);\n", "\t\t(void)length;\n"),
    ("cerbo_no_client_id_without_name", T, F, INIT_CLIENT, "\t(void)probe;\n"),
    ("cerbo_always_the_spare_client_id", T, F, INIT_CLIENT, "\t(void)probe;\n\tstrcpy(cerbo->name, CERBO_CLIENT);\n"),

    # the network, and starting over
    ("cerbo_same_network_starts_over", T, F, SAME_NETWORK, ""),
    ("cerbo_network_not_noted", T, F, JOINED, "\tif(up) at_once(cerbo);"),
    ("cerbo_network_never_gone", T, F, JOINED, "\tcerbo->network = true;\n\tif(up) at_once(cerbo);"),
    ("cerbo_joining_starts_nothing_over", T, F, JOINED, "\tcerbo->network = up;"),
    ("cerbo_starting_over_keeps_the_wait_step", T, F, ONCE_STEP, "\tcerbo->wait_ms = 0;\n"),
    ("cerbo_starting_over_keeps_the_wait", T, F, ONCE_WAIT, "\tcerbo->grace_since_ms = cerbo->clock_ms;\n}"),
    ("cerbo_starting_over_keeps_the_grace", T, F, ONCE_WAIT, "\tcerbo->wait_ms = 0;\n}"),

    # the end of a connection
    ("cerbo_session_end_starts_no_grace", T, F, ENDED_GRACE, "\tcerbo->phase = CERBO_IDLE;"),
    ("cerbo_every_end_starts_the_grace", T, F, ENDED_GRACE, "\tcerbo->grace_since_ms = cerbo->clock_ms;\n\tcerbo->phase = CERBO_IDLE;"),
    ("cerbo_connection_never_ends", T, F, ENDED_GRACE, "\tif(cerbo->phase == CERBO_SESSION) cerbo->grace_since_ms = cerbo->clock_ms;"),
    ("cerbo_packet_waits_beyond_its_connection", T, F, ENDED_FLAGS, "\tcerbo->failed = false;\n"),
    ("cerbo_failure_stays_for_the_next_connection", T, F, ENDED_FLAGS, "\tcerbo->send = false;\n"),
    ("cerbo_moved_stays_for_the_next_connection", T, F, ENDED_MOVED, "\t\treturn;\n\t}\n"),
    ("cerbo_new_address_waits_after_the_close", T, F, ENDED_MOVED, "\t\tcerbo->moved = false;\n\t}\n"),
    ("cerbo_wait_counts_from_an_older_time", T, F, ENDED_WAIT, "\tcerbo->wait_ms = RETRY_WAITS[cerbo->wait_step];\n"),
    ("cerbo_no_wait_after_a_failure", T, F, ENDED_WAIT, "\tcerbo->wait_since_ms = cerbo->clock_ms;\n\tcerbo->wait_ms = 0 * RETRY_WAITS[cerbo->wait_step];\n"),
    ("cerbo_wait_always_the_first", T, F, ENDED_WAIT, "\tcerbo->wait_since_ms = cerbo->clock_ms;\n\tcerbo->wait_ms = RETRY_WAITS[0];\n"),
    ("cerbo_waits_do_not_grow", T, F, ENDED_STEP, "\t(void)RETRY_WAIT_COUNT;"),
    ("cerbo_waits_stop_at_the_third", T, F, ENDED_STEP, ENDED_STEP.replace("RETRY_WAIT_COUNT - 1", "RETRY_WAIT_COUNT - 2")),
    ("cerbo_report_without_connection_counts", T, F, DOWN, "\tended(cerbo);"),

    # the orders
    ("cerbo_opens_when_off", T, F, IDLE_OFF, "\t\tif(!cerbo->network) return CERBO_DO_NOTHING;"),
    ("cerbo_opens_without_network", T, F, IDLE_OFF, "\t\tif(!cerbo_on(cerbo)) return CERBO_DO_NOTHING;"),
    ("cerbo_opens_before_the_wait_is_over", T, F, IDLE_WAIT, ""),
    ("cerbo_opens_a_millisecond_late", T, F, IDLE_WAIT, IDLE_WAIT.replace("< cerbo->wait_ms", "<= cerbo->wait_ms")),
    ("cerbo_opens_again_and_again", T, F, OPEN, "\t\tcerbo->open_ms = cerbo->clock_ms;\n\t\treturn CERBO_DO_OPEN;"),
    ("cerbo_time_of_the_order_to_open_not_noted", T, F, OPEN, "\t\tcerbo->phase = CERBO_OPENING;\n\t\treturn CERBO_DO_OPEN;"),
    ("cerbo_stays_connected_without_network", T, F, CLOSE_IF, "\tif(cerbo->moved || cerbo->failed)"),
    ("cerbo_stays_connected_to_the_old_address", T, F, CLOSE_IF, "\tif(!cerbo->network || cerbo->failed)"),
    ("cerbo_stays_connected_after_a_failure", T, F, CLOSE_IF, "\tif(!cerbo->network || cerbo->moved)"),
    ("cerbo_no_limit_to_the_session", T, F, LATE, "\t\tif(false)"),
    ("cerbo_session_limit_a_millisecond_late", T, F, LATE, LATE.replace(">= CERBO_SESSION_MS", "> CERBO_SESSION_MS")),
    ("cerbo_packet_sent_again_and_again", T, F, SEND, "\t\tif(!cerbo->send) return CERBO_DO_NOTHING;\n\n\t\treturn CERBO_DO_SEND;"),
    ("cerbo_packet_never_sent", T, F, SEND, "\t\treturn CERBO_DO_NOTHING;"),
    ("cerbo_send_before_close", T, F,
     LATE + "\n\t\t{\n\t\t\tended(cerbo);\n\t\t\treturn CERBO_DO_CLOSE;\n\t\t}\n" + SEND,
     "\t\tif(cerbo->send)\n\t\t{\n\t\t\tcerbo->send = false;\n\t\t\treturn CERBO_DO_SEND;\n\t\t}\n" + LATE +
     "\n\t\t{\n\t\t\tended(cerbo);\n\t\t\treturn CERBO_DO_CLOSE;\n\t\t}\n\t\treturn CERBO_DO_NOTHING;"),

    # PINGREQ and PINGRESP
    ("cerbo_session_closed_without_ping_under_way", T, F, PONG_LATE, PONG_LATE.replace("cerbo->pings_open > 0 && ", "")),
    ("cerbo_session_never_closed_for_silence", T, F, PONG_LATE, "\tif(false)"),
    ("cerbo_pong_limit_a_millisecond_late", T, F, PONG_LATE, PONG_LATE.replace(">= CERBO_PONG_MS", "> CERBO_PONG_MS")),
    ("cerbo_ping_with_every_question", T, F, PING_DUE, ""),
    ("cerbo_ping_a_millisecond_late", T, F, PING_DUE, PING_DUE.replace("< CERBO_PING_MS", "<= CERBO_PING_MS")),
    ("cerbo_ping_is_the_packet_before", T, F, PING_BUILT, ""),
    ("cerbo_ping_deadline_moves_with_every_ping", T, F, PING_OLDEST, "\tcerbo->ping_open_ms = cerbo->clock_ms;\n"),
    ("cerbo_ping_deadline_never_set", T, F, PING_OLDEST, ""),
    ("cerbo_ping_not_counted", T, F, PING_NOTED, "\tcerbo->ping_ms = cerbo->clock_ms;\n\treturn CERBO_DO_SEND;"),
    ("cerbo_ping_time_not_noted", T, F, PING_NOTED, "\tcerbo->pings_open++;\n\treturn CERBO_DO_SEND;"),
    ("cerbo_ping_no_close_first", T, F,
     PONG_LATE + "\n\t{\n\t\tended(cerbo);\n\t\treturn CERBO_DO_CLOSE;\n\t}\n" + PING_DUE,
     "\tif(cerbo->clock_ms - cerbo->ping_ms >= CERBO_PING_MS && cerbo->pings_open < 2)\n\t{\n\t\tif(cerbo->pings_open == 0) cerbo->ping_open_ms = cerbo->clock_ms;\n"
     "\t\tcerbo->pings_open++;\n\t\tcerbo->ping_ms = cerbo->clock_ms;\n\t\tcerbo->out_length = mqtt_sub_ping(cerbo->out, sizeof(cerbo->out));\n\t\treturn CERBO_DO_SEND;\n\t}\n" +
     PONG_LATE + "\n\t{\n\t\tended(cerbo);\n\t\treturn CERBO_DO_CLOSE;\n\t}\n" + PING_DUE),
    ("cerbo_pingresp_out_of_session_taken", T, F, PINGRESP_IF, "\tif(cerbo->pings_open == 0)"),
    ("cerbo_pingresp_nobody_asked_for_taken", T, F, PINGRESP_IF, "\tif(cerbo->phase != CERBO_SESSION)"),
    ("cerbo_pingresp_out_of_place_ignored", T, F, PINGRESP_FAILS, PINGRESP_IF + "\n\t{\n"),
    ("cerbo_pingresp_not_counted", T, F, "\tcerbo->pings_open--;\n", ""),
    ("cerbo_second_ping_keeps_the_deadline_of_the_first", T, F, "\tif(cerbo->pings_open > 0) cerbo->ping_open_ms = cerbo->ping_ms;\n", ""),
    ("cerbo_pingresp_time_not_noted", T, F, "\tcerbo->pong_ms = cerbo->clock_ms;\n}", "}"),

    # the connection stands
    ("cerbo_opening_reported_at_any_place", T, F, OPENED_IF, ""),
    ("cerbo_standing_connection_not_noted", T, F, OPENED, "\tmqtt_sub_begin(&cerbo->reader);\n"),
    ("cerbo_reader_goes_on_from_the_last_connection", T, F, OPENED, "\tcerbo->phase = CERBO_GREETING;\n"),
    ("cerbo_lag_measured_across_connections", T, F, OPENED_CLOCK, "\tcerbo->out_length = mqtt_sub_connect("),
    ("cerbo_connect_not_built", T, F, CONNECT, "\tcerbo->out_length = 0;\n\tcerbo->send = true;"),
    ("cerbo_connect_with_the_spare_id", T, F, CONNECT, CONNECT.replace("cerbo->name", "CERBO_CLIENT")),
    ("cerbo_connect_without_keepalive", T, F, CONNECT, CONNECT.replace("CERBO_KEEPALIVE_S", "0")),
    ("cerbo_connect_never_sent", T, F, CONNECT, CONNECT.replace("cerbo->send = true;", "cerbo->send = false;")),

    # CONNACK and SUBACK
    ("cerbo_second_connack_taken", T, F, CONNACK_IF, "\tif(cerbo->phase == CERBO_SESSION || cerbo->phase == CERBO_IDLE || cerbo->send || packet->code != 0)"),
    ("cerbo_connack_before_connect_taken", T, F, CONNACK_IF, "\tif(cerbo->phase != CERBO_GREETING || packet->code != 0)"),
    ("cerbo_refusing_connack_taken", T, F, CONNACK_IF, "\tif(cerbo->phase != CERBO_GREETING || cerbo->send || packet == NULL)"),
    ("cerbo_connack_out_of_place_ignored", T, F, CONNACK_FAILS, CONNACK_IF + "\n\t{\n"),
    ("cerbo_connack_does_not_go_on", T, F, "\tcerbo->phase = CERBO_SUBSCRIBING;\n", ""),
    ("cerbo_subscribe_not_built", T, F, SUBSCRIBE, "\tcerbo->send = true;"),
    ("cerbo_subscribe_never_sent", T, F, SUBSCRIBE, SUBSCRIBE.replace("\n\tcerbo->send = true;", "")),
    ("cerbo_subscribe_to_another_topic", T, F, SUBSCRIBE, SUBSCRIBE.replace("CERBO_TOPIC", "\"display/gx\"")),
    ("cerbo_suback_at_any_place_taken", T, F, SUBACK_IF, "\tif(cerbo->phase == CERBO_IDLE || cerbo->send || packet->id != CERBO_PACKET_ID || packet->code != 0)"),
    ("cerbo_suback_before_subscribe_taken", T, F, SUBACK_IF, SUBACK_IF.replace(" || cerbo->send", "")),
    ("cerbo_suback_with_other_id_taken", T, F, SUBACK_IF, SUBACK_IF.replace(" || packet->id != CERBO_PACKET_ID", "")),
    ("cerbo_refusing_suback_taken", T, F, SUBACK_IF, SUBACK_IF.replace(" || packet->code != 0", "")),
    ("cerbo_suback_with_qos_1_taken", T, F, SUBACK_IF, SUBACK_IF.replace("packet->code != 0", "packet->code > 2")),
    ("cerbo_suback_out_of_place_ignored", T, F, SUBACK_FAILS, SUBACK_IF + "\n\t{\n"),
    ("cerbo_suback_makes_no_session", T, F, "\tcerbo->phase = CERBO_SESSION;\n", ""),
    ("cerbo_session_not_counted", T, F, "\tcerbo->sessions++;\n", ""),
    ("cerbo_session_keeps_the_wait_step", T, F, "\tcerbo->sessions++;\n\tcerbo->wait_step = 0;\n", "\tcerbo->sessions++;\n"),
    ("cerbo_first_ping_not_from_the_suback", T, F, "\tcerbo->ping_ms = cerbo->clock_ms;\n\tcerbo->pings_open = 0;", "\tcerbo->pings_open = 0;"),
    ("cerbo_pings_of_the_session_before_stay_open", T, F, "\tcerbo->ping_ms = cerbo->clock_ms;\n\tcerbo->pings_open = 0;", "\tcerbo->ping_ms = cerbo->clock_ms;"),
    ("cerbo_silence_not_counted_from_the_suback", T, F, "\tcerbo->pings_open = 0;\n\tcerbo->quiet_since_ms = cerbo->clock_ms;", "\tcerbo->pings_open = 0;"),

    # the bytes
    ("cerbo_bytes_read_without_connection", T, F, READS, READS.replace("cerbo->phase == CERBO_IDLE || ", "")),
    ("cerbo_bytes_read_while_opening", T, F, READS, READS.replace("cerbo->phase == CERBO_OPENING || ", "")),
    ("cerbo_bytes_of_the_old_address_read", T, F, READS, READS.replace(" || cerbo->moved", "")),
    ("cerbo_bytes_read_after_a_failure", T, F, READS, READS.replace(" || cerbo->failed", "")),
    ("cerbo_broken_bytes_do_not_close", T, F, BROKEN, "\t\tif(kind == MQTT_SUB_BROKEN) return;\n"),
    ("cerbo_reads_on_behind_a_failure", T, F, AFTER, "\t}\n}"),
    ("cerbo_publish_while_greeting_taken", T, F, EARLY, "\t\telse if(cerbo->send) cerbo->failed = true;"),
    ("cerbo_publish_before_subscribe_taken", T, F, EARLY, "\t\telse if(cerbo->phase == CERBO_GREETING) cerbo->failed = true;"),
    ("cerbo_gone_by_not_counted", T, F, GONE_BY, "\t\telse if(kind == MQTT_SUB_SKIPPED) (void)0;\n"),
    ("cerbo_gone_by_read_as_bundle", T, F, GONE_BY, ""),

    # what a bundle is
    ("cerbo_topic_not_compared", T, F, TOPIC_COUNTED, ""),
    ("cerbo_topic_compared_by_its_beginning", T, F, TOPIC, "\tif(packet->topic_length < TOPIC_LENGTH || memcmp(packet->topic, CERBO_TOPIC, TOPIC_LENGTH) != 0)"),
    ("cerbo_topic_compared_by_its_length", T, F, TOPIC, "\tif(packet->topic_length != TOPIC_LENGTH)"),
    ("cerbo_other_topic_not_counted", T, F, TOPIC_COUNTED, TOPIC + "\n\t{\n\t\treturn;\n\t}\n"),
    ("cerbo_retained_taken", T, F, RETAINED, ""),
    ("cerbo_retained_not_counted", T, F, RETAINED, "\tif(packet->retained)\n\t{\n\t\treturn;\n\t}\n"),
    ("cerbo_retained_judged_before_topic", T, F, TOPIC_COUNTED + RETAINED, RETAINED + TOPIC_COUNTED),
    ("cerbo_long_bundle_taken", T, F, LONG, ""),
    ("cerbo_bundle_of_1400_refused", T, F, LONG, LONG.replace("> CERBO_BUNDLE_MAX", ">= CERBO_BUNDLE_MAX")),
    ("cerbo_long_bundle_not_counted", T, F, LONG, LONG.replace("\t\tcerbo->refused_long++;\n", "")),
    ("cerbo_length_judged_before_retained", T, F, RETAINED + LONG, LONG + RETAINED),
    ("cerbo_unreadable_text_read_with_old_tokens", T, F, READ,
     READ.replace("json_parse(json, packet->payload_length, work, work_count) < 0 || ", "(json_parse(json, packet->payload_length, work, work_count) < 0 && work == NULL) || ")),
    ("cerbo_invalid_bundle_not_counted", T, F, READ, READ.replace("\t\tcerbo->refused_invalid++;\n", "")),
    ("cerbo_version_0_taken", T, F, READ, READ.replace("\"v\", 1, INT64_MAX", "\"v\", 0, INT64_MAX")),
    ("cerbo_version_not_needed", T, F, READ, READ.replace(" || !whole(json, work, \"v\", 1, INT64_MAX, &version)", "")),
    ("cerbo_newer_version_taken", T, F, NEWER, ""),
    ("cerbo_newer_version_not_counted", T, F, NEWER, NEWER.replace("\t\tcerbo->refused_version++;\n", "")),
    ("cerbo_newer_version_not_shown", T, F, NEWER, NEWER.replace("\t\tcerbo->newer = true;\n", "")),
    ("cerbo_newer_version_counted_as_invalid", T, F, NEWER, NEWER.replace("cerbo->refused_version++", "cerbo->refused_invalid++")),
    ("cerbo_boot_not_needed", T, F, REST, REST.replace("!whole(json, work, \"boot\", 1, CERBO_BOOT_MAX, &boot) || ", "")),
    ("cerbo_boot_0_taken", T, F, REST, REST.replace("\"boot\", 1, CERBO_BOOT_MAX", "\"boot\", 0, CERBO_BOOT_MAX")),
    ("cerbo_boot_beyond_32_bit_taken", T, F, REST, REST.replace("\"boot\", 1, CERBO_BOOT_MAX", "\"boot\", 1, INT64_MAX")),
    ("cerbo_t_not_needed", T, F, REST, REST.replace("!whole(json, work, \"t\", 0, CERBO_T_MAX, &t_ms) || ", "")),
    ("cerbo_negative_t_taken", T, F, REST, REST.replace("\"t\", 0, CERBO_T_MAX", "\"t\", -1, CERBO_T_MAX")),
    ("cerbo_t_1_refused", T, F, REST, REST.replace("\"t\", 0, CERBO_T_MAX", "\"t\", 1, CERBO_T_MAX")),
    ("cerbo_values_of_any_kind_taken", T, F, REST, REST.replace(" || work[values].type != JSON_OBJECT", "")),
    ("cerbo_invalid_members_not_counted", T, F, REST, REST.replace("\t\tcerbo->refused_invalid++;\n", "")),
    ("cerbo_number_with_fraction_is_whole", T, F, WHOLE,
     "\tdouble read = 0;\n\n\tif(member < 0 || !json_number(json, &work[member], &read) || read < (double)least || read > (double)most || read > 9e15) return false;\n"
     "\t*number = (int64_t)read;\n\treturn true;"),
    ("cerbo_whole_number_not_read", T, F, WHOLE, "\treturn member >= 0 && work[member].type == JSON_NUMBER && *number >= least && *number <= most;"),
    ("cerbo_number_below_the_least_taken", T, F, WHOLE, WHOLE.replace(" && *number >= least", "").replace("&& *number <= most;", "&& *number <= most && least <= most;")),
    ("cerbo_number_above_the_most_taken", T, F, WHOLE, WHOLE.replace(" && *number <= most", " && most >= least")),

    # the store of values
    ("cerbo_store_not_emptied", T, F, EMPTIED, "\tfor(int i = 0; i < work[object].size; i++)"),
    ("cerbo_members_stepped_into", T, F, "\t\tkey += 1 + member->skip;\n", "\t\tkey += 2;\n"),
    ("cerbo_key_of_any_shape_taken", T, F, KEY_READ, KEY_READ.replace(" || !key_fits(name)", " || (name[0] == '\\0' && !key_fits(name))")),
    ("cerbo_key_of_32_bytes_refused", T, F, KEY_READ, KEY_READ.replace("sizeof(name))", "sizeof(name) - 1)")),
    ("cerbo_passed_key_not_counted", T, F, KEY_READ, KEY_READ.replace("\t\t\tcerbo->skipped++;\n", "")),
    ("cerbo_key_without_prefix_taken", T, F, KEY_HEAD, "\tif(strlen(key) <= PREFIX_LENGTH) return false;"),
    ("cerbo_key_of_the_prefix_alone_taken", T, F, KEY_HEAD, "\tif(!cerbo_key(key)) return false;"),
    ("cerbo_key_with_any_character_taken", T, F, KEY_BODY, "\t\tif(*c == ' ') return false;"),
    ("cerbo_key_with_small_letters_taken", T, F, KEY_BODY, KEY_BODY.replace("|| *c == '_'", "|| *c == '_' || (*c >= 'a' && *c <= 'z')")),
    ("cerbo_key_without_capitals", T, F, KEY_BODY, KEY_BODY.replace("(*c >= 'A' && *c <= 'Z') || ", "")),
    ("cerbo_key_without_digits", T, F, KEY_BODY, KEY_BODY.replace(" || (*c >= '0' && *c <= '9')", "")),
    ("cerbo_key_without_underscore", T, F, KEY_BODY, KEY_BODY.replace(" || *c == '_'", "")),
    ("cerbo_key_takes_at", T, F, KEY_BODY, KEY_BODY.replace("*c >= 'A'", "*c >= '@'")),
    ("cerbo_key_takes_bracket", T, F, KEY_BODY, KEY_BODY.replace("*c <= 'Z'", "*c <= '['")),
    ("cerbo_key_takes_slash", T, F, KEY_BODY, KEY_BODY.replace("*c >= '0'", "*c >= '/'")),
    ("cerbo_key_takes_colon", T, F, KEY_BODY, KEY_BODY.replace("*c <= '9'", "*c <= ':'")),
    ("cerbo_key_takes_caret", T, F, KEY_BODY, KEY_BODY.replace("*c == '_'", "(*c >= '^' && *c <= '_')")),
    ("cerbo_key_takes_backtick", T, F, KEY_BODY, KEY_BODY.replace("*c == '_'", "(*c >= '_' && *c <= '`')")),
    ("cerbo_key_loses_a", T, F, KEY_BODY, KEY_BODY.replace("*c >= 'A'", "*c > 'A'")),
    ("cerbo_key_loses_z", T, F, KEY_BODY, KEY_BODY.replace("*c <= 'Z'", "*c < 'Z'")),
    ("cerbo_key_loses_0", T, F, KEY_BODY, KEY_BODY.replace("*c >= '0'", "*c > '0'")),
    ("cerbo_key_loses_9", T, F, KEY_BODY, KEY_BODY.replace("*c <= '9'", "*c < '9'")),
    ("cerbo_number_not_taken", T, F, KINDS, KINDS.replace("\t\tif(json_number(json, member, &number)) kind = VALUE_NUMBER;\n\t\telse if", "\t\tif")),
    ("cerbo_on_is_off", T, F, KINDS, KINDS.replace("\"on\")) kind = VALUE_ON;", "\"on\")) kind = VALUE_OFF;")),
    ("cerbo_off_is_on", T, F, KINDS, KINDS.replace("\"off\")) kind = VALUE_OFF;", "\"off\")) kind = VALUE_ON;")),
    ("cerbo_on_not_taken", T, F, KINDS, KINDS.replace("\t\telse if(json_text_is(json, member, \"on\")) kind = VALUE_ON;\n", "")),
    ("cerbo_off_not_taken", T, F, KINDS, KINDS.replace("\t\telse if(json_text_is(json, member, \"off\")) kind = VALUE_OFF;\n", "")),
    ("cerbo_every_text_is_on", T, F, KINDS, KINDS.replace("json_text_is(json, member, \"on\")", "member->type == JSON_STRING")),
    ("cerbo_true_is_on", T, F, KINDS, KINDS.replace("json_text_is(json, member, \"on\")", "(member->type == JSON_TRUE || json_text_is(json, member, \"on\"))")),
    ("cerbo_passed_value_not_counted", T, F, KINDS, KINDS.replace("\t\t\tcerbo->skipped++;\n", "")),
    ("cerbo_same_key_kept_twice", T, F, FOUND, "\t\tif(strcmp(item->name, name) == 0 && i < 0) return item;"),
    ("cerbo_first_of_two_keys_counts", T, F, "\t\titem = find(values, name);\n\t\tif(item == NULL)\n", "\t\titem = find(values, name);\n\t\tif(item != NULL) continue;\n\t\tif(item == NULL)\n"),
    ("cerbo_store_takes_63", T, F, NO_ROOM, NO_ROOM.replace("values->count >= VALUES_MAX", "values->count >= VALUES_MAX - 1")),
    ("cerbo_member_without_room_not_counted", T, F, NO_ROOM, NO_ROOM.replace("\t\t\t\tcerbo->skipped++;\n", "")),
    ("cerbo_name_not_kept", T, F, "\t\t\tstrcpy(item->name, name);\n", "\t\t\titem->name[0] = '\\0';\n"),
    ("cerbo_kind_not_kept", T, F, KEPT, KEPT.replace("\t\titem->kind = kind;\n", "\t\t(void)kind;\n")),
    ("cerbo_number_not_kept", T, F, KEPT, KEPT.replace("\t\titem->number = number;\n", "")),
    ("cerbo_time_of_value_not_kept", T, F, KEPT, KEPT.replace("\t\titem->seen_ms = seen_ms;\n", "\t\t(void)seen_ms;\n")),

    # the lag
    ("cerbo_first_bundle_of_a_broker_no_new_boot", T, F, BOOT_NEW, "\tif(cerbo->boot != boot)"),
    ("cerbo_new_boot_not_noticed", T, F, BOOT_NEW, "\tif(!cerbo->has_boot)"),
    ("cerbo_new_boot_not_counted", T, F, BOOT_COUNTED, "\t\tcerbo->has_clock = false;\n\t}\n\tif(cerbo->has_clock)"),
    ("cerbo_lag_measured_across_boots", T, F, BOOT_COUNTED, "\t\tcerbo->boots++;\n\t}\n\tif(cerbo->has_clock)"),
    ("cerbo_yield_rest_not_carried", T, F, PART, PART.replace(" + cerbo->yield_rest", "")),
    ("cerbo_yield_without_the_seconds", T, F, YIELDED, "\t\tuint64_t yielded = part / 1000;"),
    ("cerbo_yield_without_the_milliseconds", T, F, YIELDED, "\t\tuint64_t yielded = (elapsed / 1000) * CERBO_YIELD_PER_MILLE;"),
    ("cerbo_base_does_not_yield", T, F, BEHIND, "\t\tuint64_t behind = cerbo->lag_ms + elapsed + 0 * yielded;"),
    ("cerbo_lag_before_forgotten", T, F, BEHIND, "\t\tuint64_t behind = elapsed - yielded;"),
    ("cerbo_time_since_the_bundle_before_forgotten", T, F, BEHIND, "\t\tuint64_t behind = cerbo->lag_ms + 0 * (elapsed - yielded);"),
    ("cerbo_yield_rest_not_noted", T, F, "\t\tcerbo->yield_rest = (uint32_t)(part % 1000);\n", ""),
    ("cerbo_t_stepping_back_is_no_lag", T, F, STEP_BACK, "\t\tif(step < 0) lag = behind;"),
    ("cerbo_step_of_t_not_taken_off", T, F, STEP_ON, "\t\telse lag = behind;"),
    ("cerbo_lag_below_0", T, F, STEP_ON, "\t\telse lag = behind - (uint64_t)step;"),
    ("cerbo_every_lag_believed", T, F, DISTRUST, "\tif(false)"),
    ("cerbo_lag_of_45_s_not_believed", T, F, DISTRUST, "\tif(lag >= CERBO_LAG_MAX_MS)"),
    ("cerbo_distrust_not_counted", T, F, DISTRUSTED, "\t\tcerbo->has_clock = false;\n\t\tlag = 0;\n"),
    ("cerbo_distrusted_lag_keeps_the_rest", T, F, DISTRUSTED, "\t\tcerbo->distrusted++;\n\t\tlag = 0;\n"),
    ("cerbo_distrusted_lag_stays", T, F, DISTRUSTED, "\t\tcerbo->distrusted++;\n\t\tcerbo->has_clock = false;\n"),
    ("cerbo_new_base_keeps_the_rest", T, F, NEW_BASE, ""),
    ("cerbo_boot_not_noted", T, F, "\tcerbo->has_boot = true;\n\tcerbo->boot = boot;\n", "\tcerbo->has_boot = true;\n"),
    ("cerbo_boot_never_known", T, F, "\tcerbo->has_boot = true;\n\tcerbo->boot = boot;\n", "\tcerbo->boot = boot;\n"),
    ("cerbo_clock_never_set", T, F, "\tcerbo->has_clock = true;\n\tcerbo->t_ms = t_ms;\n", "\tcerbo->t_ms = t_ms;\n"),
    ("cerbo_t_not_noted", T, F, "\tcerbo->has_clock = true;\n\tcerbo->t_ms = t_ms;\n", "\tcerbo->has_clock = true;\n"),
    ("cerbo_lag_not_noted", T, F, "\tcerbo->lag_ms = (uint32_t)lag;\n}", "\t(void)lag;\n}"),
    ("cerbo_values_seen_at_arrival", T, F, SEEN, "\tfill(cerbo, json, work, values, cerbo->clock_ms);"),
    ("cerbo_values_seen_before_time_0", T, F, SEEN, "\tfill(cerbo, json, work, values, cerbo->clock_ms - cerbo->lag_ms);"),
    ("cerbo_boot_cut_to_16_bit", T, F, "\treckon(cerbo, (uint32_t)boot, t_ms);", "\treckon(cerbo, (uint16_t)boot, t_ms);"),

    # what a bundle that is taken leaves
    ("cerbo_taken_bundle_keeps_newer", T, F, TAKEN, TAKEN.replace("\tcerbo->newer = false;\n", "")),
    ("cerbo_taken_bundle_not_noted", T, F, TAKEN, TAKEN.replace("\tcerbo->has_bundle = true;\n", "")),
    ("cerbo_time_of_bundle_not_noted", T, F, TAKEN, TAKEN.replace("\tcerbo->bundle_ms = cerbo->clock_ms;\n", "")),
    ("cerbo_bundle_does_not_end_the_silence", T, F, TAKEN, TAKEN.replace("\tcerbo->quiet_since_ms = cerbo->clock_ms;\n", "")),
    ("cerbo_bundle_not_counted", T, F, TAKEN, TAKEN.replace("\tcerbo->bundles++;\n", "")),

    # the view
    ("cerbo_view_not_off", T, F, VIEW_OFF, ""),
    ("cerbo_view_without_network", T, F, VIEW_WIFI, ""),
    ("cerbo_no_wifi_before_off", T, F, VIEW_OFF + VIEW_WIFI, VIEW_WIFI + VIEW_OFF),
    ("cerbo_grace_a_millisecond_longer", T, F, VIEW_GRACE, VIEW_GRACE.replace("< CERBO_GRACE_MS", "<= CERBO_GRACE_MS")),
    ("cerbo_waiting_and_unreachable_swapped", T, F, VIEW_GRACE, VIEW_GRACE.replace("CERBO_VIEW_CONNECTING : CERBO_VIEW_UNREACHABLE", "CERBO_VIEW_UNREACHABLE : CERBO_VIEW_CONNECTING")),
    ("cerbo_view_of_a_connection_is_a_session", T, F, "\tif(cerbo->phase != CERBO_SESSION)\n\t{\n\t\treturn now", "\tif(cerbo->phase == CERBO_IDLE || cerbo->phase == CERBO_OPENING)\n\t{\n\t\treturn now"),
    ("cerbo_newer_never_shown", T, F, VIEW_NEWER, ""),
    ("cerbo_live_before_newer", T, F, VIEW_NEWER + VIEW_LIVE, VIEW_LIVE + VIEW_NEWER),
    ("cerbo_live_without_bundle", T, F, VIEW_LIVE, VIEW_LIVE.replace("cerbo->has_bundle && ", "")),
    ("cerbo_live_a_millisecond_longer", T, F, VIEW_LIVE, VIEW_LIVE.replace("< CERBO_LIVE_MS", "<= CERBO_LIVE_MS")),
    ("cerbo_never_live", T, F, VIEW_LIVE, ""),
    ("cerbo_silent_by_a_pingresp_from_before", T, F, VIEW_SILENT, VIEW_SILENT.replace("cerbo->pong_ms >= cerbo->quiet_since_ms && ", "")),
    ("cerbo_silent_a_millisecond_late", T, F, VIEW_SILENT, VIEW_SILENT.replace(">= CERBO_SILENT_MS", "> CERBO_SILENT_MS")),
    ("cerbo_silent_without_proof", T, F, VIEW_SILENT, "\tif(now - cerbo->quiet_since_ms >= CERBO_SILENT_MS) return CERBO_VIEW_SILENT;"),
    ("cerbo_never_silent", T, F, VIEW_SILENT + "\n", ""),

    # what shows, ages and keys
    ("cerbo_live_alone_does_not_show", T, F, SHOW_LIVE, "\t(void)cerbo_view(cerbo, now_ms);\n"),
    ("cerbo_only_fresh_values_show", T, F, SHOW_VALUE, "\t\tif(cerbo_age(item, now_ms) == VALUE_AGE_FRESH) return true;"),
    ("cerbo_values_never_show", T, F, SHOW_VALUE, "\t\tif(cerbo_age(item, now_ms) != VALUE_AGE_GONE && i < 0) return true;"),
    ("cerbo_no_value_is_fresh", T, F, AGE_NULL, "\tif(value == NULL) return VALUE_AGE_FRESH;"),
    ("cerbo_age_steps_back_with_the_caller", T, F, AGE, "\tage = now_ms - value->seen_ms;"),
    ("cerbo_fresh_a_millisecond_longer", T, F, AGE_FRESH, "\tif(age <= CERBO_FRESH_MS) return VALUE_AGE_FRESH;"),
    ("cerbo_fresh_as_the_wican", T, F, AGE_FRESH, "\tif(age < VALUE_FRESH_MS) return VALUE_AGE_FRESH;"),
    ("cerbo_kept_a_millisecond_longer", T, F, AGE_OLD, "\tif(age <= CERBO_KEPT_MS) return VALUE_AGE_OLD;"),
    ("cerbo_kept_as_the_wican", T, F, AGE_OLD, "\tif(age < VALUE_KEPT_MS) return VALUE_AGE_OLD;"),
    ("cerbo_no_key_is_a_key", T, F, KEY, "\treturn key == NULL || strncmp(key, CERBO_PREFIX, PREFIX_LENGTH) == 0;"),
    ("cerbo_key_by_two_letters", T, F, KEY, KEY.replace("PREFIX_LENGTH) == 0", "PREFIX_LENGTH - 1) == 0")),
    ("cerbo_every_key_is_one", T, F, KEY, "\treturn key != NULL;"),
]

# The numbers the info line and GET /api/info take from the source
SUM = ("\tuint64_t sum = (uint64_t)cerbo->refused_invalid + cerbo->refused_version + cerbo->refused_retained + cerbo->refused_topic + "
       "cerbo->refused_long;")
CAPPED = "\treturn sum > UINT32_MAX ? UINT32_MAX : (uint32_t)sum;"
COUNTED = "\t\tif(cerbo_age(&cerbo->values.items[i], now_ms) != VALUE_AGE_GONE) shown++;"
# What has no text in GET /api/values is not counted: a number beyond a double, taken as infinite
TEXT = "\t\tif(!has_text(&cerbo->values.items[i])) continue;\n"
EACH = ("\tfor(int i = 0; i < cerbo->values.count; i++)\n\t{\n"
        "\t\t// The number of values GET /api/values names: that answer leaves out what has no text\n" + TEXT + COUNTED)
TEXT_NUMBER = "\tif(item->kind == VALUE_NUMBER) return isfinite(item->number);\n"
TEXT_SWITCH = "\treturn item->kind == VALUE_ON || item->kind == VALUE_OFF;"

MUTATIONS += [
    ("cerbo_refused_without_the_invalid", T, F, SUM, SUM.replace("(uint64_t)cerbo->refused_invalid + cerbo->refused_version", "(uint64_t)cerbo->refused_version")),
    ("cerbo_refused_without_the_newer", T, F, SUM, SUM.replace(" + cerbo->refused_version", "")),
    ("cerbo_refused_without_the_retained", T, F, SUM, SUM.replace(" + cerbo->refused_retained", "")),
    ("cerbo_refused_without_the_other_topics", T, F, SUM, SUM.replace(" + cerbo->refused_topic", "")),
    ("cerbo_refused_without_the_long", T, F, SUM, SUM.replace(" + cerbo->refused_long", "")),
    ("cerbo_refused_counts_members", T, F, SUM, SUM.replace("cerbo->refused_long;", "cerbo->refused_long + cerbo->skipped;")),
    ("cerbo_refused_counts_bundles", T, F, SUM, SUM.replace("cerbo->refused_long;", "cerbo->refused_long + cerbo->bundles;")),
    ("cerbo_refused_counts_distrusted_lags", T, F, SUM, SUM.replace("cerbo->refused_long;", "cerbo->refused_long + cerbo->distrusted;")),
    ("cerbo_refused_the_invalid_twice", T, F, SUM, SUM.replace(" + cerbo->refused_version", " + cerbo->refused_invalid")),
    ("cerbo_refused_summed_in_32_bit", T, F, SUM, SUM.replace("(uint64_t)cerbo->refused_invalid", "(uint64_t)(uint32_t)(cerbo->refused_invalid") .replace("cerbo->refused_long;", "cerbo->refused_long);")),
    ("cerbo_refused_wraps", T, F, CAPPED, "\treturn (uint32_t)sum;"),
    ("cerbo_refused_capped_one_early", T, F, CAPPED, "\treturn sum >= UINT32_MAX ? UINT32_MAX : (uint32_t)sum + (sum == UINT32_MAX - 1);"),
    ("cerbo_refused_capped_at_31_bit", T, F, CAPPED, "\treturn sum > INT32_MAX ? UINT32_MAX : (uint32_t)sum;"),
    ("cerbo_refused_too_many_is_none", T, F, CAPPED, "\treturn sum > UINT32_MAX ? 0 : (uint32_t)sum;"),
    ("cerbo_shown_counts_gone_values", T, F, COUNTED, COUNTED.replace("!= VALUE_AGE_GONE", "!= VALUE_AGE_GONE || now_ms >= cerbo->values.items[i].seen_ms")),
    ("cerbo_shown_only_fresh_values", T, F, COUNTED, COUNTED.replace("!= VALUE_AGE_GONE", "== VALUE_AGE_FRESH")),
    ("cerbo_shown_only_old_values", T, F, COUNTED, COUNTED.replace("!= VALUE_AGE_GONE", "== VALUE_AGE_OLD")),
    ("cerbo_shown_by_the_first_value", T, F, COUNTED, COUNTED.replace("items[i]", "items[0]")),
    ("cerbo_shown_by_the_limits_of_the_wican", T, F, COUNTED, COUNTED.replace("cerbo_age(", "values_age(")),
    ("cerbo_shown_first_value_left_out", T, F, EACH, EACH.replace("int i = 0;", "int i = 1;")),
    ("cerbo_shown_last_value_left_out", T, F, EACH, EACH.replace("i < cerbo->values.count;", "i < cerbo->values.count - 1;")),
    ("cerbo_shown_is_one_or_none", T, F, COUNTED, COUNTED.replace("shown++;", "shown = 1;")),
    ("cerbo_shown_never_later_than_the_source_has_seen", T, F, COUNTED, COUNTED.replace("now_ms)", "now_ms < cerbo->clock_ms ? now_ms : cerbo->clock_ms)")),
    # the count is the number of values GET /api/values names
    ("cerbo_shown_counts_what_has_no_text", T, F, TEXT, "\t\t(void)has_text;\n"),
    ("cerbo_shown_text_of_the_first_value", T, F, TEXT, TEXT.replace("items[i]", "items[0]")),
    ("cerbo_shown_counts_numbers_without_end", T, F, TEXT_NUMBER, "\tif(item->kind == VALUE_NUMBER) return true;\n"),
    ("cerbo_shown_counts_minus_infinite", T, F, TEXT_NUMBER, "\tif(item->kind == VALUE_NUMBER) return item->number < HUGE_VAL;\n"),
    ("cerbo_shown_counts_plus_infinite", T, F, TEXT_NUMBER, "\tif(item->kind == VALUE_NUMBER) return item->number > -HUGE_VAL;\n"),
    ("cerbo_shown_counts_what_is_no_number", T, F, TEXT_NUMBER, "\tif(item->kind == VALUE_NUMBER) return !isinf(item->number);\n"),
    ("cerbo_shown_counts_no_number", T, F, TEXT_NUMBER, "\tif(item->kind == VALUE_NUMBER) return false;\n"),
    ("cerbo_shown_counts_no_switch", T, F, TEXT_SWITCH, "\treturn false;"),
    ("cerbo_shown_counts_no_on", T, F, TEXT_SWITCH, "\treturn item->kind == VALUE_OFF;"),
    ("cerbo_shown_counts_no_off", T, F, TEXT_SWITCH, "\treturn item->kind == VALUE_ON;"),
    ("cerbo_shown_counts_every_kind", T, F, TEXT_SWITCH, "\treturn true;"),
    ("cerbo_shown_switch_needs_a_finite_number", T, F, TEXT_SWITCH, "\treturn (item->kind == VALUE_ON || item->kind == VALUE_OFF) && isfinite(item->number);"),
]


# The names of the keys the source keeps for the editor of the web interface (stage 4, 2026-10-12). A mutation
# here keeps name_count within the names: one that let it run on would write behind them, and a crash names
# no rule.
N_EACH = "\tfor(int i = 0; i < cerbo->values.count; i++)\n\t{\n\t\tconst value_t *item = &cerbo->values.items[i];\n\t\tint tried = 0;\n"
N_TEXT = "\t\tif(!has_text(item)) continue;\n"
N_SEARCH = "\t\twhile(tried < cerbo->name_count && strcmp(cerbo->names[(i + tried) % cerbo->name_count], item->name) != 0) tried++;\n"
N_KNOWN = "\t\tif(tried < cerbo->name_count) continue;\n"
N_FULL = "\t\tif(cerbo->name_count == CERBO_NAMES_MAX)\n"
N_OLD = "\t\t\twhile(old < last && find(&cerbo->values, *old) != NULL) old++;\n"
N_MOVE = "\t\t\tmemmove(old, old + 1, (size_t)(last - old) * sizeof(*old));\n"
N_COPY = "\t\tstrcpy(cerbo->names[cerbo->name_count], item->name);\n\t\tcerbo->name_count++;\n"
N_CALL = "\tnote_names(cerbo);\n"
N_FORGET = "\tcerbo->name_count = 0;\n\tif(cerbo->phase != CERBO_IDLE) cerbo->moved = true;\n"
N_NONE = "\tcerbo->name_count = 0;\n"

MUTATIONS += [
    # which keys are noted
    ("cerbo_names_never_noted", T, F, N_CALL, "\t(void)note_names;\n"),
    ("cerbo_names_noted_before_the_store_is_filled", T, F, SEEN + "\n" + N_CALL, N_CALL + SEEN + "\n"),
    ("cerbo_names_of_numbers_without_end_noted", T, F, N_TEXT, ""),
    ("cerbo_names_only_of_numbers_without_end", T, F, N_TEXT, "\t\tif(has_text(item)) continue;\n"),
    ("cerbo_names_only_of_numbers", T, F, N_TEXT, "\t\tif(!has_text(item) || item->kind != VALUE_NUMBER) continue;\n"),
    ("cerbo_names_only_of_switches", T, F, N_TEXT, "\t\tif(!has_text(item) || item->kind == VALUE_NUMBER) continue;\n"),
    ("cerbo_names_first_key_not_noted", T, F, N_EACH, N_EACH.replace("int i = 0;", "int i = 1;")),
    ("cerbo_names_last_key_not_noted", T, F, N_EACH, N_EACH.replace("i < cerbo->values.count;", "i < cerbo->values.count - 1;")),
    ("cerbo_names_only_the_first_key_noted", T, F, N_EACH, N_EACH.replace("i < cerbo->values.count;", "i < cerbo->values.count && i < 1;")),
    ("cerbo_names_forgotten_with_every_bundle", T, F, N_EACH, N_NONE + N_EACH),
    ("cerbo_names_known_ones_noted_again", T, F, N_KNOWN, ""),
    ("cerbo_names_none_is_new", T, F, N_KNOWN, "\t\tif(tried <= cerbo->name_count) continue;\n"),
    ("cerbo_names_only_the_first_one_compared", T, F, N_SEARCH, N_SEARCH.replace("cerbo->names[(i + tried) % cerbo->name_count]", "cerbo->names[0]")),
    ("cerbo_names_only_the_one_at_the_place_of_the_key_compared", T, F, N_SEARCH, N_SEARCH.replace("(i + tried) % cerbo->name_count", "i % cerbo->name_count")),
    ("cerbo_names_last_one_not_compared", T, F, N_SEARCH, N_SEARCH.replace("while(tried < cerbo->name_count &&", "while(tried < cerbo->name_count - 1 &&").replace("\n", "\n\t\tif(tried == cerbo->name_count - 1) tried++;\n")),
    ("cerbo_names_compared_in_their_first_five_bytes", T, F, N_SEARCH, N_SEARCH.replace("strcmp(cerbo->names[(i + tried) % cerbo->name_count], item->name) != 0", "strncmp(cerbo->names[(i + tried) % cerbo->name_count], item->name, 5) != 0")),
    ("cerbo_names_new_one_not_copied", T, F, N_COPY, "\t\tcerbo->name_count++;\n"),
    ("cerbo_names_new_one_not_counted", T, F, N_COPY, "\t\tstrcpy(cerbo->names[cerbo->name_count], item->name);\n"),
    ("cerbo_names_new_one_in_the_first_place", T, F, N_COPY,
     "\t\tfor(int moved = cerbo->name_count; moved > 0; moved--) strcpy(cerbo->names[moved], cerbo->names[moved - 1]);\n\t\tstrcpy(cerbo->names[0], item->name);\n\t\tcerbo->name_count++;\n"),
    ("cerbo_names_new_one_cut_at_31_bytes", T, F, N_COPY, N_COPY.replace("strcpy(cerbo->names[cerbo->name_count], item->name);", "strcpy(cerbo->names[cerbo->name_count], item->name);\n\t\tcerbo->names[cerbo->name_count][VALUE_NAME_SIZE - 2] = '\\0';")),

    # the room
    ("cerbo_names_room_for_63", T, F, N_FULL, "\t\tif(cerbo->name_count >= CERBO_NAMES_MAX - 1)\n"),
    ("cerbo_names_no_room_no_name", T, F, N_FULL, "\t\tif(cerbo->name_count == CERBO_NAMES_MAX) continue;\n\t\tif(cerbo->name_count > CERBO_NAMES_MAX)\n"),
    ("cerbo_names_no_room_all_forgotten", T, F, N_FULL, "\t\tif(cerbo->name_count == CERBO_NAMES_MAX) cerbo->name_count = 0;\n\t\tif(cerbo->name_count > CERBO_NAMES_MAX)\n"),
    ("cerbo_names_the_first_gives_way", T, F, N_OLD, ""),
    ("cerbo_names_the_last_gives_way", T, F, N_OLD, "\t\t\twhile(old < last) old++;\n"),
    ("cerbo_names_the_oldest_of_the_bundle_gives_way", T, F, N_OLD, "\t\t\twhile(old < last && find(&cerbo->values, *old) == NULL) old++;\n"),
    ("cerbo_names_the_second_without_a_value_gives_way", T, F, N_OLD, N_OLD + "\t\t\tif(old < last) old++;\n" + N_OLD.replace("\t\t\twhile", "\t\t\twhile")),
    ("cerbo_names_a_key_without_text_gives_way", T, F, N_OLD,
     "\t\t\twhile(old < last && find(&cerbo->values, *old) != NULL && has_text(find(&cerbo->values, *old))) old++;\n"),
    ("cerbo_names_do_not_move_up", T, F, N_MOVE, ""),
    ("cerbo_names_one_too_few_moves_up", T, F, N_MOVE, "\t\t\tmemmove(old, old + 1, (size_t)(last - old > 0 ? last - old - 1 : 0) * sizeof(*old));\n"),
    ("cerbo_names_move_up_from_the_first", T, F, N_MOVE, "\t\t\tmemmove(cerbo->names, cerbo->names + 1, (size_t)(last - cerbo->names) * sizeof(*old));\n\t\t\t(void)old;\n"),

    # what forgets them
    ("cerbo_new_address_keeps_the_names", T, F, N_FORGET, N_FORGET.replace(N_NONE, "")),
    ("cerbo_new_address_forgets_one_name", T, F, N_FORGET, N_FORGET.replace(N_NONE, "\tif(cerbo->name_count > 0) cerbo->name_count--;\n")),
    ("cerbo_same_address_forgets_the_names", T, F, SAME_HOST, N_NONE + SAME_HOST),
    ("cerbo_names_only_forgotten_without_an_address", T, F, N_FORGET, N_FORGET.replace(N_NONE, "\tif(cerbo->host[0] == '\\0') cerbo->name_count = 0;\n")),
    ("cerbo_names_only_forgotten_with_an_address", T, F, N_FORGET, N_FORGET.replace(N_NONE, "\tif(cerbo->host[0] != '\\0') cerbo->name_count = 0;\n")),
    ("cerbo_names_forgotten_when_the_connection_ends", T, F, DOWN, N_NONE + DOWN),
    ("cerbo_names_forgotten_without_a_network", T, F, SAME_NETWORK, SAME_NETWORK + N_NONE),
]
