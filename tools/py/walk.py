#!/usr/bin/env python3
"""Проверка проходимости скрипта: поиск пути к финалу по состояниям.

Разбирает .script сам, ровно как src/dsl.c, и ищет путь от старта до строки
финала. Движок не участвует и не меняется: это инструмент проверки, а не часть
игры.

Поиск — A* по состояниям (комната плюс множество флагов). Эвристика
h(s) — расстояние до финала в графе комнат, посчитанное в обратную сторону по
всем правилам с `go`, независимо от условий. Состояния сжимаются жадным
подбором: как только предмет доступен, он берётся, поэтому предметных ветвей не
перебирается, а маршрут остаётся настоящим и повторяемым — каждый подбор это
команда в цепочке.

    python3 tools/py/walk.py --line 320 assets/script/heart.script
"""

from __future__ import annotations

import argparse
import sys
from heapq import heappop, heappush


# ------------------------------------------------------------ разбор ---


class Rule:
    __slots__ = ("words", "guard", "effects", "act", "arg", "line")

    def __init__(self, words, guard, effects, act, arg, line):
        self.words = words          # list[str]
        self.guard = guard          # list[(bool, str)]
        self.effects = effects      # list[(bool, str)]
        self.act = act              # "say" | "go" | "end" | "win"
        self.arg = arg              # комната назначения для go
        self.line = line            # номер строки в файле, с 1


class Frag:
    __slots__ = ("guard", "line")

    def __init__(self, guard, line):
        self.guard = guard
        self.line = line


class Room:
    __slots__ = ("id", "frags", "rules")

    def __init__(self, id_):
        self.id = id_
        self.frags = []
        self.rules = []


class Script:
    def __init__(self):
        self.rooms = []
        self.start = None
        self.flag_index = {}


def _skip_ws(s, i):
    while i < len(s) and s[i] in " \t":
        i += 1
    return i


def _trim(s):
    j = len(s)
    while j > 0 and s[j - 1] in " \t":
        j -= 1
    return s[:j]


def _is_word(ch):
    return (("a" <= ch <= "z") or ("A" <= ch <= "Z") or ("0" <= ch <= "9")
            or ch == "_" or ord(ch) >= 0x80)


def _read_word(s, i, end):
    """(начало, конец) слова или None. Пробелы пропускаются внутри."""
    i = _skip_ws(s, i)
    if i >= end:
        return None
    st = i
    while i < end and _is_word(s[i]):
        i += 1
    if i == st:
        return None
    return st, i


def _eat_kw(s, i, end, kw):
    if not s.startswith(kw, i, end):
        return None
    j = i + len(kw)
    if j != end and s[j] not in " \t":
        return None
    return _skip_ws(s, j)


def _parse_items(s, i, end):
    out = []
    while True:
        i = _skip_ws(s, i)
        if i >= end or s[i] not in "+-":
            break
        present = s[i] == "+"
        w = _read_word(s, i + 1, end)
        if w is None:
            raise ValueError("после знака нет слова")
        out.append((present, s[w[0]:w[1]]))
        i = w[1]
    return out, i


def _parse_action(s, i, end):
    i = _skip_ws(s, i)
    if i < end and s[i] in "+-":
        eff, i = _parse_items(s, i, end)
        return "say", None, eff, _trim(s[_skip_ws(s, i):end])
    if end - i >= 2 and s[i:i + 2] == "go" and (i + 2 == end or s[i + 2] in " \t"):
        w = _read_word(s, i + 2, end)
        if w is None:
            raise ValueError("go без комнаты")
        return "go", s[w[0]:w[1]], [], _trim(s[_skip_ws(s, w[1]):end])
    if end - i >= 3 and s[i:i + 3] == "end" and (i + 3 == end or s[i + 3] in " \t"):
        return "end", None, [], _trim(line_tail(s, i + 3, end))
    if end - i >= 3 and s[i:i + 3] == "win" and (i + 3 == end or s[i + 3] in " \t"):
        return "win", None, [], _trim(line_tail(s, i + 3, end))
    return "say", None, [], _trim(s[i:end])


def line_tail(s, i, end):
    return s[_skip_ws(s, i):end]


