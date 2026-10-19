import contextlib
import io
import json
import os
import shutil
import subprocess
import tempfile
import unittest

import check_build

LVGL = "/project/display/managed_components/lvgl__lvgl/src/"
WANTED = "LV_ASSERT_HANDLER=if((abort(),0)){}"
# The same definition the three ways a build system may write it into a command for the shell
QUOTED = [
    '-DLV_ASSERT_HANDLER="if((abort(),0)){}"',
    "-DLV_ASSERT_HANDLER='if((abort(),0)){}'",
    "-DLV_ASSERT_HANDLER=if\\(\\(abort\\(\\),0\\)\\)\\{\\}",
]


def command(source, *options):
    return {"directory": "/project/display/build", "file": source,
            "command": " ".join(("xtensa-esp32s3-elf-gcc", "-DESP_PLATFORM") + options + ("-c", source))}


def linker_map(count=300, taken=(), discarded=(), own=True):
    """The map of a build as GNU ld writes it, in the parts check_build.py reads: `count` objects of the
    archives of ESP-IDF and of the display, those of `taken` among them, and those of `discarded` with
    sections that were thrown away. The archive of the MQTT client of ESP-IDF is loaded, as in a build whose
    main/ names the component among its requirements: without that it is not on the command line of the
    linker at all (the build of the CI on 2026-10-12)."""
    placed = ["esp-idf/core/libcore.a(mqtt_sub.c.obj)", "esp-idf/core/libcore.a(cerbo.c.obj)", "esp-idf/main/libmain.a(gx.c.obj)"]
    placed += ["esp-idf/lwip/liblwip.a(file_%d.c.obj)" % number for number in range(count - len(placed) - (1 if own else 0))]
    if own:
        placed.append("esp-idf/main/libmain.a(main.c.obj)")
    placed += list(taken)
    asked = "esp-idf/main/libmain.a(main.c.obj)" if own else "esp-idf/lwip/liblwip.a(file_0.c.obj)"
    lines = ["Archive member included to satisfy reference by file (symbol)", ""]
    for member in placed:
        lines += [member, "                              %s (some_symbol)" % asked]
    lines += ["", "Discarded input sections", ""]
    lines += [" .text          0x00000000        0x0 %s" % member for member in list(discarded) + placed[:3]]
    lines += ["", "Memory Configuration", "", "Linker script and memory map", "",
              "LOAD CMakeFiles/wican-display.elf.dir/project_elf_src_esp32s3.c.obj",
              "LOAD esp-idf/main/libmain.a", "LOAD esp-idf/core/libcore.a", "LOAD esp-idf/mqtt/libmqtt.a", "LOAD esp-idf/lwip/liblwip.a", ""]
    for number, member in enumerate(placed):
        lines += [" .text.function_%d" % number, "                0x%08x       0x10 %s" % (0x42000000 + 16 * number, member)]
    return "\n".join(lines) + "\n"


CLIENT = "esp-idf/mqtt/libmqtt.a(mqtt_client.c.obj)"


def build(count=120, options=(QUOTED[0],)):
    """The commands of a build: `count` sources of LVGL, and two of other components without the definition."""
    entries = [command("%score/lv_file_%d.c" % (LVGL, number), *options) for number in range(count)]
    entries.append(command("/project/display/components/ui/ui.c"))
    entries.append(command("/project/display/main/main.c"))
    return entries


