"""Checks the page of the display (display/main/web/index.html) and the stand-in it is tried against
(mock_display.py), both against display/API.md.

The page runs in a browser, and there is none here: what is checked is its text. It names no path that
API.md does not name, every request goes through the one function that adds the header of a change, it
has none of the constructs that make HTML or code out of text, it loads nothing from anywhere, and it is
small enough for the flash - and, compressed as the display sends it, for the radio link. Whether its
buttons do what they say is not checked here.

The mock is asked what API.md says the display answers: the examples (where display/test/fixtures has
the text, byte for byte) and each refusal, in the documented order. That holds the mock to API.md, not
the device to it: the firmware has its own tests (display/test).

CounterCheck breaks each thing once - the page on a copy of its text, the mock by a class or a number
that is wrong in one place - and expects the check that guards it to fail.

  python -m unittest -v          in display/tools, as the CI does
"""
import base64
import gzip
import hashlib
import http.client
import os
import re
import unittest
from unittest import mock

import mock_display
import page_gz

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, "..", "main", "web", "index.html")
API = os.path.join(HERE, "..", "API.md")
FIXTURES = os.path.join(HERE, "..", "test", "fixtures")

# Below 64 KB, counted as the project counts its flash (partitions.csv: "nvs 128 KB" is 0x20000). It was
# 60 KB until the page got the field for the address of the Cerbo GX and the line that says how that source
# stands, 1240 bytes that 483 bytes of room did not hold. The 60 KB were the project's own number from before
# the page was sent compressed; what the radio link carries is the limit below, which stays.
SIZE_MAX = 64 * 1024
# The page as a browser gets it: compressed (main/web.c, page_gz.py), over a radio link that is slow. Below
# 24 KB, counted the same way: at the slowest rate measured on the device - 12 KB a second, for the plain
# page of 60189 bytes, 2026-10-10 - that is two seconds. The page of that day compresses to 20464 bytes.
# Another zlib than the one of this Python may make a few bytes more or less of the same page.
COMPRESSED_MAX = 24 * 1024
HOST = "192.168.1.77"
SLOT = mock_display.SLOT_SIZE


def file_text(path):
    with open(path, encoding="utf-8") as file:
        return file.read()


def fixture(name):
    """A hand-written answer of display/test/fixtures, without the line end of the file"""
    return file_text(os.path.join(FIXTURES, name)).rstrip("\n")


# ----------------------------------------------------------------------------------------------------
# The page

def documented_requests(api_text):
    """The requests of the table "Requests" in API.md as (method, path with its query)"""
    table = api_text.split("## Requests")[1].split("## Refusals")[0]
    return re.findall(r"^\| `(GET|PUT|POST) ([^` ]+)` \|", table, re.M)


def script_of(page):
    """The JavaScript of the page: it has one script element, and that has no src"""
    return "\n".join(re.findall(r"<script>(.*?)</script>", page, re.S))


def code_of(script):
    """The script as (code, strings): the code without its comments and with every string literal emptied,
    and the literals. The page writes its strings in double quotes only (see construct_problems)."""
    code, strings = [], []
    position = 0
    while position < len(script):
        if script.startswith("//", position):
            position = script.find("\n", position) if "\n" in script[position:] else len(script)
        elif script.startswith("/*", position):
            position = script.index("*/", position) + 2
        elif script[position] == "\"":
            end = position + 1
            while script[end] != "\"":
                end += 2 if script[end] == "\\" else 1
            strings.append(script[position + 1:end])
            code.append("\"\"")
            position = end + 1
        else:
            code.append(script[position])
            position += 1
    return "".join(code), strings


def request_problems(page, api_text):
    """What keeps the page from talking to the display through the requests of API.md only, with the
    header of a change on every request that is no GET."""
    found = []
    script = script_of(page)
    code, strings = code_of(script)
    documented = set(documented_requests(api_text))

    # Every call of api() names its method and its path as literals; one path ends with a number
    calls = re.findall(r"(?<![\w.])api\((.{0,60})", script)
    used = set()
    for call in calls:
        if call.startswith("method, path"):
            continue    # the function itself
        literal = re.match(r"\"(GET|PUT|POST)\", \"(/[^\"]*)\"( \+ id)?[,)]", call)
        if literal is None:
            found.append("a request whose method and path are not written out: api(%s" % call)
            continue
        method, path, numbered = literal.groups()
        request = (method, path + "N" if numbered else path)
        if request not in documented:
            found.append("%s %s is no request of API.md" % request)
        used.add(request)
    for request in sorted(documented - used - {("GET", "/")}):
        found.append("the page never asks for %s %s" % request)

    # No path anywhere else
    paths = {path[:-1] if path.endswith("N") else path for _, path in documented}
    for literal in strings:
        if literal.startswith("/") and literal not in paths:
            found.append("a path that is no request of API.md: %s" % literal)
    if "/mock" in page:
        found.append("the page knows the mock")

    # One function sends, and it adds the header to everything that is no GET
    function = re.search(r"\nfunction api\(method, path, body, progress\)\n\{\n(.*?)\n\}\n", script, re.S)
    if function is None:
        found.append("no function api(method, path, body, progress)")
    else:
        if "xhr.open(method, path);" not in function.group(1):
            found.append("api() does not open the request it was asked for")
        if "\n\t\tif(method !== \"GET\") xhr.setRequestHeader(\"X-Display\", \"1\");\n" not in function.group(1):
            found.append("api() does not add X-Display: 1 to every request that is no GET")
    if code.count("XMLHttpRequest") != 1 or code.count(".open(") != 1:
        found.append("requests are made in more than one place")
    for other in ("fetch(", "sendBeacon", "WebSocket", "EventSource", ".submit(", "location.href", "location.assign",
                  "location.replace", "window.open", "importScripts", "import("):
        if other in code:
            found.append("another way to send a request: %s" % other)
    for element in ("<form", "<a ", "<iframe", "<object", "<embed"):
        if element in page.lower():
            found.append("an element that sends requests by itself: %s" % element)
    return found


def construct_problems(page):
    """What could turn a text that came over the network into HTML or into code. None of it is allowed at
    all, with data or without: nobody has to judge then where a text came from."""
    found = []
    for word in ("innerHTML", "outerHTML", "insertAdjacentHTML", "document.write", "eval(", "new Function",
                 "setAttribute", "srcdoc", "DOMParser", "createContextualFragment", "javascript:",
                 "setTimeout(\"", "setInterval(\""):
        if word in page:
            found.append("forbidden: %s" % word)
    # An event handler as an attribute or as a property: the page listens with addEventListener only
    for handler in re.findall(r"\bon[a-z]+\s*=", page):
        found.append("an event handler that is no listener: %s" % handler)
    # code_of() reads double quotes only, and a text between other quotes would pass every check above
    script = script_of(page)
    if "'" in script or "`" in script:
        found.append("a quote in the script that is no double quote")
    if page.count("<script") != 1 or "<script>" not in page:
        found.append("not exactly one script element without attributes")
    return found


def external_problems(page):
    """What the page would load from somewhere, or where it would send somebody"""
    found = []
    if "://" in page:
        found.append("an address with a scheme")
    for attribute, value in re.findall(r"\b(src|href|action|poster|data|srcset)\s*=\s*\"([^\"]*)\"", page):
        if (attribute, value) != ("href", "data:,"):
            found.append("%s=\"%s\"" % (attribute, value))
    code, strings = code_of(script_of(page))
    for assigned in re.findall(r"\b(?:href|src)\b\s*[:=]\s*[^,;}]*", code):
        if not assigned.startswith("href: URL.createObjectURL("):
            found.append("an address set by the script: %s" % assigned)
    if any(literal.startswith("//") for literal in strings):
        found.append("an address without its scheme")
    styles = "".join(re.findall(r"<style>(.*?)</style>", page, re.S)) + "".join(re.findall(r"\bstyle\s*=\s*\"[^\"]*\"", page))
    for word in ("url(", "@import", "@font-face", "image-set("):
        if word in styles or any(word in literal for literal in strings):
            found.append("a style that loads something: %s" % word)
    for word in ("<img", "<video", "<audio", "<source", "<base", "<script src"):
        if word in page.lower():
            found.append("loads something: %s" % word)
    if len(re.findall(r"<link\b", page)) != page.count("<link rel=\"icon\" href=\"data:,\">"):
        found.append("a link element that is not the empty icon")
    return found


def editor_marks(page):
    """What makes the editor of the page call a value of the Cerbo unknown, as a list like the *_problems
    above and below; its counter-check is EDITOR_MUTATIONS, not PAGE_MUTATIONS. The editor marks a value whose
    key the catalogue of the adapter does not hold - "nicht im Katalog", and that the display shows "n. v."
    for it - by itself, without asking the display. A key that begins with GX_ is in no catalogue and never
    will be (API.md: PUT /api/layout never lists one as unknown), and the display shows its value from what
    the Cerbo sent: the mark must not stand at one - else it stood at every value of the five built-in pages
    of the Cerbo as soon as the catalogue is loaded."""
    found = []
    script = script_of(page)
    note = " · nicht im Katalog: das Display zeigt „n. v.“"
    if code_of(script)[1].count(note) != 1:
        found.append("the note of a key the catalogue does not hold stands in %d places, not in one" % code_of(script)[1].count(note))
    elif ("catalogLoaded() && !has(S.catalog, item.key) && !text(item.key).startsWith(\"%s\") ? el(\"span\", \"note\", [\"%s\"]) : \"\""
          % (mock_display.CERBO_PREFIX, note)) not in script:
        found.append("the note of a key the catalogue does not hold is not: the catalogue is loaded, it does not hold the key, "
                     "and the key does not begin with %s" % mock_display.CERBO_PREFIX)
    return found


def editor_offers(page):
    """What keeps the editor of the page from offering the values of the Cerbo GX and its built-in pages
    (API.md: GET /api/catalog and GET /api/layout/gx with a second source), as a list like the *_problems
    above and below; its counter-check is OFFER_MUTATIONS, not PAGE_MUTATIONS. The keys of the Cerbo come
    with the catalogue, and the editor lists what the catalogue names; what is looked at here is the rest:
    the button "Cerbo-Seiten anhängen" on a display that names a Cerbo and on no other, the pages asked for
    there alone, copies of them appended within the twelve pages a layout may have and with a word where they
    do not fit, and a value of the Cerbo that starts with what those pages say of its key."""
    found = []
    script = script_of(page)
    code = code_of(script)[0]

    # The button: one, hidden from the start, and no change of the display - it needs no release
    if page.count("<button id=\"gxadd\" hidden>Cerbo-Seiten anhängen</button>") != 1 or page.count("id=\"gxadd\"") != 1:
        found.append("no button Cerbo-Seiten anhängen that is hidden from the start")
    # ... shown on a display that names a Cerbo and has such pages, said where the editor follows every change
    shown = re.findall(r"\$\(\"gxadd\"\)\.hidden = ([^;]*);", script)
    head = re.search(r"\nfunction head\(\)\n\{\n(.*?)\n\}\n", script, re.S)
    if shown != ["!isObject(part(S, \"info\").cerbo) || !S.gx || !S.gx.pages.length"]:
        found.append("the button is not shown exactly on a display that names a Cerbo and has pages of it")
    elif head is None or "$(\"gxadd\").hidden = " not in head.group(1):
        found.append("the button does not follow the display where the editor looks at its views")

    # The pages are asked for on a display that names a Cerbo, there alone, until it has answered
    asked = sorted(line for line in code.split("\n") if "loadGx()" in line)
    if asked != ["\tif(isObject(info.cerbo) && !S.gx) await loadGx();", "async function loadGx()"]:
        found.append("the pages of the Cerbo are not asked for exactly on a display that names one and has not answered them yet")
    # What came is read as the editor reads every layout; a display without such pages leaves none to
    # append, and no answer at all asks again
    if "\n\tif(r.status) S.gx = r.status === 200 && adopt(r.text) || {pages: []};\n" not in script or len(re.findall(r"\bS\.gx = ", code)) != 1:
        found.append("the pages are not kept as a layout the editor read, or as none for a display that has none")

    # Appended: copies, behind the pages of the editor, within the limit of layout.h and with a word
    function = re.search(r"\nfunction gxAppend\(\)\n\{\n(.*?)\n\}\n", script, re.S)
    if function is None:
        found.append("no function gxAppend()")
    else:
        body = function.group(1)
        refused = re.search(r"\n\tif\(over > 0\)\n\t\{\n\t\ttell\(\"vmsg\", [^\n]*, \"bad\"\);\n\t\treturn;\n\t\}\n", body)
        appended = body.find("\tmore.forEach(page => pages.push(page));\n\tdraw();\n\tchanged();\n")
        if "\tconst pages = S.layout.pages;\n\tconst more = JSON.parse(JSON.stringify(S.gx.pages));\n" not in body:
            found.append("what is appended are not copies of the pages of the Cerbo")
        if "\tconst over = pages.length + more.length - PAGES_MAX;\n" not in body or refused is None:
            found.append("pages that do not fit are not refused with a word")
        elif appended < refused.end():
            found.append("the pages are not appended behind the ones of the editor once they are known to fit, with the editor following")
    if "\nconst PAGES_MAX = %d;\n" % mock_display.PAGES_MAX not in script:
        found.append("the limit of the pages is not the one of layout.h")
    if "\n\tgxadd: () => S.layout && S.gx && gxAppend(),\n" not in script:
        found.append("the button does not append the pages, or does it without views or without pages")

    # A value of the Cerbo that is put on a page starts with a copy of what the built-in pages say of its key
    item = re.search(r"\nfunction gxItem\(key\)\n\{\n(.*?)\n\}\n", script, re.S)
    if "\t\t\tS.item = gxItem(name) || {key: name, map: []};\n" not in script:
        found.append("a value that is put on a page does not start with what the pages of the Cerbo say of its key")
    if item is None or "(S.gx ? S.gx.pages : [])" not in item.group(1) or ".find(item => item.key === key);" not in item.group(1) \
            or "\n\treturn found ? JSON.parse(JSON.stringify(found)) : null;" not in item.group(1):
        found.append("gxItem() is not a copy of the first value with that key on the pages of the Cerbo, and nothing without them")

    # The keys of the Cerbo are in the catalogue: it is read anew when the Cerbo has more or fewer values to show
    if " || part(info, \"cerbo\").values !== part(before, \"cerbo\").values) loadCatalog();\n" not in script:
        found.append("the catalogue is not read anew when the number of values of the Cerbo changes")
    return found


def release_problems(page):
    """What keeps the page from following a release for good (API.md: "lasting" of /api/info). Such a
    release has no end, and its left_s is 0: a page that makes an end of that locks its own buttons. The bar
    and the buttons take the end of the release from one place, S.until, through left() - so that place has
    to know the member, and the bar must not count down what has no end."""
    found = []
    code, strings = code_of(script_of(page))
    ends = re.findall(r"\bS\.until = ([^;]*);", code)
    if len(ends) != 1:
        found.append("the end of the release is set in %d places, not in one" % len(ends))
    else:
        if not re.fullmatch(r"(\w+)\.open !== true \? 0 : \1\.lasting === true \? Infinity : Date\.now\(\) \+ Number\(\1\.left_s\) \* 1000", ends[0]):
            found.append("the end of the release is not: none while it is closed, never while it lasts, else left_s from now")
    if code.count("S.until") != 2 or "const left = () => Math.ceil((S.until - Date.now()) / 1000);" not in code:
        found.append("the end of the release is read somewhere else than in left()")
    if re.search(r"\(isFinite\(seconds\) \? \"\" \+ Math\.floor\(seconds / 60\) [^;:]* : \"\"\)", code) is None:
        found.append("the bar counts down a release that has no end")
    if "Web-Zugriff frei" not in strings or ", noch " not in strings:
        found.append("the bar does not say that the web access is free without saying for how long")
    return found


def color_problems(page):
    """What keeps the editor from giving a page the colour of its dot on the display (layout.h: "color",
    "#rrggbb") or from taking it away again. The picker of a browser holds "#rrggbb" in lower case and nothing
    else, no colour is not one of its values: whether the page has one is a box of its own. A colour the
    display would not take - from an import, say - is not given to the picker, which would turn it into black
    without a word; it stays in the views, and the display names it."""
    found = []
    script = script_of(page)
    if "\nconst isColor = value => /^#[0-9a-f]{6}$/i.test(value);\n" not in script:
        found.append("no test of the one form of a colour the display takes")
    if "el(\"input\", {type: \"color\", value: isColor(page.color) ? page.color : \"#ffffff\"})" not in script:
        found.append("the picker is given a colour it cannot hold")
    if "el(\"input\", {type: \"checkbox\", checked: \"color\" in page})" not in script:
        found.append("the box does not say whether the page has a colour")
    if "\n\tcolor.dataset.path = \"pages[\" + S.page + \"].color\";\n" not in script:
        found.append("the picker is not the field the display names when it refuses a colour")
    if "edits(own, \"change\", () => paint(own.checked && color.value));" not in script:
        found.append("the box does not take the colour away")
    if "\n\t\town.checked = true;\n\t\tpaint(color.value);\n" not in script:
        found.append("a colour that is picked leaves the box as it was")
    return found


def cerbo_words(api_text):
    """The words `cerbo.view` of GET /api/info can be, as API.md lists them"""
    return re.findall(r"`(\w+)`", api_text.split("- `cerbo.view`:")[1].split("- `cerbo.values`:")[0])


def cerbo_members(api_text):
    """The members of the object `cerbo` of GET /api/info, from the example of API.md"""
    return re.findall(r"\"(\w+)\":", re.search(r"\n \"cerbo\":\{([^}]*)\}", api_text).group(1))


def cerbo_problems(page, api_text):
    """What keeps the page from setting the address of the Cerbo GX and from saying how that source stands
    (API.md: `cerbo` of the settings and of GET /api/info) - on a display that names a Cerbo and on one that
    does not: without an address neither answer has the member, and a page has to cope with both."""
    found = []
    script = script_of(page)

    # The field: a text of 15 characters at most, locked with the other settings while the web access is not free
    field = re.search(r"<label class=\"f\">([^<]*)<input id=\"cerbo\"([^>]*)></label>", page)
    if field is None:
        found.append("no field for the address of the Cerbo")
    else:
        if "IP-Adresse" not in field.group(1) or "leer = aus" not in field.group(1):
            found.append("the field does not say what it takes, or not that an empty one switches the source off")
        if "maxlength=\"15\"" not in field.group(2) or "class=\"chg\"" not in field.group(2) or "type=" in field.group(2):
            found.append("the field is no text of 15 characters at most that is locked with the other settings")
    # Filled from the settings and sent when it was changed, as every setting is
    listed = re.search(r"\nconst SETTINGS = \[([^\]]*)\];\n", script)
    if listed is None or "\"cerbo\"" not in listed.group(1).split(", "):
        found.append("the address is not one of the settings the page fills in and sends")
    # Sent as the text it is, never as a number, and also when it is empty: that takes the address away
    if "input.type === \"text\" ? input.value.trim() : Number(input.value)" not in script:
        found.append("the address is not sent as a text")
    if "\n\tif(input.value !== \"\" || input.type === \"text\") r = await api(\"POST\", \"/api/settings\"," not in script:
        found.append("an emptied address is not sent")

    # The line that says how the source stands: none on a display that names no Cerbo, and nothing is read
    # of an object that is not there
    line = re.search(r"\n\tif\(isObject\(cerbo\)\) \$\(\"cerbonow\"\)\.textContent = ([^;\n]*);\n", script)
    if "<p id=\"cerbonow\" class=\"dim\" hidden></p>" not in page or "\n\tconst cerbo = info.cerbo;\n" not in script \
            or "\n\t$(\"cerbonow\").hidden = !isObject(cerbo);\n" not in script or line is None:
        found.append("no line for the Cerbo that is hidden while GET /api/info names none")
    else:
        # In the code, not in its comments
        if len(re.findall(r"\bcerbo\.", code_of(script)[0])) != len(re.findall(r"\bcerbo\.", line.group(1))):
            found.append("the object cerbo is read where nobody has looked whether there is one")
        # Its members, by the names API.md gives them, and each as a text: what came over the network is no number
        named = re.findall(r"(.{0,13})\bcerbo\.(\w+)", line.group(1))
        for before, member in named:
            if member not in cerbo_members(api_text):
                found.append("a member of cerbo that API.md does not name: %s" % member)
            if not before.endswith("text(") and not (member == "view" and before.endswith("word(CERBO, ")):
                found.append("cerbo.%s is put into the line as it came" % member)
        if not {"host", "view"} <= {member for before, member in named}:
            found.append("the line names neither the address nor how the source stands")
    # Every word the display has for the source has its text here
    table = re.search(r"\nconst CERBO = \{\n(.*?)\n\};\n", script, re.S)
    words = re.findall(r"\b(\w+): \"", table.group(1)) if table else []
    for word in cerbo_words(api_text):
        if word not in words:
            found.append("no text for the word %s of cerbo.view" % word)
    return found


