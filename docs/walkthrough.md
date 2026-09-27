# How the resolver works, step by step

A reading guide to the code on `m2/hardening`, written for the session where
we go through it together. Read it top to bottom once; then open the files
it names and read them in the same order. Every section ends with the
questions worth asking about it, which is where the session will spend its
time.

Nothing here is a substitute for `docs/resolver-plan.md` (why the project is
shaped the way it is), `docs/innertube-notes.md` (the protocol facts) and
`docs/m2-plan.md` (what M2 added). This is the tour; those are the maps.

## 1. What a "resolve" is

The bot needs three things to play a YouTube link: a title to show, the
canonical page URL to remember, and a direct URL to a media stream that
FFmpeg can open. Today it gets them by starting `yt-dlp.exe` and reading
three lines from its stdout. The library answers the same question in
process, in one function:

```cpp
ytres::Resolver resolver;
ytres::Result<ytres::VideoInfo> info = resolver.resolve("https://youtu.be/dQw4w9WgXcQ");
if (info) {
    std::optional<ytres::Format> audio = info.value.bestAudio();
    // info.value.title, info.value.webpageUrl, audio->url
}
```

Where does the answer come from? Not from the YouTube web page. Every
YouTube client — the website, the phone apps, the TV app — talks to one
JSON API called InnerTube. A client sends `POST /youtubei/v1/player` with
"who I am" (a client name and version) and "which video", and gets back a
JSON document with the video's details and a list of stream formats. That
document is the whole game. yt-dlp's YouTube extractor is, at its core, a
very careful InnerTube client; ours is a much smaller one.

The catch is *who you claim to be*. YouTube treats clients differently:

- Most of them get formats whose URLs are scrambled ("ciphered") and can only
  be unscrambled by running YouTube's player JavaScript — 2 MB of obfuscated
  code that changes every few days. That is the part of yt-dlp that needs a
  JavaScript runtime.
- Most of them also need a "PO Token": a proof-of-origin blob minted by
  running YouTube's BotGuard virtual machine. That is out of reach for a
  hobby project, by design.
- One client, as of September 2026, needs neither: `visionos`, the Apple
  Vision Pro app. yt-dlp's own source lists it as the client to use when no
  JavaScript runtime is available. We read that in their client table, and
  it became the whole proof of concept.

So the library never runs JavaScript and never mints tokens. It asks
InnerTube as `visionos`, and the URLs come back plain. When YouTube closes
that door — they closed a similar one, `android_vr`, on 2026-08-17 — the
client table is one row to edit, and the bot falls back to yt-dlp meanwhile.

*Questions for the session: why does the client identity matter to YouTube
at all? What is the bot check, and why does it target who is asking rather
than what is asked?*

## 2. One request, end to end

Open `docs/innertube-notes.md` next to this. The player request is:

```
POST https://www.youtube.com/youtubei/v1/player?prettyPrint=false
Content-Type: application/json
X-YouTube-Client-Name: 101
X-YouTube-Client-Version: 1.02
Origin: https://www.youtube.com
User-Agent: Mozilla/5.0 (Macintosh; ...) Safari/605.1.15
X-Goog-Visitor-Id: <visitor data>            (when we have it)

{ "context": { "client": { "clientName": "VISIONOS", "clientVersion": "1.02",
                           "deviceMake": "Apple", ..., "hl": "en",
                           "timeZone": "UTC", "utcOffsetMinutes": 0,
                           "visitorData": "<visitor data>" } },
  "videoId": "dQw4w9WgXcQ",
  "playbackContext": { "contentPlaybackContext": { "html5Preference": "HTML5_PREF_WANTS" } },
  "contentCheckOk": true, "racyCheckOk": true }
```

Every header and key is there because yt-dlp sends it. The rule in
`src/innertube.h` says why nothing more is sent: a field yt-dlp never sends
is a fingerprint YouTube could single out.

The response is a large JSON document. The parts we read:

| Path | What it is |
|---|---|
| `playabilityStatus.status` | `OK` or a failure such as `LOGIN_REQUIRED`, `UNPLAYABLE`, `ERROR` |
| `playabilityStatus.reason` (+ `errorScreen...subreason`) | YouTube's human-readable explanation |
| `videoDetails.videoId` | must equal what we asked for; YouTube occasionally answers about another video |
| `videoDetails.title`, `author`, `lengthSeconds`, `isLive` | the metadata |
| `streamingData.adaptiveFormats[]` | audio-only and video-only streams: `itag`, `url`, `mimeType`, `bitrate`, `averageBitrate`, `contentLength`, `audioTrack`, `isDrc`, ... |
| `streamingData.formats[]` | muxed audio+video streams (the `visionos` client sends none) |