def parse(text):
    """Разбор в том же порядке и с теми же решениями, что src/dsl.c."""
    sc = Script()
    room = None
    for n, line in enumerate(text.split("\n"), 1):
        if line.endswith("\r"):
            line = line[:-1]
        if line == "" or line[0] == "#":
            continue
        end = len(line)
        i = _skip_ws(line, 0)

        j = _eat_kw(line, i, end, "start")
        if j is not None:
            w = _read_word(line, j, end)
            if w is None:
                raise ValueError("start без слова, строка %d" % n)
            sc.start = line[w[0]:w[1]]
            continue

        j = _eat_kw(line, i, end, "room")
        if j is not None:
            w = _read_word(line, j, end)
            if w is None:
                raise ValueError("room без имени, строка %d" % n)
            room = Room(line[w[0]:w[1]])
            sc.rooms.append(room)
            continue

        if room is None:
            raise ValueError("строка %d вне комнаты" % n)

        colon = line.find(":")
        if colon < 0:
            raise ValueError("нет двоеточия, строка %d" % n)
        head_end = colon

        # Голые слова до двоеточия: их наличие и решает, команда это или
        # описание комнаты. Подписанные слова не считаются.
        words = []
        q = 0
        while q < head_end:
            w = _read_word(line, q, head_end)
            if w is None:
                break
            words.append(line[w[0]:w[1]])
            q = _skip_ws(line, w[1])

        if not words:
            guard = []
            p = _skip_ws(line, 0)
            if p != head_end:
                guard, p = _parse_items(line, p, head_end)
                if _skip_ws(line, p) != head_end:
                    raise ValueError("лишнее перед двоеточием, строка %d" % n)
            if not _trim(line[colon + 1:]):
                raise ValueError("пустое описание, строка %d" % n)
            room.frags.append(Frag(guard, n))
            continue

        p = _skip_ws(line, 0)
        cw = []
        while p < head_end and line[p] not in "+-":
            w = _read_word(line, p, head_end)
            if w is None:
                raise ValueError("не слово в команде, строка %d" % n)
            cw.append(line[w[0]:w[1]])
            p = _skip_ws(line, w[1])
        if not cw:
            raise ValueError("команда без слов, строка %d" % n)
        guard, p = _parse_items(line, p, head_end)
        if _skip_ws(line, p) != head_end:
            raise ValueError("лишнее перед двоеточием, строка %d" % n)

        act, arg, eff, _text = _parse_action(line, colon + 1, end)
        room.rules.append(Rule(cw, guard, eff, act, arg, n))

    if not sc.rooms:
        raise ValueError("нет комнат")
    if sc.start is None:
        sc.start = sc.rooms[0].id

    for r in sc.rooms:
        for rule in r.rules:
            for _, name in rule.guard:
                sc.flag_index.setdefault(name, len(sc.flag_index))
            for _, name in rule.effects:
                sc.flag_index.setdefault(name, len(sc.flag_index))
        for frag in r.frags:
            for _, name in frag.guard:
                sc.flag_index.setdefault(name, len(sc.flag_index))
    return sc


# --------------------------------------------------- жадный подбор ---


def greedy_items(sc):
    """Предметы, которые берутся сразу, как только доступны.

    Предмет — флаг без подчёркивания. Из набора выкидывается всё, чего требуют
    отсутствующим: концовка, переход, или команда без парного успешного варианта.
    Самообслуживание вида «взять X -X : +X» требованием не считается — это правило,
    которое предмет и ставит.
    """
    items = set()
    flat = []
    for r in sc.rooms:
        for rule in r.rules:
            flat.append(rule)
            for _, name in rule.guard:
                if not name.startswith("_"):
                    items.add(name)
            for present, name in rule.effects:
                if not name.startswith("_"):
                    items.add(name)

    forbidden = set()
    for rule in flat:
        for present, name in rule.guard:
            if present or name not in items:
                continue
            if any(p and nm == name for p, nm in rule.effects):
                continue                      # правило само ставит предмет
            if rule.act in ("end", "win", "go"):
                forbidden.add(name)          # нужен отсутствующим
                continue
            if any(p and nm in items for p, nm in rule.effects):
                # Правило с «без предмета» меняет другой предмет: с предметом в
                # руках оно не сработает, и предмет не появится.
                forbidden.add(name)
                continue
            twin = any(t is not rule and t.words == rule.words
                       and any(p and nm == name for p, nm in t.guard)
                       for t in flat)
            if not twin:
                forbidden.add(name)
    return items - forbidden, forbidden


# ------------------------------------------------------- исполнение ---