def size_problems(data):
    return ["%d bytes, at most %d" % (len(data), SIZE_MAX - 1)] if len(data) >= SIZE_MAX else []


def compressed_problems(data):
    size = len(page_gz.compress(data))
    return ["%d bytes when it is compressed, at most %d" % (size, COMPRESSED_MAX - 1)] if size >= COMPRESSED_MAX else []


class Page(unittest.TestCase):
    def setUp(self):
        with open(PAGE, "rb") as file:
            self.data = file.read()
        self.page = self.data.decode("utf-8")
        self.api = file_text(API)

    def test_the_table_of_api_md_is_read(self):
        requests = documented_requests(self.api)
        self.assertEqual(len(requests), 19)
        self.assertIn(("PUT", "/api/layout?mode=apply"), requests)
        self.assertIn(("GET", "/api/layout/gx"), requests)
        self.assertIn(("GET", "/api/ticket?id=N"), requests)

    def test_the_script_is_read(self):
        code, strings = code_of("call(\"a\\\"b\", 1); // \"no string\"\n/* \"nor this\" */ x = \"//c\";")
        self.assertEqual(strings, ["a\\\"b", "//c"])
        self.assertEqual(code, "call(\"\", 1); \n x = \"\";")
        self.assertGreater(len(code_of(script_of(self.page))[1]), 300)

    def test_requests_are_those_of_api_md_and_carry_the_header(self):
        self.assertEqual(request_problems(self.page, self.api), [])

    def test_nothing_makes_html_or_code_of_a_text(self):
        self.assertEqual(construct_problems(self.page), [])

    def test_nothing_is_loaded_from_anywhere(self):
        self.assertEqual(external_problems(self.page), [])

    def test_the_editor_calls_no_value_of_the_cerbo_unknown(self):
        self.assertEqual(editor_marks(self.page), [])

    def test_the_editor_offers_the_values_and_the_pages_of_the_cerbo(self):
        self.assertEqual(editor_offers(self.page), [])

    def test_a_release_for_good_has_no_end_on_the_page(self):
        self.assertEqual(release_problems(self.page), [])

    def test_the_cerbo_is_set_and_told(self):
        self.assertEqual(cerbo_words(self.api), ["off", "no_wifi", "connecting", "unreachable", "quiet", "silent", "newer", "live"])
        self.assertEqual(cerbo_members(self.api), ["host", "view", "values", "ok", "refused", "connects", "boots", "lag_ms"])
        self.assertEqual(cerbo_problems(self.page, self.api), [])

    def test_a_page_is_given_a_colour_and_loses_it(self):
        self.assertEqual(color_problems(self.page), [])

    def test_size(self):
        # Of the page as it is: that is what lies in the flash, next to the compressed one, and what a
        # client gets that does not read gzip
        self.assertEqual(size_problems(self.data), [])

    def test_compressed_size(self):
        self.assertEqual(compressed_problems(self.data), [])


# ----------------------------------------------------------------------------------------------------
# The mock

def reader(data, meanwhile=None):
    """read(count) over the bytes of a body. meanwhile: called once, before the last bytes are handed out."""
    state = {"at": 0, "called": meanwhile is None}

    def read(count):
        piece = data[state["at"]:state["at"] + count]
        state["at"] += len(piece)
        if state["at"] >= len(data) and not state["called"]:
            state["called"] = True
            meanwhile()
        return piece
    return read