Each format's `url` is a `googlevideo.com` link that expires (`expire=` in
its query, about six hours out) and only works from the IP address that
asked for it. That is why the bot resolves right before playing, and why
`VideoInfo::expiresAtUnix` exists for a consumer that queues.

**The visitor data.** The very first live run showed that the bare request
above gets "Sign in to confirm you're not a bot" for nine videos out of ten.
YouTube wants a *visitor id* — an anonymous token it hands out when you load
any YouTube page. The library gets one by fetching the watch page
(`https://www.youtube.com/watch?v=<id>`) once and pulling
`INNERTUBE_CONTEXT.client.visitorData` out of the `ytcfg.set({...})` blob in
the HTML. One token serves every video afterwards, so the page is fetched on
the first resolve and then cached (section 3, step 3).

*Questions: which of the request's fields are identity and which are
"context"? What in the response tells you the difference between "this
video cannot be played" and "you cannot play it"?*

## 3. The code path of `resolve()`

Files, in the order the data flows. Read `src/resolver.cpp` first and keep
it open; it is the spine, and every other file is one vertebra.

```
include/ytres/ytres.h      the public contract (types, Error, Resolver)
include/ytres/http.h       the HttpClient seam
src/resolver.cpp           Resolver::Impl: the flow below
src/url_parse.*            step 1
src/visitor_cache.*        step 3
src/watch_page.*           step 3
src/innertube.*            steps 4 and 5
src/format_pick.cpp        step 6
src/curl_http_client.*     every HTTP request (section 5)
```

**Step 0 — the guards.** `Resolver::resolve()` in `resolver.cpp` is a
`try`/`catch` around everything: no exception ever escapes the public API.
An exception that reaches it is a bug or an exhausted heap, so it becomes
`Error::Internal`, never something that reads like YouTube changing. Then
`Impl::resolve()` rejects a non-positive `requestTimeout` and clamps the
resolve's deadline to 0–24 h — `steady_clock` counts nanoseconds in 64 bits,
so `milliseconds::max()` as "no deadline" would wrap into the past.

**Step 1 — what video?** `parseVideoId()` in `url_parse.cpp` turns any
accepted form (`watch?v=`, `youtu.be/`, `/shorts/`, `/live/`, `/embed/`,
`music.youtube.com`, a bare 11-character id) into the id, or `BadInput`.
Pure string work, no I/O, and the only place that knows about URL shapes.
The canonical page URL is always rebuilt as `https://www.youtube.com/watch?v=<id>`.

**Step 2 — the ladder.** `Options::clients` (default `{VisionOS}`) becomes a
vector of pointers into the client table, resolved up front so a bad list
fails before anything is sent. Section 4 covers how the ladder is walked.

**Step 3 — visitor data.** `visitorDataToSend()` asks the `VisitorCache`.
If it has a value less than six hours old, that is used and no page is
fetched: this is the warm path, one request, 118–187 ms. Otherwise
`fetchVisitorData()` GETs the watch page (with the client's user agent and
a `SOCS=CAI` cookie so an EU consent page cannot replace it), and
`watchpage::visitorData()` scans the HTML for the `ytcfg.set(` object and
reads the token out of it. The token is validated (length ≤ 4096, a small
character set) before it can become a header. Whatever is found goes into
the cache; a page that fails or has no token costs a warning, not the
resolve — the player request is still worth making bare, and a stale token
still beats none.

**Step 4 — the player request.** `askPlayer()` builds the request through
`innertube::playerRequest()` (the client's row from the table plus the id,
language and visitor data), sends it, and hands the body to
`innertube::parsePlayerResponse()`.

**Step 5 — reading the answer.** `parsePlayerResponse()` in `innertube.cpp`
is deliberately paranoid: every JSON read is type-checked through small
helpers (`child`, `readString`, `readInt`, `isTruthy`), and a missing or
wrong-typed field reads as empty or zero, never as a throw. Because this
library runs inside the bot, an unchecked `.at()` on a surprising response
would be a bot-killing bug. The order of checks:

1. `playabilityStatus.status` — four statuses are accepted (`OK`,
   `LIVE_STREAM_OFFLINE`, `AGE_CHECK_REQUIRED`, `AGE_VERIFICATION_REQUIRED`);
   anything else goes to `playabilityFailure()`, section 4.
2. `videoDetails.videoId` must match, else `Parse`.
3. The metadata is copied out.
4. Both format arrays are walked. A format is skipped, with a count kept
   for the error message, when it has a `signatureCipher` and no `url`
   (needs the JavaScript we do not run), or when yt-dlp itself would never
   pick it: a live segment (`targetDurationSec`), an OTF stream, or DRM
   (`drmFamilies`). The `isTruthy()` helper exists because yt-dlp tests
   these fields with Python truthiness — `drmFamilies: []` counts as absent.
5. Expiry: `expire=` from the first URL's query, else now +
   `expiresInSeconds`.
6. No usable format left → `NoFormats`, with a message that says why
   (a live stream, for instance, needs the HLS manifest we do not fetch).

**Step 6 — which stream.** `VideoInfo::bestAudio()` in `format_pick.cpp`
picks, among audio-only formats, itag 251 (Opus), else 140 (AAC), else the
highest bitrate — first among formats that are the original audio track and
not a dynamic-range-compressed copy, and only if that finds nothing, among
all audio. The two-pass rule exists because a dubbed video lists one 251 per
language and `audioTrack.audioIsDefault` marks the original; the first pass
of review found that the wrong-language dub could be picked.

*Questions: at which step would a change in YouTube's JSON show up, and
what would it look like from the outside? Why is `parsePlayerResponse()`
pure (the clock comes in as a parameter)?*

## 4. Errors, and what the bot should do with them

`ytres::Error` in `ytres.h` is the contract the bot will program against.
Grouped by what a caller does:

| The video is the problem — tell the user | `Unavailable`, `AgeRestricted`, `GeoBlocked`, `LoginRequired` |
| YouTube's view of the caller — fall back, back off | `BotCheck`, `Http` (429, 5xx) |
| The client cannot read this — try the next client | `NoFormats`, `Parse`, `PlayerScript` |
| The call itself | `Cancelled`, `Timeout`, `Network`, `BadInput` |
| A bug in the library — log it | `Internal` |