class CheckBuild(unittest.TestCase):
    def test_a_build_with_the_definition_is_fine(self):
        for quoted in QUOTED:
            self.assertEqual(check_build.problems(build(options=(quoted,))), [], quoted)

    def test_arguments_as_a_list_are_read_as_well(self):
        entries = build()
        # The list is the one that counts where an entry has both: the command here lacks the definition
        for entry in entries:
            if LVGL in entry["file"]:
                entry["command"] = "gcc -c " + entry["file"]
                entry["arguments"] = ["gcc", "-D" + WANTED, "-c", entry["file"]]
        self.assertEqual(check_build.problems(entries), [])
        entries[5]["arguments"] = ["gcc", "-D", WANTED, "-c", entries[5]["file"]]
        self.assertEqual(check_build.problems(entries), [])

    def test_a_build_without_the_definition_is_reported(self):
        found = check_build.problems(build(options=()))
        self.assertEqual(len(found), 1)
        self.assertIn("120 of 120", found[0])

    def test_one_source_without_it_is_reported_by_name(self):
        entries = build()
        entries[7] = command(LVGL + "draw/lv_draw_other.c")
        found = check_build.problems(entries)
        self.assertEqual(len(found), 1)
        self.assertIn("1 of 120", found[0])
        self.assertIn("lv_draw_other.c", found[0])

    def test_another_value_is_not_the_definition(self):
        # Cut at a semicolon, as CMake would cut a definition that had one; the loop LVGL has by itself
        for other in ('-DLV_ASSERT_HANDLER="abort()"', '-DLV_ASSERT_HANDLER="while(1)"', "-DLV_ASSERT_HANDLER",
                      '-DLV_ASSERT_HANDLER_INCLUDE="stdlib.h"', '-DXLV_ASSERT_HANDLER="if((abort(),0)){}"'):
            found = check_build.problems(build(options=(other,)))
            self.assertEqual(len(found), 1, other)
            self.assertIn("120 of 120", found[0])

    def test_a_name_that_only_begins_like_it_is_another_macro(self):
        later = '-DLV_ASSERT_HANDLER_INCLUDE="stdlib.h"'
        self.assertEqual(check_build.problems(build(options=(QUOTED[0], later))), [])

    def test_the_last_definition_counts(self):
        self.assertEqual(check_build.problems(build(options=('-DLV_ASSERT_HANDLER="while(1)"', QUOTED[0]))), [])
        for later in ('-DLV_ASSERT_HANDLER="while(1)"', "-ULV_ASSERT_HANDLER", "-U LV_ASSERT_HANDLER"):
            found = check_build.problems(build(options=(QUOTED[0], later)))
            self.assertEqual(len(found), 1, later)

    def test_too_few_sources_are_no_build(self):
        for count in (0, 99):
            found = check_build.problems(build(count=count))
            self.assertEqual(len(found), 1)
            self.assertIn("only %d sources" % count, found[0])
        self.assertEqual(check_build.problems(build(count=100)), [])

    def test_sources_of_other_components_do_not_count(self):
        # Neither as sources of LVGL that lack the definition nor towards their number
        entries = [command("/project/display/components/ui/ui_%d.c" % number, QUOTED[0]) for number in range(200)]
        found = check_build.problems(entries)
        self.assertEqual(len(found), 1)
        self.assertIn("only 0 sources", found[0])

    def test_command_line(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "compile_commands.json")
            for entries, definition, status, word in ((build(), None, 0, "OK"),
                                                      (build(options=()), None, 1, "PROBLEM"),
                                                      (build(), "LV_ASSERT_HANDLER=while(1);", 1, "PROBLEM")):
                with open(path, "w", encoding="utf-8") as handle:
                    json.dump(entries, handle)
                argv = ["--commands", path] + (["--definition", definition] if definition else [])
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    self.assertEqual(check_build.main(argv), status)
                self.assertTrue(output.getvalue().startswith(word), output.getvalue())