class Api(unittest.TestCase):
    """The mock against display/API.md. The class of the display is an attribute so that CounterCheck can
    run a test against one that is wrong."""
    display_class = mock_display.Display

    def setUp(self):
        self.clock = mock_display.SimulatedClock()
        self.display = self.display_class(clock=self.clock)
        # The display of these tests is one whose owner switched "Dauerhaft frei" off: a change needs the
        # release of ten minutes, as on every display before the web interface was released for good by
        # default. The tests of the default say so (delivered()).
        self.display.lasting(False)

    def delivered(self):
        """The display as it comes: its web interface released for good"""
        self.display = self.display_class(clock=self.clock)
        return self.display

    def call(self, method, target, body=None, host=HOST, header=True, length=True, read=None):
        """One request as a browser sends it: (status, body), or None without an answer"""
        headers = {}
        if host is not None:
            headers["host"] = host
        if method != "GET" and header:
            headers["x-display"] = "1" if header is True else header
        if method != "GET" and length:
            headers["content-length"] = str(len(body or b"") if length is True else length)
        answer = self.display.handle(method, target, headers, read or reader(body or b""))
        if answer is None:
            return None
        self.assertEqual(answer[1], "text/html; charset=utf-8" if (method, target, answer[0]) == ("GET", "/", 200) else "application/json")
        return answer[0], answer[2].decode("utf-8")

    def release(self):
        self.display.release(True)

    def ask_reset(self):
        """A question that waits, the simplest one"""
        answer = self.call("POST", "/api/reset")
        self.assertEqual(answer[0], 202)
        return answer

    # ------------------------------------------------------------------------------------------------
    # Bodies

    def test_info(self):
        # The example of API.md: 60 s of a release are gone, the firmware waits for "Update in Ordnung?"
        display = self.display
        display.access.switch_on(4651 * 1000)
        display.update_pending = True
        display.source = "stored"
        self.assertEqual(display.info_json(4711 * 1000), fixture("web_info.json"))
        # And what a request gets: the same writer, the state of now
        display.update_pending = False
        self.clock.advance(4711)
        status, body = self.call("GET", "/api/info")
        self.assertEqual(status, 200)
        self.assertEqual(body, display.info_json(4711 * 1000))
        self.assertIn("\"up\":4711,", body)
        self.assertNotIn("geheim123", body)

    def test_catalog(self):
        self.display.catalog = [["@BATT_V", "V", "", False, False]]
        self.assertEqual(self.call("GET", "/api/catalog"), (200, fixture("app_web_catalog_start.json")))
        self.display.catalog = [["@BATT_V", "V", "", False, True], ["SPEED", "km/h", "speed", True, True],
                                ["RPM", "rpm", "frequency", True, True], ["COOLANT", "°C", "temperature", True, True],
                                ["FUEL", "%", "none", True, True], ["INTAKE_TEMP", "°C", "temperature", True, True]]
        self.assertEqual(self.call("GET", "/api/catalog"), (200, fixture("app_web_catalog.json")))
        # The catalogue of the mock itself: the battery first, then the profile in its order
        names = re.findall(r"\"([^\"]+)\":\{\"unit\"", mock_display.Display().catalog_json())
        self.assertEqual(names[:3], ["@BATT_V", "ENGINE_RPM", "CHARGE_AIR_TEMP_PRE_IC"])
        self.assertEqual(len(names), 38)

    def test_values(self):
        display = self.display
        display.values = lambda now: [("ENGINE_RPM", 812.5, now), ("DPF_REGEN_STATUS", "on", now - 5000)]
        self.assertEqual(self.call("GET", "/api/values"), (200, fixture("web_values.json")))
        display.view = lambda: "ecu_offline"
        display.values = lambda now: [("COOLANT_TMP", 88.25, now), ("GLOW_ACTIVE", "off", now - 2999),
                                      ("@BATT_V", 12.4, now - 3000), ("ECU_DISTANCE", 187432, now),
                                      ("RAIL_PRESSURE", -0.0, now - 9999), ("OUTSIDE_TMP", -40, now),
                                      ("GONE", 1, now - 10000)]
        self.clock.advance(20)
        self.assertEqual(self.call("GET", "/api/values"), (200, fixture("web_values_mixed.json")))

    def test_values_of_the_mock_move_and_age(self):
        first = self.call("GET", "/api/values")[1]
        self.assertIn("\"view\":\"live\"", first)
        self.assertEqual(first.count("\"age\":\"fresh\""), 38)
        self.clock.advance(1)
        second = self.call("GET", "/api/values")[1]
        rpm = [re.search(r"\"ENGINE_RPM\":\{\"v\":([0-9.]+),", body).group(1) for body in (first, second)]
        self.assertNotEqual(rpm[0], rpm[1])
        # Fresh under 3 s, old under 10 s, then left out
        self.display.set_adapter(False)
        self.clock.advance(2.999)
        self.assertEqual(self.call("GET", "/api/values")[1].count("\"age\":\"fresh\""), 38)
        self.clock.advance(0.001)
        self.assertEqual(self.call("GET", "/api/values")[1].count("\"age\":\"old\""), 38)
        self.clock.advance(6.999)
        self.assertEqual(self.call("GET", "/api/values")[1].count("\"age\":\"old\""), 38)
        self.clock.advance(0.001)
        self.assertEqual(self.call("GET", "/api/values"), (200, "{\"view\":\"no_answer\",\"values\":{}}"))

    def test_layout_reports(self):
        with open(mock_display.BUILTIN_LAYOUT, "rb") as file:
            builtin = file.read()
        self.assertEqual(self.call("GET", "/api/layout"), (200, builtin.decode("utf-8")))
        for name, status, report in (("app_web_layout_refused.json", 400, "app_web_report_refused.json"),
                                     ("app_web_layout_warning.json", 200, "app_web_report_warning.json"),
                                     ("app_web_layout_hidden.json", 200, "app_web_report_hidden.json")):
            with self.subTest(name):
                body = (fixture(name) + "\n").encode("utf-8")
                self.assertEqual(self.call("PUT", "/api/layout?mode=check", body), (status, fixture(report)))
        self.assertEqual(self.call("PUT", "/api/layout?mode=check", builtin), (200, fixture("app_web_report_builtin.json")))
        # The check needs no release and changes nothing
        self.assertEqual(self.call("GET", "/api/layout"), (200, builtin.decode("utf-8")))
        self.assertIn("\"layout\":{\"name\":\"W906 OM651 Standard\",\"source\":\"builtin\"}", self.call("GET", "/api/info")[1])
        # "unknown" is empty while no catalogue is loaded
        self.display.catalog = self.display.catalog[:1]
        body = (fixture("app_web_layout_warning.json") + "\n").encode("utf-8")
        self.assertIn("\"unknown\":[]", self.call("PUT", "/api/layout?mode=check", body)[1])

    def test_layout_problems(self):
        def layout(page):
            return ("{\"format\":\"wican-display-layout\",\"v\":1,\"pages\":[%s]}" % page).encode("utf-8")

        def item(members):
            return layout("{\"items\":[{%s}]}" % members)

        refused = {
            b"{\"format\":": ("", "not valid JSON"),
            b"[]": ("", "not a JSON object"),
            b"{\"format\":\"wican-display-layout\",\"v\":1,\"pages\":[" + b"[" * 8 + b"]" * 8 + b"]}": ("", "not valid JSON"),
            b"{\"format\":\"other\",\"v\":1,\"pages\":[]}": ("format", "not wican-display-layout"),
            b"{\"format\":\"wican-display-layout\",\"v\":1.0,\"pages\":[]}": ("v", "not an integer"),
            b"{\"format\":\"wican-display-layout\",\"v\":2,\"pages\":[]}": ("v", "layout of a newer display"),
            b"{\"format\":\"wican-display-layout\",\"v\":0,\"pages\":[]}": ("v", "below 1"),
            b"{\"format\":\"wican-display-layout\",\"v\":1,\"name\":7,\"pages\":[]}": ("name", "not a text"),
            b"{\"format\":\"wican-display-layout\",\"v\":1}": ("pages", "missing"),
            b"{\"format\":\"wican-display-layout\",\"v\":1,\"pages\":{}}": ("pages", "not an array"),
            layout(""): ("pages", "empty"),
            layout(",".join(["{\"items\":[{\"key\":\"K\"}]}"] * 13)): ("pages", "too many pages"),
            layout("7"): ("pages[0]", "not an object"),
            layout("{\"title\":\"%s\",\"items\":[{\"key\":\"K\"}]}" % ("ä" * 12 + "x")): ("pages[0].title", "too long"),
            layout("{\"hidden\":1,\"items\":[{\"key\":\"K\"}]}"): ("pages[0].hidden", "not a boolean"),
            layout("{\"color\":1,\"items\":[{\"key\":\"K\"}]}"): ("pages[0].color", "not a text"),
            layout("{\"color\":\"#12345\",\"items\":[{\"key\":\"K\"}]}"): ("pages[0].color", "not #rrggbb"),
            layout("{\"color\":\"red\",\"items\":[{\"key\":\"K\",\"dec\":4}]}"): ("pages[0].items[0].dec", "not an integer 0 to 3"),
            layout("{\"color\":\"red\",\"items\":[{\"key\":\"K\"}]},{\"items\":[]}"): ("pages[0].color", "not #rrggbb"),
            layout("{}"): ("pages[0].items", "missing"),
            layout("{\"items\":[]}"): ("pages[0].items", "empty"),
            layout("{\"items\":[%s]}" % ",".join(["{\"key\":\"K\"}"] * 7)): ("pages[0].items", "too many items"),
            layout("{\"items\":[{\"key\":\"K\"}]},{\"items\":[{\"key\":\"A\"},{}]}"): ("pages[1].items[1].key", "missing"),
            item("\"key\":\"\""): ("pages[0].items[0].key", "empty"),
            item("\"key\":\"a\\u0007\""): ("pages[0].items[0].key", "control character"),
            item("\"key\":\"K\",\"label\":\"\\ud83d\""): ("pages[0].items[0].label", "half a surrogate pair"),
            item("\"key\":\"K\",\"unit\":\"123456789\""): ("pages[0].items[0].unit", "too long"),
            item("\"key\":\"K\",\"dec\":4"): ("pages[0].items[0].dec", "not an integer 0 to 3"),
            item("\"key\":\"K\",\"widget\":7"): ("pages[0].items[0].widget", "not a text"),
            item("\"key\":\"K\",\"scale\":0"): ("pages[0].items[0].scale", "zero"),
            item("\"key\":\"K\",\"min\":null"): ("pages[0].items[0].min", "not a number"),
            item("\"key\":\"K\",\"max\":1e999"): ("pages[0].items[0].max", "not finite"),
            item("\"key\":\"K\",\"min\":2,\"max\":2"): ("pages[0].items[0].min", "min is not below max"),
            item("\"key\":\"K\",\"map\":[]"): ("pages[0].items[0].map", "not an object"),
            item("\"key\":\"K\",\"map\":{%s}" % ",".join("\"%d\":\"x\"" % n for n in range(9))): ("pages[0].items[0].map", "too many entries"),
            item("\"key\":\"K\",\"map\":{\"1\":\"a\",\"2\":2}"): ("pages[0].items[0].map[1]", "not a text"),
            item("\"key\":\"K\",\"map\":{\"123456789012\":\"a\"}"): ("pages[0].items[0].map[0]", "too long"),
            # Of several problems the first in the order of layout.h is the one named
            item("\"dec\":9,\"key\":\"K\",\"label\":7"): ("pages[0].items[0].label", "not a text"),
        }
        for body, (path, problem) in refused.items():
            with self.subTest(body=body[:90]):
                self.assertEqual(self.call("PUT", "/api/layout?mode=check", body),
                                 (400, "{\"ok\":false,\"path\":\"%s\",\"problem\":\"%s\"}" % (path, problem)))
        # The first of two members with the same name counts, as for the reader of the display
        self.assertEqual(self.call("PUT", "/api/layout?mode=check", item("\"key\":\"K\",\"dec\":1,\"dec\":9"))[0], 200)
        # Taken with a warning; the first one is told, the others are counted
        status, body = self.call("PUT", "/api/layout?mode=check", (fixture("layout_warnings.json") + "\n").encode("utf-8"))
        self.assertEqual(status, 200)
        self.assertIn("\"warnings\":6,\"warning_path\":\"pages[0].items[1].widget\",\"warning\":\"unknown widget, shown as number\"", body)
        status, body = self.call("PUT", "/api/layout?mode=check", (fixture("layout_every_member.json") + "\n").encode("utf-8"))
        self.assertEqual((status, "\"warnings\":0," in body), (200, True))

    def test_layout_apply_save_reset(self):
        sent = (fixture("app_web_layout_hidden.json") + "\n").encode("utf-8")
        self.release()
        self.assertEqual(self.call("PUT", "/api/layout?mode=apply", sent), (200, fixture("app_web_report_hidden.json")))
        # The text that was sent, byte for byte; applied but not stored
        self.assertEqual(self.call("GET", "/api/layout"), (200, sent.decode("utf-8")))
        self.assertIn("\"layout\":{\"name\":\"Nichts zu sehen\",\"source\":\"preview\"}", self.call("GET", "/api/info")[1])
        # A restart ends the preview
        self.display.restart()
        self.clock.advance(5)
        self.assertIn("\"source\":\"builtin\"", self.call("GET", "/api/info")[1])

        self.release()
        self.assertEqual(self.call("PUT", "/api/layout?mode=save", sent)[0], 200)
        self.assertIn("\"source\":\"stored\"", self.call("GET", "/api/info")[1])
        self.display.restart()
        self.clock.advance(5)
        self.assertEqual(self.call("GET", "/api/layout"), (200, sent.decode("utf-8")))
        # A layout that is refused changes nothing
        self.release()
        self.assertEqual(self.call("PUT", "/api/layout?mode=save", b"{}")[0], 400)
        self.assertEqual(self.call("GET", "/api/layout"), (200, sent.decode("utf-8")))
        # The reset answers the report of the views the display then uses; its body is not looked at
        self.assertEqual(self.call("POST", "/api/layout/reset", b"whatever"), (200, fixture("app_web_report_builtin.json")))
        self.assertIn("\"source\":\"builtin\"", self.call("GET", "/api/info")[1])
        self.display.restart()
        self.clock.advance(5)
        self.assertIn("\"name\":\"W906 OM651 Standard\",\"source\":\"builtin\"", self.call("GET", "/api/info")[1])

    def test_dtc_last(self):
        self.display.set_lists("none")
        self.assertEqual(self.call("GET", "/api/dtc/last"), (200, fixture("web_dtc_last_none.json")))
        # The example of API.md, taken apart into its two lists
        example = fixture("web_dtc_last.json")
        read, rest = example[len("{\"read\":"):].split(",\"read_age_s\":95,\"before_clear\":")
        self.display.read, self.display.before_clear = read, rest[:-1]
        self.display.read_ms = self.display.now()
        self.clock.advance(95.999)
        self.assertEqual(self.call("GET", "/api/dtc/last"), (200, example))
        self.display.read = None
        self.assertEqual(self.call("GET", "/api/dtc/last"), (200, "{\"read\":null,\"read_age_s\":0,\"before_clear\":%s}" % rest[:-1]))
        # What the mock holds when it starts is a result of the adapter
        self.assertIn("{\"read\":{\"state\":\"done\",\"action\":\"read\",", mock_display.Display().dtc_last_json(0))

    def test_wifi(self):
        display = self.display
        display.profiles = [["Werkstatt", "geheim123", "192.168.1.50"], ["WiCAN_a1b2c3d4e5f6", "@meatpi#", ""], ["Camping", "", ""]]
        display.seen = [["Werkstatt", -52, True], ["", -60, True], ["WiCAN_a1b2c3d4e5f6", -40, True], ["Freifunk", -88, False]]
        self.assertEqual(self.call("GET", "/api/wifi"), (200, fixture("web_wifi.json")))
        display.profiles, display.seen, display.current = [], [], ""
        self.assertEqual(self.call("GET", "/api/wifi"), (200, fixture("web_wifi_empty.json")))
        self.assertIn("\"wifi\":{\"ssid\":\"\",\"ip\":\"\",\"rssi\":0,\"ap\":true,", self.call("GET", "/api/info")[1])
        display.profiles = [["WiCAN_", "x" * 8, ""]]
        self.assertIn("\"wican_ap\":false", self.call("GET", "/api/wifi")[1])

    def test_wifi_store_and_forget(self):
        display = self.display
        self.release()
        self.assertEqual(self.call("POST", "/api/wifi", (fixture("web_wifi_request.json")).encode("utf-8")), (202, fixture("app_web_asked_1.json")))
        self.assertEqual(display.asked_detail, "Werkstatt")
        self.clock.advance(2)
        self.assertEqual(display.press(), "wifi")
        self.assertEqual(self.call("GET", "/api/ticket?id=1"), (200, fixture("app_web_ticket_confirmed.json")))

        # Without a password the stored one is kept; for a new network it means an open one. A new one is
        # the first of the list, and only the knob stores anything.
        for body in (b"{\"ssid\":\"Werkstatt\",\"host\":\"\"}", b"{\"ssid\":\"Neu\"}"):
            self.assertEqual(self.call("POST", "/api/wifi", body)[0], 202)
            self.assertNotIn("Neu", self.call("GET", "/api/wifi")[1])
            self.clock.advance(2)
            display.press()
        self.assertEqual(display.profiles, [["Neu", "", ""], ["Werkstatt", "geheim123", ""]])
        answer = self.call("GET", "/api/wifi")[1]
        self.assertTrue(answer.startswith("{\"current\":\"Werkstatt\",\"profiles\":[{\"ssid\":\"Neu\",\"host\":\"\",\"password\":false,"))
        self.assertNotIn("geheim123", answer)
        # Up to 4 networks: with a fifth the last one falls out
        for name in ("A", "B", "C"):
            self.call("POST", "/api/wifi", ("{\"ssid\":\"%s\",\"password\":\"12345678\"}" % name).encode("utf-8"))
            self.clock.advance(2)
            display.press()
        self.assertEqual([profile[0] for profile in display.profiles], ["C", "B", "A", "Neu"])

        for body in (b"", b"[]", b"{\"password\":\"12345678\"}", b"{\"ssid\":\"\"}", b"{\"ssid\":7}", b"{\"ssid\":\"" + b"x" * 33 + b"\"}",
                     b"{\"ssid\":\"a\\nb\"}", b"{\"ssid\":\"N\",\"password\":\"1234567\"}", b"{\"ssid\":\"N\",\"password\":\"" + b"x" * 65 + b"\"}",
                     b"{\"ssid\":\"N\",\"host\":\"" + b"h" * 40 + b"\"}", b"{\"ssid\":\"N\",\"ssid\":\"\"}"):
            with self.subTest(body=body):
                self.assertEqual(self.call("POST", "/api/wifi", body), (400, fixture("app_web_body.json")))
        self.assertEqual(self.call("GET", "/api/ticket?id=7")[1], "{\"ticket\":7,\"state\":\"unknown\",\"left_s\":0}")

        display.profiles.append(["Camping", "", ""])
        forget = fixture("web_forget_request.json").encode("utf-8")
        self.assertEqual(self.call("POST", "/api/wifi/forget", forget), (200, fixture("app_web_ok.json")))
        self.assertEqual(self.call("POST", "/api/wifi/forget", forget), (404, fixture("app_web_not_found.json")))
        self.assertEqual(self.call("POST", "/api/wifi/forget", b"{\"host\":\"x\"}"), (400, fixture("app_web_body.json")))

    def test_settings(self):
        self.release()
        self.assertEqual(self.call("POST", "/api/settings", b"{}"), (200, fixture("app_web_settings_defaults.json")))
        self.assertEqual(self.call("POST", "/api/settings", b"{\"brightness\":40,\"reverse\":true,\"standby_s\":0,\"later\":[1]}"),
                         (200, fixture("app_web_settings_changed.json")))
        self.assertEqual(self.call("POST", "/api/settings", fixture("app_web_settings_browser.json").encode("utf-8")),
                         (200, fixture("app_web_settings_browser.json")))
        self.assertIn("\"settings\":" + fixture("app_web_settings_browser.json"), self.call("GET", "/api/info")[1])
        refused = {
            b"": "none", b"[]": "none", b"{\"brightness\":80": "none",
            b"{\"brightness\":4}": "brightness", b"{\"brightness\":101}": "brightness", b"{\"brightness\":80.0}": "brightness",
            b"{\"night\":\"25\"}": "night", b"{\"night_mode\":1}": "night_mode", b"{\"reverse\":null}": "reverse",
            b"{\"standby_s\":3601}": "standby_s", b"{\"standby_s\":-1}": "standby_s",
            # The first one in the order of the text, and nothing of the text is taken
            b"{\"night\":50,\"standby_s\":1e2,\"brightness\":0}": "standby_s",
            b"{\"night\":50,\"night\":4}": "night",
        }
        for body, member in refused.items():
            with self.subTest(body=body):
                self.assertEqual(self.call("POST", "/api/settings", body), (400, fixture("app_web_member_%s.json" % member)))
        self.assertEqual(self.call("POST", "/api/settings", b"{\"standby_s\":3600,\"brightness\":5}")[1],
                         "{\"brightness\":5,\"night\":10,\"night_mode\":true,\"reverse\":true,\"standby_s\":3600,\"web_open\":false}")

    # ------------------------------------------------------------------------------------------------
    # The second source: the Cerbo GX

    def test_cerbo_settings(self):
        display = self.display
        self.release()
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"192.168.88.2\"}"), (200, fixture("app_web_settings_cerbo.json")))
        self.assertEqual(display.settings_json(), fixture("app_web_settings_cerbo.json"))
        self.assertIn("\"settings\":" + fixture("app_web_settings_cerbo.json") + "}", self.call("GET", "/api/info")[1])
        # A member that is missing keeps its value, this one as the others
        self.assertEqual(self.call("POST", "/api/settings", b"{\"night\":25}"), (200, fixture("app_web_settings_cerbo.json")))
        refused = [b"{\"cerbo\":\"einstein\"}", b"{\"cerbo\":\"venus.local\"}", b"{\"cerbo\":\"192.168.88\"}", b"{\"cerbo\":\"192.168.88.2.1\"}",
                   b"{\"cerbo\":\"192.168.88.256\"}", b"{\"cerbo\":\"192.168.088.2\"}", b"{\"cerbo\":\"192.168.88.2 \"}", b"{\"cerbo\":\" \"}",
                   b"{\"cerbo\":\"192.168.88.2:1883\"}", b"{\"cerbo\":\"192.168.88.2\\n\"}", b"{\"cerbo\":3232258050}", b"{\"cerbo\":null}",
                   b"{\"cerbo\":false}", b"{\"cerbo\":[\"192.168.88.2\"]}",
                   # The first bad member in the order of the text, and nothing of the text is taken
                   b"{\"brightness\":50,\"cerbo\":\"192.168.88.256\",\"night\":4}", b"{\"cerbo\":\"192.168.88.9\",\"cerbo\":7}"]
        for body in refused:
            with self.subTest(body=body):
                self.assertEqual(self.call("POST", "/api/settings", body), (400, fixture("app_web_member_cerbo.json")))
        self.assertEqual(self.call("POST", "/api/settings", b"{\"night\":4,\"cerbo\":\"einstein\"}"), (400, fixture("app_web_member_night.json")))
        self.assertEqual(display.settings_json(), fixture("app_web_settings_cerbo.json"))
        for address in ("0.0.0.0", "255.255.255.255", "10.0.0.138"):
            with self.subTest(address=address):
                self.assertEqual(self.call("POST", "/api/settings", ("{\"cerbo\":\"%s\"}" % address).encode("ascii"))[1],
                                 fixture("app_web_settings_cerbo.json").replace("192.168.88.2", address))
        self.assertEqual(self.call("POST", "/api/settings", fixture("settings_longest_cerbo.json").replace("255,", "100,").replace("65535", "3600").encode("ascii")),
                         (200, fixture("settings_longest_cerbo.json").replace("255,", "100,").replace("65535", "3600")))
        # The empty text takes it away: the answer of a display that never had one, byte for byte
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"\",\"brightness\":80,\"night\":25,\"standby_s\":60}"),
                         (200, fixture("app_web_settings_defaults.json")))
        self.assertNotIn("cerbo", self.call("GET", "/api/info")[1])
        # The release is needed, the knob is not
        display.release(False)
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"192.168.88.2\"}"), (403, fixture("app_web_locked.json")))
        self.assertEqual(display.settings["cerbo"], "")

    def test_cerbo_answers(self):
        # The examples the tests of the firmware hold: a display with the address of a Cerbo two seconds after
        # its start, three bundles taken, the last one at 2040
        display = self.display
        display.access.switch_on(2100)
        display.settings["cerbo"] = "192.168.88.2"
        # ... whose views end with the pages of the Cerbo, as those of every display with the address
        display.follow_source()
        display.cerbo = "silent"
        display.cerbo_base = (("GX_BATT_SOC", 87.5), ("GX_BATT_V", 26.41), ("GX_BATT_I", -6.2), ("GX_SW_B2B", "on"), ("GX_SW_VALVE", "off"),
                              ("GX_TANK_FRESH", 77))
        display.cerbo_seen_ms = 2040
        display.cerbo_numbers = {"ok": 3, "connects": 1}
        self.assertEqual(display.info_json(2100), fixture("app_web_info_cerbo.json"))
        display.values = lambda now: [("@BATT_V", 12.4, 2000), ("SPEED", 0, 2000), ("RPM", 790, 2000), ("COOLANT", 81.5, 2000),
                                      ("FUEL", 62, 2000), ("INTAKE_TEMP", 19, 2000)]
        self.assertEqual(display.values_json(2100), fixture("app_web_values_cerbo.json"))
        self.assertEqual(display.values_json(4999), fixture("app_web_values_cerbo.json"))
        # Each source with its own limits: those of the adapter are old after 3 s, those of the Cerbo after 5
        self.assertEqual(display.values_json(5000), fixture("app_web_values_cerbo_fresh.json"))
        self.assertEqual(display.values_json(7039), fixture("app_web_values_cerbo_fresh.json"))
        self.assertIn("\"GX_BATT_SOC\":{\"v\":87.5,\"age\":\"old\"}", display.values_json(7040))
        # ... gone after 10 s, and after 60. The Cerbo counts as delivering for 10 s behind its last bundle.
        self.assertIn("{\"view\":\"live\",\"cerbo\":\"live\",\"values\":{\"GX_BATT_SOC\":{\"v\":87.5,\"age\":\"old\"},", display.values_json(12039))
        self.assertEqual(display.values_json(12040), fixture("app_web_values_cerbo_alone.json"))
        self.assertEqual(display.values_json(19999), fixture("app_web_values_cerbo_alone.json"))
        self.assertEqual(display.values_json(62039).count("\"age\":\"old\""), 6)
        self.assertEqual(display.values_json(62040).count("\"age\":"), 0)
        self.assertIn("\"values\":0,\"ok\":3,", display.info_json(62040))
        # And what a request gets: the same writers
        self.clock.advance(2.1)
        self.assertEqual(self.call("GET", "/api/values"), (200, fixture("app_web_values_cerbo.json")))

    def test_cerbo_views(self):
        # The story of the firmware (test_app_web.c, test_cerbo_views()): the views of a display with a Cerbo
        display = self.display
        with open(mock_display.BUILTIN_LAYOUT, "rb") as file:
            builtin = file.read()
        with open(mock_display.BUILTIN_GX_LAYOUT, "rb") as file:
            pages = file.read()
        whole = fixture("app_web_layout_cerbo.json")
        self.assertEqual(len(whole.encode("utf-8")), 4604)
        self.release()
        self.assertEqual(self.call("GET", "/api/layout"), (200, builtin.decode("utf-8")))
        # The address is given: the five pages of the Cerbo behind the views, as the one text the display writes
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"192.168.88.2\"}")[0], 200)
        self.assertEqual(self.call("GET", "/api/layout"), (200, whole))
        self.assertIn("\"layout\":{\"name\":\"W906 OM651 Standard + Cerbo\",\"source\":\"builtin\"}", self.call("GET", "/api/info")[1])
        # ... which the editor can send back: twelve pages, no key of the Cerbo unknown with the catalogue loaded
        self.assertEqual(self.call("PUT", "/api/layout?mode=check", whole.encode("utf-8")), (200, fixture("app_web_report_cerbo.json")))
        self.assertEqual(self.call("PUT", "/api/layout?mode=check", pages),
                         (200, "{\"ok\":true,\"name\":\"Cerbo\",\"pages\":5,\"items\":30,\"warnings\":0,\"warning_path\":\"\",\"warning\":\"\",\"unknown\":[]}"))
        mixed = (b"{\"format\":\"wican-display-layout\",\"v\":1,\"name\":\"M\",\"pages\":[{\"items\":[{\"key\":\"GX_BATT_SOC\"},{\"key\":\"NOBODY\"},"
                 b"{\"key\":\"GX\"},{\"key\":\"ENGINE_RPM\"},{\"key\":\"GX_NEVER_SENT\"}]}]}")
        self.assertEqual(self.call("PUT", "/api/layout?mode=check", mixed),
                         (200, "{\"ok\":true,\"name\":\"M\",\"pages\":1,\"items\":5,\"warnings\":0,\"warning_path\":\"\",\"warning\":\"\",\"unknown\":[\"NOBODY\",\"GX\"]}"))
        # Another address in its place changes nothing about the views; none takes the pages away
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"192.168.88.9\"}")[0], 200)
        self.assertEqual(self.call("GET", "/api/layout"), (200, whole))
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"\"}")[0], 200)
        self.assertEqual(self.call("GET", "/api/layout"), (200, builtin.decode("utf-8")))
        self.assertIn("\"layout\":{\"name\":\"W906 OM651 Standard\",\"source\":\"builtin\"}", self.call("GET", "/api/info")[1])
        # The reset answers the report of what is chosen: seven pages without the address, twelve with it
        self.assertEqual(self.call("POST", "/api/layout/reset", b""), (200, fixture("app_web_report_builtin.json")))
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"192.168.88.2\"}")[0], 200)
        self.assertEqual(self.call("POST", "/api/layout/reset", b""), (200, fixture("app_web_report_cerbo.json")))
        self.assertEqual(self.call("GET", "/api/layout"), (200, whole))
        # Views of the user are never appended to, and keep the pages of the Cerbo they have
        stored = (fixture("app_layout_stored.json") + "\n").encode("utf-8")
        self.assertEqual(self.call("PUT", "/api/layout?mode=save", stored)[0], 200)
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"\"}")[0], 200)
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"192.168.88.2\"}")[0], 200)
        self.assertEqual(self.call("GET", "/api/layout"), (200, stored.decode("utf-8")))
        self.assertEqual(self.call("PUT", "/api/layout?mode=save", whole.encode("utf-8")), (200, fixture("app_web_report_cerbo.json")))
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"\"}")[0], 200)
        self.assertEqual(self.call("GET", "/api/layout"), (200, whole))
        self.assertIn("\"layout\":{\"name\":\"W906 OM651 Standard + Cerbo\",\"source\":\"stored\"}", self.call("GET", "/api/info")[1])
        # A preview stays what was sent while the address comes
        self.assertEqual(self.call("PUT", "/api/layout?mode=apply", stored)[0], 200)
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"192.168.88.2\"}")[0], 200)
        self.assertEqual(self.call("GET", "/api/layout"), (200, stored.decode("utf-8")))
        # A restart with the address stored and no views of the user: the twelve pages from the start
        self.assertEqual(self.call("POST", "/api/layout/reset", b"")[0], 200)
        display.restart()
        self.clock.advance(mock_display.RESTART_MS / 1000 + 1)
        self.assertEqual(self.call("GET", "/api/layout"), (200, whole))

    def test_cerbo_catalog(self):
        # The story of the firmware (test_app_web.c, test_cerbo_catalog()): the other vehicle and a Cerbo
        display = self.display
        other = [["@BATT_V", "V", "", False, True], ["SPEED", "km/h", "speed", True, True], ["RPM", "rpm", "frequency", True, True],
                 ["COOLANT", "°C", "temperature", True, True], ["FUEL", "%", "none", True, True], ["INTAKE_TEMP", "°C", "temperature", True, True]]
        six = (("GX_BATT_SOC", 87.5), ("GX_BATT_V", 26.41), ("GX_BATT_I", -6.2), ("GX_SW_B2B", "on"), ("GX_SW_VALVE", "off"), ("GX_TANK_FRESH", 77))

        def entry(key):
            return "\"%s\":{\"unit\":\"\",\"class\":\"\",\"profile\":false,\"delivered\":true}" % key

        def with_keys(name, *keys):
            return fixture(name)[:-1] + "".join("," + entry(key) for key in keys) + "}"

        display.catalog = [list(row) for row in other]
        display.cerbo_base = six
        self.release()
        # Without the address the Cerbo is nobody's business, whatever it sends
        self.assertEqual(self.call("GET", "/api/catalog"), (200, fixture("app_web_catalog.json")))
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"192.168.88.2\"}")[0], 200)
        self.assertEqual(self.call("GET", "/api/catalog"), (200, fixture("app_web_catalog_cerbo.json")))
        # A key the Cerbo does not name any more stays, a new one comes behind - and not one whose value has no text
        display.cerbo_base = (("GX_T_BOILER", 61.1), ("GX_BATT_SOC", 87.4), ("GX_T_WATER", float("inf")))
        self.clock.advance(2)
        self.assertEqual(self.call("GET", "/api/values")[1].count("\"GX_"), 2)
        self.assertEqual(self.call("GET", "/api/catalog"), (200, with_keys("app_web_catalog_cerbo.json", "GX_T_BOILER")))
        # The Cerbo is gone and none of its values is left: its keys are
        display.set_cerbo("unreachable")
        self.clock.advance(70)
        self.assertEqual(self.call("GET", "/api/values")[1].count("\"GX_"), 0)
        self.assertEqual(self.call("GET", "/api/catalog"), (200, with_keys("app_web_catalog_cerbo.json", "GX_T_BOILER")))
        # Another address forgets them; the address again notes them anew, in the order of the bundle
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"192.168.88.9\"}")[0], 200)
        self.assertEqual(self.call("GET", "/api/catalog"), (200, fixture("app_web_catalog.json")))
        display.set_cerbo("sends")
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"192.168.88.2\"}")[0], 200)
        self.assertEqual(self.call("GET", "/api/catalog"), (200, with_keys("app_web_catalog.json", "GX_T_BOILER", "GX_BATT_SOC")))
        # A restart begins without any
        display.set_cerbo("silent")
        display.restart()
        self.clock.advance(5)
        self.assertEqual(self.call("GET", "/api/catalog"), (200, fixture("app_web_catalog.json")))
        self.release()
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"\"}")[0], 200)
        self.assertEqual(self.call("GET", "/api/catalog"), (200, fixture("app_web_catalog.json")))

        # The prefix is the Cerbo's: an entry of the adapter under it is written without the address, and left
        # out with it - before the Cerbo delivered anything, and as the key of the Cerbo once it did
        display.catalog = [list(row) for row in other] + [["GX_BATT_SOC", "", "", False, True]]
        self.assertEqual(self.call("GET", "/api/catalog"), (200, with_keys("app_web_catalog.json", "GX_BATT_SOC")))
        display.cerbo_base = six
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"192.168.88.2\"}")[0], 200)
        self.assertEqual(self.call("GET", "/api/catalog"), (200, fixture("app_web_catalog.json")))
        display.set_cerbo("sends")
        self.assertEqual(self.call("GET", "/api/catalog"), (200, fixture("app_web_catalog_cerbo.json")))

        # The room: 64 keys. A 65th takes the place of the oldest one the bundle did not bring.
        def names():
            return re.findall(r"\"(GX_[A-Z0-9_]+)\":\{", self.call("GET", "/api/catalog")[1])

        display.catalog = [list(row) for row in other]
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"192.168.88.9\"}")[0], 200)
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"192.168.88.2\"}")[0], 200)
        row = ["GX_K%02d" % number for number in range(70)]
        display.cerbo_base = tuple((key, 1) for key in row[:64])
        self.assertEqual(names(), row[:64])
        display.cerbo_base = tuple((key, 1) for key in row[:63] + ["GX_NEW"])
        self.assertEqual(names(), row[:63] + ["GX_NEW"])
        display.cerbo_base = (("GX_X1", 1),)
        self.assertEqual(names(), row[1:63] + ["GX_NEW", "GX_X1"])
        display.cerbo_base = (("GX_K01", float("inf")), ("GX_X2", "off"))
        self.assertEqual(names(), ["GX_K01"] + row[3:63] + ["GX_NEW", "GX_X1", "GX_X2"])
        # With 64 keys of 32 bytes the catalogue of the mock has room; one of the adapter that is far larger has none
        display.cerbo_base = tuple(("GX_KEY_OF_32_BYTES_%02d_ABCDEFGHIJ" % number, 1) for number in range(64))
        status, body = self.call("GET", "/api/catalog")
        self.assertEqual((status, len(body.encode("utf-8")) - len(fixture("app_web_catalog.json").encode("utf-8"))), (200, 5824))
        display.catalog = [["N%031d" % number, "u" * 11, "c" * 23, True, True] for number in range(96)]
        self.assertEqual(self.call("GET", "/api/catalog"), (200, display.catalog_json()))
        self.assertEqual(len(display.catalog_json()), 2 + 96 * (57 + 32 + 11 + 23) + 95 + 5824)
        display.catalog = [["\\" * 32, "u" * 11, "c" * 23, True, True] for number in range(96)]
        self.assertEqual(self.call("GET", "/api/catalog"), (500, "{\"error\":\"too_large\"}"))

    def test_cerbo_pages(self):
        display = self.display
        with open(mock_display.BUILTIN_GX_LAYOUT, "rb") as file:
            pages = file.read()
        self.assertEqual(len(pages), 2473)
        # The file as the firmware holds it: without the address and without the release ...
        self.assertEqual(self.call("GET", "/api/layout/gx"), (200, pages.decode("utf-8")))
        # ... a text the display takes, five pages
        self.assertEqual(self.call("PUT", "/api/layout?mode=check", pages),
                         (200, "{\"ok\":true,\"name\":\"Cerbo\",\"pages\":5,\"items\":30,\"warnings\":0,\"warning_path\":\"\",\"warning\":\"\",\"unknown\":[]}"))
        self.assertEqual(re.findall(r"\"title\":\"([^\"]*)\"", pages.decode("utf-8")), ["Batterie", "Energie", "Alternator", "Wasser", "Klima"])
        # ... with the address, where the views in use are another text, and under views of the user
        self.release()
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"192.168.88.2\"}")[0], 200)
        self.assertEqual(self.call("GET", "/api/layout/gx"), (200, pages.decode("utf-8")))
        self.assertEqual(self.call("GET", "/api/layout"), (200, fixture("app_web_layout_cerbo.json")))
        stored = (fixture("app_layout_stored.json") + "\n").encode("utf-8")
        self.assertEqual(self.call("PUT", "/api/layout?mode=save", stored)[0], 200)
        self.assertEqual(self.call("GET", "/api/layout/gx"), (200, pages.decode("utf-8")))
        self.assertEqual(self.call("GET", "/api/layout"), (200, stored.decode("utf-8")))
        # Read only, and only at this path
        self.assertEqual(self.call("POST", "/api/layout/gx", b"{}"), (405, "{\"error\":\"method\"}"))
        self.assertEqual(self.call("PUT", "/api/layout/gx?mode=save", stored), (405, "{\"error\":\"method\"}"))
        self.assertEqual(self.call("GET", "/api/layout/gx?mode=check"), (400, "{\"error\":\"query\"}"))
        for target in ("/api/layout/gx/", "/api/layout/GX", "/api/layout/g", "/api/gx"):
            self.assertEqual(self.call("GET", target), (404, "{\"error\":\"not_found\"}"), target)
        self.assertEqual(self.call("GET", "/api/layout/gx", host="example.org"), (403, "{\"error\":\"host\"}"))

    def test_cerbo_owns_its_prefix(self):
        # The story of the firmware (test_app_web.c): the other vehicle with a value named GX_BATT_SOC, a key
        # the Cerbo publishes as well. Without an address it is a value like every other ...
        display = self.display
        display.values = lambda now: [("@BATT_V", 12.4, 2000), ("SPEED", 0, 2000), ("GX_BATT_SOC", 12.5, 2000), ("RPM", 790, 2000),
                                      ("COOLANT", 81.5, 2000), ("FUEL", 62, 2000), ("INTAKE_TEMP", 19, 2000)]
        plain = display.values_json(2100)
        self.assertEqual(plain, fixture("app_web_values.json").replace("\"RPM\"", "\"GX_BATT_SOC\":{\"v\":12.5,\"age\":\"fresh\"},\"RPM\""))
        # ... with one the prefix is the Cerbo's: the key stands once, with the value of the Cerbo
        display.settings["cerbo"] = "192.168.88.2"
        display.cerbo = "silent"
        display.cerbo_base = (("GX_BATT_SOC", 87.5), ("GX_BATT_V", 26.41), ("GX_BATT_I", -6.2), ("GX_SW_B2B", "on"), ("GX_SW_VALVE", "off"),
                              ("GX_TANK_FRESH", 77))
        display.cerbo_seen_ms = 2040
        self.assertEqual(display.values_json(2100), fixture("app_web_values_cerbo.json"))
        self.assertEqual(display.values_json(2100).count("\"GX_BATT_SOC\""), 1)
        # ... without exception: also where the Cerbo has no value of that name, and where it has none at all
        display.cerbo_base = (("GX_TANK_FRESH", 77),)
        self.assertEqual(display.values_json(2100), fixture("app_web_values.json").replace("\"view\":\"live\"", "\"view\":\"live\",\"cerbo\":\"live\"")
                         .replace("}}}", "},\"GX_TANK_FRESH\":{\"v\":77,\"age\":\"fresh\"}}}"))
        display.cerbo_seen_ms = None
        self.assertEqual(display.values_json(2100), fixture("app_web_values.json").replace("\"view\":\"live\"", "\"view\":\"live\",\"cerbo\":\"quiet\""))
        # The address is taken away: the answer of before
        display.settings["cerbo"] = ""
        self.assertEqual(display.values_json(2100), plain)

    def test_cerbo_counts_what_it_names(self):
        # A number beyond a double (the Cerbo sent 1e999): held as infinite, named by no answer, not counted
        display = self.display
        display.settings["cerbo"] = "192.168.88.2"
        display.cerbo = "silent"
        display.cerbo_base = (("GX_BATT_SOC", 87.5), ("GX_BATT_V", float("inf")), ("GX_BATT_I", float("-inf")), ("GX_SW_B2B", "on"),
                              ("GX_SW_VALVE", "off"), ("GX_TANK_FRESH", 77))
        display.cerbo_seen_ms = 2040
        display.cerbo_numbers = {"ok": 3, "connects": 1}
        display.values = lambda now: [("@BATT_V", 12.4, 2000), ("SPEED", 0, 2000), ("RPM", 790, 2000), ("COOLANT", float("inf"), 2000),
                                      ("FUEL", 62, 2000), ("INTAKE_TEMP", 19, 2000)]
        values = display.values_json(2100)
        self.assertEqual(values, fixture("app_web_values_cerbo.json").replace("\"COOLANT\":{\"v\":81.5,\"age\":\"fresh\"},", "")
                         .replace("\"GX_BATT_V\":{\"v\":26.41,\"age\":\"fresh\"},", "").replace("\"GX_BATT_I\":{\"v\":-6.2,\"age\":\"fresh\"},", ""))
        self.assertEqual(values.count("\"GX_"), 4)
        self.assertNotIn("inf", values)
        self.assertIn("\"cerbo\":{\"host\":\"192.168.88.2\",\"view\":\"live\",\"values\":4,\"ok\":3,", display.info_json(2100))
        # The two agree at every age: four until the values are gone, then none
        for now in (2100, 7040, 62039, 62040):
            with self.subTest(now=now):
                named = display.values_json(now).count("\"GX_")
                self.assertEqual(named, 4 if now < 62040 else 0)
                self.assertIn("\"values\":%d,\"ok\":3," % named, display.info_json(now))

    def test_cerbo_comes_and_goes(self):
        display = self.display
        self.release()
        self.clock.advance(10)
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"192.168.88.2\"}")[0], 200)
        info = self.call("GET", "/api/info")[1]
        self.assertIn("\"cerbo\":{\"host\":\"192.168.88.2\",\"view\":\"live\",\"values\":8,\"ok\":1,\"refused\":0,\"connects\":1,\"boots\":1,\"lag_ms\":0},\"layout\"", info)
        values = self.call("GET", "/api/values")[1]
        self.assertTrue(values.startswith("{\"view\":\"live\",\"cerbo\":\"live\",\"values\":{\"@BATT_V\":"))
        self.assertEqual(values.count("\"GX_"), 8)
        self.assertEqual(values.count("\"age\":\"fresh\""), 38 + 8)
        # Every value of the Cerbo stands behind every value of the adapter
        self.assertLess(values.rindex("\"GLOW_ACTIVE\""), values.index("\"GX_BATT_SOC\""))
        self.clock.advance(5.5)
        self.assertIn("\"view\":\"live\",\"values\":8,\"ok\":6,", self.call("GET", "/api/info")[1])

        # Its flow falls silent: the values grow old, and the display says what it knows
        display.set_cerbo("silent")
        self.clock.advance(4.999)
        self.assertEqual(self.call("GET", "/api/values")[1].count("\"age\":\"fresh\""), 38 + 8)
        self.clock.advance(0.001)
        values = self.call("GET", "/api/values")[1]
        self.assertEqual((values.count("\"age\":\"fresh\""), values.count("\"age\":\"old\"")), (38, 8))
        self.assertIn("\"cerbo\":\"live\"", values)
        self.clock.advance(5)
        self.assertIn("\"cerbo\":\"quiet\"", self.call("GET", "/api/values")[1])
        self.clock.advance(9.999)
        self.assertIn("\"view\":\"quiet\",\"values\":8,\"ok\":6,", self.call("GET", "/api/info")[1])
        self.clock.advance(0.001)
        self.assertIn("\"view\":\"silent\",\"values\":8,\"ok\":6,", self.call("GET", "/api/info")[1])
        self.clock.advance(39.999)
        self.assertEqual(self.call("GET", "/api/values")[1].count("\"GX_"), 8)
        self.clock.advance(0.001)
        self.assertEqual(self.call("GET", "/api/values")[1].count("\"GX_"), 0)
        self.assertIn("\"view\":\"silent\",\"values\":0,\"ok\":6,", self.call("GET", "/api/info")[1])

        # It is gone: the display waits for fifteen seconds, then says so
        display.set_cerbo("unreachable")
        self.assertIn("\"cerbo\":{\"host\":\"192.168.88.2\",\"view\":\"connecting\",", self.call("GET", "/api/info")[1])
        self.clock.advance(14.999)
        self.assertIn("\"view\":\"connecting\",\"values\":0,", self.call("GET", "/api/info")[1])
        self.clock.advance(0.001)
        self.assertIn("\"view\":\"unreachable\",\"values\":0,\"ok\":6,\"refused\":0,\"connects\":1,", self.call("GET", "/api/info")[1])
        self.assertTrue(self.call("GET", "/api/values")[1].startswith("{\"view\":\"live\",\"cerbo\":\"unreachable\",\"values\":{\"@BATT_V\":"))
        # ... and back: a second session
        display.set_cerbo("sends")
        self.assertIn("\"view\":\"live\",\"values\":8,\"ok\":7,\"refused\":0,\"connects\":2,", self.call("GET", "/api/info")[1])

        # Another address: nothing listens there, and the values of the old one are gone at once
        self.release()
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"192.168.88.9\"}")[0], 200)
        values = self.call("GET", "/api/values")[1]
        self.assertEqual(values.count("\"GX_"), 0)
        self.assertIn("\"cerbo\":\"connecting\"", values)
        self.clock.advance(15)
        self.assertIn("\"cerbo\":{\"host\":\"192.168.88.9\",\"view\":\"unreachable\",\"values\":0,\"ok\":7,", self.call("GET", "/api/info")[1])
        # Outside of every network the display says that first
        display.current = ""
        self.assertIn("\"view\":\"no_wifi\"},\"cerbo\":{\"host\":\"192.168.88.9\",\"view\":\"no_wifi\",", self.call("GET", "/api/info")[1].replace("\"live\"", "\"no_wifi\"", 1))
        display.current = "Werkstatt"
        # No address: the display of before
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"\"}"), (200, fixture("app_web_settings_defaults.json")))
        self.assertNotIn("cerbo", self.call("GET", "/api/info")[1])
        self.assertNotIn("cerbo", self.call("GET", "/api/values")[1])
        self.assertNotIn("GX_", self.call("GET", "/api/values")[1])
        # A restart begins the source anew
        self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"192.168.88.2\"}")[0], 200)
        display.restart()
        self.clock.advance(5)
        self.assertIn("\"view\":\"live\",\"values\":8,\"ok\":1,\"refused\":0,\"connects\":1,", self.call("GET", "/api/info")[1])

    def test_no_cerbo_without_an_address(self):
        # A display nobody gave the address answers what it answered before there was a second source,
        # whatever a Cerbo in its network does
        for state in ("sends", "silent", "unreachable"):
            with self.subTest(cerbo=state):
                display = self.display_class(clock=self.clock)
                self.display = display
                display.lasting(False)
                display.set_cerbo(state)
                display.access.switch_on(4651 * 1000)
                display.update_pending = True
                display.source = "stored"
                self.assertEqual(display.info_json(4711 * 1000), fixture("web_info.json"))
                display.values = lambda now: [("ENGINE_RPM", 812.5, now), ("DPF_REGEN_STATUS", "on", now - 5000)]
                self.assertEqual(self.call("GET", "/api/values"), (200, fixture("web_values.json")))
                self.assertEqual(display.settings_json(), fixture("settings_web_closed.json"))
                self.release()
                self.assertEqual(self.call("POST", "/api/settings", b"{}"), (200, fixture("app_web_settings_defaults.json")))
                self.assertEqual(self.call("POST", "/api/settings", b"{\"cerbo\":\"\"}"), (200, fixture("app_web_settings_defaults.json")))

    # ------------------------------------------------------------------------------------------------
    # The release and the knob

    def test_release(self):
        locked = (403, fixture("app_web_locked.json"))
        changes = (("PUT", "/api/layout?mode=apply"), ("PUT", "/api/layout?mode=save"), ("POST", "/api/layout/reset"),
                   ("POST", "/api/wifi"), ("POST", "/api/wifi/forget"), ("POST", "/api/settings"), ("POST", "/api/reboot"),
                   ("POST", "/api/reset"), ("POST", "/api/ota"))
        for method, target in changes:
            with self.subTest(target):
                self.assertEqual(self.call(method, target, b"{}"), locked)
        self.assertEqual(self.call("GET", "/api/info")[1].count("\"release\":{\"open\":false,\"left_s\":0,\"lasting\":false}"), 1)

        # 10 minutes after it was given
        self.release()
        self.clock.advance(0.001)
        self.assertIn("\"release\":{\"open\":true,\"left_s\":600,\"lasting\":false}", self.call("GET", "/api/info")[1])
        self.clock.advance(599.998)
        self.assertIn("\"release\":{\"open\":true,\"left_s\":1,\"lasting\":false}", self.call("GET", "/api/info")[1])
        self.clock.advance(0.001)
        self.assertEqual(self.call("POST", "/api/settings", b"{}"), locked)

        # or after the last accepted change - also one that is then refused for another reason
        self.release()
        self.clock.advance(500)
        self.assertEqual(self.call("POST", "/api/settings", b"no settings")[0], 400)
        self.clock.advance(599)
        self.assertEqual(self.call("POST", "/api/wifi/forget", b"{\"ssid\":\"Nirgends\"}")[0], 404)
        self.clock.advance(599)
        # at the latest 30 minutes after it was switched on, whatever was changed since
        self.assertEqual(self.call("POST", "/api/settings", b"{}")[0], 200)
        self.assertIn("\"release\":{\"open\":true,\"left_s\":102,\"lasting\":false}", self.call("GET", "/api/info")[1])
        self.clock.advance(101.999)
        self.assertEqual(self.call("GET", "/api/values")[0], 200)
        self.assertIn("\"left_s\":1,\"lasting\":false}", self.call("GET", "/api/info")[1])
        self.clock.advance(0.001)
        self.assertEqual(self.call("POST", "/api/settings", b"{}"), locked)

        # A question to the knob that is not asked renews nothing, neither does a check
        self.release()
        self.clock.advance(500)
        self.assertEqual(self.call("POST", "/api/wifi", b"no network"), (400, fixture("app_web_body.json")))
        self.assertEqual(self.call("PUT", "/api/layout?mode=check", b"{}")[0], 400)
        self.clock.advance(100)
        self.assertEqual(self.call("POST", "/api/settings", b"{}"), locked)

        # It ends when it is switched off at the display, and with a restart
        self.release()
        self.display.release(False)
        self.assertEqual(self.call("POST", "/api/settings", b"{}"), locked)
        self.release()
        self.display.restart()
        self.clock.advance(5)
        self.assertEqual(self.call("POST", "/api/settings", b"{}"), locked)

    def test_release_for_good(self):
        # The display as it comes: the example of API.md, and every change without anybody at its knob
        display = self.delivered()
        display.update_pending = True
        display.source = "stored"
        self.assertEqual(display.info_json(4711 * 1000), fixture("web_info_lasting.json"))
        display.update_pending = False
        display.source = "builtin"
        lasting = "\"release\":{\"open\":true,\"left_s\":0,\"lasting\":true}"
        self.assertIn(lasting, self.call("GET", "/api/info")[1])
        self.assertEqual(display.settings_json(), fixture("settings_defaults.json"))
        self.assertEqual(self.call("POST", "/api/settings", b"{}"), (200, fixture("settings_defaults.json")))
        self.assertEqual(self.call("PUT", "/api/layout?mode=apply", fixture("app_layout_stored.json").encode("utf-8"))[0], 200)
        self.assertEqual(self.call("POST", "/api/layout/reset")[0], 200)
        # Hours and many changes later: a release of ten minutes would have ended with the fourth
        for _ in range(66):
            self.clock.advance(540)
            self.assertEqual(self.call("POST", "/api/settings", b"{\"brightness\":41}")[0], 200)
        self.clock.advance(7200)
        self.assertIn(lasting, self.call("GET", "/api/info")[1])
        self.assertEqual(self.call("POST", "/api/settings", b"{\"brightness\":80}"), (200, fixture("settings_defaults.json")))
        # The switch of the ten minutes does nothing to it, either way
        display.release(False)
        self.assertIn(lasting, self.call("GET", "/api/info")[1])
        display.release(True)
        self.clock.advance(1801)
        self.assertIn(lasting, self.call("GET", "/api/info")[1])
        self.assertEqual(self.call("POST", "/api/settings", b"{}")[0], 200)

        # What needs the knob needs it all the same: the question waits its 60 s and no longer
        self.assertEqual(self.ask_reset(), (202, fixture("app_web_asked_1.json")))
        self.assertEqual(self.call("GET", "/api/ticket?id=1")[1], "{\"ticket\":1,\"state\":\"waiting\",\"left_s\":60}")
        self.clock.advance(59.999)
        self.assertEqual(self.call("GET", "/api/ticket?id=1")[1], "{\"ticket\":1,\"state\":\"waiting\",\"left_s\":1}")
        self.clock.advance(0.001)
        self.assertEqual(self.call("GET", "/api/ticket?id=1")[1], "{\"ticket\":1,\"state\":\"expired\",\"left_s\":0}")
        self.assertIn(lasting, self.call("GET", "/api/info")[1])
        # An upload begins whenever it comes: the release for good always has the 5 minutes it asks for
        image = mock_display.firmware_image(version="0.2.0", size=300000)
        self.assertEqual(self.call("POST", "/api/ota", image), (202, fixture("app_web_asked_2.json")))
        display.press_long()
        self.assertEqual(display.version, "0.1.0")

        # Switched off at the display: closed at once, and a question that waits is refused
        self.assertEqual(self.ask_reset()[1], fixture("app_web_asked_3.json"))
        display.lasting(False)
        self.assertEqual(self.call("GET", "/api/ticket?id=3")[1], "{\"ticket\":3,\"state\":\"refused\",\"left_s\":0}")
        closed = "\"release\":{\"open\":false,\"left_s\":0,\"lasting\":false}"
        self.assertIn(closed, self.call("GET", "/api/info")[1])
        self.assertIn("\"settings\":" + fixture("settings_web_closed.json") + "}", self.call("GET", "/api/info")[1])
        self.assertEqual(self.call("POST", "/api/settings", b"{}"), (403, fixture("app_web_locked.json")))
        # Switched on over a release of ten minutes: its two ends are gone
        display.release(True)
        self.assertIn("\"release\":{\"open\":true,\"left_s\":600,\"lasting\":false}", self.call("GET", "/api/info")[1])
        display.lasting(True)
        self.clock.advance(1801)
        self.assertIn(lasting, self.call("GET", "/api/info")[1])
        self.assertEqual(self.call("POST", "/api/settings", b"{}")[0], 200)
        # ... and switched off again nothing of those ten minutes is left
        display.lasting(False)
        display.release(True)
        display.lasting(True)
        display.lasting(False)
        self.assertIn(closed, self.call("GET", "/api/info")[1])

        # What is stored decides at the start: a restart gives it again, or leaves the display closed
        display.lasting(True)
        display.restart()
        self.clock.advance(5)
        self.assertIn(lasting, self.call("GET", "/api/info")[1])
        self.assertEqual(self.call("POST", "/api/settings", b"{}")[0], 200)
        display.lasting(False)
        display.restart()
        self.clock.advance(5)
        self.assertIn(closed, self.call("GET", "/api/info")[1])
        self.assertEqual(self.call("POST", "/api/settings", b"{}"), (403, fixture("app_web_locked.json")))

    def test_settings_cannot_turn_the_switch(self):
        # The release for good is given and taken back at the display alone
        display = self.delivered()
        refused = (400, fixture("app_web_member_web_open.json"))
        for body in (b"{\"web_open\":false}", b"{\"brightness\":50,\"web_open\":false}", b"{\"web_open\":false,\"cerbo\":\"192.168.88.2\"}",
                     b"{\"web_open\":true,\"web_open\":false}", b"{\"web_open\":0}", b"{\"web_open\":\"false\"}", b"{\"web_open\":null}"):
            with self.subTest(body=body):
                self.assertEqual(self.call("POST", "/api/settings", body), refused)
                self.assertEqual(display.settings_json(), fixture("settings_defaults.json"))
                self.assertIn("\"release\":{\"open\":true,\"left_s\":0,\"lasting\":true}", self.call("GET", "/api/info")[1])
        # A text that is refused for another member is answered with that one
        self.assertEqual(self.call("POST", "/api/settings", b"{\"web_open\":false,\"brightness\":4}"), (400, fixture("app_web_member_brightness.json")))
        # Named as it is, or not at all, the text is taken as before: a page may send back what it got
        self.assertEqual(self.call("POST", "/api/settings", b"{\"web_open\":true}"), (200, fixture("settings_defaults.json")))
        self.assertEqual(self.call("POST", "/api/settings", b"{\"web_open\":false,\"web_open\":true}"), (200, fixture("settings_defaults.json")))
        self.assertEqual(self.call("POST", "/api/settings", fixture("settings_defaults.json").encode("utf-8")), (200, fixture("settings_defaults.json")))
        self.assertEqual(self.call("POST", "/api/settings", b"{\"web_open\":true,\"brightness\":40}")[1],
                         fixture("settings_defaults.json").replace("\"brightness\":80", "\"brightness\":40"))

        # ... in neither direction: a device in the WiFi does not make a release of ten minutes one for good
        display.lasting(False)
        display.release(True)
        self.clock.advance(100)
        self.assertEqual(self.call("POST", "/api/settings", b"{\"web_open\":true}"), refused)
        self.assertEqual(display.settings["web_open"], False)
        # The refused request renewed the release of ten minutes, as every refused change, and no more
        self.assertIn("\"release\":{\"open\":true,\"left_s\":600,\"lasting\":false}", self.call("GET", "/api/info")[1])
        self.assertEqual(self.call("POST", "/api/settings", b"{\"web_open\":false,\"brightness\":80}"), (200, fixture("settings_web_closed.json")))
        # Asked for again and again it is refused until the release ends, 30 minutes after it was given
        for _ in range(2):
            self.clock.advance(590)
            self.assertEqual(self.call("POST", "/api/settings", b"{\"web_open\":true}"), refused)
        self.clock.advance(590)
        self.assertEqual(self.call("POST", "/api/settings", b"{\"web_open\":true}"), (403, fixture("app_web_locked.json")))
        self.assertEqual(display.settings["web_open"], False)

    def test_tickets(self):
        display = self.display
        self.release()
        # The examples of API.md: the 17th question, 18 of its 60 seconds gone
        for _ in range(16):
            self.ask_reset()
            display.press_long()
        self.assertEqual(self.ask_reset(), (202, fixture("web_asked.json")))
        self.clock.advance(18)
        self.assertEqual(self.call("GET", "/api/ticket?id=17"), (200, fixture("web_ticket.json")))
        self.assertEqual(self.call("GET", "/api/ticket?id=017")[1], fixture("web_ticket.json"))
        # Known are the last ticket and the one before it
        self.assertEqual(self.call("GET", "/api/ticket?id=16")[1], "{\"ticket\":16,\"state\":\"refused\",\"left_s\":0}")
        self.assertEqual(self.call("GET", "/api/ticket?id=15")[1], "{\"ticket\":15,\"state\":\"unknown\",\"left_s\":0}")
        self.assertEqual(self.call("GET", "/api/ticket?id=18")[1], "{\"ticket\":18,\"state\":\"unknown\",\"left_s\":0}")
        # Nobody pressed the knob
        self.clock.advance(41.999)
        self.assertEqual(self.call("GET", "/api/ticket?id=17")[1], "{\"ticket\":17,\"state\":\"waiting\",\"left_s\":1}")
        self.clock.advance(0.001)
        self.assertEqual(self.call("GET", "/api/ticket?id=17")[1], "{\"ticket\":17,\"state\":\"expired\",\"left_s\":0}")
        self.assertIsNone(display.press())

    def test_ticket_ends(self):
        display = self.display
        self.release()
        self.assertEqual(self.ask_reset(), (202, fixture("app_web_asked_1.json")))
        self.assertEqual(self.call("GET", "/api/ticket?id=1"), (200, fixture("app_web_ticket_waiting.json")))
        # A press in the first 1.5 seconds does not count
        self.clock.advance(1.499)
        self.assertIsNone(display.press())
        self.assertEqual(self.call("GET", "/api/ticket?id=1")[1], "{\"ticket\":1,\"state\":\"waiting\",\"left_s\":59}")
        self.clock.advance(0.001)
        self.assertEqual(display.press(), "reset")
        # After a confirmed factory reset the display restarts and cannot answer any more
        self.assertIsNone(self.call("GET", "/api/ticket?id=1"))
        self.clock.advance(5)
        # The numbers start again at 1 after a restart; WiFi and settings are gone, the views stay
        self.assertEqual(self.call("GET", "/api/ticket?id=1")[1], fixture("app_web_ticket_unknown.json").replace("7", "1"))
        self.assertEqual(self.call("GET", "/api/wifi")[1][:27], "{\"current\":\"\",\"profiles\":[]")
        # ... and with the settings the switch "Dauerhaft frei": the display is as it came, released for good.
        # Its owner switches that off again.
        self.assertIn("\"release\":{\"open\":true,\"left_s\":0,\"lasting\":true}", self.call("GET", "/api/info")[1])
        self.assertIn("\"settings\":" + fixture("settings_defaults.json") + "}", self.call("GET", "/api/info")[1])
        display.lasting(False)

        # Refused at the display, also at once; the release goes on
        self.release()
        self.assertEqual(self.ask_reset(), (202, fixture("app_web_asked_1.json")))
        display.press_long()
        self.assertEqual(self.call("GET", "/api/ticket?id=1"), (200, fixture("app_web_ticket_refused.json")))
        # Refused because the release was switched off, or ended (the last minute of its 30)
        self.assertEqual(self.ask_reset()[1], fixture("app_web_asked_2.json"))
        display.release(False)
        self.assertEqual(self.call("GET", "/api/ticket?id=2")[1], "{\"ticket\":2,\"state\":\"refused\",\"left_s\":0}")
        self.release()
        for _ in range(3):
            self.clock.advance(590)
            self.assertEqual(self.call("POST", "/api/settings", b"{}")[0], 200)
        self.assertEqual(self.ask_reset()[1], fixture("app_web_asked_3.json"))
        self.assertEqual(self.call("GET", "/api/ticket?id=3")[1], "{\"ticket\":3,\"state\":\"waiting\",\"left_s\":30}")
        self.clock.advance(30)
        self.assertEqual(self.call("GET", "/api/ticket?id=3")[1], "{\"ticket\":3,\"state\":\"refused\",\"left_s\":0}")

    # ------------------------------------------------------------------------------------------------
    # The firmware

    def test_ota(self):
        display = self.display
        image = mock_display.firmware_image(version="0.2.0", size=300000)
        self.release()
        self.assertEqual(self.call("POST", "/api/ota", image), (202, fixture("app_web_asked_1.json")))
        # The display shows the version of the file and asks for the knob; only the press starts it
        self.assertEqual((display.asked_detail, display.version, display.uploading), ("0.2.0", "0.1.0", False))
        self.clock.advance(2)
        self.assertEqual(display.press(), "firmware")
        self.assertIsNone(self.call("GET", "/api/info"))
        self.clock.advance(5)
        info = self.call("GET", "/api/info")[1]
        self.assertIn("\"version\":\"0.2.0\",", info)
        self.assertIn("\"slot\":\"ota_1\",", info)
        self.assertIn("\"rolled_back\":false,\"update_pending\":true,", info)
        # No upload while the running firmware still waits for "Update in Ordnung?"
        self.release()
        self.assertEqual(self.call("POST", "/api/ota", image), (409, fixture("app_web_busy.json")))
        # Without an answer within 5 minutes the version before it runs again
        self.clock.advance(298.999)
        self.assertEqual(self.call("GET", "/api/values")[0], 200)
        self.clock.advance(0.001)
        self.assertIsNone(self.call("GET", "/api/info"))
        self.clock.advance(5)
        info = self.call("GET", "/api/info")[1]
        self.assertIn("\"version\":\"0.1.0\",", info)
        self.assertIn("\"slot\":\"ota_0\",", info)
        self.assertIn("\"rolled_back\":true,\"update_pending\":false,", info)

        # Confirmed at the display, it stays - also over a restart
        self.release()
        self.assertEqual(self.call("POST", "/api/ota", image)[0], 202)
        self.clock.advance(2)
        display.press()
        self.clock.advance(5)
        display.update_ok()
        display.restart()
        self.clock.advance(5)
        self.assertIn("\"version\":\"0.2.0\",", self.call("GET", "/api/info")[1])
        # and any restart before that takes it back
        self.release()
        self.assertEqual(self.call("POST", "/api/ota", mock_display.firmware_image(version="0.3.0", size=5000))[0], 202)
        self.clock.advance(2)
        display.press()
        self.clock.advance(5)
        display.restart()
        self.clock.advance(5)
        self.assertIn("\"version\":\"0.2.0\",", self.call("GET", "/api/info")[1])

    def test_ota_refused_files(self):
        good = mock_display.firmware_image(size=4000)
        files = {
            "too_short": good[:111],
            "no_image": b"\x00" + good[1:],
            "wrong_chip": mock_display.firmware_image(size=4000, chip=0x0005),
            "no_description": good[:32] + b"\x00\x00\x00\x00" + good[36:],
            "wrong_project": mock_display.firmware_image("wican-fw", "4.21", 4000),
        }
        self.release()
        for word, data in files.items():
            with self.subTest(word):
                self.assertEqual(self.call("POST", "/api/ota", data), (422, fixture("app_web_%s.json" % word)))
                self.assertEqual((self.display.uploading, self.display.access.ticket), (False, 0))
        # A project name without its end is no description either
        self.assertEqual(self.call("POST", "/api/ota", good[:80] + b"x" * 32 + good[112:])[1], fixture("app_web_no_description.json"))
        self.assertEqual(self.call("POST", "/api/ota", b""), (413, fixture("app_web_too_large.json")))
        self.assertEqual(self.call("POST", "/api/ota", good, length=SLOT + 1), (413, fixture("app_web_too_large.json")))
        self.assertEqual(self.call("POST", "/api/ota", mock_display.firmware_image(size=SLOT))[0], 202)

    def test_ota_that_breaks_or_comes_late(self):
        display = self.display
        image = mock_display.firmware_image(size=50000)
        # A broken upload
        self.release()
        self.assertEqual(self.call("POST", "/api/ota", image, read=reader(image[:20000])), (500, fixture("app_web_upload.json")))
        self.assertEqual((display.uploading, display.access.ticket), (False, 0))
        # One the display had ended (here: by a restart)
        self.assertEqual(self.call("POST", "/api/ota", image, read=reader(image, lambda: setattr(display, "uploading", False))),
                         (500, fixture("app_web_upload.json")))
        # The release has ended when the upload is complete: the file is not asked for
        self.assertEqual(self.call("POST", "/api/ota", image, read=reader(image, lambda: self.clock.advance(600))),
                         (403, fixture("app_web_locked.json")))
        self.assertEqual(display.access.ticket, 0)

        # An upload begins only while the release lasts another 5 minutes: not in the last 5 of its 30
        self.release()
        for _ in range(2):
            self.clock.advance(590)
            self.assertEqual(self.call("POST", "/api/settings", b"{}")[0], 200)
        # 1500.001 s after it was given 299.999 s are left, which are 300 rounded up
        self.clock.advance(320.001)
        self.assertEqual(self.call("POST", "/api/ota", image)[0], 202)
        display.press_long()
        self.clock.advance(0.999)
        self.assertEqual(self.call("POST", "/api/ota", image), (403, fixture("app_web_locked.json")))
        self.assertIn("\"release\":{\"open\":true,\"left_s\":299,\"lasting\":false}", self.call("GET", "/api/info")[1])
        self.release()
        self.assertEqual(self.call("POST", "/api/ota", image)[0], 202)
        # While a question waits no upload begins, while one runs no second one
        self.assertEqual(self.call("POST", "/api/ota", image), (409, fixture("app_web_asking.json")))
        display.press_long()
        display.uploading = True
        self.assertEqual(self.call("POST", "/api/ota", image), (409, fixture("app_web_busy.json")))

    # ------------------------------------------------------------------------------------------------
    # The refusals

    def test_refusals(self):
        display = self.display
        answers = {word: "{\"error\":\"%s\"}" % word for word in ("not_found", "method", "host", "header", "query", "length", "too_large",
                                                                    "busy", "asking", "hot", "body", "upload")}
        self.release()

        for target in ("/api/nothing", "/api/info/", "/API/INFO", "/api", "//api/info", "/api/info%3F", "/index.html", ""):
            self.assertEqual(self.call("GET", target), (404, answers["not_found"]), target)
        for method, target in (("POST", "/api/info"), ("GET", "/api/reboot"), ("PUT", "/api/wifi"), ("POST", "/api/layout"),
                               ("OPTIONS", "/api/layout"), ("DELETE", "/api/wifi"), ("HEAD", "/"), ("POST", "/")):
            self.assertEqual(self.call(method, target, b"{}"), (405, answers["method"]), (method, target))

        for host in (None, "", "wican-display", "wican-display.local.example.org", "example.org", "192.168.1.256", "192.168.1",
                     "192.168.01.1", "192.168.1.77:", "192.168.1.77:123456", " 192.168.1.77", "[::1]", "localhost"):
            self.assertEqual(self.call("GET", "/api/info", host=host), (403, answers["host"]), host)
        for host in ("wican-display.local", "WiCAN-Display.LOCAL:80", "10.0.0.1:8080", "0.0.0.0", "255.255.255.255:1"):
            self.assertEqual(self.call("GET", "/api/info", host=host)[0], 200, host)

        for header in (False, "2", "true", "11"):
            self.assertEqual(self.call("POST", "/api/reboot", header=header), (403, answers["header"]), header)
        self.assertEqual(self.call("PUT", "/api/layout?mode=check", b"{}", header=False), (403, answers["header"]))

        for target in ("/api/layout", "/api/layout?", "/api/layout?mode=", "/api/layout?mode=Check", "/api/layout?mode=check&mode=check",
                       "/api/layout?mode=check&x=1", "/api/layout?x=1&mode=check", "/api/layout?" + "m" * 64):
            self.assertEqual(self.call("PUT", target, b"{}"), (400, answers["query"]), target)
        for target in ("/api/ticket", "/api/ticket?id=", "/api/ticket?id=0", "/api/ticket?id=-1", "/api/ticket?id=1x",
                       "/api/ticket?id=4294967296", "/api/ticket?id=00000000001", "/api/ticket?id=1&id=1", "/api/ticket?ID=1",
                       "/api/info?x", "/api/wifi?id=1", "/?x=1"):
            self.assertEqual(self.call("GET", target), (400, answers["query"]), target)
        self.assertEqual(self.call("POST", "/api/reboot?now", b""), (400, answers["query"]))
        self.assertEqual(self.call("GET", "/api/ticket?id=4294967295")[0], 200)
        self.assertEqual(self.call("GET", "/api/info?")[0], 200)

        for method, target in (("PUT", "/api/layout?mode=check"), ("POST", "/api/layout/reset"), ("POST", "/api/reboot"), ("POST", "/api/ota")):
            self.assertEqual(self.call(method, target, length=False), (411, answers["length"]), target)
            self.assertEqual(self.call(method, target, length="12 "), (411, answers["length"]), target)

        self.assertEqual(self.call("PUT", "/api/layout?mode=check", b" " * 16385), (413, answers["too_large"]))
        self.assertEqual(self.call("PUT", "/api/layout?mode=check", b" " * 16384)[0], 400)
        for target in ("/api/layout/reset", "/api/wifi", "/api/wifi/forget", "/api/settings", "/api/reboot", "/api/reset"):
            self.assertEqual(self.call("POST", target, b" " * 513), (413, answers["too_large"]), target)
        self.assertEqual(self.call("POST", "/api/settings", b"{}" + b" " * 510)[0], 200)

        # Busy: the display reads or clears the fault memory, shows the clear dialog, or receives a firmware
        for busy in ("reading", "uploading"):
            setattr(display, busy, True)
            for target in ("/api/reboot", "/api/reset", "/api/ota"):
                self.assertEqual(self.call("POST", target, b"x"), (409, answers["busy"]), (busy, target))
            setattr(display, busy, False)
        # A question to the knob while a firmware is received
        display.uploading = True
        self.assertEqual(self.call("POST", "/api/wifi", b"{\"ssid\":\"N\"}"), (409, answers["busy"]))
        display.uploading = False
        # What makes the display leave its network, while it reads or clears the fault memory
        display.reading = True
        self.assertEqual(self.call("POST", "/api/wifi", b"{\"ssid\":\"N\"}"), (409, answers["busy"]))
        self.assertEqual(self.call("POST", "/api/wifi/forget", b"{\"ssid\":\"Werkstatt\"}"), (409, answers["busy"]))
        display.reading = False
        # A question to the knob, or the begin of an upload, while the heat keeps the screen dark
        display.set_hot(True)
        for target, body in (("/api/wifi", b"{\"ssid\":\"N\"}"), ("/api/reset", b""), ("/api/ota", mock_display.firmware_image(size=200))):
            self.assertEqual(self.call("POST", target, body), (409, answers["hot"]), target)
        display.set_hot(False)
        self.assertEqual(self.call("POST", "/api/wifi", b"{\"ssid\":\"N\"}")[0], 202)

        # A question to the knob, or the begin of an upload, while a question still waits
        for target, body in (("/api/wifi", b"{\"ssid\":\"N\"}"), ("/api/reset", b""), ("/api/ota", mock_display.firmware_image(size=200))):
            self.assertEqual(self.call("POST", target, body), (409, answers["asking"]), target)
        # What needs no knob goes on, and so does the restart: the question is lost with it
        self.assertEqual(self.call("POST", "/api/settings", b"{}")[0], 200)
        display.press_long()

        self.assertEqual(self.call("POST", "/api/wifi", b"{}"), (400, answers["body"]))
        self.assertEqual(self.call("POST", "/api/wifi/forget", b"{\"ssid\":\"Nirgends\"}"), (404, answers["not_found"]))
        self.assertEqual(self.call("POST", "/api/ota", b"x" * 200), (422, "{\"error\":\"no_image\"}"))

        # An answer that has no room in the 20480 bytes of the display; views that have no text
        display.catalog = [["\x01" * 32, "\x02" * 11, "\x03" * 23, True, True]] * 96
        self.assertEqual(self.call("GET", "/api/catalog"), (500, answers["too_large"]))
        display.layout = b""
        self.assertEqual(self.call("GET", "/api/layout"), (500, answers["too_large"]))
        image = mock_display.firmware_image(size=9000)
        self.assertEqual(self.call("POST", "/api/ota", image, read=reader(image[:8999])), (500, answers["upload"]))

        self.assertEqual(self.call("POST", "/api/reboot", b""), (200, "{\"ok\":true}"))
        self.assertIsNone(self.call("GET", "/api/info"))

    def test_refusals_in_the_documented_order(self):
        display = self.display
        image = mock_display.firmware_image(size=200)

        def status(method, target, body=b"{}", **how):
            answer = self.call(method, target, body, **how)
            return "%d %s" % (answer[0], re.search(r"\"error\":\"([a-z_]+)\"", answer[1]).group(1))

        # The release is closed, the display is busy and nothing is right about the request: one reason
        # after the other is taken away, and the next one of the table is the answer
        display.reading = True
        self.assertEqual(status("POST", "/api/nothing", host="example.org", header=False, length=False), "404 not_found")
        self.assertEqual(status("GET", "/api/reset?x=1", host="example.org"), "405 method")
        self.assertEqual(status("POST", "/api/reset?x=1", host="example.org", header=False, length=False), "403 host")
        self.assertEqual(status("POST", "/api/reset?x=1", header=False, length=False), "403 header")
        self.assertEqual(status("POST", "/api/reset?x=1", length=False), "403 locked")
        self.assertEqual(status("PUT", "/api/layout?mode=nothing", length=False), "403 locked")
        self.release()
        self.assertEqual(status("POST", "/api/reset?x=1", length=False), "400 query")
        self.assertEqual(status("PUT", "/api/layout?mode=nothing", length=False), "400 query")
        self.assertEqual(status("POST", "/api/reset", length=False), "411 length")
        self.assertEqual(status("POST", "/api/reset", b" " * 513), "413 too_large")
        self.assertEqual(status("POST", "/api/ota", b""), "413 too_large")
        self.assertEqual(status("POST", "/api/reset"), "409 busy")
        self.assertEqual(status("POST", "/api/ota", b"no firmware"), "409 busy")
        display.reading = False

        # Answered by the request itself: locked, busy, asking, hot, the rest
        self.assertEqual(self.ask_reset()[0], 202)
        # The heat with a question that waits and a busy display, as the device never has it: the heat
        # refuses the question that waits
        display.hot = True
        display.uploading = True
        self.assertEqual(status("POST", "/api/wifi", b"no network"), "409 busy")
        display.uploading = False
        display.reading = True
        self.assertEqual(status("POST", "/api/wifi", b"no network"), "409 busy")
        self.assertEqual(status("POST", "/api/wifi/forget", b"no network"), "409 busy")
        display.reading = False
        self.assertEqual(status("POST", "/api/wifi", b"no network"), "409 asking")
        self.assertEqual(status("POST", "/api/ota", b"no firmware"), "409 asking")
        display.press_long()
        display.uploading = True
        self.assertEqual(status("POST", "/api/wifi", b"no network"), "409 busy")
        display.uploading = False
        self.assertEqual(status("POST", "/api/wifi", b"no network"), "409 hot")
        self.assertEqual(status("POST", "/api/reset", b""), "409 hot")
        self.assertEqual(status("POST", "/api/ota", b"no firmware"), "409 hot")
        display.hot = False
        self.assertEqual(status("POST", "/api/wifi", b"no network"), "400 body")
        self.assertEqual(status("POST", "/api/ota", b"no firmware"), "422 too_short")
        large = mock_display.firmware_image(size=9000)
        self.assertEqual(status("POST", "/api/ota", large, read=reader(large[:8999])), "500 upload")
        self.assertEqual(status("POST", "/api/wifi/forget", b"no network"), "400 body")
        self.assertEqual(status("POST", "/api/wifi/forget", b"{\"ssid\":\"Nirgends\"}"), "404 not_found")
        # The begin of an upload: busy (a firmware that waits to be confirmed) before asking
        display.update_pending = True
        self.assertEqual(self.ask_reset()[0], 202)
        self.assertEqual(status("POST", "/api/ota", image), "409 busy")
        display.update_pending = False
        self.assertEqual(status("POST", "/api/ota", image), "409 asking")
        # and locked before asking: in the last five minutes of a release no upload begins
        display.press_long()
        self.release()
        for _ in range(2):
            self.clock.advance(590)
            self.assertEqual(self.call("POST", "/api/settings", b"{}")[0], 200)
        self.clock.advance(321)
        self.assertEqual(self.ask_reset()[0], 202)
        self.assertEqual(status("POST", "/api/ota", image), "403 locked")

    def test_busy_keeps_the_adapter(self):
        display = self.display
        busy = (409, fixture("app_web_busy.json"))
        self.release()
        display.profiles.append(["Camping", "", ""])
        # The display reads or clears the fault memory: nothing takes its network away from under it
        display.reading = True
        self.clock.advance(100)
        for body in (b"{\"ssid\":\"Camping\"}", b"{\"ssid\":\"Werkstatt\"}", b"{\"ssid\":\"Nirgends\"}", b"no network"):
            self.assertEqual(self.call("POST", "/api/wifi/forget", body), busy, body)
        self.assertEqual([profile[0] for profile in display.profiles], ["Werkstatt", "Camping"])
        self.assertIn("\"current\":\"Werkstatt\"", self.call("GET", "/api/wifi")[1])
        # The request found the release open and has renewed it, as every change that is refused as busy
        self.assertIn("\"release\":{\"open\":true,\"left_s\":600,\"lasting\":false}", self.call("GET", "/api/info")[1])
        self.clock.advance(100)
        for body in (b"{\"ssid\":\"Neu\",\"password\":\"12345678\"}", b"{\"ssid\":\"Werkstatt\",\"host\":\"10.0.0.9\"}", b"no network"):
            self.assertEqual(self.call("POST", "/api/wifi", body), busy, body)
        # A question that is not asked renews nothing, and there is no ticket
        self.assertIn("\"release\":{\"open\":true,\"left_s\":500,\"lasting\":false}", self.call("GET", "/api/info")[1])
        self.assertEqual(self.call("GET", "/api/ticket?id=1")[1], "{\"ticket\":1,\"state\":\"unknown\",\"left_s\":0}")
        self.assertIsNone(display.press())
        # What leaves the network alone goes on
        self.assertEqual(self.call("POST", "/api/settings", b"{}")[0], 200)
        self.assertEqual(self.call("PUT", "/api/layout?mode=apply", display.builtin)[0], 200)
        # The read has ended
        display.reading = False
        self.assertEqual(self.call("POST", "/api/wifi/forget", b"{\"ssid\":\"Camping\"}"), (200, fixture("app_web_ok.json")))
        self.assertEqual(self.call("POST", "/api/wifi", b"{\"ssid\":\"Neu\",\"password\":\"12345678\"}"), (202, fixture("app_web_asked_1.json")))

    def test_heat(self):
        display = self.display
        hot = (409, fixture("app_web_hot.json"))
        image = mock_display.firmware_image(size=5000)
        questions = (("/api/wifi", b"{\"ssid\":\"Neu\",\"password\":\"12345678\"}"), ("/api/reset", b""), ("/api/ota", image))
        self.release()
        self.assertIn("\"temp_c\":47,\"heat\":\"normal\"", self.call("GET", "/api/info")[1])
        # While the heat keeps the screen dark no upload begins: the display takes nothing of the file
        display.set_hot(True)
        self.assertEqual(self.call("POST", "/api/ota", image), hot)
        self.assertEqual((display.uploading, display.upload_version), (False, ""))
        display.set_hot(False)

        for number, (target, body) in enumerate(questions, 1):
            with self.subTest(target=target):
                # A question that waits when the heat switches the light off is refused at once
                self.assertEqual(self.call("POST", target, body)[0], 202)
                self.clock.advance(2)
                display.set_hot(True)
                self.assertEqual(self.call("GET", "/api/ticket?id=%d" % number)[1], "{\"ticket\":%d,\"state\":\"refused\",\"left_s\":0}" % number)
                self.assertIsNone(display.press())
                # ... and while it lasts none is asked and no upload begins
                self.assertEqual(self.call("POST", target, body), hot)
                self.assertEqual(self.call("GET", "/api/ticket?id=%d" % (number + 1))[1], "{\"ticket\":%d,\"state\":\"unknown\",\"left_s\":0}" % (number + 1))
                self.assertFalse(display.uploading)
                display.set_hot(False)
        self.assertEqual(display.profiles, [["Werkstatt", "geheim123", "192.168.1.50"]])
        self.assertEqual(display.version, "0.1.0")

        display.set_hot(True)
        self.assertIn("\"temp_c\":88,\"heat\":\"off\"", self.call("GET", "/api/info")[1])
        # A question that is not asked renews nothing; the begin of an upload is a change like every other
        self.clock.advance(100)
        self.assertEqual(self.call("POST", "/api/reset"), hot)
        self.assertEqual(self.call("POST", "/api/wifi", questions[0][1]), hot)
        self.assertIn("\"left_s\":500,\"lasting\":false}", self.call("GET", "/api/info")[1])
        self.assertEqual(self.call("POST", "/api/ota", image), hot)
        self.assertIn("\"left_s\":600,\"lasting\":false}", self.call("GET", "/api/info")[1])
        # What asks nothing at the display goes on
        self.assertEqual(self.call("POST", "/api/settings", b"{}")[0], 200)
        self.assertEqual(self.call("POST", "/api/wifi/forget", b"{\"ssid\":\"Nirgends\"}")[0], 404)
        self.assertEqual(self.call("PUT", "/api/layout?mode=apply", display.builtin)[0], 200)
        display.set_hot(False)

        # The heat comes while a firmware arrives: at its end the file is not asked for
        self.assertEqual(self.call("POST", "/api/ota", image, read=reader(image, lambda: display.set_hot(True))), hot)
        self.assertFalse(display.uploading)
        self.assertEqual(display.access.waiting(display.now()), None)
        # The update question has no answer that takes it back: it stays
        display.update_pending = True
        display.previous = ("0.0.9", "display-v0.0.9", "ota_1")
        display.set_hot(True)
        self.assertIn("\"update_pending\":true", self.call("GET", "/api/info")[1])
        # Cooled down, the display asks again
        display.set_hot(False)
        display.update_ok()
        self.assertEqual(self.call("POST", "/api/reset")[0], 202)

    def test_the_page_compressed(self):
        with open(PAGE, "rb") as file:
            page = file.read()
        packed = page_gz.compress(page)
        kind = "text/html; charset=utf-8"
        vary = ("Vary", "Accept-Encoding")

        def ask(accept, method="GET", target="/"):
            """The answer to a client that sent this Accept-Encoding (None: it sent none): status, type, which
            form of the page the body is, and the headers the answer has besides"""
            headers = {"host": HOST}
            if accept is not None:
                headers["accept-encoding"] = accept
            status, kind, body, more = self.display.handle(method, target, headers, reader(b""))
            return status, kind, {page: "as it is", packed: "compressed"}.get(body, "no page"), more

        # As it is without the header, for a list that does not name gzip, that refuses it, or that is no
        # list of codings: the answer says only that there is another form
        for accept in (None, "", "identity", "deflate, br", "*", "*;q=1", "x-gzip", "gzipx", "gzip;q=0", "gzip; q=0.000",
                       "GZIP;Q=0", "gzip, gzip;q=0", "identity;q=0", "gzip;q=2", "gzip;q=1.5", "gzip;q=0.5000",
                       "gzip;q=", "gzip;level=9", "gzip;q=0.5;x=1", "gzip deflate", "(x), gzip", ";q=1, gzip"):
            with self.subTest(accept=accept):
                self.assertEqual(ask(accept), (200, kind, "as it is", (vary,)))
        # Compressed for a client that names gzip, and named so
        for accept in ("gzip", "gzip, deflate", "gzip, deflate, br", "gzip, deflate, br, zstd", "GZip", ",gzip,",
                       "deflate, gzip;q=0.5", "gzip;q=1.000", "gzip ; q=0.001", "gzip;q=0.1, identity;q=1", "*;q=0, gzip"):
            with self.subTest(accept=accept):
                self.assertEqual(ask(accept), (200, kind, "compressed", (vary, ("Content-Encoding", "gzip"))))
        # ... into fewer bytes, which unpack to the page
        self.assertLess(len(packed), len(page))
        self.assertTrue(gzip.decompress(packed) == page)
        # A list of 63 bytes or more has no room in the display and is not read
        self.assertEqual(ask("gzip," + "br," * 19)[2], "compressed")
        self.assertEqual(ask("gzip, " + "br," * 19)[2:], ("as it is", (vary,)))
        self.assertEqual(ask("br," * 21 + "gzip")[2:], ("as it is", (vary,)))
        # No other answer has a second form
        self.assertEqual(ask("gzip", target="/api/info"), (200, "application/json", "no page", ()))
        self.assertEqual(ask("gzip", target="/api/nothing")[::3], (404, ()))
        self.assertEqual(ask("gzip", target="/?x=1")[::3], (400, ()))
        self.assertEqual(ask("gzip", method="HEAD")[::3], (405, ()))

    # ------------------------------------------------------------------------------------------------
    # The server

    def test_over_http(self):
        server = mock_display.Server(self.display).start()
        self.addCleanup(server.close)
        self.assertEqual(server.host, "127.0.0.1")

        def ask(method, target, headers, body=None):
            connection = http.client.HTTPConnection(server.host, server.port, timeout=5)
            self.addCleanup(connection.close)
            connection.putrequest(method, target, skip_host=True, skip_accept_encoding=True)
            for name, value in headers.items():
                connection.putheader(name, value)
            connection.endheaders(body)
            answer = connection.getresponse()
            return answer.status, dict((name.lower(), value) for name, value in answer.getheaders()), answer.read()

        own = {"Host": "%s:%d" % (server.host, server.port)}
        change = dict(own, **{"X-Display": "1"})
        with open(PAGE, "rb") as file:
            page = file.read()
        answers = [
            ask("GET", "/", own),
            ask("GET", "/api/info", own),
            ask("GET", "/api/nothing", own),
            # What a browser asks before a page of another origin may send the header: never answered with a yes
            ask("OPTIONS", "/api/layout", dict(own, Origin="http://example.org", **{"Access-Control-Request-Method": "PUT"})),
            ask("POST", "/api/settings", dict(change, **{"Content-Length": "2"}), b"{}"),
            ask("GET", "/api/info", {"Host": "localhost:%d" % server.port}),
            ask("GET", "/mock", own),
            ask("POST", "/mock/release?on", own),
            ask("POST", "/mock/release?on", change),
            ask("POST", "/api/reboot", change),
            ask("POST", "/api/ota", dict(change, **{"Content-Length": "5000"}), mock_display.firmware_image(size=5000)),
            ask("GET", "/mock/firmware?wican", own),
            # As a browser asks for the page
            ask("GET", "/", dict(own, **{"Accept-Encoding": "gzip, deflate"})),
        ]
        self.assertEqual([status for status, _, _ in answers], [200, 200, 404, 405, 403, 403, 200, 403, 200, 411, 202, 200, 200])
        self.assertEqual((answers[0][1]["content-type"], answers[0][2]), ("text/html; charset=utf-8", page))
        # The page in its two forms: each says that there is another, and the compressed one what it is
        self.assertEqual((answers[0][1].get("vary"), answers[0][1].get("content-encoding")), ("Accept-Encoding", None))
        self.assertEqual((answers[12][1].get("vary"), answers[12][1].get("content-encoding")), ("Accept-Encoding", "gzip"))
        self.assertEqual(answers[12][1]["content-type"], "text/html; charset=utf-8")
        self.assertEqual(answers[12][1]["content-length"], str(len(answers[12][2])))
        self.assertTrue(gzip.decompress(answers[12][2]) == page)
        self.assertEqual([headers.get("vary", headers.get("content-encoding")) for _, headers, _ in answers[1:12]], [None] * 11)
        self.assertEqual(answers[1][1]["content-type"], "application/json")
        self.assertEqual(answers[4][2], fixture("app_web_locked.json").encode("utf-8"))
        self.assertEqual(answers[5][2], b"{\"error\":\"host\"}")
        self.assertEqual(mock_display.ota_check(answers[11][2][:112], 1, 1), ("wrong_project", "4.21"))
        # The protection by the header rests on this: no answer ever allows another origin anything
        for _, headers, _ in answers:
            self.assertEqual([name for name in headers if name.startswith("access-control")], [])

    def test_the_strip_is_no_part_of_the_display(self):
        display = self.display
        own = {"host": HOST}
        self.assertEqual(display.control("GET", "/mock", own)[0], 200)
        self.assertEqual(display.control("GET", "/mock", {"host": "example.org"})[0], 403)
        self.assertEqual(display.control("POST", "/mock/release?on", own)[0], 403)
        own["x-display"] = "1"
        self.assertEqual(display.control("POST", "/mock/release?on", own)[0], 200)
        self.assertEqual(self.call("POST", "/api/reset")[0], 202)
        self.assertIn("\"question\": \"Werkseinstellungen?\"", display.control("GET", "/mock/state", own)[2].decode("utf-8"))
        # The heat takes the question from the screen
        state = display.control("POST", "/mock/hot?on", own)[2].decode("utf-8")
        self.assertIn("\"hot\": true", state)
        self.assertIn("\"question\": \"\"", state)
        self.assertIn("\"hot\": false", display.control("POST", "/mock/hot?off", own)[2].decode("utf-8"))
        self.assertEqual(self.call("POST", "/api/reset")[0], 202)
        display.control("POST", "/mock/skip?2", own)
        self.assertIn("\"down\": true", display.control("POST", "/mock/press", own)[2].decode("utf-8"))
        for target in ("/mock/skip?-5", "/mock/skip?x", "/mock/lists?all"):
            self.assertEqual(display.control("POST", target, own)[0], 400, target)
        self.assertEqual(display.control("POST", "/mock/nothing", own)[0], 404)
        # The display itself knows none of it
        self.clock.advance(5)
        self.assertEqual(self.call("GET", "/mock/state")[0], 404)


