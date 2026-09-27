# M2 plan: hardening

Implementation plan for whoever codes it. Everything needed is in this file,
`docs/resolver-plan.md` (the overall plan) and `docs/innertube-notes.md` (the
protocol facts and what the POC observed live). Branch: `m2/hardening`, stacked on
`poc/resolve` (Kamil merges through pull requests, one per milestone, so the
branches stay separate and nothing is pushed by anyone but him).

## Where the POC left things

`poc/resolve` resolves one video through the `visionos` client. Two requests
per resolve: the watch page (for `visitorData`, without which YouTube
bot-checks 9 of 10 videos) and the player API. 1.1 to 1.7 s per resolve, most
of it the page. Cancellation lands within about a second, because curl's easy
interface polls its progress callback once a second. Errors are specific
except that the bot check and a genuine sign-in requirement share
`LoginRequired`. The client table has one entry. There is no differential
check against yt-dlp beyond one manual run.

M2 turns that into something the bot could link: fast in the steady state,
cancellable within the bot's 50 ms poll, honest about what went wrong, able to
fall through a ladder of clients, and watched by a harness that says when
YouTube changed something.

## 1. Visitor-data cache (the big win)

`Resolver::Impl` keeps `{visitorData, fetchedAt}` behind a mutex. `resolve()`
uses the cached value if there is one; only a cold Resolver fetches the page
first. If the player request comes back as a bot check, fetch the page once
more, replace the cached value, and retry the player request once. If that is
still a bot check, return `BotCheck`. A watch-page failure keeps whatever was
cached. Expire the cache after 6 hours as belt and braces.

Steady state is one request per resolve. Measure it: report the time of the
second resolve on a warm Resolver next to the POC's 1.1 to 1.7 s.

Send `Cookie: SOCS=CAI` on the watch-page request, as yt-dlp does, so an EU
consent interstitial cannot replace the page on other networks (none was seen
from Poland; the header is cheap).

## 2. Cancellation through curl's multi interface

Replace `curl_easy_perform` in `CurlHttpClient::send` with one multi handle
per call driving the one easy handle: `curl_multi_perform`, then
`curl_multi_poll` with a 20 ms timeout, checking `HttpRequest::cancelled`
between polls. Set `CURLOPT_QUICK_EXIT` so a cancel during a threaded DNS
lookup does not wait for `getaddrinfo`. Keep the progress callback as the
in-transfer check. Map an abort to `Error::Cancelled` exactly as today.

Measure it against a non-routable address (10.255.255.1) with a cancel at
100 ms: the call must return within 200 ms. That check touches the network
stack, so it lives in `ytres_live_tests` (section 7), not in the default
suite.

## 3. Error taxonomy

Add two codes to `ytres::Error`, with the header comment saying what a caller
should do with each:

- `BotCheck` — YouTube wants proof the caller is not a bot ("Sign in to
  confirm you're not a bot"). About who is asking, not the video: retry with
  fresh visitor data (the library does that once itself), then fall back to
  another resolver. Today this is mapped to `LoginRequired`; move it. A
  `LOGIN_REQUIRED` whose reason mentions "not a bot" is `BotCheck`; any other
  `LOGIN_REQUIRED` stays `LoginRequired`.
- `Internal` — a bug or resource failure inside the library, never YouTube's
  doing. Unexpected exceptions map here instead of `Parse`, so a client
  ladder does not read an out-of-memory as "this client stopped working".

Update the enum's copy in `docs/resolver-plan.md` to match.

## 4. Client ladder

Honour `Options::clients` in order. The table in `innertube.cpp` gains a
second real entry, `ClientId::Web`, from yt-dlp's `INNERTUBE_CLIENTS['web']`
(`clientName` WEB, `clientVersion` 2.20260708.00.00,
`INNERTUBE_CONTEXT_CLIENT_NAME` 1, the standard desktop user agent). It needs
the JS player, so it will yield ciphered formats and end in `NoFormats`; it is
here to exercise the ladder with a real client, and as the template for the
next client that works. The default stays `{VisionOS}`.

Which failures move to the next client and which stop the ladder:

| From a client | Next client? |
|---|---|
| `BotCheck` (after the one retry), `NoFormats`, `Http`, `Parse` | yes |
| `Unavailable`, `AgeRestricted`, `GeoBlocked`, `LoginRequired` | no — the video is the problem |
| `Cancelled`, `Timeout`, `Network`, `Internal` | no |

The message of the final failure names every client tried. Log each step.

Add `--client <name>` to the CLI so a client can be exercised alone, and
record a `web` player response as a fixture with `--dump`.

## 5. Deadline propagation

Each HTTP request's timeout is the smaller of `Options::requestTimeout` and
what remains of `Request::deadline`. A resolve of two or three requests must
never overrun its deadline. Test with the fake: a deadline of 50 ms and a
fake that reports the timeout it was given.

## 6. Concurrency

A test runs eight threads doing twenty resolves each against the fake through
one Resolver, with the cache enabled, and asserts every result is `Ok` with
the expected id, and that the page was fetched no more than eight times
(once per thread at worst, once in total if the mutex does its job). MSVC has
no thread sanitizer; the point is that nothing crashes and the counts hold.

## 7. Live tests and the differential harness

Two things that touch the network, both excluded from the default `ctest`:

- `ytres_live_tests`: a doctest target that resolves the corpus below and
  checks each expected error code, plus the cancellation-latency check from
  section 2. Runs on demand.
- `tools/differential.py` (Python 3, stdlib only): for every id in
  `tests/corpus.txt`, run `ytres_cli` and `yt-dlp.exe` (`--no-playlist
  --no-warnings -f bestaudio --print title --print webpage_url --print
  format_id --print urls`), compare title, page URL and itag, print one row
  per id with both timings, and exit non-zero on any mismatch. yt-dlp's path
  comes from `--yt-dlp` or from PATH; the one on this machine is
  `C:\ytdlp\yt-dlp.exe`.

`tests/corpus.txt`: one id per line with a comment saying what it is for.
Build it by searching with `yt-dlp.exe --flat-playlist --print id
"ytsearch1:<query>"` and then confirming each one behaves as expected:
an ordinary video (`dQw4w9WgXcQ`), a Polish title (search Dawid Podsiadło),
one over an hour (search "10 hours"), a 24/7 live stream (search "lofi girl
live"; expect `NoFormats`, live streams need the manifest), an auto-generated
"- Topic" music upload, an age-restricted one (search and confirm
`AgeRestricted`), and the deleted id `00000000000` (`Unavailable`).

Document both in the README.

## Out of scope for M2

Search and playlists (M3, M4). The JS tier (M5). A static vcpkg triplet
(M6). Connection sharing across requests (a `CURLSH` share handle with
`CURL_LOCK_DATA_CONNECT`) — worth doing later, but the cache already removes
the request it would have helped most.

## Report back

1. Warm-resolve time on the cache versus the POC's 1.1 to 1.7 s.
2. Cancellation latency measured, before and after.
3. The corpus with each id's outcome, and the differential harness's table.
4. Build warnings (0 expected), `ctest` summary, and the live-test summary.
5. `git log --oneline master..m2/hardening` and the files added or changed.
6. Any deviation from this plan and why, and anything M3 should know.
