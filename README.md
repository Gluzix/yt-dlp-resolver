# ytres

A C++17 library that turns a YouTube link into the video's title, its
canonical page URL and a direct audio stream URL, by asking YouTube's
InnerTube API the way yt-dlp does. It exists to replace the `yt-dlp.exe`
process the Discord bot starts for every track: the bot links the library
instead, and the one to two seconds of process startup per song go away.

It is a library. `ytres_cli` is a development harness for trying it against
live YouTube and recording test fixtures, not the product.

This is milestone M4 of [docs/resolver-plan.md](docs/resolver-plan.md),
playlists, planned in [docs/m4-plan.md](docs/m4-plan.md), on top of M3's
search ([docs/m3-plan.md](docs/m3-plan.md)) and M2's hardening
([docs/m2-plan.md](docs/m2-plan.md)): one video at a time, down a ladder of
InnerTube clients that by default holds `visionos` alone, the client that
needs neither YouTube's player JavaScript nor a PO Token. A `Resolver` keeps
the watch page's visitor data between resolves, so a warm resolve is one
request of about 150 ms; a cancel lands within about 25 ms, and one deadline
bounds every request of a call. A search is one request for the first
twenty or so videos, a playlist one request per hundred videos it lists.
[docs/innertube-notes.md](docs/innertube-notes.md) has the exact requests and
what YouTube answered to them.

> **M3 and M4 were written without being built.** Kamil asked that nothing
> be compiled or run while they were coded, so the search and playlist code,
> their tests and the `--search` and `--playlist` harness have never met a
> compiler. Until the steps under "Before this is done" in
> [docs/m3-plan.md](docs/m3-plan.md) and [docs/m4-plan.md](docs/m4-plan.md)
> are done - a build with 0 warnings, `ctest`, the live tests, two live
> searches, two live playlists and a comparison with yt-dlp - treat both as
> drafts.

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

## Using it

```cpp
ytres::Resolver resolver;

ytres::Result<ytres::VideoInfo> info = resolver.resolve("https://youtu.be/dQw4w9WgXcQ");
if (info) {
    std::optional<ytres::Format> audio = info.value.bestAudio();
    // info.value.title, info.value.webpageUrl, audio->url
}

ytres::Result<std::vector<ytres::SearchResult>> found = resolver.search("Dawid Podsiadło", 5);
for (const ytres::SearchResult &video : found.value) {
    // video.videoId, video.title, video.author, video.durationSeconds,
    // video.isLive, video.isUpcoming; ytres::watchUrl(video.videoId)
}

ytres::Result<ytres::Playlist> list = resolver.playlist("https://www.youtube.com/playlist?list=PLrAXtmErZgOdP_8GztsuKi9nrraNbKKp4", 50);
// list.value.title, list.value.totalCount
for (const ytres::PlaylistEntry &entry : list.value.entries) {
    // entry.videoId, entry.title, entry.durationSeconds; ytres::watchUrl(entry.videoId)
}
```

`search(query, max, request)` returns up to `max` videos, best match first,
and an empty list when YouTube finds none. It asks as YouTube's `web` client
with yt-dlp's videos-only filter, whatever `Options::clients` says, and
skips the channels, playlists and shelves YouTube still puts among the
results - an artist's name brings the artist's channel first. The first page
holds about twenty videos; a larger `max` follows YouTube's continuation, one
request per page, up to ten pages. It sends the visitor data a resolve has
cached, but fetches none of its own; with none cached, a later page carries
the visitor data YouTube named on the page before it, as yt-dlp does. A live stream comes back with
`isLive` and no length (`resolve()` answers it with `NoFormats`), a
scheduled one with `isUpcoming`: the first result that is neither is the
one to play. `max` of 0 or a blank query is `BadInput`. A first page on
which YouTube counts results but holds none the library can read is `Parse`,
not an empty list: YouTube has changed its answer, and the caller should
fall back rather than tell the user nothing was found. When a later page
fails, the result carries that failure *and* the videos read before it, so
a search cancelled on page three still has the first two.