# ----------------------------------------------------------------------------------------------------
# The checks fail when they have to

def swapped(text, old, new):
    """The text with one thing changed - and an error if the thing is not there to be changed"""
    if old not in text:
        raise AssertionError("the page has no %r to break" % old)
    return text.replace(old, new, 1)


# For each check of the page: what is broken in a copy of its text - a piece of it and what stands there
# instead, or a function that makes the broken text
PAGE_MUTATIONS = {
    "request_problems": {
        "a path API.md does not name": ("api(\"GET\", \"/api/values\")", "api(\"GET\", \"/api/value\")"),
        "a method API.md does not name for the path": ("api(\"POST\", \"/api/reboot\")", "api(\"GET\", \"/api/reboot\")"),
        "a path that is put together": ("api(\"GET\", \"/api/wifi\")", "api(\"GET\", \"/api/\" + S.tab)"),
        "a path outside of api()": ("const SETTINGS = ", "const ELSEWHERE = \"/api/elsewhere\";\nconst SETTINGS = "),
        "a request left out": ("api(\"POST\", \"/api/layout/reset\")", "api(\"POST\", \"/api/reboot\")"),
        "the header only for POST": ("if(method !== \"GET\") xhr.setRequestHeader", "if(method === \"POST\") xhr.setRequestHeader"),
        "the header left out": ("\t\tif(method !== \"GET\") xhr.setRequestHeader(\"X-Display\", \"1\");\n", ""),
        "the header with another value": ("setRequestHeader(\"X-Display\", \"1\")", "setRequestHeader(\"X-Display\", \"0\")"),
        "a second way to send": ("poll();\n</script>", "fetch(\"/api/reboot\", {method: \"POST\"});\npoll();\n</script>"),
        "a second request object": ("poll();\n</script>", "new XMLHttpRequest().open(\"POST\", \"/api/reboot\");\npoll();\n</script>"),
        "a form": ("<main>", "<main><form method=\"post\"><button>x</button></form>"),
        "the mock": ("<main>", "<main><p>/mock</p>"),
    },
    "construct_problems": {
        "markup from a text": ("$(id).textContent = message || \"\";", "$(id).innerHTML = message || \"\";"),
        "markup next to a field": ("field.parentNode.insertBefore(",
                                   "field.insertAdjacentHTML(\"afterend\", message); field.parentNode.insertBefore("),
        "a written document": ("poll();\n</script>", "document.write(S.view);\npoll();\n</script>"),
        "evaluated text": ("data = JSON.parse(xhr.responseText);", "data = eval(\"(\" + xhr.responseText + \")\");"),
        "a function made of text": ("data = JSON.parse(xhr.responseText);", "data = new Function(\"return \" + xhr.responseText)();"),
        "a timer with text": ("setTimeout(poll, 3000);", "setTimeout(\"poll()\", 3000);"),
        "a handler as an attribute": ("<button id=\"reload\">", "<button id=\"reload\" onclick=\"location.reload()\">"),
        "a handler set as an attribute": ("node.addEventListener(\"click\", click);", "node.setAttribute(\"onclick\", click);"),
        "a handler as a property": ("node.addEventListener(\"click\", click);", "node.onclick = click;"),
        "a script address": ("<main>", "<main><p title=\"javascript:alert(1)\"></p>"),
        "a text the reader of the script does not see": ("poll();\n</script>", "api('GET', '/api/elsewhere');\npoll();\n</script>"),
        "a second script": ("</body>", "<script>S.view = 1;</script></body>"),
    },
    "external_problems": {
        "a style sheet": ("<title>", "<link rel=\"stylesheet\" href=\"style.css\"><title>"),
        "a script from elsewhere": ("<title>", "<script src=\"https://example.org/x.js\"></script><title>"),
        "a picture": ("<main>", "<main><img src=\"logo.png\" alt=\"\">"),
        "a font": ("*{box-sizing:border-box}", "@font-face{font-family:x;src:url(x.woff2)}*{box-sizing:border-box}"),
        "an address in a text": ("const SETTINGS = ", "const HELP = \"http://example.org\";\nconst SETTINGS = "),
        "an address without a scheme": ("const SETTINGS = ", "const HELP = \"//example.org/x\";\nconst SETTINGS = "),
        "a link that leads away": ("href: URL.createObjectURL(new Blob([json], {type: \"application/json\"}))", "href: S.view"),
    },
    "release_problems": {
        "a release for good with the end its 0 seconds give": ("release.lasting === true ? Infinity : ", ""),
        "lasting taken for true whatever it is": ("release.lasting === true ? Infinity", "release.lasting ? Infinity"),
        "a release for good that ends after a day": ("release.lasting === true ? Infinity", "release.lasting === true ? Date.now() + 86400000"),
        "a closed release that lasts": ("release.open !== true ? 0 : release.lasting === true ? Infinity", "release.lasting === true ? Infinity : release.open !== true ? 0"),
        "a countdown of what has no end": ("(isFinite(seconds) ? \", noch \"", "(seconds ? \", noch \""),
        "the bar always with a time": ("\"Web-Zugriff frei\" + (isFinite(seconds) ? \", noch \" + ", "\"Web-Zugriff frei, noch \" + (isFinite(seconds) ? "),
        "a second place that sets the end": ("poll();\n</script>", "S.until = 0;\npoll();\n</script>"),
        "the buttons with a time of their own": ("left() <= 0 ? \"erst am Display", "S.until <= Date.now() ? \"erst am Display"),
    },
    "cerbo_problems": {
        "no field": ("<input id=\"cerbo\" maxlength=\"15\"", "<input id=\"kerbo\" maxlength=\"15\""),
        "a field for numbers": ("<input id=\"cerbo\" maxlength=\"15\"", "<input id=\"cerbo\" type=\"number\" maxlength=\"15\""),
        "a field for a name": ("<input id=\"cerbo\" maxlength=\"15\"", "<input id=\"cerbo\" maxlength=\"39\""),
        "a field that is never locked": ("spellcheck=\"false\" class=\"chg\"></label>\n\t<p id=\"cerbonow\"", "spellcheck=\"false\"></label>\n\t<p id=\"cerbonow\""),
        "a field that does not say how to switch off": ("(IP-Adresse des MQTT-Brokers, leer = aus)", "(IP-Adresse des MQTT-Brokers)"),
        "not one of the settings": ("\"standby_s\", \"cerbo\"];", "\"standby_s\"];"),
        "sent as a number": ("input.type === \"text\" ? input.value.trim() : Number(input.value)", "Number(input.value)"),
        "an emptied address is not sent": ("if(input.value !== \"\" || input.type === \"text\") r =", "if(input.value !== \"\") r ="),
        "a line on a display without a Cerbo": ("$(\"cerbonow\").hidden = !isObject(cerbo);", "$(\"cerbonow\").hidden = false;"),
        "a line that is there from the start": ("<p id=\"cerbonow\" class=\"dim\" hidden></p>", "<p id=\"cerbonow\" class=\"dim\"></p>"),
        "read where there is no Cerbo": ("\tif(isObject(cerbo)) $(\"cerbonow\").textContent = ", "\t$(\"cerbonow\").textContent = "),
        "read a second time, unasked": ("\tconst up = Number(info.up);", "\tconst up = Number(info.up) + cerbo.ok;"),
        "a word without its text": (" quiet: \"wartet\",", ""),
        "a member API.md does not name": ("text(cerbo.connects)", "text(cerbo.sessions)"),
        "a number as it came": ("text(cerbo.lag_ms)", "cerbo.lag_ms"),
        "the word as it came": ("word(CERBO, cerbo.view)", "cerbo.view"),
        "a line without the address": ("\"Cerbo \" + text(cerbo.host) + \": \" + ", "\"Cerbo: \" + "),
    },
    "color_problems": {
        "no test of the form": ("const isColor = value => /^#[0-9a-f]{6}$/i.test(value);", "const isColor = value => /^#[0-9a-f]{3,6}$/i.test(value);"),
        "a picker given any colour": ("value: isColor(page.color) ? page.color : \"#ffffff\"", "value: page.color"),
        "a box that is never ticked": ("checked: \"color\" in page}", "checked: false}"),
        "a field the display does not name": ("color.dataset.path = \"pages[\" + S.page + \"].color\";", "color.dataset.path = \"pages[\" + S.page + \"].items\";"),
        "a colour that cannot be taken away": ("paint(own.checked && color.value)", "paint(color.value)"),
        "a picked colour that leaves the box empty": ("\t\town.checked = true;\n\t\tpaint(color.value);", "\t\tpaint(color.value);"),
    },
    "size_problems": {
        "one byte too many": lambda page: page + " " * (SIZE_MAX - len(page.encode("utf-8"))),
    },
    "compressed_problems": {
        "text that does not compress": lambda page: page + noise(8192),
    },
}

