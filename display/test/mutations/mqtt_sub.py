"""Mutations of display/components/core/mqtt_sub.c, see ../redproof.py.

A mutation here must leave the reader inside its room: where a rule keeps bytes from being written behind it,
the mutation writes them to a wrong place inside instead of taking the limit away. A test that only crashes
names no rule.
"""

F = "components/core/mqtt_sub.c"
H = "components/core/mqtt_sub.h"
T = "test_mqtt_sub"

ID_CHARACTER = "\treturn (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');"
CONNECT_GUARD = "\tif(out == NULL || name == NULL) return 0;\n"
ID_LOOP = "\tfor(; *name != '\\0' && count < MQTT_SUB_ID_MAX; name++)"
ID_TAKEN = "\t\tif(id_character(*name)) id[count++] = (uint8_t)*name;"
CONNECT_FITS = "\tif(count == 0 || CONNECT_HEAD + count > size) return 0;"
SUBSCRIBE_GUARD = "\tif(out == NULL || topic == NULL || id == 0) return 0;\n"
TOPIC_LIMIT = "\t\tif(length >= MQTT_SUB_OUT_MAX - SUBSCRIBE_AROUND) return 0;\n"
PATTERN = "\t\tif(topic[length] == '#' || topic[length] == '+') return 0;\n"
SUBSCRIBE_FITS = "\tif(length == 0 || SUBSCRIBE_AROUND + length > size) return 0;"
PING_GUARD = "\tif(out == NULL || size < 2) return 0;"
KNOWN = "\tif(first == FIRST_CONNACK || first == FIRST_SUBACK || first == FIRST_PINGRESP) return true;"
KNOWN_PUBLISH = "\treturn (first & ~PUBLISH_RETAIN) == FIRST_PUBLISH;"
FITS_CONNACK = "\tif(first == FIRST_CONNACK) return remaining == 2;"
FITS_SUBACK = "\tif(first == FIRST_SUBACK) return remaining == 3;"
FITS_PINGRESP = "\tif(first == FIRST_PINGRESP) return remaining == 0;"
FITS_PUBLISH = "\treturn remaining >= 3;"
SESSION_PRESENT = "\t\tif(room[0] != 0) return broken(reader);"
SUBACK_ID = "\t\tpacket->id = (uint16_t)(((uint32_t)room[0] << 8) | room[1]);"
TOPIC_LENGTH = "\ttopic_length = ((uint32_t)room[0] << 8) | room[1];"
TOPIC_FITS = "\tif(topic_length == 0 || topic_length > reader->remaining - 2) return broken(reader);"
RETAINED = "\tpacket->retained = (reader->first & PUBLISH_RETAIN) != 0;"
DEAD = "\tif(reader->state == READ_BROKEN) return MQTT_SUB_BROKEN;\n"
TAKE = "\t\t\tsize_t take = *length < missing ? *length : missing;"
KEEP = "\t\t\tif(reader->state == READ_BODY) memcpy(reader->room + reader->have, *data, take);"
GONE_BY = "\t\t\tif(reader->state == READ_SKIP) return hand_out(reader, packet, MQTT_SUB_SKIPPED);\n"
FIRST = "\t\t\tif(!first_known(byte)) return broken(reader);\n"
DIGIT = "\t\treader->remaining += (uint32_t)(byte & 0x7Fu) * reader->scale;"
FIFTH = "\t\t\tif(reader->scale == LENGTH_LAST_SCALE) return broken(reader);\n"
JUDGED = "\t\tif(!length_fits(reader->first, reader->remaining)) return broken(reader);\n"
EMPTY = "\t\tif(reader->remaining == 0) return complete(reader, packet);\n"
ROOM = "\t\treader->state = reader->remaining > MQTT_SUB_PACKET_MAX ? READ_SKIP : READ_BODY;"

