# ytres

A C++17 library that turns a YouTube link into the video's title, its
canonical page URL and a direct audio stream URL, by asking YouTube's
InnerTube API the way yt-dlp does. It exists to replace the `yt-dlp.exe`
process the Discord bot starts for every track: the bot links the library
instead, and the one to two seconds of process startup per song go away.

It is a library. `ytres_cli` is a development harness for trying it against
live YouTube and recording test fixtures, not the product.

This is milestone M2 of [docs/resolver-plan.md](docs/resolver-plan.md), the
hardening of the M0 and M1 proof of concept, planned in
[docs/m2-plan.md](docs/m2-plan.md): one video at a time, down a ladder of
InnerTube clients that by default holds `visionos` alone, the client that
needs neither YouTube's player JavaScript nor a PO Token. A `Resolver` keeps
the watch page's visitor data between resolves, so a warm resolve is one
request of about 150 ms; a cancel lands within about 25 ms, and one deadline
bounds every request of a resolve. Search and playlists come later.
[docs/innertube-notes.md](docs/innertube-notes.md) has the exact requests and
what YouTube answered to them.

## Requirements

Windows 10 or 11, Visual Studio 2022 (MSVC), CMake 3.20+ and vcpkg at
`C:/vcpkg`. libcurl, nlohmann-json and doctest come through the `vcpkg.json`
manifest; the first configure builds libcurl from source and takes a few
minutes. The differential harness also wants Python 3 and yt-dlp.

## Building

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

`ctest` runs `ytres_tests`, which never touches the network: it replays
responses recorded in `tests/fixtures/`.

## Running

```
build\Debug\ytres_cli.exe https://www.youtube.com/watch?v=dQw4w9WgXcQ
```

prints the title, the canonical page URL and the best audio stream URL, one
per line: the three lines the bot reads from yt-dlp today. On failure it
prints the error code and YouTube's reason to stderr and exits with 1.

- `--formats` lists every usable format instead.
- `--dump <file>` also writes YouTube's player response to `<file>`; that is
  how fixtures are recorded. The dump is scrubbed of the requesting IP and
  visitor data automatically.
- `--client visionos|web` asks that one InnerTube client instead of the
  library's ladder, to see what YouTube tells it today. `web` needs the
  player JavaScript and a PO Token, so YouTube turns it away (`NoFormats`);
  it is in the table to exercise the ladder.

Stream URLs expire after a few hours and work only from the IP address that
asked for them.

## Errors

Every call returns a `Status` whose `code` says what went wrong and whose
`message` is safe to show a user. What a caller, such as the bot, does with
each:

| Code | What happened | What to do |
|---|---|---|
| `Unavailable`, `AgeRestricted`, `GeoBlocked`, `LoginRequired` | The video is the problem: gone or private, age-gated, blocked in this country, or behind a sign-in. The message is YouTube's reason. | Tell the user why. Another resolver on the same line, without cookies, is unlikely to do better. |
| `BotCheck` | YouTube wants proof that the caller is no bot ("Sign in to confirm you're not a bot"). It is about who asks, not the video; the library has tried once to fetch fresh visitor data and, when it got some, asked once more. | Fall back to another resolver, such as yt-dlp, and stop asking the library for a while. |
| `NoFormats`, `Http`, `Parse` | Every client in the ladder failed: nothing usable came back (a live stream, SABR streaming only, a client YouTube refused), or an answer the library could not read. The message names each client and what it answered. | Fall back to another resolver. |
| `Network`, `Timeout` | The network failed, or a request timed out, or the call's deadline ran out. | Try again later, or fall back. |
| `Cancelled` | `Request::cancelled` said so. | Nothing. |
| `Internal` | A bug or a resource failure inside the library, never YouTube's doing. | Log it as a bug; another resolver may still get the video. |
| `BadInput` | Not a YouTube video link or id, or an empty or unknown client list. | Fix the call. |

When every client in the ladder fails, the code is the most telling of their
answers: `BotCheck` first, then `Http`, then `Parse`, then `NoFormats`. A
`Network`, `Timeout` or `Internal` failure that ends the ladder after earlier
clients failed keeps their answers in its message, and an earlier `BotCheck`
outranks a later `Network` or `Timeout`. `Cancelled` and the video's own
codes always come back as they are, with YouTube's words.

`PlayerScript` is reserved for a JavaScript tier that does not exist.

## Live tests

`ytres_live_tests` checks the library against live YouTube. It resolves every
video in the corpus and checks that each gets the code the corpus expects,
checks that a warm resolve is one request, cancels a request to the
unroutable 10.255.255.1 and a live resolve at 100 ms and wants both back
within 200 ms, and walks the client ladder from `web` to `visionos`. It is
built with everything else but never added to `ctest`, since it needs the
network and YouTube changes under it:

```
build\tests\Debug\ytres_live_tests.exe
```

A failure there says "look" rather than "the library broke": a corpus video
may have gone, or YouTube may have changed something.

## The corpus

`tests/corpus.txt` holds the videos the live tests and the harness resolve,
one per line: the id, the `Error` ytres should report for it (`Ok` for one
that plays), and after a `#` what it is for. Between them they cover an
ordinary video, a Polish title, one over an hour, a 24/7 live stream, an
auto-generated "- Topic" upload, an age-restricted video and one that does
not exist. When a video goes away, replace it with another of its kind: find
one with `yt-dlp --flat-playlist --print id "ytsearch1:<query>"` and confirm
it with `ytres_cli`.

## Differential harness

```
python tools/differential.py [--yt-dlp PATH] [--ytres PATH] [--corpus PATH]
```

runs `ytres_cli` and yt-dlp (`-f bestaudio`) on every video in the corpus and
compares the title, the page URL and the itag each picks, printing a row per
video with both timings; it exits non-zero on any mismatch. It is the early
warning that YouTube changed something. yt-dlp comes from `--yt-dlp` or PATH;
`ytres_cli` defaults to `build\Debug\ytres_cli.exe`. A video the corpus
expects ytres to refuse passes when ytres refuses it with that code, whatever
yt-dlp makes of it: yt-dlp plays a live stream through HLS, which the library
does not read. Python 3, standard library only.