# For editor_marks(): what is broken in a copy of the page, as above
EDITOR_MUTATIONS = {
    "the note at every key the catalogue does not hold": (" && !text(item.key).startsWith(\"GX_\")", ""),
    "the note at the keys of the Cerbo alone": (" && !text(item.key).startsWith(\"GX_\")", " && text(item.key).startsWith(\"GX_\")"),
    "a prefix of two letters": (".startsWith(\"GX_\")", ".startsWith(\"GX\")"),
    "the prefix anywhere in the key": (".startsWith(\"GX_\")", ".includes(\"GX_\")"),
    "the note before the catalogue is loaded": ("catalogLoaded() && !has(S.catalog, item.key) && ", "!has(S.catalog, item.key) && "),
    "the note a second time, at every value": ("const up = button(", "label.appendChild(el(\"span\", \"note\", [\" · nicht im Katalog: das Display zeigt „n. v.“\"]));\n\t\tconst up = button("),
}


# For editor_offers(): what is broken in a copy of the page, as above
OFFER_MUTATIONS = {
    "no button": ("<button id=\"gxadd\" hidden>", "<button id=\"gxmore\" hidden>"),
    "a button that is there from the start": ("<button id=\"gxadd\" hidden>", "<button id=\"gxadd\">"),
    "a button on every display": ("!isObject(part(S, \"info\").cerbo) || !S.gx || !S.gx.pages.length", "!S.gx || !S.gx.pages.length"),
    "a button on a display that has no pages of the Cerbo": ("!isObject(part(S, \"info\").cerbo) || !S.gx || !S.gx.pages.length", "!isObject(part(S, \"info\").cerbo) || !S.gx"),
    "a button that is shown once and never follows": ("\t$(\"gxadd\").hidden = !isObject(part(S, \"info\").cerbo) || !S.gx || !S.gx.pages.length;\n}",
                                                      "}\n$(\"gxadd\").hidden = !isObject(part(S, \"info\").cerbo) || !S.gx || !S.gx.pages.length;"),
    "the pages asked for on every display": ("if(isObject(info.cerbo) && !S.gx) await loadGx();", "if(!S.gx) await loadGx();"),
    "the pages asked for with every look at the display": ("if(isObject(info.cerbo) && !S.gx) await loadGx();", "if(isObject(info.cerbo)) await loadGx();"),
    "the pages asked for a second time, unasked": ("poll();\n</script>", "loadGx();\npoll();\n</script>"),
    "a display without the pages is asked again and again": ("if(r.status) S.gx = r.status === 200 && adopt(r.text) || {pages: []};", "if(r.status === 200) S.gx = adopt(r.text);"),
    "the pages as they came, not read as a layout": ("S.gx = r.status === 200 && adopt(r.text) || {pages: []};", "S.gx = r.status === 200 && r.body || {pages: []};"),
    "the pages set from elsewhere": ("poll();\n</script>", "S.gx = S.layout;\npoll();\n</script>"),
    "the pages themselves appended, not copies": ("const more = JSON.parse(JSON.stringify(S.gx.pages));", "const more = S.gx.pages;"),
    "the views in use appended": ("const more = JSON.parse(JSON.stringify(S.gx.pages));", "const more = JSON.parse(JSON.stringify(S.layout.pages));"),
    "a thirteenth page": ("\tif(over > 0)\n", "\tif(over > 1)\n"),
    "pages beyond the limit appended without a word": ("const over = pages.length + more.length - PAGES_MAX;", "const over = 0;"),
    "a word, and the pages appended all the same": (" löschen.\", \"bad\");\n\t\treturn;\n", " löschen.\", \"bad\");\n"),
    "refused without a word": ("\t\ttell(\"vmsg\", \"Die \" + more.length + \" Cerbo-Seiten passen nicht mehr", "\t\tconsole.log(\"Die \" + more.length + \" Cerbo-Seiten passen nicht mehr"),
    "a limit of thirteen pages": ("const PAGES_MAX = 12;", "const PAGES_MAX = 13;"),
    "the pages in front of the ones of the editor": ("more.forEach(page => pages.push(page));", "more.reverse().forEach(page => pages.unshift(page));"),
    "pages the display is never asked to check": ("more.forEach(page => pages.push(page));\n\tdraw();\n\tchanged();", "more.forEach(page => pages.push(page));\n\tdraw();"),
    "pages the editor does not draw": ("more.forEach(page => pages.push(page));\n\tdraw();\n\tchanged();", "more.forEach(page => pages.push(page));\n\tchanged();"),
    "the button without views": ("gxadd: () => S.layout && S.gx && gxAppend(),", "gxadd: () => S.gx && gxAppend(),"),
    "the button does nothing": ("gxadd: () => S.layout && S.gx && gxAppend(),", "gxadd: () => S.layout && S.gx,"),
    "a value of the Cerbo without what its pages say": ("S.item = gxItem(name) || {key: name, map: []};", "S.item = {key: name, map: []};"),
    "the value of the pages itself, not a copy": ("return found ? JSON.parse(JSON.stringify(found)) : null;", "return found || null;"),
    "the first value of the pages for every key": (".find(item => item.key === key);", ".find(item => item.key);"),
    "the pages read where there are none": ("(S.gx ? S.gx.pages : [])", "S.gx.pages"),
    "a catalogue that never learns of the keys of the Cerbo": (" || part(info, \"cerbo\").values !== part(before, \"cerbo\").values) loadCatalog();", ") loadCatalog();"),
}