class CRule:
    """Правило с готовыми масками: условие проверяется двумя сравнениями."""

    __slots__ = ("key", "pos", "neg", "eff", "act", "arg", "line")

    def __init__(self, rule, bits):
        self.key = tuple(rule.words)
        self.pos = 0
        self.neg = 0
        for present, name in rule.guard:
            bit = bits.get(name, 0)
            if present:
                self.pos |= bit
            else:
                self.neg |= bit
        self.eff = [(present, bits.get(name, 0)) for present, name in rule.effects]
        self.act = rule.act
        self.arg = rule.arg
        self.line = rule.line


class CRoom:
    __slots__ = ("id", "rules", "by_key", "ext")

    def __init__(self, room, bits):
        self.id = room.id
        self.rules = [CRule(r, bits) for r in room.rules]
        self.by_key = {}
        for i, r in enumerate(self.rules):
            self.by_key.setdefault(r.key, []).append(i)
        # Ключи, продлевающие каждый ключ: ими занят game_more, и пока хоть одно
        # правило с таким ключом живо, команда не дописывается.
        self.ext = {}
        for k in self.by_key:
            self.ext[k] = [u for u in self.by_key
                           if len(u) > len(k) and u[:len(k)] == k]


class World:
    """Правила исполнения ровно в том порядке, в каком их выполняет game.c."""

    def __init__(self, sc):
        self.sc = sc
        self.bits = {name: 1 << i for name, i in sc.flag_index.items()}
        self.rooms = {r.id: CRoom(r, self.bits) for r in sc.rooms}
        self.order = [self.rooms[r.id] for r in sc.rooms]

    def live(self, room, mask):
        """Номера правил, условия которых выполнены, в порядке исходника."""
        out = []
        rules = room.rules
        for i, r in enumerate(rules):
            if (mask & r.pos) == r.pos and not (mask & r.neg):
                out.append(i)
        return out

    def executable(self, room, mask):
        """Команды, которые палитра сейчас предлагает: (слова, номер правила)."""
        live = self.live(room, mask)
        if not live:
            return []
        keys = {room.rules[i].key for i in live}
        first = {}
        for i in live:
            first.setdefault(room.rules[i].key, i)
        out = []
        for k, i in first.items():
            blocked = False
            for u in room.ext.get(k, ()):
                if u in keys:
                    blocked = True
                    break
            if not blocked:
                out.append((k, i))
        return out

    def apply(self, room, idx, mask):
        """Маска после эффектов правила: эффекты идут до действия."""
        for present, bit in room.rules[idx].eff:
            if present:
                mask |= bit
            else:
                mask &= ~bit
        return mask

    def pick_up(self, room, mask, greedy_bits):
        """Одна команда, которая берёт предмет и ничего больше.

        Подбирается только то, что не двигает по комнате и не заканчивает игру.
        """
        for key, idx in self.executable(room, mask):
            rule = room.rules[idx]
            if rule.act != "say":
                continue
            new_mask = self.apply(room, idx, mask)
            if (new_mask & greedy_bits & ~mask) and not (mask & greedy_bits & ~new_mask):
                return key, new_mask
        return None

    def close(self, room, mask, greedy_bits):
        """Подбирает всё, до чего можно дотянуться не выходя из комнаты.

        Возвращает пары (команда, состояние после неё), чтобы каждый подбор был
        отдельным шагом маршрута со своим состоянием.
        """
        steps = []
        while True:
            got = self.pick_up(room, mask, greedy_bits)
            if got is None:
                return steps
            steps.append(got)
            mask = got[1]


# ------------------------------------------------------ эвристика ---


def room_distances(w, targets):
    """Обратный обход по графу переходов, без учёта условий.

    Условия игнорируются намеренно: падающая оценка честнее завышенной. Граф
    несимметричен — переход между двумя комнатами бывает односторонним, —
    поэтому расстояние считается от финала назад.
    """
    back = {}
    for room in w.order:
        for rule in room.rules:
            if rule.act != "go" or rule.arg not in w.rooms:
                continue
            back.setdefault(rule.arg, set()).add(room.id)

    dist = {t: 0 for t in targets}
    queue = list(targets)
    while queue:
        cur = queue.pop(0)
        for prev in back.get(cur, ()):
            if prev not in dist:
                dist[prev] = dist[cur] + 1
                queue.append(prev)
    return dist


def endings_of(w):
    """Все концовки скрипта: (строка, вид, комната)."""
    out = []
    for room in w.order:
        for rule in room.rules:
            if rule.act in ("end", "win"):
                out.append((rule.line, rule.act, room.id))
    seen = {}
    for line, act, room_id in out:
        seen.setdefault(line, (act, room_id))
    return [(line, seen[line][0], seen[line][1]) for line in sorted(seen)]


