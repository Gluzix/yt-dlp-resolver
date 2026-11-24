# Black-box tests in Python: plan

Implementation plan for whoever codes it. Everything needed is in this file,
`README.md` (how the harness is run and what it prints) and
`docs/innertube-notes.md`. Branch: `tests/python-blackbox`, stacked on
`fix/dump-scrub` (the dump scrubber's fix, which the dump tests need).
Kamil merges through pull requests; nothing is pushed by anyone but him.

## What this is, and what it is not

An **addition** to the C++ tests, never a replacement. `ytres_tests`
(doctest, offline, recorded fixtures) and `ytres_live_tests` stay exactly as
they are, and `ctest` keeps running only `ytres_tests`.

The new layer tests the library from the outside, the way a user of the
harness would: it runs `ytres_cli`, reads what it prints, and compares that
with what `yt-dlp` says about the same video, search or playlist. Two things
make Python the right tool for it: driving two programs and comparing their
output is a few lines, and a readable report is easy to print. Its real job
is to notice YouTube changing something before the bot's users do.

`tools/differential.py` already does this for single videos. It stays as it
is in this round; whether to fold it into the new suite is Kamil's decision
afterwards.

## Layout

```
tests/blackbox/
  requirements.txt     pytest, nothing else
  conftest.py          options, fixtures, the live gate, the report
  harness.py           running a program with a timeout; ytres_cli and yt-dlp
                       as Python functions that return plain data
  test_offline.py      what needs no network
  test_resolve_live.py
  test_search_live.py
  test_playlist_live.py
  test_dump_live.py
  corpus_search.txt    queries and what to expect of each
  corpus_playlists.txt playlists and what to expect of each
```

Videos keep coming from `tests/corpus.txt`, which `ytres_live_tests` and
`tools/differential.py` read too: one corpus of videos, three readers.

Python 3.9 or later, standard library plus pytest. No package is installed
into the machine's own Python: the README tells people to make a virtual
environment in `.venv/` (git-ignored).

```
python -m venv .venv
.venv\Scripts\python -m pip install -r tests/blackbox/requirements.txt
.venv\Scripts\python -m pytest tests/blackbox            (offline part only)
.venv\Scripts\python -m pytest tests/blackbox --live     (everything; talks to YouTube)
```

## Options

- `--live`: without it every test that touches the network is skipped, with
  the reason shown. A plain `pytest` run must never send a request.
- `--ytres-cli PATH`: default `build/Debug/ytres_cli.exe` (no `.exe` off
  Windows), or the `YTRES_CLI` environment variable. Missing: the whole run
  stops with one clear line saying to build it or pass the option.
- `--yt-dlp PATH`: default the `YT_DLP` environment variable, else the one on
  `PATH`. Missing: every comparison with yt-dlp is skipped with the reason;
  the tests of ytres on its own still run.

## The harness module

`harness.py` holds what every test needs, each piece small and typed:

- `run(command, timeout)`: starts the program without a shell, passes
  arguments as a list, decodes both streams as UTF-8, and on a timeout kills
  the whole process tree (on Windows `taskkill /F /T`, because `yt-dlp.exe`
  is a launcher whose child would otherwise keep the pipes open — the fix
  `tools/differential.py` already carries).
- `Ytres`: `resolve(target)`, `search(query, max)`, `playlist(target, max)`,
  each returning a small dataclass or a failure `(code, message)` parsed from
  the last `"<Error>: <message>"` line on stderr; `dump` variants that also
  write the scrubbed response to a file.
- `YtDlp`: `best_audio(video)`, `search_ids(query, n)`, `playlist(url, n)`,
  always with `--no-warnings --encoding utf-8`.
- Readers for the three corpus files, in the format `tests/corpus.txt`
  already uses: fields, then `#` and what the line is for.
- `redact(text)`: stream urls carry the requesting IP address. Nothing the
  suite prints, asserts on or writes may show a stream url or an address:
  every message goes through `redact`, which reduces a stream url to its host
  and itag.

## Being a good citizen

- Live runs are paced: at least 1.5 seconds between one program start and
  the next, enforced in one place.
- The first sign of a bot check (`BotCheck`, "not a bot", HTTP 429) ends the
  live part of the run: every later live test is skipped with that reason
  rather than hammering on.
- A whole live run stays under about 45 runs of `ytres_cli` and 20 of
  `yt-dlp`, and under five minutes.

## What is tested

**Offline** (`test_offline.py`, no network, safe anywhere): the usage text
and exit code 1 for no arguments, an unknown option, a repeated option and
two modes at once; `--max` with no value, a non-number and a negative
number; `BadInput` with `"<Error>: <message>"` on stderr and exit code 1 for
a target that is not a YouTube link, `--search ""`, `--search x --max 0`,
`--playlist` with a mix id (`RDdQw4w9WgXcQ`) and with `--max 0`. These
answers come back before any request is made.

**Videos** (`test_resolve_live.py`, one test per line of `tests/corpus.txt`):
the expected code for each; for the ones that play, three lines on stdout —
a title, `https://www.youtube.com/watch?v=<id>`, and a stream url on a
`googlevideo.com` host with an `itag` and an `expire` in the future — and
the same title, page url and itag as `yt-dlp -f bestaudio`.

**Search** (`test_search_live.py`, from `corpus_search.txt`): for a common
query, exactly `max` rows at `max` 5 and at 25 (two pages), every id valid
and distinct, every row with a title and a channel; a query whose first
result is stable ("rick astley never gonna give you up" → `dQw4w9WgXcQ`); a
query in Polish letters arriving intact (the first result's channel or title
holds the letters; this is the regression test for the UTF-8 manifest); a
nonsense query giving exit code 0 and no rows. Against `yt-dlp ytsearchN:`
the comparison is tolerant, since the two ask YouTube a little differently:
ytres's first result is among yt-dlp's first ten, and at least half of
ytres's first ten are among yt-dlp's first twenty.

**Playlists** (`test_playlist_live.py`, from `corpus_playlists.txt`): the
7-video playlist `PLC2bGavj05vj1BDIQbhCDiXywCU4_P9Pk` (title, count, ids);
the long one `PLrAXtmErZgOdP_8GztsuKi9nrraNbKKp4` at `max` 150 (two pages,
150 distinct entries, count at least 150); the same playlist given as a
`watch?v=...&list=...` link; a YouTube Music curated playlist
(`RDCLAK5uy_...`, the one `tests/fixtures/playlist_music_curated.json` was
recorded from); a playlist that does not exist → `Unavailable`, "does not
exist". Against `yt-dlp --flat-playlist --playlist-items :N --print id` the
comparison is exact: the same ids in the same order.

**Dumps** (`test_dump_live.py`): `--dump` for a video, a search and a
playlist writes JSON that parses and holds no IPv4 address but
`203.0.113.7` and no visitor data but `FIXTURE`.

## The report

At the end of a live run pytest prints one table, built from what the tests
recorded: one row per video, search and playlist with what ytres said, what
yt-dlp said, how long each took and the verdict. `--junitxml` works as for
any pytest run, for a scheduled run to keep.

## Keeping it honest

- A live failure message shows both sides (redacted), so that "YouTube
  changed" can be told from "we broke something" at a glance.
- Videos and playlists go away. The corpus files say, as `tests/corpus.txt`
  does, how each entry was found and that a vanished one is replaced by
  another of its kind rather than by a weaker expectation.
- The suite's own pure functions (the corpus readers, the parsers of
  `ytres_cli`'s output, `redact`) get offline tests of their own.

## Documentation

README: a "Black-box tests" section (what they are, the four commands, the
options, what a failure means). `docs/walkthrough.md`: the tests section
gains this fourth layer. `.gitignore`: `.venv/`, `.pytest_cache/`.

## Out of scope

Running on a schedule (a standing task on Kamil's PC is his to ask for).
Running in GitHub Actions. Testing the bot. Changing or retiring
`tools/differential.py`. Any change to the library or the harness — if a
test exposes a defect in them, report it; do not fix it here.