def noise(count):
    """Letters and digits without any order in them, the same with every call"""
    text = ""
    while len(text) < count:
        text += base64.b64encode(hashlib.sha256(text.encode("ascii")).digest()).decode("ascii")
    return text[:count]


class WrongRoute(mock_display.Display):
    """Looks at the header before the host"""

    def route(self, method, path, query, headers, now):
        if method != "GET" and headers.get("x-display") != "1" and any(entry[:2] == (method, path) for entry in mock_display.ROUTES):
            return None, (403, "header")
        return super().route(method, path, query, headers, now)


class WrongStage(mock_display.Display):
    """Looks at the question before the busy display"""

    def _ask_refused(self, busy, now):
        if self.access.is_open(now) and self.access.waiting(now) is not None:
            return 409, mock_display.error_body("asking")
        return super()._ask_refused(busy, now)


class NoHostRule(mock_display.Display):
    def route(self, method, path, query, headers, now):
        return super().route(method, path, query, dict(headers, host="192.168.1.77"), now)


class WrongInfo(mock_display.Display):
    def info_json(self, now):
        return super().info_json(now).replace("\"up\":", "\"uptime\":")


class WrongCatalog(mock_display.Display):
    def catalog_json(self):
        return super().catalog_json().replace("\"delivered\":true", "\"delivered\":false")


