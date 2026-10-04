#!/usr/bin/env python3
"""Convert the author's source quest DSL into the project's script DSL.

Provenance of the two games this handles:

    assets/script/source/rats.py
        https://gist.github.com/true-grue/126146820c9c17652a30ffdd1b7b96f4
        "КРЫСОЛОВ", after A. Green
    assets/script/source/field.py
        https://gist.github.com/true-grue/abdb21a8aa1f5e52b293999f5a858f8f
        "SWORD FROM THE CASTLE"

The sources are Python files holding the game in a GAME triple-quoted string:

    ROOM улица
    NAME Заснеженная улица
    DESC * текст
    идти крыльцо * : goto крыльцо_банка текст

The project format is line based and much smaller:

    start улица
    room улица Заснеженная улица
    : say текст
    идти крыльцо : say текст | go крыльцо_банка

This tool exists so the translation is reproducible and reviewable rather than
hand-copied, and so the project format never grows a feature for one game. It is
an asset generator: it runs at authoring time and the engine loads only the
result. The engine never sees the source dialect.

Mapping, with the reason for each row:

    ROOM id + NAME t      room id t
    DESC * t              : t
    DESC a t              [a] t
    DESC -a t             [-a] t
    DESC a,-b t           [a,-b] t
    v o *   : goto r t    v o : go r t
    v o *   : goto r      v o : go r
    v o c   : give f t    v o c : +f t
    v o c   : msg t       v o c : t
    v o c   : end t       v o c : end t
    v o c   : win t       v o c : win t

A goto keeps its text on the move itself. That is what lets the message print
before the destination room and removes the old trap where a print swallowed the rest
of the line. The source dialect never puts two actions in one rule, so nothing is
lost by dropping the separator.

Underscores in identifiers are left untouched. The source swaps them for spaces
when it prints a command, and so does the palette, because that is a display
concern and not the identifier's business.

Anything not in the table above is refused rather than guessed. A converter that
silently drops a construct it does not understand is worse than no converter,
because the result still looks like a game.

Usage:
    tools/py/source2dsl.py assets/script/source/rats.py assets/script/rats.script
"""

import re
import sys

IDENT = r"[^\s,-]+"


class Bad(Exception):
    pass


def extract_game(text):
    m = re.search(r'GAME\s*=\s*"""(.*?)\n"""', text, re.S)
    if m is None:
        raise Bad("no GAME triple-quoted string")
    return m.group(1)


def extract_start(text):
    m = re.search(r"run_game\(\s*'([^']+)'", text)
    if m is None:
        raise Bad("no run_game('room') call")
    return m.group(1)


def extract_title(text):
    """The banner the source prints before starting, which is the game's title.

    It lives outside the GAME string, so without this the converted script would
    have no name anywhere.
    """
    m = re.findall(r'print\(\s*"([^"]+)"\s*\)', text)
    for candidate in m:
        if candidate and not candidate.startswith("Действия"):
            return candidate
    return ""


def cond_list(condition):
    """`*` is unconditional; otherwise a comma list of flags, `~` negating.

    The commas go: the project format separates items with spaces and gives every one
    of them an explicit sign. A bare word there would be a command, so a condition the
    source left unsigned has to become +flag.
    """
    if condition == "*":
        return ""
    if not re.fullmatch(r"(~?%s)(,~?%s)*" % (IDENT, IDENT), condition):
        raise Bad("unsupported condition %r" % condition)
    out = []
    for item in condition.split(","):
        if item.startswith("~"):
            out.append("-" + item[1:])
        else:
            out.append("+" + item)
    return " ".join(out)


