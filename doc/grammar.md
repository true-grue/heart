# Script DSL

The format describing rooms, flags and player commands. Parsed by `src/dsl/`,
never compiled to C: content is data, and the loader turns it into structures.

## The one thing to remember

Every line is **words and conditions, colon, effects and consequence**.

```
: always true.
+фонарик: if the flag is set.
-фонарик: if the flag is not set.
взять фонарик -фонарик : if the player does that, and the flag is not set.
показать конспекты декан +конспекты : a command of three words.
```

A description says nothing but conditions in front of the colon. A command says one
or more bare words first. **There is no keyword telling them apart, and that is the
point:** a description has no verb, so a head made only of signed items is prose, and
a head with at least one bare word is something the player can do.

A bare word cannot be a condition. `-а,б` used to be legal and is not any more,
because an unsigned word before the colon is now a command, and `открыть сейф
записка` would have read as two commands instead of one with a condition. Every item
carries its sign. The side of the colon says what the list means; the sign says
whether the flag must be there or must not.

Nothing else is structural. There are no brackets and no commas, and every reserved
character has exactly one job:

| | |
|---|---|
| `:` | between condition and consequence |
| `-` | the flag must be **absent** |
| `+` | the flag must be **present**, or, after the colon, **gets set** |
| `-` | the flag must be **absent**, or, after the colon, **gets cleared** |

Everything else is an English word: `room`, `start`, `go`, `end`, `win`. A word is
easier to hold in the head than a symbol, and the format already speaks in English
keywords, so an arrow would only be a second way of saying the same thing.

## Grammar

The grammar itself is `doc/grammar.bnf`, and it is the only place it is written
down. This file explains it; it does not restate it, because a copy is a second
version that will drift. Every production in the `.bnf` has a test in
`tests/tests.c` named after it.

`text` runs to the end of the line and keeps every interior space. Only trailing
spaces and tabs are dropped. Nothing inside `text` is escaped and nothing in it is
reserved, so a line may begin with a quotation mark and still be prose:

```
идти подвал -_guard_defeated : "Туда-а нельз-з-зя", — шипит охранник.
```

A room owns every description and command that follows it, up to the next `room`.

## Semantics

**Commands — first match wins.** Within a room, the first command whose whole word
sequence and condition hold wins.

**Room descriptions — all matches print, in source order.** The unconditional line is
one fragment among many. First-match-wins here would make every conditional fragment
unreachable, so it cannot be the rule.

**Conditions** are a conjunction of flags, each with a sign. No condition means no
requirement, which is how a command with no guard is written.

**Effects** are applied before the action runs, so the text already sees the new flags,
and so does the room a `go` leads into. `-flag` clears one, which is how a command
takes something out of the player's hands.

**`go room text`** prints its text first and enters the room afterwards. The player
therefore reads "You climbed the icy steps" and then the description of the porch,
which is the order the source games used.

**`end` and `win`** finish the game.

## Why these rules and not other ones

- **Every item is signed.** It is what lets a description and a command share one
  line shape with no keyword standing between them.
- **A command is one to four words.** Four is `RULE_MAX_WORDS`, and the command
  palette has exactly that many slots, so a command the screen cannot show cannot
  exist. A fifth word is refused rather than guessed at.
- **How long a command is comes from the script, not from the loader.** The
  interface asks what may follow the words chosen so far, so a room of two-word
  commands plays exactly as it did before, and a three-word command grows a slot.
- **A word is a letter, a digit or an underscore.** A period is prose, so `go холл.`
  enters `холл` and prints nothing extra.

## What this format deliberately cannot do

- **One action per command.** The three games converted so far never need two, and
  dropping the separator is what removed the trap where a print swallowed the rest of
  the line.
- **No conditional effects.** An effect always happens when the rule runs; there is no
  way to say "and if the flag is there". A second guarded rule says it instead.
- **No unconditional trigger.** The source dialect has `TEST` lines that fire with no
  command; there is no counterpart here and the converter refuses them.
- **No disjunction, no comparisons, no counters.** A condition is a conjunction of
  plain flags.
- **No items as entities.** A flag and an inventory item are the same thing.
- **Unknown input is not expressible.** The palette offers only commands whose
  conditions hold, so the interface cannot produce a command the format cannot
  answer.

## Notes for authors

- Prose carries no keyword at all. `: В холле полумрак.`
- Do not begin a prose line with a bare word, or it will be read as a command. Sign
  the condition: `+фонарик:`, not `фонарик:`.
- Underscores in an id are for the eye: the palette shows `зал_заседаний` as
  `зал заседаний`, because that is a display concern.
- Prefer one word for a verb and one for an object: `осмотреть пальто`. They are
  buttons on screen, so each word is a chip.
- A flag whose id begins with `_` is state, not a thing the player carries. The
  interface hides those.
- The validator reports undefined flags, duplicate rooms, shadowed commands,
  unresolved targets, unreachable rooms and a missing `start`. A script that ships
  produces no diagnostics at all.

## Assets

`tutorial.script` and `heart.script` are written by hand. `field.script` came from a
generator that no longer exists: the scripts are now maintained directly
originals; edit the source or the tool, never the generated file.