# ------------------------------------------------------------ обход ---


def target_specs(w, targets):
    """Что требуют правила целевых строк: (строка, надо иметь, надо не иметь).

    Флаг, который никто никогда не снимает, однажды взведённый, бьёт по всем
    состояниям сразу: состояние с ним мёртвое для этой цели, и обходить его
    незачем. Поэтому такие требования превращаются в отсечение, а не в оценку.
    """
    cleared = 0
    for room in w.order:
        for rule in room.rules:
            for present, bit in rule.eff:
                if not present:
                    cleared |= bit
    specs = []
    for room in w.order:
        for rule in room.rules:
            if rule.line in targets:
                specs.append((rule.line, rule.pos, rule.neg & ~cleared))
    return specs


def search(w, greedy_bits, targets, limit):
    """A* по состояниям. targets — строки концовок, любая из них и есть цель.

    Оценка — расстояние до комнаты цели плюс число её условий, которые ещё не
    выполнены. Оценка занижена намеренно: завышенная заставила бы A* принять
    первый найденный маршрут за лучший, а вопрос здесь в достижимости, а не в
    оптимальности.
    """
    specs = target_specs(w, targets)
    if not specs:
        raise SystemExit("нет правил на строках %s" % ", ".join(map(str, targets)))
    finish_rooms = {room.id for room in w.order for r in room.rules
                    if r.line in targets}
    dist = room_distances(w, finish_rooms)
    far = 1 << 20
    start_room = w.sc.start

    def h(room_id, mask):
        base = dist.get(room_id, far)
        best = far
        for _line, mp, ma in specs:
            if mask & ma:
                continue                      # это состояние мёртвое для цели
            cost = base + bin(mp & ~mask).count("1")
            if cost < best:
                best = cost
        return best

    start_mask = 0
    if h(start_room, start_mask) >= far:
        return None, None, {"expanded": 0, "states": 1, "pickups": 0,
                            "overflow": False}

    # Узел: [комната, маска, родитель, слова, подбор]
    nodes = [[start_room, start_mask, -1, None, False]]
    g = {(start_room, start_mask): 0}
    heap = [(h(start_room, start_mask), 0, 0)]
    pickups = 0
    expanded = 0

    while heap:
        _f, gv, idx = heappop(heap)
        room_id, mask, _p, _w, _a = nodes[idx]
        if gv != g.get((room_id, mask)):
            continue
        expanded += 1
        if expanded % 20000 == 0:
            print("    разобрано %d, в очереди %d, состояний %d"
                  % (expanded, len(heap), len(g)), file=sys.stderr, flush=True)
        if len(g) > limit:
            return None, None, {"expanded": expanded, "states": len(g),
                                "pickups": pickups, "overflow": True}

        room = w.rooms[room_id]
        for key, ridx in w.executable(room, mask):
            rule = room.rules[ridx]
            new_mask = w.apply(room, ridx, mask)
            if rule.act in ("end", "win"):
                if rule.line in targets:
                    nodes.append([room_id, new_mask, idx, key, False])
                    return nodes, len(nodes) - 1, {
                        "expanded": expanded, "states": len(g),
                        "pickups": pickups, "overflow": False}
                continue

            new_room = rule.arg if rule.act == "go" else room_id
            chain = _chain(w, new_room, new_mask, greedy_bits, idx, key, nodes)
            nodes.extend(chain)
            last_idx = len(nodes) - 1
            last_room, last_mask = nodes[last_idx][0], nodes[last_idx][1]
            nh = h(last_room, last_mask)
            if nh >= far:
                continue                      # цель отсюда недостижима
            ng = gv + len(chain) - 1
            if ng < g.get((last_room, last_mask), 1 << 30):
                g[(last_room, last_mask)] = ng
                pickups += len(chain) - 1
                heappush(heap, (ng + nh, ng, last_idx))

    return None, None, {"expanded": expanded, "states": len(g),
                        "pickups": pickups, "overflow": False}


def _chain(w, room_id, mask, greedy_bits, parent, key, nodes):
    """Узел команды и, если можно, узлы жадного подбора после него."""
    first = [room_id, mask, parent, key, False]
    if not greedy_bits:
        return [first]
    steps = w.close(w.rooms[room_id], mask, greedy_bits)
    if not steps:
        return [first]
    out = [first]
    # Первый узел ещё не в nodes, поэтому цепочка подборов растёт от него.
    base = len(nodes)
    for skey, smask in steps:
        out.append([room_id, smask, base, skey, True])
        base += 1
    return out