`playlist(urlOrId, max, request)` returns the first `max` videos of a public
playlist in its order, with its title and the count YouTube gives for it
(`totalCount`, 0 when unknown). It takes a `youtube.com` `/playlist` or
`/watch` link, or a `youtu.be` link, that carries `list=`, or a bare id
starting `PL`, `UU`, `FL`, `OLAK5uy_`, `EC`, `UL` or `PU`. Like a search it
asks as the `web` client, sends the visitor data a resolve has cached and
fetches none of its own. YouTube sends a hundred videos a page, and the
library asks for no more pages than `max` needs: the first 50 videos of a
6000-video playlist cost one request. It reads 200 pages at most, so a list
longer than 20,000 videos (a big channel's uploads, `UU...`) comes back Ok
but cut there, with a warning in the log. YouTube hides unavailable videos
itself, and the library drops any it lists anyway (`[Private video]`,
`[Deleted video]`, no title), so `entries` may hold fewer than
`totalCount`. A playlist that does not exist is `Unavailable`, with
YouTube's words; a mix (`RD...`), Watch Later, Liked videos and the other
lists made for one signed-in viewer are `BadInput`, as is `max` of 0. As for
search, a first page that counts videos but holds none the library can read
is `Parse`, not an empty playlist, and a later page's failure comes back
with the title, the count and the entries read before it.

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

```
build\Debug\ytres_cli.exe --search "Dawid Podsiadło" --max 25
```

searches instead, and prints one line per video: the page URL, the length in
seconds (or `live` or `upcoming`), the channel and the title, separated by
tabs. `--max` defaults to 5; 25 takes two pages. A search that fails on a
later page prints the videos it got before the error. `--dump <file>` writes
the first search page, scrubbed the same way; `--formats` and `--client`
belong to a resolve and are refused with `--search`.

```
build\Debug\ytres_cli.exe --playlist PLrAXtmErZgOdP_8GztsuKi9nrraNbKKp4 --max 150
```

lists a playlist instead: a first line with its title and the count YouTube
gives, separated by a tab, then one line per video with the page URL, the
length in seconds and the title, separated by tabs. `--max` defaults to 50;
150 takes two pages. A playlist that fails on a later page prints what it
got before the error. `--dump <file>` writes the first browse response,
scrubbed; `--formats` and `--client` are refused with `--playlist` too.

Stream URLs expire after a few hours and work only from the IP address that
asked for them.

## Errors

Every call returns a `Status` whose `code` says what went wrong and whose
`message` is safe to show a user. What a caller, such as the bot, does with
each:

| Code | What happened | What to do |
|---|---|---|
| `Unavailable`, `AgeRestricted`, `GeoBlocked`, `LoginRequired` | The video is the problem: gone or private, age-gated, blocked in this country, or behind a sign-in. For a playlist, `Unavailable` means it does not exist. The message is YouTube's reason. | Tell the user why. Another resolver on the same line, without cookies, is unlikely to do better. |
| `BotCheck` | YouTube wants proof that the caller is no bot ("Sign in to confirm you're not a bot"). It is about who asks, not the video; the library has tried once to fetch fresh visitor data and, when it got some, asked once more. | Fall back to another resolver, such as yt-dlp, and stop asking the library for a while. |
| `NoFormats`, `Http`, `Parse` | Every client in the ladder failed: nothing usable came back (a live stream, SABR streaming only, a client YouTube refused), or an answer the library could not read. The message names each client and what it answered. | Fall back to another resolver. |
| `Network`, `Timeout` | The network failed, or a request timed out, or the call's deadline ran out. | Try again later, or fall back. |
| `Cancelled` | `Request::cancelled` said so. | Nothing. |
| `Internal` | A bug or a resource failure inside the library, never YouTube's doing. | Log it as a bug; another resolver may still get the video. |
| `BadInput` | Not a YouTube video link or id, an empty or unknown client list, a search for no videos or with a blank query, a playlist for no videos, not a public playlist's link or id (a mix, Watch Later or Liked videos among them), or a request timeout of zero or less. | Fix the call. |

When every client in the ladder fails, the code is the most telling of their
answers: `BotCheck` first, then `Http`, then `Parse`, then `NoFormats`. A
`Network`, `Timeout` or `Internal` failure that ends the ladder after earlier
clients failed keeps their answers in its message, and an earlier `BotCheck`
outranks a later `Network` or `Timeout`. `Cancelled` and the video's own
codes always come back as they are, with YouTube's words.

`PlayerScript` is reserved for a JavaScript tier that does not exist.

A search or a playlist has no ladder: it asks one client, and its codes are
the call's (`Cancelled`, `Timeout`, `Network`, `BadInput`, `Internal`) or
YouTube's answer (`Http` for a refusal such as a 429, `Parse` for an answer
it could not read, and for a playlist `Unavailable` when it does not exist).
A failure on a page after the first keeps what was already read in `value`.

## Fixtures

`tests/fixtures/` holds real responses, recorded with `ytres_cli --dump` and
scrubbed of the requesting address (203.0.113.7) and visitor data
(`FIXTURE`):

| Fixture | What it is |
|---|---|
| `player_dQw4w9WgXcQ.json` | `visionos`'s answer for an ordinary video |
| `player_web_dQw4w9WgXcQ.json` | `web`'s refusal of the same video without a PO Token |
| `player_bot_check.json` | the bot check, asked without visitor data |
| `player_unavailable.json` | a video that does not exist |
| `search_videos.json` | the first page for "Dawid Podsiadło": a channel, four videos, the continuation |
| `search_continuation.json` | its second page: three videos and the next continuation |
| `search_empty.json` | a search that found nothing |
| `search_live.json` | "lofi girl live": three live streams |
| `playlist_small.json` | a 7-video playlist on one page, in the new `lockupViewModel` layout |
| `playlist_long_first.json` | the first page of a 447-video playlist: three videos and the continuation among them |
| `playlist_long_continuation.json` | its second page: three videos and the next continuation |
| `playlist_missing.json` | a playlist that does not exist |

The search and playlist pages are cut down to a few entries each. The old
playlist layout (`playlistVideoRenderer`) could not be recorded any more, so
its test page is written by hand and says so.

## Live tests

`ytres_live_tests` checks the library against live YouTube. It resolves every
video in the corpus and checks that each gets the code the corpus expects,
checks that a warm resolve is one request, cancels a request to the
unroutable 10.255.255.1 and a live resolve at 100 ms and wants both back
within 200 ms, walks the client ladder from `web` to `visionos`, searches
once ("Rick Astley", five videos) and wants each result to carry a valid
id, a title and a channel, and lists the first 150 videos of a 447-video
playlist and wants two requests, a title, a count above 150 and 150 entries
with valid ids and titles. The search and the playlist are what notice
YouTube changing the shape of those answers: the offline tests only replay
the answers recorded on 2026-10-01. It is built with everything else but
never added to `ctest`, since it needs the network and YouTube changes
under it:

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