def convert(body, start_room, title, source_name):
    rooms = {}      # id -> name
    order = []      # declaration order
    emitted = []    # (room_id, line)
    current = None  # the room being described
    awaiting = None # a ROOM line whose NAME has not arrived yet

    def need_room(lineno, what):
        if current is None:
            raise Bad("line %d: %s outside a room" % (lineno, what))

    for lineno, raw in enumerate(body.strip().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        w = line.split()

        if w[0] == "ROOM" and len(w) == 2:
            if awaiting is not None:
                raise Bad("line %d: ROOM %s has no NAME" % (lineno, awaiting))
            if w[1] in rooms:
                raise Bad("line %d: duplicate room %s" % (lineno, w[1]))
            awaiting = w[1]
            continue

        if w[0] == "NAME":
            if awaiting is None:
                raise Bad("line %d: NAME outside a room" % lineno)
            current = awaiting
            awaiting = None
            rooms[current] = " ".join(w[1:])
            order.append(current)
            emitted.append((current, "room %s %s" % (current, rooms[current])))
            continue

        if w[0] == "DESC":
            need_room(lineno, "DESC")
            text = " ".join(w[2:])
            if not text:
                raise Bad("line %d: DESC with no text" % lineno)
            g = cond_list(w[1])
            emitted.append((current, "%s %s" % ((g + ":") if g else ":", text)))
            continue

        if w[0] == "TEST":
            raise Bad("line %d: TEST has no counterpart in the project format. "
                      "It fires without a player command, which is a semantic "
                      "the format deliberately does not have." % lineno)

        # [verb, object, condition, ':', effect, *words]
        if len(w) < 5 or w[3] != ":":
            raise Bad("line %d: not a rule: %s" % (lineno, line[:70]))
        verb, obj, cond, effect, words = w[0], w[1], w[2], w[4], w[5:]
        g = cond_list(cond)
        head = "%s %s%s :" % (verb, obj, (" " + g) if g else "")

        if effect == "goto":
            if not words:
                raise Bad("line %d: goto with no room" % lineno)
            text = " ".join(words[1:])
            act = "go %s" % words[0]
            emitted.append((current, head + " " + act + (" " + text if text else "")))
        elif effect == "give":
            if len(words) < 2:
                raise Bad("line %d: give needs a flag and text" % lineno)
            emitted.append((current, head + " +%s %s"
                            % (words[0], " ".join(words[1:]))))
        elif effect == "msg":
            if not words:
                raise Bad("line %d: msg with no text" % lineno)
            emitted.append((current, head + " " + " ".join(words)))
        elif effect in ("end", "win"):
            if not words:
                raise Bad("line %d: %s with no text" % (lineno, effect))
            emitted.append((current, head + " %s %s" % (effect, " ".join(words))))
        else:
            raise Bad("line %d: unknown effect %r" % (lineno, effect))

    if awaiting is not None:
        raise Bad("ROOM %s has no NAME" % awaiting)
    if start_room not in rooms:
        raise Bad("start room %s is not declared" % start_room)

    targets = set()
    for _, text in emitted:
        _, _, tail = text.partition(":")
        tail = tail.strip()
        if tail.startswith("go "):
            targets.add(tail[3:].split()[0])
    missing = targets - set(rooms)
    if missing:
        raise Bad("goto to undeclared room(s): %s" % ", ".join(sorted(missing)))

    out = ["# Generated by tools/py/source2dsl.py from %s. Do not edit: change"
           % source_name,
           "# the source or the tool, then regenerate.",
           "# title: %s" % title,
           "# rooms: %d, lines: %d" % (len(order), len(emitted)),
           "",
           "start %s" % start_room,
           ""]
    for room in order:
        for owner, text in emitted:
            if owner == room:
                out.append(text)
        out.append("")
    return "\n".join(out), len(order), len(emitted), title


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    src, dst = argv[1], argv[2]
    try:
        with open(src, encoding="utf-8") as f:
            text = f.read()
        out, nrooms, nlines, title = convert(extract_game(text),
                                             extract_start(text),
                                             extract_title(text),
                                             src)
    except (Bad, OSError) as exc:
        sys.stderr.write("source2dsl: %s: %s\n" % (src, exc))
        return 1
    with open(dst, "w", encoding="utf-8") as f:
        f.write(out)
    sys.stderr.write("source2dsl: %s -> %s, %d rooms, %d lines, title %r\n"
                     % (src, dst, nrooms, nlines, title))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))