class ReportsNothingUnknown(mock_display.Display):
    def report_json(self, layout):
        return re.sub(r"\"unknown\":\[.*\]", "\"unknown\":[]", super().report_json(layout))


class WrongLists(mock_display.Display):
    def dtc_last_json(self, now):
        return super().dtc_last_json(now).replace("\"read_age_s\":95", "\"read_age_s\":96")


class TellsPasswords(mock_display.Display):
    def wifi_json(self):
        return super().wifi_json().replace("\"password\":false", "\"password\":true")


class TakesNamesForTheCerbo(mock_display.Display):
    """Takes every text as the address of the Cerbo"""

    def _settings(self, data, now=None):
        with mock.patch.object(mock_display, "ADDRESS", re.compile(r".*", re.DOTALL)):
            return super()._settings(data, now)


class WritesAnEmptyAddress(mock_display.Display):
    def settings_json(self):
        text = super().settings_json()
        return text if "cerbo" in text else text[:-1] + ",\"cerbo\":\"\"}"


class NamesTheCerboAlways(mock_display.Display):
    """The object and the member of the second source also on a display without its address"""

    def cerbo_json(self, now):
        return super().cerbo_json(now) or "\"cerbo\":{\"host\":\"\",\"view\":\"off\",\"values\":0,\"ok\":0,\"refused\":0,\"connects\":0,\"boots\":0,\"lag_ms\":0},"


class NamesTheViewOfTheCerboAlways(mock_display.Display):
    def values_json(self, now):
        text = super().values_json(now)
        return text if "\"cerbo\":" in text else text.replace(",\"values\":{", ",\"cerbo\":\"off\",\"values\":{", 1)


class WrongCerbo(mock_display.Display):
    def cerbo_json(self, now):
        return super().cerbo_json(now).replace("\"connects\":", "\"sessions\":")


class AppendsToViewsOfTheUser(mock_display.Display):
    """Chooses the views anew when the second source is switched, whatever views are in use"""

    def follow_source(self):
        self._use(self.chosen(), "builtin")


class KeepsThePagesOfTheCerbo(mock_display.Display):
    """Does not take the pages of the Cerbo away with its address"""

    def follow_source(self):
        if self.settings["cerbo"] != "":
            super().follow_source()


class ResetsToTheVehicleAlone(mock_display.Display):
    """The reset of the views forgets the pages of the Cerbo"""

    def _layout(self, route, data):
        if route != "layout_reset":
            return super()._layout(route, data)
        self.stored = None
        self._use(self.builtin, "builtin")
        return 200, self.report_json(dict(mock_display.check_layout(self.builtin), warnings=[]))


class NamesOnlyWhatTheCerboSendsNow(mock_display.Display):
    """The catalogue with the keys of the last bundle alone"""

    def catalog_json(self):
        self.cerbo_named = [name for name in self.cerbo_named if name in dict(self.cerbo_base)]
        return super().catalog_json()


class NamesTheKeysWithoutAnAddress(mock_display.Display):
    """The keys of a Cerbo in the catalogue of a display nobody gave its address"""

    def catalog_json(self):
        text = super().catalog_json()
        if self.settings["cerbo"] != "":
            return text
        return text[:-1] + "".join(",\"%s\":{\"unit\":\"\",\"class\":\"\",\"profile\":false,\"delivered\":true}" % name for name, value in self.cerbo_base) + "}"


class KeepsTheKeysOfTheOldAddress(mock_display.Display):
    def _settings(self, data, now=None):
        named = list(self.cerbo_named)
        answer = super()._settings(data, now)
        if self.settings["cerbo"] != "":
            self.cerbo_named = named
        return answer


class KeepsTheKeysOverARestart(mock_display.Display):
    def restart(self, reason="sw"):
        named = list(self.cerbo_named)
        super().restart(reason)
        self.cerbo_named = named


class NamesTheKeysBeforeTheAdapter(mock_display.Display):
    def catalog_json(self):
        named, self.cerbo_named = self.cerbo_named, []
        try:
            text = super().catalog_json()
        finally:
            self.cerbo_named = named
        if self.settings["cerbo"] == "" or not named or text == "{}":
            return text
        return "{" + "".join("\"%s\":{\"unit\":\"\",\"class\":\"\",\"profile\":false,\"delivered\":true}," % name for name in named) + text[1:]


class KeysOfTheCerboNotDelivered(mock_display.Display):
    def catalog_json(self):
        head, mark, keys = super().catalog_json().partition(",\"GX_")
        return head + mark + keys.replace("\"delivered\":true", "\"delivered\":false")


class HandsOutTheViewsInUse(mock_display.Display):
    """GET /api/layout/gx answers the views in use"""

    def _get(self, route, ticket, now):
        return super()._get("layout" if route == "layout_gx" else route, ticket, now)


class PagesOfTheCerboOnlyWithItsAddress(mock_display.Display):
    def _get(self, route, ticket, now):
        if route == "layout_gx" and self.settings["cerbo"] == "":
            return 404, mock_display.error_body("not_found")
        return super()._get(route, ticket, now)


class PagesOfTheCerboWithoutWhitespace(mock_display.Display):
    """The pages written anew, not the file as it is"""

    def _get(self, route, ticket, now):
        status, body = super()._get(route, ticket, now)
        return (status, re.sub(r"\n\s*", "", body)) if route == "layout_gx" else (status, body)


class CerboBehindTheLayout(mock_display.Display):
    """The object of the second source at another place of the info"""

    def info_json(self, now):
        text = super().info_json(now)
        cerbo = self.cerbo_json(now)
        return text.replace(cerbo, "", 1).replace("\"http\":", cerbo + "\"http\":", 1) if cerbo else text