class CheckMap(unittest.TestCase):
    def test_a_firmware_without_a_client_is_fine(self):
        # The archive of the client is loaded where a component requires it, and the display has its own
        # reader of MQTT: neither is an object of a client
        text = linker_map()
        self.assertIn("LOAD esp-idf/mqtt/libmqtt.a\n", text)
        self.assertIn("libcore.a(mqtt_sub.c.obj)", text)
        self.assertEqual(check_build.map_problems(text), [])
        self.assertEqual(len(check_build.map_objects(text)), 300)

    def test_an_object_of_the_client_is_reported_by_name(self):
        found = check_build.map_problems(linker_map(taken=(CLIENT,)))
        self.assertEqual(len(found), 1)
        self.assertIn("1 objects of an MQTT client", found[0])
        self.assertIn("libmqtt.a(mqtt_client.c.obj)", found[0])
        # What the client pulls in with it counts as well, each object once however often the map names it
        more = ("esp-idf/mqtt/libmqtt.a(mqtt_msg.c.obj)", "esp-idf/mqtt/libmqtt.a(mqtt_outbox.c.obj)")
        found = check_build.map_problems(linker_map(taken=(CLIENT,) + more, discarded=(CLIENT,)))
        self.assertEqual(len(found), 1)
        self.assertIn("3 objects of an MQTT client", found[0])

    def test_an_object_whose_sections_were_all_thrown_away_was_taken_all_the_same(self):
        # Somebody asked for it, or the linker had not looked into the archive
        found = check_build.map_problems(linker_map(discarded=(CLIENT,)))
        self.assertEqual(len(found), 1)
        self.assertIn("libmqtt.a(mqtt_client.c.obj)", found[0])

    def test_the_client_under_another_roof_is_one_as_well(self):
        # As a managed component, with a path of Windows, in capitals
        for member in ("esp-idf/espressif__mqtt/libespressif__mqtt.a(mqtt_client.c.obj)",
                       "esp-idf\\mqtt\\libmqtt.a(mqtt_client.c.obj)", "esp-idf/mqtt/libMQTT.a(mqtt_client.c.obj)",
                       "/opt/esp/idf/components/esp-mqtt/libmqtt5.a(mqtt5_client.c.obj)"):
            found = check_build.map_problems(linker_map(taken=(member,)))
            self.assertEqual(len(found), 1, member)
            self.assertIn("1 objects of an MQTT client", found[0])

    def test_a_hyphen_or_a_dot_in_the_name_of_the_archive_hides_no_client(self):
        # The client of ESP-IDF cloned into components/ under the name of its repository, Paho, MQTT-C: each of
        # these passed with OK until 2026-10-12, when the pattern knew letters, digits and the underscore only
        for member in ("esp-idf/esp-mqtt/libesp-mqtt.a(mqtt_client.c.obj)", "esp-idf/paho/libpaho-mqtt3c.a(MQTTClient.c.obj)",
                       "esp-idf/mqtt-c/libmqtt-c.a(mqtt.c.obj)", "esp-idf/paho/libpaho.mqtt.embedded-c.a(MQTTPacket.c.obj)",
                       "esp-idf/x/libx-MQTT.v5.a(client.c.obj)", "esp-idf/x/lib-mqtt-.a(client.c.obj)"):
            found = check_build.map_problems(linker_map(taken=(member,)))
            self.assertEqual(len(found), 1, member)
            self.assertIn("1 objects of an MQTT client", found[0])
            self.assertIn(member.replace("\\", "/").rsplit("/", 1)[-1], found[0])
        # ... and an archive without the word is none for a hyphen or a dot in its name
        for member in ("esp-idf/esp-tls/libesp-tls.a(esp_tls.c.obj)", "esp-idf/x/libm.q.t.t.a(file.c.obj)", "esp-idf/x/libmq-tt.a(file.c.obj)",
                       "esp-idf/mqtt-notes/libnotes.a(mqtt-client.c.obj)"):
            self.assertEqual(check_build.map_problems(linker_map(taken=(member,))), [], member)

    def test_only_the_archive_tells_whose_object_it_is(self):
        # A source of the display may bear the word in its name, and a directory may: no client
        for member in ("esp-idf/main/libmain.a(mqtt_notes.c.obj)", "esp-idf/mqtt/libother.a(file.c.obj)",
                       "esp-idf/core/libcore.a(mqtt_sub.c.obj)"):
            self.assertEqual(check_build.map_problems(linker_map(taken=(member,))), [], member)

    def test_what_is_no_map_of_a_whole_build_is_none(self):
        # An empty file, a map cut short, the map of something else: none of them shows that there is no client
        for text in ("", "LOAD esp-idf/main/libmain.a\n", linker_map(count=199), linker_map(own=False)):
            found = check_build.map_problems(text)
            self.assertEqual(len(found), 1)
            self.assertIn("not the map of a whole build", found[0])
        self.assertEqual(check_build.map_problems(linker_map(count=200)), [])
        # ... also when it names the client: that is said of a map that can be believed
        self.assertIn("not the map", check_build.map_problems(linker_map(count=50, taken=(CLIENT,)))[0])

    def test_command_line(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "wican-display.map")
            commands = os.path.join(directory, "compile_commands.json")
            with open(commands, "w", encoding="utf-8") as handle:
                json.dump(build(), handle)
            for text, both, status, word in ((linker_map(), False, 0, "OK: no object of an MQTT client among the 300"),
                                             (linker_map(taken=(CLIENT,)), False, 1, "PROBLEM"),
                                             (linker_map(), True, 0, "OK: every source of LVGL"),
                                             (linker_map(taken=(CLIENT,)), True, 1, "PROBLEM")):
                # Bytes that are no text, as a path of the build may have them
                with open(path, "wb") as handle:
                    handle.write(text.encode("utf-8") + b"\xff\xfe\n")
                argv = ["--map", path] + (["--commands", commands] if both else [])
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    self.assertEqual(check_build.main(argv), status)
                self.assertTrue(output.getvalue().startswith(word), output.getvalue())
            # Asked to check nothing, it does not say that all is well
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as stopped:
                check_build.main([])
            self.assertEqual(stopped.exception.code, 2)


HERE = os.path.dirname(os.path.abspath(__file__))
WORKFLOW = os.path.join(HERE, "..", "..", ".github", "workflows", "display.yml")