The most instructive function is `playabilityFailure()` in `innertube.cpp`.
YouTube reports an age gate, a private video *and* the bot check all as
`LOGIN_REQUIRED`; only the reason text tells them apart. So the reason is
examined *first* ("confirm your age", "your country", "private video",
"not a bot") and the status only decides what is left. The first review
caught the original code doing it the other way round, which made every
age-gated video look like a bot check. The refused-client case ("The page
needs to be reloaded", what `web` gets without a PO Token) is checked
last and only for `UNPLAYABLE`, so it can never swallow a real failure.

**The ladder.** `Impl::resolve()` walks the clients in order. After each
failure, `triesNextClient()` decides: a failure *about the client* passes
the video on; a failure about the video, the network or the call ends the
resolve with that code and YouTube's own words. When the ladder runs out,
the reported code is the most *actionable* one seen (`weight()`: `BotCheck`
over `Http` over `Parse` over `NoFormats`), and the message names every
client and what it said — so `web`'s guaranteed `NoFormats` can never hide
`visionos`'s `BotCheck`, which is the one code the bot must react to.

**The one retry.** A `BotCheck` from a client triggers, once per resolve, a
fresh watch-page fetch and a second player request. That handles a cached
token that quietly wore out. If the page brings nothing, the `BotCheck`
stands.

*Questions: for each code, what should the bot say to the user, and should
it try yt-dlp? Where would a `Retry-After` from a 429 belong?*

## 5. The HTTP client

`include/ytres/http.h` declares an interface with one method:
`HttpClient::send(const HttpRequest &) -> Result<HttpResponse>`. Everything
above it is written against that interface, which is what makes the tests
offline (section 7) and would let an Android build swap in something else.
`src/curl_http_client.cpp` is the default implementation, over libcurl.

Read `CurlHttpClient::send()` top to bottom; it is one function, and every
line answers a specific question:

- **Lifetime.** The multi handle, the easy handle and the header list are
  `unique_ptr`s with custom deleters, declared in the order libcurl wants
  them destroyed in reverse. `Attachment` is a tiny RAII object whose
  destructor calls `curl_multi_remove_handle`; it is declared *after* both
  handles, so it runs *before* either cleanup, on every return path. This
  is how "no leak, whatever happens" is made structural instead of
  remembered.
- **Why the multi interface at all.** The POC used `curl_easy_perform`,
  which only asks the progress callback about once a second while it waits
  for DNS, a connection or the first byte. The bot cancels resolves
  constantly (a skip kills the pending resolve), and the plan wants that to
  land within its 50 ms poll. So the transfer is driven by a loop:
  `curl_multi_perform`, check the cancel, `curl_multi_poll` for at most
  20 ms, repeat. Measured: a cancel lands 20–27 ms after it is requested,
  in every state including mid-DNS. `CURLOPT_QUICK_EXIT` is what lets a
  cancel during a threaded DNS lookup return without waiting for
  `getaddrinfo`.
- **Callbacks are C frames.** `onBody` and `onProgress` are called by
  libcurl's C code, through which no C++ exception may unwind. So `onBody`
  catches a failed `append` and aborts the transfer instead, and
  `CancelCheck` wraps the caller's `std::function` so that a check that
  throws stops the transfer and is reported as `Internal` — a throwing
  cancel check is the caller's bug, not a cancel.
- **Bounds.** The body is capped at 8 MB (`MAX_BODY_BYTES`), on the
  *decompressed* bytes, so a gzip bomb stays bounded. `CURLOPT_TIMEOUT_MS`
  and `CURLOPT_CONNECTTIMEOUT_MS` come from the request; the Resolver sets
  that to the smaller of `Options::requestTimeout` and what remains of the
  resolve's deadline (`Impl::send()`), so two or three requests can never
  overrun one deadline.
- **What is deliberately absent.** No `CURLOPT_SSL_VERIFYPEER 0`, ever. No
  `CURLOPT_ERRORBUFFER`: on Windows, Schannel fills it with text in the
  local code page, and `Status::message` promises UTF-8 — the same
  mojibake class the bot's `ensureUtf8` workaround exists for. Messages
  come from `curl_easy_strerror`'s fixed English table instead, and an
  error page's first 200 bytes go to the Debug log, not the message.
- **Accept-Encoding.** Set to `""`, which means "everything this libcurl can
  decode": the 1.3 MB watch page arrives as 153 KB.

*Questions: walk one return path (say, the response-too-large one) and name
what gets freed and in what order. What would break if `Attachment` were
declared before the handles?*

## 6. Threads

The bot's `ResolverWorker` resolves the next song while the current one
plays, and a skip cancels from yet another thread. So the rules, stated at
the top of `resolver.cpp` and `visitor_cache.h`:

- A `Resolver` is safe to call from several threads at once. `options` and
  the `HttpClient` are never written after construction. Each resolve
  carries its own `Call` (id, request, deadline) down the stack; nothing
  per-resolve lives in the `Impl`.
- The one shared thing is the visitor token. `VisitorCache` guards it with a
  mutex that is held only inside `get()` and `put()` — never across an HTTP
  request. The consequence is accepted on purpose: two cold resolves that
  race both fetch the page, and the later value wins. Holding the lock
  across the fetch would make the other threads wait while still needing to
  honour their own cancel and deadline; not worth it for the bot's two
  threads. The concurrency test (`tests/concurrency_test.cpp`) drives eight
  threads through one cold `Resolver` and checks the counts.
- Cancellation is cooperative all the way down: `Request::cancelled` is a
  `std::function<bool()>` the caller supplies, polled between multi polls
  and from libcurl's progress callback. In process, there is no killing a
  thread; this is the replacement for the job object the bot uses to kill
  `yt-dlp.exe` today.
- libcurl's own global state is initialised exactly once through
  `std::call_once`.

*Questions: what could go wrong if the mutex were held across the page
fetch? Why must the cancel check itself never block?*

## 7. Tests

Three layers, each with a different relationship to the network:

**`ytres_tests` — offline, always.** `tests/fake_http_client.h` implements
`HttpClient` in memory: it answers the watch page and the player request
separately, can fail either (a 429, a network error, a timeout), records
every request it was given, and can report the timeout each request
carried. The recorded responses in `tests/fixtures/` were captured live with
`ytres_cli --dump`, which scrubs the requesting IP and the visitor tokens
before writing (203.0.113.7 and `"FIXTURE"` are the placeholders). With
those two pieces, the whole path from URL to `bestAudio()` runs
deterministically, and the tests pin what the Resolver actually *sends* —
URL, headers, body — not only what it parses. 87 cases; `ctest` runs only
this target.

**`ytres_live_tests` — on demand.** The same doctest framework, but against
real YouTube: it resolves `tests/corpus.txt` (an ordinary video, a Polish
title, a 10-hour video, a 24/7 live stream, a "- Topic" upload, an
age-restricted one, a deleted id) and checks each expected code, and it
measures cancellation latency against a non-routable address. Not in
`ctest`, because YouTube's answers change and the network is not a test
fixture.

**`tools/differential.py` — the early-warning system.** Runs `ytres_cli` and
`yt-dlp.exe` over the corpus and compares title, page URL and chosen itag,
with timings. yt-dlp has a team patching YouTube breakage within hours; this
script is how a solo maintainer notices before Discord users do. Run it
whenever something feels off, and after any YouTube change you hear about.

*Questions: what would a YouTube change to the JSON do to each layer? Which
layer catches a regression in header construction, and which catches
YouTube starting to require a header we do not send?*

## 8. Build

- `CMakeLists.txt` defines three targets: `ytres` (a static library),
  `ytres_cli` and `ytres_tests`; `tests/CMakeLists.txt` adds
  `ytres_live_tests`. C++17, `/W4 /permissive- /we4715 /utf-8`, zero
  warnings kept as a hard rule. `/utf-8` makes source and string literals
  UTF-8 regardless of the machine's code page.
- `vcpkg.json` declares the dependencies (`curl`, `nlohmann-json`,
  `doctest`) with a pinned baseline, and the vcpkg toolchain file builds
  them into `build/vcpkg_installed` on the first configure. That is why the
  first configure takes minutes: curl is compiled from source, with
  Schannel for TLS (the Windows certificate store; no `cacert.pem`).
- The `x64-windows` triplet is vcpkg's *dynamic* one, so libcurl is a DLL
  copied next to each executable. The plan chose a static library precisely
  to avoid DLLs beside the bot's exe; switching to `x64-windows-static-md`
  fixes that and is an M6 decision, because it depends on how the bot
  consumes the library.

*Questions: what does `add_subdirectory(yt-dlp-resolver)` from the bot's
CMake need, and what does an installed package need instead?*

## 9. The process, for the curious

Each milestone ran the same loop, and each step exists because the previous
one failed at some point:

1. **Plan** (`docs/*-plan.md`): written before code, with the facts read
   from yt-dlp's source rather than remembered — the client table, the
   request shape, the accepted statuses. The most valuable half hour of the
   project was the one that found `visionos`; it deleted the JavaScript
   engine from the critical path.
2. **Brief**: a self-contained instruction to an implementer that has never
   seen the project — build command, conventions, branch, what to report.
   The pre-planned fallback ("if you get bot-checked, do this") is why the
   first live run did not stall.
3. **Implement**: logical commits, each built and tested before it lands.
4. **Review**: a second agent reads the diff against the plan and, crucially,
   *probes* the built library with throwaway programs. That is how the
   age-gate-as-bot-check bug, the wrong-language dub, the deadline overflow
   and the too-broad refused-client check were found — none of them by
   reading alone.
5. **Fix round**: findings go back with exact lines and the fix, as separate
   commits, then a second review pass.

Three things worth remembering from it: verify facts at their source when
the ground moves weekly; keep tests offline so that YouTube cannot make them
flaky; and treat "the message said what the code did not do" (the BotCheck
retry wording) as a defect, because the header is the contract the bot's
author reads.

## Where to go next

- **M3, search**: `POST /youtubei/v1/search` with a videos-only filter, so a
  band name no longer returns the artist's channel first. Reuses the visitor
  cache and the one-retry pattern.
- **M4, playlists**: `POST /youtubei/v1/browse` with `VL<playlistId>` and
  continuation tokens, bounded by the caller's `max`.
- **M6, the bot**: an `IMediaResolver` seam in the bot with two
  implementations, native first and yt-dlp as the permanent fallback. The
  bot's `ResolvedMedia`/`PlaylistListing` types are already the right shape.
