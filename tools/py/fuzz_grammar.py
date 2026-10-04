#!/usr/bin/env python3
"""Reads doc/grammar.bnf, generates scripts from it, and asks the loader what it
made of them.

The point is not coverage of the grammar, which three fixed fixtures cannot give, and
not to replace those fixtures: a fuzzer only catches an input it happens to produce.
It is here for the class of input nobody thought to write down.

Two passes:
  * every shipped asset must load clean, so the grammar cannot quietly drift away
    from the games that use it;
  * generated scripts that the grammar says are well formed must load, so the parser
    cannot quietly refuse something the format allows.

Usage: fuzz_grammar.py [--count N] [--seed S] [--bin build/quest] [--quiet]
"""

import argparse
import pathlib
import random
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
BNF = ROOT / "doc" / "grammar.bnf"
ASSETS = sorted((ROOT / "assets" / "script").glob("*.script"))

# A cmd keyword at the head of the action is a keyword, not prose, so a description
# cannot start with one of them. This is written down in grammar.bnf; the generator
# honours it instead of inventing a disagreement that is already documented.
CMDS = ("go", "end", "win")


def strip_comments(text):
    """Only whole-line comments. A ';' ends a production in this file, so it cannot
    also open a trailing comment without eating the grammar."""
    return "\n".join(l for l in text.splitlines() if not l.lstrip().startswith(";"))


class Term:
    """One thing a sequence can ask for. Only the subset grammar.bnf uses:
    a name, a quoted literal, [ optional ], { repeated }, and ( one of | these )."""

    def __init__(self, kind, name=None, seq=None, alts=None):
        self.kind = kind          # 'name' | 'lit' | 'opt' | 'rep' | 'alt'
        self.name = name
        self.seq = seq or []
        self.alts = alts or []


def parse_seq(body):
    """A sequence, split on whitespace only outside brackets and quotes."""
    terms, buf, i, depth = [], "", 0, 0
    while i < len(body):
        ch = body[i]
        if ch == '"':
            j = body.index('"', i + 1)
            if buf.strip():
                terms.append(Term("name", buf.strip()))
                buf = ""
            terms.append(Term("lit", body[i + 1:j]))
            i = j + 1
            continue
        if ch in "[({":
            close = {"]": "]", "(": ")", "{": "}", "[": "]"}[ch]
            inner = body[i + 1:body.index(close, i + 1)]
            kind = {"[": "opt", "{": "rep", "(": "alt"}[ch]
            if buf.strip():
                terms.append(Term("name", buf.strip()))
                buf = ""
            if kind == "alt":
                terms.append(Term("alt", alts=[parse_seq(part)
                                               for part in split_top(inner, "|")]))
            else:
                terms.append(Term(kind, seq=parse_seq(inner)))
            i = body.index(close, i + 1) + 1
            continue
        if ch == " ":
            if buf.strip():
                terms.append(Term("name", buf.strip()))
                buf = ""
        else:
            buf += ch
        i += 1
    if buf.strip():
        terms.append(Term("name", buf.strip()))
    return terms


def split_top(text, sep):
    """Split on sep only at bracket depth zero and outside quotes. The grammar puts
    '|' inside { letter | digit } and inside ( "+" | "-" ), and splitting there would
    cut a production into pieces that name nothing."""
    parts, buf, depth, i = [], "", 0, 0
    while i < len(text):
        ch = text[i]
        if ch == '"':
            j = text.index('"', i + 1)
            buf += text[i:j + 1]
            i = j + 1
            continue
        if ch in "[({":
            depth += 1
        elif ch in "])}":
            depth -= 1
        if ch == sep and depth == 0:
            parts.append(buf)
            buf = ""
        else:
            buf += ch
        i += 1
    parts.append(buf)
    return parts


def read_grammar(path):
    text = strip_comments(path.read_text(encoding="utf-8"))
    rules = {}
    for stmt in text.split(";"):
        if "=" not in stmt:
            continue
        name, rhs = stmt.split("=", 1)
        name = name.strip()
        if not name or "(" in name:
            continue
        rules[name] = [parse_seq(alt) for alt in split_top(rhs, "|")]
    return rules


