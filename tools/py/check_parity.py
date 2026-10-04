#!/usr/bin/env python3
"""Сверяет кадры обхода между сборками Linux и Windows.

Один исходник, один скрипт, один и тот же вход — а кадры различаются. Так нашлась
подсветка, которая не работала «только под Windows»: платформенного кода в том месте
не было, но game_room_text_spans не сообщала, сколько отрезков записала, и вызывающие
искали конец массива по записи, которую никто не делали. На Linux там лежали нули, на
MinGW — нет.

Написано после того, как это ушло полдня: 68 тестов баг не видели, статический аудит
не видел, а увидели кадры, сравнённые попиксельно.

Пропускает себя, а не проходит, когда нет Wine или кросс-компилятора. Молчаливый
успех там, где проверка не performed, хуже отсутствия проверки.
"""

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

GAMES = ("tutorial", "field", "heart")
WIN_CC = "x86_64-w64-mingw32-gcc"
EXE = os.path.join("build", "quest.exe")


def have(tool):
    return shutil.which(tool) is not None


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def run_walk(binary, game, outdir, wine):
    """Один обход. Кадры обязаны появиться: их нет — это ошибка, а не пустой результат."""
    argv = ([wine] if wine else []) + [binary, game, "--walk", outdir]
    proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=600)
    if proc.returncode != 0:
        return "обход %s вернул %d: %s" % (
            binary, proc.returncode, proc.stderr.decode("utf-8", "replace")[-400:])
    return None


def frames(outdir):
    return sorted(f for f in os.listdir(outdir) if f.endswith(".ppm"))


def compare_game(game, tmp, use_wine):
    a = os.path.join(tmp, game + "-linux")
    b = os.path.join(tmp, game + "-windows")
    os.makedirs(a, exist_ok=True)
    os.makedirs(b, exist_ok=True)

    err = run_walk(os.path.join("build", "quest"), game, a, None)
    if err:
        return [err]
    err = run_walk(EXE, game, b, "wine" if use_wine else None)
    if err:
        return [err]

    fa, fb = frames(a), frames(b)
    if not fa:
        return ["%s: linux не записал ни одного кадра" % game]
    if fa != fb:
        return ["%s: кадров разное количество, linux=%d windows=%d"
                % (game, len(fa), len(fb))]

    bad = []
    for name in fa:
        if digest(os.path.join(a, name)) != digest(os.path.join(b, name)):
            bad.append(name)
    if bad:
        return ["%s: кадры различаются: %s" % (game, ", ".join(bad))]
    return []


def main():
    if not have("wine") or not have(WIN_CC) or not os.path.exists(EXE):
        missing = []
        if not have("wine"):
            missing.append("wine")
        if not have(WIN_CC):
            missing.append(WIN_CC)
        if not os.path.exists(EXE):
            missing.append(EXE)
        print("пропуск: нет %s" % ", ".join(missing))
        print("соберите Windows-бинарник: make win")
        return 0

    with tempfile.TemporaryDirectory(prefix="parity-") as tmp:
        problems = []
        for game in GAMES:
            problems += compare_game(game, tmp, use_wine=True)
            print("%-10s %s" % (game, "различается" if problems else "совпадает"))

    if problems:
        print("\nрасхождение сборок:")
        for p in problems:
            print("  " + p)
        return 1
    print("\nвсе %d игр: кадры обхода совпадают побайтово" % len(GAMES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