class OwnsThePrefixOnlyWhereTheCerboHasTheKey(mock_display.Display):
    """Leaves a value of the adapter with the prefix of the Cerbo out only while the Cerbo has a value of that name"""

    def values_json(self, now):
        taken = {name for name, value, seen in self.cerbo_values(now)}
        with mock.patch.object(mock_display, "CERBO_PREFIX", "NO_SUCH_PREFIX_"):
            text = super().values_json(now)
        if self.settings["cerbo"] != "":
            for name in taken:
                text = re.sub("\"%s\":\\{[^}]*\\},(?=.*\"%s\":)" % (name, name), "", text, count=1)
        return text


class OwnsThePrefixWithoutAnAddress(mock_display.Display):
    """Leaves a value of the adapter with the prefix of the Cerbo out also on a display without its address"""

    def values_json(self, now):
        text = super().values_json(now)
        return text if self.settings["cerbo"] != "" else re.sub("\"%s[^\"]*\":\\{[^}]*\\},?" % mock_display.CERBO_PREFIX, "", text)


class CountsTheWholeStore(mock_display.Display):
    """cerbo.values of the info as it was: every value of the store that is not gone, a number without end among them"""

    def cerbo_json(self, now):
        with mock.patch.object(mock_display, "has_text", lambda value: True):
            return super().cerbo_json(now)


class KeepsTheValuesOfTheOldAddress(mock_display.Display):
    def _settings(self, data, now=None):
        seen = self.cerbo_seen_ms
        answer = super()._settings(data, now)
        if self.settings["cerbo"] != "":
            self.cerbo_seen_ms = seen
        return answer


class ClosedAsItComes(mock_display.Display):
    """The display starts closed whatever its setting says"""

    def _release_as_set(self):
        pass


class CountsTheReleaseForGoodDown(mock_display.Display):
    def info_json(self, now):
        return super().info_json(now).replace("\"left_s\":0,\"lasting\":true", "\"left_s\":%d,\"lasting\":true" % mock_display.ENDLESS_S)


class EndsTheReleaseForGoodAtTheSwitchOfTenMinutes(mock_display.Display):
    def release(self, on):
        if not on:
            self.access.switch_off(self.now())
        else:
            super().release(on)


class RestartsClosed(mock_display.Display):
    """Gives the release for good at its very first start alone"""

    def restart(self, reason="sw"):
        super().restart(reason)
        self.access.switch_off(self.now())


class KeepsTheReleaseForGoodWhenSwitchedOff(mock_display.Display):
    """Switched off the setting says so, and the web interface stays open until the next start"""

    def lasting(self, on):
        if on:
            super().lasting(on)
        else:
            self.settings["web_open"] = False


class LeavesTenMinutesBelowTheReleaseForGood(mock_display.Display):
    def lasting(self, on):
        was_open = self.access.open and not self.access.lasting
        super().lasting(on)
        if on and was_open:
            self.was_timed = True
        if not on and getattr(self, "was_timed", False):
            self.access.switch_on(self.now())


class TurnsTheSwitchThroughTheWeb(mock_display.Display):
    """POST /api/settings gives the release for good and takes it back"""

    def _settings(self, data, now=None):
        status, body = super()._settings(data, now)
        if body == "{\"error\":\"body\",\"member\":\"web_open\"}":
            self.settings["web_open"] = not self.settings["web_open"]
            if self.settings["web_open"]:
                self.access.switch_on_for_good(self.now())
            else:
                self.access.switch_off(self.now())
            return 200, self.settings_json()
        return status, body


class RefusesTheSwitchNamedAsItIs(mock_display.Display):
    def _settings(self, data, now=None):
        if b"web_open" in data:
            return 400, "{\"error\":\"body\",\"member\":\"web_open\"}"
        return super()._settings(data, now)


class TakesTheRestOfATextThatTurnsTheSwitch(mock_display.Display):
    def _settings(self, data, now=None):
        status, body = super()._settings(data, now)
        if body == "{\"error\":\"body\",\"member\":\"web_open\"}" and b"brightness" in data:
            self.settings["brightness"] = 50
        return status, body


class StoresAtOnce(mock_display.Display):
    """Stores a network without the knob"""

    def _ask(self, question, detail, now):
        if question == "wifi":
            self._store_network(self.asked_network)
        return super()._ask(question, detail, now)


class TakesHalfTheSettings(mock_display.Display):
    def _settings(self, data):
        status, body = super()._settings(data)
        if status == 400:
            self.settings["night"] = 50
        return status, body


class KeepsThePreview(mock_display.Display):
    def restart(self, reason="sw"):
        layout, name, source = self.layout, self.layout_name, self.source
        super().restart(reason)
        self.layout, self.layout_name, self.source = layout, name, source


class NoRollback(mock_display.Display):
    def _settle(self):
        self.access.waiting(self.now())


class AsksForBrokenUploads(mock_display.Display):
    def _upload(self, read, received, file_size):
        status, body = super()._upload(read, received, file_size)
        return (status, body) if status != 500 else self._ask("firmware", self.upload_version, self.now())


class ForgetsWhileItReads(mock_display.Display):
    """Forgets a network while the display reads the fault memory"""

    def _change(self, route, data, now):
        reading, self.reading = self.reading, self.reading and route != "wifi_forget"
        try:
            return super()._change(route, data, now)
        finally:
            self.reading = reading


class AsksForANetworkWhileItReads(mock_display.Display):
    def _change(self, route, data, now):
        reading, self.reading = self.reading, self.reading and route != "wifi_store"
        try:
            return super()._change(route, data, now)
        finally:
            self.reading = reading


class AsksInTheDark(mock_display.Display):
    """Asks a question on a screen the heat keeps dark"""

    def _ask_refused(self, busy, now):
        hot, self.hot = self.hot, False
        try:
            return super()._ask_refused(busy, now)
        finally:
            self.hot = hot


class UploadsInTheDark(mock_display.Display):
    def _upload_begin(self, first, file_size, now):
        hot, self.hot = self.hot, False
        try:
            return super()._upload_begin(first, file_size, now)
        finally:
            self.hot = hot


class KeepsTheQuestionInTheDark(mock_display.Display):
    def set_hot(self, on):
        self.hot = on
        self.numbers["temp_c"] = mock_display.HOT_C if on else mock_display.COOL_C


class HeatBeforeTheQuestion(mock_display.Display):
    """Looks at the heat before the question that waits"""

    def _ask_refused(self, busy, now):
        if self.access.is_open(now) and not busy and self.hot:
            return 409, mock_display.error_body("hot")
        return super()._ask_refused(busy, now)


class HeatBeforeTheBusyDisplay(mock_display.Display):
    def _ask_refused(self, busy, now):
        if self.access.is_open(now) and self.hot and self.access.waiting(now) is None:
            return 409, mock_display.error_body("hot")
        return super()._ask_refused(busy, now)


class NamesNoHeat(mock_display.Display):
    def info_json(self, now):
        return super().info_json(now).replace("\"heat\":\"off\"", "\"heat\":\"normal\"")


class AnswersTheStrip(mock_display.Display):
    def control(self, method, target, headers):
        return super().control(method, target, dict(headers, **{"x-display": "1"}))


class CompressesAsItLikes(mock_display.Display):
    """The page compressed, but not into the bytes the build of the firmware makes"""

    def handle(self, method, target, headers, read):
        answer = super().handle(method, target, headers, read)
        if answer is not None and ("Content-Encoding", "gzip") in answer[3]:
            return answer[:2] + (gzip.compress(gzip.decompress(answer[2]), 1, mtime=1),) + answer[3:]
        return answer


class WithoutHeader(mock_display.Display):
    """A display whose answers lack one of the headers they have besides status, type and length"""
    name = None

    def handle(self, method, target, headers, read):
        answer = super().handle(method, target, headers, read)
        if answer is None:
            return None
        return answer[:3] + (tuple(header for header in answer[3] if header[0] != self.name),)


class NamesNoCoding(WithoutHeader):
    name = "Content-Encoding"


class NamesNoOtherForm(WithoutHeader):
    name = "Vary"


class CompressesEverything(mock_display.Display):
    """Every answer says that it is compressed"""

    def handle(self, method, target, headers, read):
        answer = super().handle(method, target, headers, read)
        if answer is None or answer[3]:
            return answer
        return answer[:3] + ((("Content-Encoding", "gzip"),),)


class SendsNoMore(mock_display.Handler):
    """A server that sends status, type, length and body, and not the headers an answer has besides"""

    def _answer(self):
        self.wfile = Untold(self.wfile)
        super()._answer()

    do_GET = do_POST = do_PUT = do_OPTIONS = _answer


class Untold:
    """A stream whose answers do not say that the page has two forms, nor which one this is"""

    def __init__(self, stream):
        self.stream = stream

    def write(self, data):
        head, mark, body = data.partition(b"\r\n\r\n")
        for line in (b"\r\nVary: Accept-Encoding", b"\r\nContent-Encoding: gzip"):
            head = head.replace(line, b"")
        return self.stream.write(head + mark + body)

    def __getattr__(self, name):
        return getattr(self.stream, name)


class AllowsOrigins(mock_display.Handler):
    def _answer(self):
        self.wfile = Tapped(self.wfile)
        super()._answer()

    do_GET = do_POST = do_PUT = do_OPTIONS = _answer


class Tapped:
    """A stream whose answers allow every origin"""

    def __init__(self, stream):
        self.stream = stream

    def write(self, data):
        return self.stream.write(data.replace(b"\r\nContent-Type:", b"\r\nAccess-Control-Allow-Origin: *\r\nContent-Type:", 1))

    def __getattr__(self, name):
        return getattr(self.stream, name)


def taking_every_layout(data):
    """check_layout() that refuses nothing"""
    try:
        return CHECK_LAYOUT(data)
    except mock_display.Refused:
        return {"name": "", "pages": [["K"]], "warnings": []}


CHECK_LAYOUT = mock_display.check_layout

# For each test of the mock: a display that is wrong in one place (a class), or a rule of the mock that
# is another (the name of something in mock_display, and what it is replaced by)
API_MUTATIONS = {
    "test_info": WrongInfo,
    "test_catalog": WrongCatalog,
    "test_values": ("VALUE_FRESH_MS", 2999),
    "test_values_of_the_mock_move_and_age": ("VALUE_KEPT_MS", 9999),
    "test_layout_reports": ReportsNothingUnknown,
    "test_layout_problems": ("check_layout", taking_every_layout),
    "test_layout_apply_save_reset": KeepsThePreview,
    "test_dtc_last": WrongLists,
    "test_wifi": TellsPasswords,
    "test_wifi_store_and_forget": StoresAtOnce,
    "test_settings": TakesHalfTheSettings,
    "test_cerbo_settings": TakesNamesForTheCerbo,
    "test_cerbo_answers": WrongCerbo,
    "test_cerbo_comes_and_goes": ("CERBO_KEPT_MS", 59999),
    "test_cerbo_owns_its_prefix": ("CERBO_PREFIX", "NO_SUCH_PREFIX_"),
    "test_cerbo_catalog": NamesOnlyWhatTheCerboSendsNow,
    "test_cerbo_pages": HandsOutTheViewsInUse,
    "test_cerbo_views": ("appended_views", lambda first, second: first),
    "test_cerbo_counts_what_it_names": CountsTheWholeStore,
    "test_no_cerbo_without_an_address": NamesTheCerboAlways,
    "test_release": ("OPEN_MAX_MS", 1801 * 1000),
    "test_release_for_good": ClosedAsItComes,
    "test_settings_cannot_turn_the_switch": TurnsTheSwitchThroughTheWeb,
    "test_tickets": ("CONFIRM_MS", 61 * 1000),
    "test_ticket_ends": ("ASK_SHOWN_MS", 1499),
    "test_ota": NoRollback,
    "test_ota_refused_files": ("OTA_PROJECT", "wican-fw"),
    "test_ota_that_breaks_or_comes_late": ("UPLOAD_LEFT_S", 299),
    "test_refusals": NoHostRule,
    "test_refusals_in_the_documented_order": WrongRoute,
    "test_busy_keeps_the_adapter": ForgetsWhileItReads,
    "test_heat": AsksInTheDark,
    "test_the_page_compressed": ("accepts_gzip", lambda value: True),
    "test_over_http": ("Handler", AllowsOrigins),
    "test_the_strip_is_no_part_of_the_display": AnswersTheStrip,
}
# More than one thing a test guards is broken for it
API_MUTATIONS_MORE = {
    "test_refusals_in_the_documented_order": [WrongStage, HeatBeforeTheQuestion, HeatBeforeTheBusyDisplay],
    "test_cerbo_settings": [WritesAnEmptyAddress, ("ADDRESS", re.compile(r"[0-9.]*")), ("ADDRESS", re.compile(r"(?:[0-9]{1,3}\.){3}[0-9]{1,3}"))],
    "test_cerbo_answers": [("CERBO_FRESH_MS", 4999), ("CERBO_FRESH_MS", 3000), ("CERBO_KEPT_MS", 10000), ("CERBO_LIVE_MS", 9940), CerboBehindTheLayout],
    "test_cerbo_comes_and_goes": [("CERBO_FRESH_MS", 5001), ("CERBO_LIVE_MS", 10001), ("CERBO_SILENT_MS", 20001), ("CERBO_GRACE_MS", 15001),
                                  ("CERBO_ADDRESS", "192.168.88.9"), KeepsTheValuesOfTheOldAddress, NamesTheCerboAlways],
    "test_cerbo_owns_its_prefix": [OwnsThePrefixOnlyWhereTheCerboHasTheKey, OwnsThePrefixWithoutAnAddress],
    "test_cerbo_catalog": [NamesTheKeysWithoutAnAddress, KeepsTheKeysOfTheOldAddress, KeepsTheKeysOverARestart, NamesTheKeysBeforeTheAdapter, KeysOfTheCerboNotDelivered,
                           ("CERBO_PREFIX", "NO_SUCH_PREFIX_"), ("CERBO_NAMES_MAX", 63), ("CERBO_NAMES_MAX", 65), ("has_text", lambda value: True), ("OUT_SIZE", 60000),
                           ("note_names", lambda named, bundle: named.extend(name for name, value in bundle if mock_display.has_text(value) and name not in named))],
    "test_cerbo_pages": [PagesOfTheCerboOnlyWithItsAddress, PagesOfTheCerboWithoutWhitespace, ("BUILTIN_GX_LAYOUT", mock_display.BUILTIN_LAYOUT)],
    "test_cerbo_views": [AppendsToViewsOfTheUser, KeepsThePagesOfTheCerbo, ResetsToTheVehicleAlone, ("CERBO_PREFIX", "NO_SUCH_PREFIX_"),
                         ("LAYOUT_ITEM_MEMBERS", ("key", "label", "unit", "scale", "widget", "dec", "min", "max", "warn_lo", "warn_hi", "crit_lo", "crit_hi", "map")),
                         ("appended_views", lambda first, second: first + b"\n" + second)],
    "test_cerbo_counts_what_it_names": [("has_text", lambda value: True), ("has_text", lambda value: isinstance(value, str) or value > float("-inf"))],
    "test_no_cerbo_without_an_address": [NamesTheViewOfTheCerboAlways, WritesAnEmptyAddress],
    "test_busy_keeps_the_adapter": [AsksForANetworkWhileItReads],
    "test_heat": [UploadsInTheDark, KeepsTheQuestionInTheDark, NamesNoHeat],
    "test_refusals": [("BODY_SMALL_MAX", 513), ("OUT_SIZE", 60000), AsksForBrokenUploads],
    "test_release": [("OPEN_MS", 601 * 1000)],
    "test_release_for_good": [CountsTheReleaseForGoodDown, EndsTheReleaseForGoodAtTheSwitchOfTenMinutes,
                              LeavesTenMinutesBelowTheReleaseForGood, RestartsClosed, KeepsTheReleaseForGoodWhenSwitchedOff, ("ENDLESS_S", 299), ("CONFIRM_MS", 61 * 1000)],
    "test_settings_cannot_turn_the_switch": [RefusesTheSwitchNamedAsItIs, TakesTheRestOfATextThatTurnsTheSwitch, ("OPEN_MS", 601 * 1000)],
    "test_ota_that_breaks_or_comes_late": [AsksForBrokenUploads],
    "test_the_page_compressed": [("accepts_gzip", lambda value: value is not None and "gzip" in value),
                                 ("accepts_gzip", lambda value: False), ("CODINGS_ROOM", 80), ("CODINGS_ROOM", 63),
                                 CompressesAsItLikes, NamesNoCoding, NamesNoOtherForm, CompressesEverything],
    "test_over_http": [("Handler", SendsNoMore)],
}


def failures(name, mutation):
    """Runs one test of Api against a mock that is wrong; returns what the test found"""
    class Broken(Api):
        pass

    result = unittest.TestResult()
    if isinstance(mutation, tuple):
        with mock.patch.object(mock_display, mutation[0], mutation[1]):
            Broken(name).run(result)
    else:
        Broken.display_class = mutation
        Broken(name).run(result)
    return result.failures + result.errors


class CounterCheck(unittest.TestCase):
    def test_every_check_of_the_page_is_broken_once(self):
        checks = {"request_problems", "construct_problems", "external_problems", "release_problems", "cerbo_problems", "color_problems",
                  "size_problems", "compressed_problems"}
        self.assertEqual(set(PAGE_MUTATIONS), checks)
        self.assertEqual({name for name in globals() if name.endswith("_problems")}, checks)

    def test_a_broken_page_is_found(self):
        page = file_text(PAGE)
        api_text = file_text(API)
        for check, mutations in PAGE_MUTATIONS.items():
            for what, mutate in mutations.items():
                with self.subTest(check=check, broken=what):
                    broken = mutate(page) if callable(mutate) else swapped(page, *mutate)
                    found = {"request_problems": lambda: request_problems(broken, api_text),
                             "construct_problems": lambda: construct_problems(broken),
                             "external_problems": lambda: external_problems(broken),
                             "release_problems": lambda: release_problems(broken),
                             "cerbo_problems": lambda: cerbo_problems(broken, api_text),
                             "color_problems": lambda: color_problems(broken),
                             "size_problems": lambda: size_problems(broken.encode("utf-8")),
                             "compressed_problems": lambda: compressed_problems(broken.encode("utf-8"))}[check]()
                    self.assertNotEqual(found, [])

    def test_a_request_api_md_no_longer_names_is_found(self):
        page = file_text(PAGE)
        api_text = file_text(API)
        self.assertNotEqual(request_problems(page, swapped(api_text, "| `POST /api/reboot` |", "| `POST /api/restart` |")), [])
        self.assertNotEqual(request_problems(page, swapped(api_text, "| `GET /api/ticket?id=N` |", "| `GET /api/ticket?n=N` |")), [])

    def test_an_editor_that_calls_values_of_the_cerbo_unknown_is_found(self):
        page = file_text(PAGE)
        self.assertGreaterEqual(len(EDITOR_MUTATIONS), 6)
        for what, mutation in EDITOR_MUTATIONS.items():
            with self.subTest(broken=what):
                self.assertNotEqual(editor_marks(swapped(page, *mutation)), [])

    def test_an_editor_that_does_not_offer_the_cerbo_is_found(self):
        page = file_text(PAGE)
        self.assertGreaterEqual(len(OFFER_MUTATIONS), 25)
        for what, mutation in OFFER_MUTATIONS.items():
            with self.subTest(broken=what):
                self.assertNotEqual(editor_offers(swapped(page, *mutation)), [])

    def test_every_test_of_the_mock_is_broken_once(self):
        tests = {name for name in dir(Api) if name.startswith("test_")}
        self.assertEqual(set(API_MUTATIONS), tests)
        self.assertLessEqual(set(API_MUTATIONS_MORE), tests)

    def test_a_wrong_mock_is_found(self):
        for name, first in API_MUTATIONS.items():
            for mutation in [first] + API_MUTATIONS_MORE.get(name, []):
                label = mutation.__name__ if isinstance(mutation, type) else "%s = %r" % mutation
                with self.subTest(test=name, wrong=label):
                    self.assertNotEqual(failures(name, mutation), [])


if __name__ == "__main__":
    unittest.main()