LETTERS = "абвгдежзиклмнопрстуфхцчшщыэюяabcdnoprstuvxz_"
SYMBOLS = "+-"
KEYWORDS = ("room", "start")


class Gen:
    def __init__(self, rules, rnd):
        self.rules = rules
        self.rnd = rnd

    def word(self, n=None):
        n = n or self.rnd.randint(1, 9)
        w = "".join(self.rnd.choice(LETTERS) for _ in range(n))
        if self.rnd.random() < 0.3:
            w += str(self.rnd.randint(0, 99))
        return w

    def term(self, t):
        if t.kind == "lit":
            return t.name
        if t.kind == "name":
            return self.expand(t.name)
        if t.kind == "opt":
            return self.seq(t.seq) if self.rnd.random() < 0.5 else ""
        if t.kind == "rep":
            return self.seq(t.seq) * self.rnd.randint(0, 3)
        if t.kind == "alt":
            return self.seq(self.rnd.choice(t.alts))
        return ""

    def seq(self, terms):
        return "".join(self.term(t) for t in terms)

    def text(self):
        n = self.rnd.randint(1, 14)
        words = [self.word(self.rnd.randint(1, 8)) for _ in range(n)]
        # Never lead prose with a cmd keyword: the grammar says a description cannot.
        while words and words[0] in CMDS:
            words[0] = self.word(4)
        return " ".join(words)

    def expand(self, name):
        if name == "WS":
            return " "
        if name == "NL":
            return "\n"
        if name == "text":
            return self.text()
        if name == "word":
            return self.word()
        if name == "letter":
            return self.rnd.choice(LETTERS)
        if name == "digit":
            return str(self.rnd.randint(0, 9))
        if name == "blank":
            return " " * self.rnd.randint(0, 3)
        if name.endswith(("*", "+", "?")):
            base = name[:-1]
            times = {"*": self.rnd.randint(0, 2), "+": self.rnd.randint(1, 2),
                     "?": self.rnd.randint(0, 1)}[name[-1]]
            return "".join(self.expand(base) for _ in range(times))
        if name in self.rules:
            return self.seq(self.rnd.choice(self.rules[name]))
        raise KeyError("no production %r in grammar.bnf" % name)


def run(binary, path):
    out = subprocess.run([binary, "--load", str(path)], capture_output=True, text=True)
    lines = out.stdout.strip().splitlines()
    fields = dict(line.split(" ", 1) for line in lines if " " in line)
    return out.returncode, fields


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=300)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--bin", default=str(ROOT / "build" / "quest"))
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    rules = read_grammar(BNF)
    if "rule" not in rules:
        print("doc/grammar.bnf has no rule production", file=sys.stderr)
        return 2
    rnd = random.Random(args.seed)
    gen = Gen(rules, rnd)

    bad = 0
    for asset in ASSETS:
        code, f = run(args.bin, asset)
        if code != 0 or f.get("status") != "ok" or f.get("errors") != "0":
            print("ASSET %s: status=%s errors=%s" %
                  (asset.name, f.get("status"), f.get("errors")))
            bad += 1
        elif not args.quiet:
            print("asset ok  %-16s rooms=%s rules=%s" %
                  (asset.name, f.get("rooms"), f.get("rules")))

    tmp = pathlib.Path("/tmp/fuzz_dsl.script")
    # A script with no room is refused by the loader, and that is semantics, not
    # grammar: "a script has at least one room" is not derivable from the shape of a
    # line. So the generator supplies the frame and fuzzes the body, otherwise every
    # run drowns in the same non-finding.
    head = "start %s\n\nroom %s Комната\n\n" % (gen.word(4), gen.word(4))
    for i in range(args.count):
        body = head + gen.expand("rule") * gen.rnd.randint(1, 6)
        tmp.write_text(body, encoding="utf-8")
        code, f = run(args.bin, tmp)
        if code != 0 or f.get("status") != "ok":
            bad += 1
            print("REJECTED #%d status=%s errline=%s" % (i, f.get("status"),
                                                         f.get("errline")))
            print("  " + body.replace("\n", "\n  ")[:400])
            if bad > 5:
                break

    if not args.quiet:
        print("generated %d scripts, %d rejected" % (args.count, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