MUTATIONS = [
    # the limits
    ("mqtt_room_one_byte_smaller", T, H, "#define MQTT_SUB_PACKET_MAX     1536", "#define MQTT_SUB_PACKET_MAX     1535"),
    ("mqtt_room_one_byte_larger", T, H, "#define MQTT_SUB_PACKET_MAX     1536", "#define MQTT_SUB_PACKET_MAX     1537"),
    ("mqtt_built_packets_may_be_longer", T, H, "#define MQTT_SUB_OUT_MAX        64", "#define MQTT_SUB_OUT_MAX        65"),
    ("mqtt_client_id_of_24", T, H, "#define MQTT_SUB_ID_MAX         23", "#define MQTT_SUB_ID_MAX         24"),

    # CONNECT
    ("mqtt_connect_without_room_says_built", T, F, CONNECT_GUARD, "\tif(name == NULL) return 0;\n\tif(out == NULL) return CONNECT_HEAD;\n"),
    ("mqtt_connect_without_name_built", T, F, CONNECT_GUARD, "\tif(out == NULL) return 0;\n\tif(name == NULL) name = \"x\";\n"),
    ("mqtt_id_takes_every_character", T, F, ID_CHARACTER, "\t(void)c;\n\treturn true;"),
    ("mqtt_id_without_digits", T, F, ID_CHARACTER, ID_CHARACTER.replace("(c >= '0' && c <= '9') || ", "")),
    ("mqtt_id_without_small_letters", T, F, ID_CHARACTER, ID_CHARACTER.replace("(c >= 'a' && c <= 'z') || ", "")),
    ("mqtt_id_without_capitals", T, F, ID_CHARACTER, ID_CHARACTER.replace(" || (c >= 'A' && c <= 'Z')", "")),
    ("mqtt_id_takes_slash", T, F, ID_CHARACTER, ID_CHARACTER.replace("c >= '0'", "c >= '/'")),
    ("mqtt_id_takes_colon", T, F, ID_CHARACTER, ID_CHARACTER.replace("c <= '9'", "c <= ':'")),
    ("mqtt_id_takes_backtick", T, F, ID_CHARACTER, ID_CHARACTER.replace("c >= 'a'", "c >= '`'")),
    ("mqtt_id_takes_brace", T, F, ID_CHARACTER, ID_CHARACTER.replace("c <= 'z'", "c <= '{'")),
    ("mqtt_id_takes_at", T, F, ID_CHARACTER, ID_CHARACTER.replace("c >= 'A'", "c >= '@'")),
    ("mqtt_id_takes_bracket", T, F, ID_CHARACTER, ID_CHARACTER.replace("c <= 'Z'", "c <= '['")),
    ("mqtt_id_loses_zero", T, F, ID_CHARACTER, ID_CHARACTER.replace("c >= '0'", "c > '0'")),
    ("mqtt_id_loses_nine", T, F, ID_CHARACTER, ID_CHARACTER.replace("c <= '9'", "c < '9'")),
    ("mqtt_id_loses_small_a", T, F, ID_CHARACTER, ID_CHARACTER.replace("c >= 'a'", "c > 'a'")),
    ("mqtt_id_loses_small_z", T, F, ID_CHARACTER, ID_CHARACTER.replace("c <= 'z'", "c < 'z'")),
    ("mqtt_id_loses_capital_a", T, F, ID_CHARACTER, ID_CHARACTER.replace("c >= 'A'", "c > 'A'")),
    ("mqtt_id_loses_capital_z", T, F, ID_CHARACTER, ID_CHARACTER.replace("c <= 'Z'", "c < 'Z'")),
    ("mqtt_id_of_22", T, F, ID_LOOP, ID_LOOP.replace("count < MQTT_SUB_ID_MAX", "count < MQTT_SUB_ID_MAX - 1")),
    ("mqtt_id_limit_counts_bytes_of_the_name", T, F,
     ID_LOOP + "\n\t{\n" + ID_TAKEN,
     "\tfor(size_t seen = 0; *name != '\\0' && seen < MQTT_SUB_ID_MAX; name++, seen++)\n\t{\n" + ID_TAKEN),
    ("mqtt_connect_with_empty_id", T, F, CONNECT_FITS, CONNECT_FITS.replace("count == 0 || ", "")),
    ("mqtt_connect_built_without_room", T, F, CONNECT_FITS, CONNECT_FITS.replace("count > size", "count > size + MQTT_SUB_OUT_MAX")),
    ("mqtt_connect_exact_fit_refused", T, F, CONNECT_FITS, CONNECT_FITS.replace("count > size", "count >= size")),
    ("mqtt_connect_one_byte_over", T, F, CONNECT_FITS, CONNECT_FITS.replace("count > size", "count > size + 1")),
    ("mqtt_connect_is_a_publish", T, F, "#define FIRST_CONNECT       0x10u", "#define FIRST_CONNECT       0x30u"),
    ("mqtt_connect_with_flags_in_first_byte", T, F, "#define FIRST_CONNECT       0x10u", "#define FIRST_CONNECT       0x12u"),
    ("mqtt_connect_length_wrong", T, F, "\tout[1] = (uint8_t)(CONNECT_HEAD - 2 + count);", "\tout[1] = (uint8_t)(CONNECT_HEAD + count);"),
    ("mqtt_connect_protocol_name_length", T, F, "\tout[2] = 0;\n\tout[3] = 4;", "\tout[2] = 0;\n\tout[3] = 6;"),
    ("mqtt_connect_protocol_name", T, F, "\tout[4] = 'M';", "\tout[4] = 'm';"),
    ("mqtt_connect_protocol_name_last", T, F, "\tout[7] = 'T';", "\tout[7] = 't';"),
    ("mqtt_connect_level_3", T, F, "\tout[8] = 4;", "\tout[8] = 3;"),
    ("mqtt_connect_level_5", T, F, "\tout[8] = 4;", "\tout[8] = 5;"),
    ("mqtt_connect_keeps_session", T, F, "#define CLEAN_SESSION       0x02u", "#define CLEAN_SESSION       0x00u"),
    ("mqtt_connect_with_will", T, F, "#define CLEAN_SESSION       0x02u", "#define CLEAN_SESSION       0x06u"),
    ("mqtt_connect_with_will_retain", T, F, "#define CLEAN_SESSION       0x02u", "#define CLEAN_SESSION       0x26u"),
    ("mqtt_connect_with_user_name", T, F, "#define CLEAN_SESSION       0x02u", "#define CLEAN_SESSION       0x82u"),
    ("mqtt_connect_with_password", T, F, "#define CLEAN_SESSION       0x02u", "#define CLEAN_SESSION       0x42u"),
    ("mqtt_connect_reserved_flag", T, F, "#define CLEAN_SESSION       0x02u", "#define CLEAN_SESSION       0x03u"),
    ("mqtt_keepalive_low_byte_first", T, F,
     "\tout[10] = (uint8_t)(keepalive_s >> 8);\n\tout[11] = (uint8_t)(keepalive_s & 0xFFu);",
     "\tout[11] = (uint8_t)(keepalive_s >> 8);\n\tout[10] = (uint8_t)(keepalive_s & 0xFFu);"),
    ("mqtt_keepalive_high_byte_lost", T, F, "\tout[10] = (uint8_t)(keepalive_s >> 8);", "\tout[10] = 0;"),
    ("mqtt_keepalive_low_byte_lost", T, F, "\tout[11] = (uint8_t)(keepalive_s & 0xFFu);", "\tout[11] = 30;"),
    ("mqtt_connect_id_length_wrong", T, F, "\tout[13] = (uint8_t)count;", "\tout[13] = (uint8_t)(count + 1);"),
    # Not simply taken away: gcc refuses a source whose id is filled and never used (seen in the CI on 2026-10-11)
    ("mqtt_connect_id_not_written", T, F, "\tmemcpy(out + CONNECT_HEAD, id, count);\n", "\t(void)id;\n"),
    ("mqtt_connect_returns_wrong_length", T, F, "\treturn CONNECT_HEAD + count;", "\treturn CONNECT_HEAD + count - 1;"),

    # SUBSCRIBE
    ("mqtt_subscribe_without_room_says_built", T, F, SUBSCRIBE_GUARD, "\tif(topic == NULL || id == 0) return 0;\n\tif(out == NULL) return SUBSCRIBE_AROUND;\n"),
    ("mqtt_subscribe_without_topic_built", T, F, SUBSCRIBE_GUARD, "\tif(out == NULL || id == 0) return 0;\n\tif(topic == NULL) topic = \"x\";\n"),
    ("mqtt_subscribe_with_identifier_0", T, F, SUBSCRIBE_GUARD, SUBSCRIBE_GUARD.replace(" || id == 0", "")),
    ("mqtt_subscribe_topic_of_58", T, F, TOPIC_LIMIT, TOPIC_LIMIT.replace("length >= ", "length > ")),
    ("mqtt_subscribe_topic_limit_off", T, F, TOPIC_LIMIT, ""),
    ("mqtt_subscribe_topic_of_57_refused", T, F, TOPIC_LIMIT, TOPIC_LIMIT.replace("length >= ", "length + 1 >= ")),
    ("mqtt_subscribe_to_hash", T, F, PATTERN, PATTERN.replace("topic[length] == '#' || ", "")),
    ("mqtt_subscribe_to_plus", T, F, PATTERN, PATTERN.replace(" || topic[length] == '+'", "")),
    ("mqtt_subscribe_to_empty_topic", T, F, SUBSCRIBE_FITS, SUBSCRIBE_FITS.replace("length == 0 || ", "")),
    ("mqtt_subscribe_built_without_room", T, F, SUBSCRIBE_FITS, SUBSCRIBE_FITS.replace("length > size", "length > size + MQTT_SUB_OUT_MAX")),
    ("mqtt_subscribe_exact_fit_refused", T, F, SUBSCRIBE_FITS, SUBSCRIBE_FITS.replace("length > size", "length >= size")),
    ("mqtt_subscribe_one_byte_over", T, F, SUBSCRIBE_FITS, SUBSCRIBE_FITS.replace("length > size", "length > size + 1")),
    ("mqtt_subscribe_is_an_unsubscribe", T, F, "#define FIRST_SUBSCRIBE     0x82u", "#define FIRST_SUBSCRIBE     0xA2u"),
    ("mqtt_subscribe_is_a_publish", T, F, "#define FIRST_SUBSCRIBE     0x82u", "#define FIRST_SUBSCRIBE     0x32u"),
    ("mqtt_subscribe_without_its_flag", T, F, "#define FIRST_SUBSCRIBE     0x82u", "#define FIRST_SUBSCRIBE     0x80u"),
    ("mqtt_subscribe_length_wrong", T, F, "\tout[1] = (uint8_t)(SUBSCRIBE_AROUND - 2 + length);", "\tout[1] = (uint8_t)(SUBSCRIBE_AROUND + length);"),
    ("mqtt_identifier_low_byte_first", T, F,
     "\tout[2] = (uint8_t)(id >> 8);\n\tout[3] = (uint8_t)(id & 0xFFu);", "\tout[3] = (uint8_t)(id >> 8);\n\tout[2] = (uint8_t)(id & 0xFFu);"),
    ("mqtt_identifier_high_byte_lost", T, F, "\tout[2] = (uint8_t)(id >> 8);", "\tout[2] = 0;"),
    ("mqtt_identifier_low_byte_lost", T, F, "\tout[3] = (uint8_t)(id & 0xFFu);", "\tout[3] = 1;"),
    ("mqtt_subscribe_topic_length_wrong", T, F, "\tout[5] = (uint8_t)length;", "\tout[5] = (uint8_t)(length - 1);"),
    ("mqtt_subscribe_topic_not_written", T, F, "\tmemcpy(out + 6, topic, length);\n", ""),
    ("mqtt_subscribe_asks_for_qos_1", T, F, "\tout[6 + length] = 0;", "\tout[6 + length] = 1;"),
    ("mqtt_subscribe_asks_for_qos_2", T, F, "\tout[6 + length] = 0;", "\tout[6 + length] = 2;"),
    ("mqtt_subscribe_returns_wrong_length", T, F, "\treturn SUBSCRIBE_AROUND + length;", "\treturn SUBSCRIBE_AROUND + length - 1;"),

    # PINGREQ
    ("mqtt_ping_without_room_says_built", T, F, PING_GUARD, "\tif(out == NULL) return 2;\n\tif(size < 2) return 0;"),
    ("mqtt_ping_built_into_one_byte", T, F, PING_GUARD, PING_GUARD.replace("size < 2", "size < 1")),
    ("mqtt_ping_exact_fit_refused", T, F, PING_GUARD, PING_GUARD.replace("size < 2", "size <= 2")),
    ("mqtt_ping_is_a_disconnect", T, F, "#define FIRST_PINGREQ       0xC0u", "#define FIRST_PINGREQ       0xE0u"),
    ("mqtt_ping_is_a_publish", T, F, "#define FIRST_PINGREQ       0xC0u", "#define FIRST_PINGREQ       0x30u"),
    ("mqtt_ping_with_a_length", T, F, "\tout[0] = FIRST_PINGREQ;\n\tout[1] = 0;", "\tout[0] = FIRST_PINGREQ;\n\tout[1] = 1;"),

    # nothing else is built
    ("mqtt_fourth_builder", T, H,
     "size_t mqtt_sub_ping(uint8_t *out, size_t size);\n",
     "size_t mqtt_sub_ping(uint8_t *out, size_t size);\nsize_t mqtt_sub_publish(uint8_t *out, size_t size, const char *topic, const char *text);\n"),

    # the reader: a new connection, and broken
    ("mqtt_begin_does_not_begin", T, F, "\treader->state = READ_FIRST;\n}\n\nstatic mqtt_sub_kind_t broken", "\t(void)reader;\n}\n\nstatic mqtt_sub_kind_t broken"),
    ("mqtt_broken_is_forgotten", T, F, "\treader->state = READ_BROKEN;\n\treturn MQTT_SUB_BROKEN;", "\treader->state = READ_FIRST;\n\treturn MQTT_SUB_BROKEN;"),
    ("mqtt_broken_reader_reads_on", T, F, DEAD, ""),
    ("mqtt_broken_reader_says_none", T, F, DEAD, "\tif(reader->state == READ_BROKEN) return MQTT_SUB_NONE;\n"),

    # the first byte
    ("mqtt_first_byte_not_judged", T, F, FIRST, "\t\t\t(void)first_known(byte);\n"),
    ("mqtt_connack_unknown", T, F, KNOWN, KNOWN.replace("first == FIRST_CONNACK || ", "")),
    ("mqtt_suback_unknown", T, F, KNOWN, KNOWN.replace("first == FIRST_SUBACK || ", "")),
    ("mqtt_pingresp_unknown", T, F, KNOWN, KNOWN.replace(" || first == FIRST_PINGRESP", "")),
    ("mqtt_publish_unknown", T, F, KNOWN_PUBLISH, "\treturn false;"),
    ("mqtt_retained_publish_unknown", T, F, KNOWN_PUBLISH, "\treturn first == FIRST_PUBLISH;"),
    ("mqtt_publish_qos_1_taken", T, F, KNOWN_PUBLISH, "\treturn (first & ~(PUBLISH_RETAIN | 0x02u)) == FIRST_PUBLISH;"),
    ("mqtt_publish_qos_2_taken", T, F, KNOWN_PUBLISH, "\treturn (first & ~(PUBLISH_RETAIN | 0x04u)) == FIRST_PUBLISH;"),
    ("mqtt_publish_dup_taken", T, F, KNOWN_PUBLISH, "\treturn (first & ~(PUBLISH_RETAIN | 0x08u)) == FIRST_PUBLISH;"),
    ("mqtt_connack_flags_in_first_byte_taken", T, F, KNOWN, KNOWN.replace("first == FIRST_CONNACK", "(first & 0xF0u) == FIRST_CONNACK")),
    ("mqtt_suback_flags_in_first_byte_taken", T, F, KNOWN, KNOWN.replace("first == FIRST_SUBACK", "(first & 0xF0u) == FIRST_SUBACK")),
    ("mqtt_pingresp_flags_in_first_byte_taken", T, F, KNOWN, KNOWN.replace("first == FIRST_PINGRESP", "(first & 0xF0u) == FIRST_PINGRESP")),
    ("mqtt_puback_taken", T, F, KNOWN, KNOWN.replace("first == FIRST_CONNACK", "first == 0x40u || first == FIRST_CONNACK")),
    ("mqtt_unsuback_taken", T, F, KNOWN, KNOWN.replace("first == FIRST_CONNACK", "first == 0xB0u || first == FIRST_CONNACK")),
    ("mqtt_connack_is_another_byte", T, F, "#define FIRST_CONNACK       0x20u", "#define FIRST_CONNACK       0x10u"),
    ("mqtt_suback_is_another_byte", T, F, "#define FIRST_SUBACK        0x90u", "#define FIRST_SUBACK        0xB0u"),
    ("mqtt_pingresp_is_another_byte", T, F, "#define FIRST_PINGRESP      0xD0u", "#define FIRST_PINGRESP      0xC0u"),
    ("mqtt_publish_is_another_byte", T, F, "#define FIRST_PUBLISH       0x30u", "#define FIRST_PUBLISH       0x40u"),

    # the Remaining Length
    ("mqtt_length_bit_7_counts", T, F, DIGIT, DIGIT.replace("(byte & 0x7Fu)", "byte")),
    ("mqtt_length_second_byte_counts_256", T, F, "\t\t\treader->scale *= 128u;", "\t\t\treader->scale *= 256u;"),
    ("mqtt_length_bytes_all_count_the_same", T, F, "\t\t\treader->scale *= 128u;\n", ""),
    ("mqtt_length_of_five_bytes_taken", T, F, FIFTH, ""),
    ("mqtt_length_of_four_bytes_refused", T, F, "#define LENGTH_LAST_SCALE   (128u * 128u * 128u)", "#define LENGTH_LAST_SCALE   (128u * 128u)"),
    ("mqtt_length_goes_on_from_the_last_packet", T, F, "\t\t\treader->remaining = 0;\n", ""),
    ("mqtt_length_scale_from_the_last_packet", T, F, "\t\t\treader->scale = 1;\n", ""),
    ("mqtt_length_ends_with_bit_7_set", T, F, "\t\tif((byte & 0x80u) != 0)", "\t\tif((byte & 0x80u) == 0)"),

    # the length of each kind
    ("mqtt_length_not_judged", T, F, JUDGED, "\t\t(void)length_fits(reader->first, reader->remaining);\n"),
    ("mqtt_connack_may_be_longer", T, F, FITS_CONNACK, FITS_CONNACK.replace("remaining == 2", "remaining >= 2")),
    ("mqtt_connack_may_be_shorter", T, F, FITS_CONNACK, FITS_CONNACK.replace("remaining == 2", "remaining <= 2")),
    ("mqtt_suback_may_be_longer", T, F, FITS_SUBACK, FITS_SUBACK.replace("remaining == 3", "remaining >= 3")),
    ("mqtt_suback_may_be_shorter", T, F, FITS_SUBACK, FITS_SUBACK.replace("remaining == 3", "remaining <= 3")),
    ("mqtt_pingresp_may_have_a_body", T, F, FITS_PINGRESP, FITS_PINGRESP.replace("remaining == 0", "remaining <= 1")),
    ("mqtt_pingresp_may_be_long", T, F, FITS_PINGRESP, FITS_PINGRESP.replace("remaining == 0", "remaining != 1")),
    ("mqtt_publish_of_2_bytes_taken", T, F, FITS_PUBLISH, "\treturn remaining >= 2;"),
    ("mqtt_publish_of_3_bytes_refused", T, F, FITS_PUBLISH, "\treturn remaining > 3;"),
    ("mqtt_publish_of_any_length_taken", T, F, FITS_PUBLISH, "\t(void)remaining;\n\treturn true;"),
    ("mqtt_empty_packet_waits_for_a_byte", T, F, EMPTY, ""),

    # too long for the room
    ("mqtt_publish_of_1536_goes_by", T, F, ROOM, ROOM.replace("remaining > MQTT_SUB_PACKET_MAX", "remaining >= MQTT_SUB_PACKET_MAX")),
    ("mqtt_long_publish_is_broken", T, F, ROOM,
     "\t\tif(reader->remaining > MQTT_SUB_PACKET_MAX) return broken(reader);\n\t\treader->state = READ_BODY;"),
    ("mqtt_bytes_that_go_by_are_kept", T, F, KEEP,
     "\t\t\tmemcpy(reader->room + (reader->state == READ_BODY ? reader->have : 0), *data, (reader->state == READ_BODY || take < MQTT_SUB_PACKET_MAX) ? take : MQTT_SUB_PACKET_MAX);"),
    ("mqtt_gone_by_is_told_as_packet", T, F, GONE_BY, ""),
    ("mqtt_gone_by_is_told_as_none", T, F, GONE_BY,
     "\t\t\tif(reader->state == READ_SKIP)\n\t\t\t{\n\t\t\t\treader->state = READ_FIRST;\n\t\t\t\tcontinue;\n\t\t\t}\n"),
    ("mqtt_gone_by_is_told_as_broken", T, F, GONE_BY, "\t\t\tif(reader->state == READ_SKIP) return broken(reader);\n"),

    # the bytes of a packet
    ("mqtt_takes_a_byte_of_the_next_packet", T, F, TAKE, "\t\t\tsize_t take = *length <= missing ? *length : missing + 1;"),
    ("mqtt_body_not_kept", T, F, KEEP, "\t\t\t(void)0;"),
    ("mqtt_first_byte_of_body_lost", T, F, "\t\t\treader->have = 0;", "\t\t\treader->have = 1;"),
    ("mqtt_data_not_moved_on", T, F, "\t\t\t*data += take;\n", ""),
    ("mqtt_data_not_moved_past_header", T, F, "\t\t*data += 1;\n", ""),
    ("mqtt_packet_handed_out_twice", T, F, "\treader->state = READ_FIRST;\n\tpacket->code = 0;", "\t(void)reader;\n\tpacket->code = 0;"),

    # what a packet carries
    ("mqtt_code_of_the_packet_before", T, F, "\tpacket->code = 0;\n", ""),
    ("mqtt_identifier_of_the_packet_before", T, F, "\tpacket->id = 0;\n", ""),
    ("mqtt_retained_of_the_packet_before", T, F, "\tpacket->retained = false;\n", ""),
    ("mqtt_topic_of_the_packet_before", T, F, "\tpacket->topic = NULL;\n", ""),
    ("mqtt_topic_length_of_the_packet_before", T, F, "\tpacket->topic_length = 0;\n", ""),
    ("mqtt_payload_of_the_packet_before", T, F, "\tpacket->payload = NULL;\n", ""),
    ("mqtt_payload_length_of_the_packet_before", T, F, "\tpacket->payload_length = 0;\n", ""),
    ("mqtt_pingresp_told_as_connack", T, F, "return hand_out(reader, packet, MQTT_SUB_PINGRESP);", "return hand_out(reader, packet, MQTT_SUB_CONNACK);"),
    ("mqtt_connack_with_kept_session_taken", T, F, SESSION_PRESENT, "\t\tif((room[0] & 0xFEu) != 0) return broken(reader);"),
    ("mqtt_connack_reserved_flags_taken", T, F, SESSION_PRESENT, "\t\tif((room[0] & 0x01u) != 0) return broken(reader);"),
    ("mqtt_connack_flags_not_judged", T, F, SESSION_PRESENT + "\n", ""),
    ("mqtt_connack_written_before_judged", T, F,
     SESSION_PRESENT + "\n\t\thand_out(reader, packet, MQTT_SUB_CONNACK);",
     "\t\thand_out(reader, packet, MQTT_SUB_CONNACK);\n" + SESSION_PRESENT),
    ("mqtt_connack_code_is_flags", T, F, "\t\tpacket->code = room[1];", "\t\tpacket->code = room[0];"),
    ("mqtt_connack_told_as_suback", T, F, "\t\tpacket->code = room[1];\n\t\treturn MQTT_SUB_CONNACK;", "\t\tpacket->code = room[1];\n\t\treturn MQTT_SUB_SUBACK;"),
    ("mqtt_suback_identifier_low_byte_first", T, F, SUBACK_ID, "\t\tpacket->id = (uint16_t)(((uint32_t)room[1] << 8) | room[0]);"),
    ("mqtt_suback_identifier_high_byte_lost", T, F, SUBACK_ID, "\t\tpacket->id = room[1];"),
    ("mqtt_suback_identifier_low_byte_lost", T, F, SUBACK_ID, "\t\tpacket->id = (uint16_t)((uint32_t)room[0] << 8);"),
    ("mqtt_suback_code_is_identifier", T, F, "\t\tpacket->code = room[2];", "\t\tpacket->code = room[1];"),
    ("mqtt_suback_told_as_connack", T, F, "\t\tpacket->code = room[2];\n\t\treturn MQTT_SUB_SUBACK;", "\t\tpacket->code = room[2];\n\t\treturn MQTT_SUB_CONNACK;"),
    ("mqtt_topic_length_low_byte_only", T, F, TOPIC_LENGTH, "\ttopic_length = room[1];"),
    ("mqtt_topic_length_low_byte_first", T, F, TOPIC_LENGTH, "\ttopic_length = ((uint32_t)room[1] << 8) | room[0];"),
    ("mqtt_empty_topic_taken", T, F, TOPIC_FITS, TOPIC_FITS.replace("topic_length == 0 || ", "")),
    ("mqtt_topic_beyond_packet_taken", T, F, TOPIC_FITS, TOPIC_FITS.replace(" || topic_length > reader->remaining - 2", "")),
    ("mqtt_topic_one_byte_beyond_taken", T, F, TOPIC_FITS, TOPIC_FITS.replace("reader->remaining - 2", "reader->remaining - 1")),
    ("mqtt_topic_to_the_end_refused", T, F, TOPIC_FITS, TOPIC_FITS.replace("topic_length > reader->remaining - 2", "topic_length >= reader->remaining - 2")),
    ("mqtt_publish_written_before_judged", T, F,
     TOPIC_FITS + "\n\n\thand_out(reader, packet, MQTT_SUB_PUBLISH);",
     "\thand_out(reader, packet, MQTT_SUB_PUBLISH);\n" + TOPIC_FITS + "\n"),
    ("mqtt_retained_never", T, F, RETAINED, "\tpacket->retained = false;"),
    ("mqtt_retained_always", T, F, RETAINED, "\tpacket->retained = true;"),
    ("mqtt_retained_is_another_bit", T, F, "#define PUBLISH_RETAIN      0x01u", "#define PUBLISH_RETAIN      0x02u"),
    ("mqtt_topic_begins_at_its_length", T, F, "\tpacket->topic = room + 2;", "\tpacket->topic = room;"),
    ("mqtt_topic_length_wrong", T, F, "\tpacket->topic_length = topic_length;", "\tpacket->topic_length = topic_length - 1;"),
    ("mqtt_payload_begins_in_the_topic", T, F, "\tpacket->payload = room + 2 + topic_length;", "\tpacket->payload = room + 2;"),
    ("mqtt_payload_length_with_the_topic", T, F, "\tpacket->payload_length = reader->remaining - 2 - topic_length;", "\tpacket->payload_length = reader->remaining - 2;"),
    ("mqtt_payload_one_byte_short", T, F, "\tpacket->payload_length = reader->remaining - 2 - topic_length;", "\tpacket->payload_length = reader->remaining - 3 - topic_length;"),
]
