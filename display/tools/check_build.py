#!/usr/bin/env python3
"""Checks two things about a build of the firmware that a build which succeeds does not show.

That a definition made for the sources of LVGL has reached the compiler. components/ui/CMakeLists.txt
defines LV_ASSERT_HANDLER for the sources of LVGL, which are another component: a failed assertion of LVGL
has to end in abort(), which restarts the display, and not in the loop without end LVGL has for it. Without
the definition LVGL builds just as well, with the loop. What the compiler was really given is in the
compile_commands.json CMake writes next to the build, and that is what is read here.

That the firmware holds no MQTT client. The display reads the broker of a Cerbo GX with a reader of its
own that can build a CONNECT, a SUBSCRIBE and a PINGREQ and nothing else (components/core/mqtt_sub.h,
main/gx.c): on that broker a published text switches relays, and what cannot be built cannot be sent.
ESP-IDF v5.5.2 brings a client that can publish (components/mqtt, libmqtt.a). Its archive is on the
command line of the linker in every build (tools/cmake/project.cmake links every component), so a source
that declares one of its functions itself can call it without a line of a CMakeLists.txt changing. Whether
one does is in the map the linker writes next to the firmware: it names every object it took from an
archive, and it takes one only when something asks for it.

  python3 check_build.py --commands ../build/compile_commands.json
  python3 check_build.py --map ../build/wican-display.map
"""
import argparse
import json
import re
import shlex
import sys

LVGL_SOURCES = "/lvgl__lvgl/src/"
NAME = "LV_ASSERT_HANDLER"
DEFINITION = NAME + "=if((abort(),0)){}"
# LVGL 9.5.0 has several hundred sources. Fewer than this and the list is not the one of a whole build.
AT_LEAST = 100

# An object the linker took from an archive, as its map names it wherever it speaks of one - in the list
# "Archive member included to satisfy reference by file (symbol)", with the sections it placed, with those
# it discarded: esp-idf/main/libmain.a(main.c.obj). A line "LOAD esp-idf/mqtt/libmqtt.a" names no object:
# every archive of the build is on the command line of the linker, whether anything of it is taken or not.
MEMBER = re.compile(r"([^\s()]+\.a)\(([^\s()]+)\)")
# The client of ESP-IDF v5.5.2 is libmqtt.a; as a managed component it would be libespressif__mqtt.a
MQTT_ARCHIVE = re.compile(r"lib\w*mqtt\w*\.a", re.I)
# What every firmware of the display has: without it the text is not its map
OWN_OBJECT = ("libmain.a", "main.c.obj")
# The firmware is linked of well over a thousand objects. Fewer than this and the map is not that of a build.
MAP_AT_LEAST = 200


def arguments(entry):
    """What the compiler is called with for one source, as the shell would hand it over."""
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def definitions(args, name):
    """Every value the macro `name` is given on the command line, in order; None for an -U."""
    found = []
    position = 0
    while position < len(args):
        arg = args[position]
        for option in ("-D", "-U"):
            if arg == option and position + 1 < len(args):
                position += 1
                arg = option + args[position]
                break
        if arg.startswith("-D") and arg[2:].split("=", 1)[0] == name:
            found.append(arg[2:])
        elif arg.startswith("-U") and arg[2:] == name:
            found.append(None)
        position += 1
    return found


def problems(entries, definition=DEFINITION, part=LVGL_SOURCES, at_least=AT_LEAST):
    """Texts of everything that is wrong, empty if every source of LVGL is compiled with the definition."""
    name = definition.split("=", 1)[0]
    sources = [entry for entry in entries
               if part in entry.get("file", "").replace("\\", "/") and entry["file"].endswith(".c")]
    if len(sources) < at_least:
        return ["only %d sources with %s in their path, at least %d expected: not the commands of a whole build"
                % (len(sources), part, at_least)]

    # The compiler goes by the last definition of a name
    wrong = [entry["file"] for entry in sources if definitions(arguments(entry), name)[-1:] != [definition]]
    if wrong:
        return ["%d of %d sources of LVGL are not compiled with -D%s, the first: %s"
                % (len(wrong), len(sources), definition, wrong[0])]
    return []


def map_objects(text):
    """Every object of an archive the map names, as (file name of the archive, object), each once"""
    return sorted({(archive.replace("\\", "/").rsplit("/", 1)[-1], member) for archive, member in MEMBER.findall(text)})


def map_problems(text, at_least=MAP_AT_LEAST):
    """Texts of everything that is wrong, empty if the map is that of a firmware without an MQTT client."""
    objects = map_objects(text)
    if len(objects) < at_least or OWN_OBJECT not in objects:
        return ["%d objects of archives and %s(%s) %s among them, at least %d and that one expected: not the map of a "
                "whole build" % (len(objects), OWN_OBJECT[0], OWN_OBJECT[1], "is" if OWN_OBJECT in objects else "not", at_least)]
    client = ["%s(%s)" % found for found in objects if MQTT_ARCHIVE.fullmatch(found[0])]
    if client:
        return ["%d objects of an MQTT client are linked into the firmware, the first: %s" % (len(client), client[0])]
    return []


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--commands", help="compile_commands.json of the build: LVGL is compiled with the definition")
    parser.add_argument("--definition", default=DEFINITION, help="name=value as the compiler has to get it")
    parser.add_argument("--map", help="map file of the build: no MQTT client is linked")
    options = parser.parse_args(argv)
    if options.commands is None and options.map is None:
        parser.error("nothing to check: --commands, --map or both")

    found = []
    said = []
    if options.commands is not None:
        with open(options.commands, encoding="utf-8") as handle:
            entries = json.load(handle)
        found += problems(entries, options.definition)
        said.append("every source of LVGL is compiled with -D" + options.definition)
    if options.map is not None:
        # The linker writes the paths of the build as they are; no byte of them has to be a text
        with open(options.map, encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        found += map_problems(text)
        said.append("no object of an MQTT client among the %d the firmware is linked of" % len(map_objects(text)))
    for text in found:
        print("PROBLEM: " + text)
    if not found:
        print("OK: " + "; ".join(said))
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