def route(nodes, final):
    """Разворачивает цепочку узлов в список шагов от старта."""
    steps = []
    n = final
    while True:
        steps.append(nodes[n])
        if nodes[n][2] < 0:
            break
        n = nodes[n][2]
    steps.reverse()
    return [s for s in steps if s[3] is not None]


def replay(w, steps):
    """Проверка маршрута исполнением: доигрывает шаги и смотрит, чем кончилось.

    Маршрут из графа можно напечатать и ошибиться; если он не повторяется,
    он не маршрут.
    """
    room_id = w.sc.start
    mask = 0
    for step in steps:
        key = step[3]
        room = w.rooms[room_id]
        hit = None
        for k, idx in w.executable(room, mask):
            if k == key:
                hit = idx
                break
        if hit is None:
            return "шаг \u00ab%s\u00bb недоступен в комнате %s" % (
                " ".join(key), room_id)
        rule = room.rules[hit]
        mask = w.apply(room, hit, mask)
        if rule.act == "go":
            room_id = rule.arg
        elif rule.act in ("end", "win"):
            return rule.line
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description="Поиск пути к финалу по состояниям.")
    ap.add_argument("script", help="файл .script")
    ap.add_argument("--line", type=int, action="append",
                    help="строка концовки, к которой идём (можно много раз)")
    ap.add_argument("--endings", action="store_true",
                    help="проверить все концовки скрипта по очереди")
    ap.add_argument("--show", action="store_true",
                    help="напечатать маршрут (только с одним --line)")
    ap.add_argument("--no-greedy", action="store_true",
                    help="без жадного подбора предметов")
    ap.add_argument("--limit", type=int, default=1000000,
                    help="предел состояний на один поиск")
    args = ap.parse_args(argv)

    with open(args.script, encoding="utf-8") as f:
        sc = parse(f.read())

    w = World(sc)
    greedy_bits = 0
    if not args.no_greedy:
        items, forbidden = greedy_items(sc)
        for name in items:
            greedy_bits |= w.bits[name]
        print("жадный подбор: %d предметов, отклонено: %s"
              % (len(items), ", ".join(sorted(forbidden)) or "нет"))
    else:
        print("жадный подбор: выключен")

    if args.endings:
        wanted = [e[0] for e in endings_of(w)]
    elif args.line:
        wanted = args.line
    else:
        ap.error("нужен --line или --endings")

    known = {e[0] for e in endings_of(w)}
    for line in wanted:
        if line not in known:
            ap.error("строка %d не является концовкой" % line)

    kind = {e[0]: e[1] for e in endings_of(w)}
    room_of = {e[0]: e[2] for e in endings_of(w)}

    print("концовок в скрипте: %d, проверяем: %d, предел: %d состояний"
          % (len(known), len(wanted), args.limit))
    reached = 0
    longest = None
    last_steps = None
    last_line = None
    print("%-6s %-5s %-12s %s" % ("строка", "вид", "комната", "вердикт"))
    for line in wanted:
        nodes, final, st = search(w, greedy_bits, {line}, args.limit)
        if st["overflow"]:
            verdict = "ПРЕДЕЛ, это не значит, что её нет"
        elif nodes is None:
            verdict = "НЕДОСТИЖИМА"
        else:
            steps = route(nodes, final)
            again = replay(w, steps)
            last_steps, last_line = steps, line
            if again != line:
                verdict = "МАРШРУТ НЕ ПОВТОРЯЕТСЯ: %s" % (again,)
            else:
                reached += 1
                if longest is None or len(steps) > longest[1]:
                    longest = (line, len(steps))
                verdict = "да, %2d шагов, разобрано %d состояний" % (
                    len(steps), st["expanded"])
        print("%-6d %-5s %-12s %s"
              % (line, kind[line], room_of[line], verdict), flush=True)

    print("достигнуто концовок: %d из %d" % (reached, len(wanted)))
    if longest:
        print("самый длинный маршрут: строка %d, %d шагов" % longest)
    if args.show and last_steps:
        print("\nмаршрут к строке %d:" % last_line)
        room = w.sc.start
        for i, step in enumerate(last_steps, 1):
            print("  %2d. [%s] %s" % (i, step[0], " ".join(step[3])))
    return 0 if reached == len(wanted) else 3


if __name__ == "__main__":
    sys.exit(main())