def step_script(name):
    """The lines of `run: |` of the step with this name in the workflow of the CI, as the runner hands them to
    its shell"""
    with open(WORKFLOW, encoding="utf-8") as handle:
        lines = handle.read().split("\n")
    at = lines.index("      - name: " + name)
    if lines[at + 1] != "        run: |":
        raise ValueError("the step %s has no script" % name)
    script = []
    for line in lines[at + 2:]:
        if line.strip() and not line.startswith("          "):
            break
        script.append(line[10:])
    return "\n".join(script) + "\n"


@unittest.skipUnless(shutil.which("bash") and os.path.exists(WORKFLOW), "needs bash and the workflow of the CI")
class CounterCheckSteps(unittest.TestCase):
    """The two steps of .github/workflows/display.yml that break the build on purpose and wait for the check
    to say so. Each has to pass on the one line it waits for and on nothing else: until 2026-10-12 both took
    any status but 0 for the report, so a map that was not there made the counter-check green."""

    def run_step(self, name, files):
        """The status of the step in a checkout that has check_build.py and these files of a build"""
        with tempfile.TemporaryDirectory() as directory:
            os.makedirs(os.path.join(directory, "display", "tools"))
            os.makedirs(os.path.join(directory, "display", "build"))
            shutil.copy(os.path.join(HERE, "check_build.py"), os.path.join(directory, "display", "tools"))
            for file, text in files.items():
                with open(os.path.join(directory, "display", "build", file), "w", encoding="utf-8") as handle:
                    handle.write(text)
            # As the runner calls it (the default shell of a step on Linux)
            run = subprocess.run(["bash", "--noprofile", "--norc", "-e", "-c", step_script(name)], cwd=directory, capture_output=True,
                                 encoding="utf-8", errors="replace")
            return run.returncode, run.stdout + run.stderr

    def test_the_client_is_reported(self):
        name = "Counter-check, the client is reported"
        more = ("esp-idf/mqtt/libmqtt.a(mqtt_msg.c.obj)", "esp-idf/mqtt/libmqtt.a(mqtt_outbox.c.obj)", "esp-idf/mqtt/libmqtt.a(platform_esp32.c.obj)")
        status, said = self.run_step(name, {"wican-display.map": linker_map(count=916, taken=(CLIENT,) + more)})
        self.assertEqual(status, 0, said)
        self.assertIn("PROBLEM: 4 objects of an MQTT client are linked into the firmware, the first: libmqtt.a(mqtt_client.c.obj)", said)
        # What is no such report: no map, a map without the client, a map that is none, the client of another name
        for what, files in (("no map", {}), ("no client", {"wican-display.map": linker_map(count=916)}),
                            ("an empty map", {"wican-display.map": ""}),
                            ("a map cut short", {"wican-display.map": linker_map(count=50, taken=(CLIENT,))}),
                            ("another archive", {"wican-display.map": linker_map(taken=("esp-idf/main/libmain.a(mqtt_client.c.obj)",))})):
            status, said = self.run_step(name, files)
            self.assertNotEqual(status, 0, what + ": " + said)

    def test_the_absence_of_the_definition_is_reported(self):
        name = "Counter-check, its absence is reported"
        status, said = self.run_step(name, {"compile_commands.json": json.dumps(build(count=463, options=()))})
        self.assertEqual(status, 0, said)
        self.assertIn("PROBLEM: 463 of 463 sources of LVGL are not compiled with -DLV_ASSERT_HANDLER=if((abort(),0)){}", said)
        one = build(count=120)
        one[7] = command(LVGL + "draw/lv_draw_other.c")
        most = build(count=120, options=())
        for number in range(10):
            most[number] = command("%score/lv_file_%d.c" % (LVGL, number), QUOTED[0])
        for what, files in (("no commands", {}), ("the definition is there", {"compile_commands.json": json.dumps(build(count=463))}),
                            ("no JSON", {"compile_commands.json": "not the commands of a build"}),
                            ("too few sources", {"compile_commands.json": json.dumps(build(count=50, options=()))}),
                            ("one source without it", {"compile_commands.json": json.dumps(one)}),
                            ("ten sources still with it", {"compile_commands.json": json.dumps(most)})):
            status, said = self.run_step(name, files)
            self.assertNotEqual(status, 0, what + ": " + said)

    def test_the_steps_are_the_ones_of_the_workflow(self):
        # Both scripts run the check itself, on the files the build writes
        self.assertIn("python3 display/tools/check_build.py --map display/build/wican-display.map", step_script("Counter-check, the client is reported"))
        self.assertIn("python3 display/tools/check_build.py --commands display/build/compile_commands.json",
                      step_script("Counter-check, its absence is reported"))


if __name__ == "__main__":
    unittest.main()
