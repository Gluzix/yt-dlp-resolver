# YouTube resolver library plan

Implementation plan for whoever codes it. The YouTube facts below were read
from yt-dlp's current sources on 2026-09-17 (`yt_dlp/extractor/youtube/_base.py`
and `_video.py` at master), not guessed. Anything still unverified is marked
as such; verify it before you rely on it, because this is the one part of the
project that rots on a timescale of weeks.

Nothing here is built yet. The directory is empty and is not a git repository.

## Goal

A C++ library that answers the three questions the Discord bot currently
shells out to `yt-dlp.exe` for:

| Bot call | Today | Library |
|---|---|---|
| `resolveMedia(url)` | `yt-dlp --print title,webpage_url,urls` | `Resolver::resolve()` |
| `firstVideoUrl(query)` | `yt-dlp ytsearch5: --flat-playlist` | `Resolver::search()` |
| `listPlaylist(url, n)` | `yt-dlp --flat-playlist` | `Resolver::playlist()` |

It is a **library**, not an executable. A command-line target exists only as
a development harness. The consumer links it; the process spawn and the one
to two seconds of PyInstaller startup per track disappear, which is the whole
point.

The core must stay portable: C++17, no Win32, no Qt, no Android. The bot is
the first consumer, not the only intended one.

Out of scope, deliberately: sites other than YouTube, downloading (the
library returns URLs, FFmpeg does the rest), cookies and sign-in, subtitles,
SponsorBlock, and any of the ~1800 extractors that make yt-dlp large.

## What resolution actually involves

A resolve is a POST to InnerTube:

```
POST https://www.youtube.com/youtubei/v1/player
Content-Type: application/json
X-YouTube-Client-Name: <numeric id>
X-YouTube-Client-Version: <version>
Origin: https://www.youtube.com
User-Agent: <the client's own UA>

{"videoId": "...", "context": {"client": {...}},
 "contentCheckOk": true, "racyCheckOk": true}
```

No API key in the URL any more; the client identity lives in the headers and
in `context.client`. The response carries `playabilityStatus.status`,
`videoDetails` (title, author, `lengthSeconds`) and `streamingData` with
`formats` (muxed) and `adaptiveFormats`, plus `expiresInSeconds`.

Each format has either a ready `url` or a `signatureCipher` blob that must be
descrambled by a function lifted out of YouTube's `base.js`. Separately, the
URL's `n` query parameter must be run through another, heavily obfuscated
`base.js` function or the download is throttled to roughly playback speed.

**Which of that you actually have to do depends entirely on the client you
identify as**, and that is the most important design fact in this document.

## The client ladder

yt-dlp keeps a table of InnerTube clients recording, per client, whether it
needs the JS player and whether YouTube demands a PO Token (the BotGuard
attestation blob). As of 2026-09-17 its defaults are:

```python
_DEFAULT_CLIENTS        = ('visionos', 'web')
_DEFAULT_JSLESS_CLIENTS = ('visionos',)
```

and the `visionos` client is declared with `REQUIRE_JS_PLAYER: False` and
**no PO Token policy at all**. `web`, by contrast, carries
`GvsPoTokenPolicy(required=True)` for both HTTPS and DASH.

Two consequences, and they reshape the project:

1. **The working path needs no JavaScript.** No `base.js`, no QuickJS, no
   signature descrambling, no `n` transform. A `visionos` player request
   returns usable URLs directly. The hardest part of this project is, right
   now, not on the critical path.
2. **The JS path would not save us anyway.** The clients that need the JS
   player are largely the same ones that need a PO Token, and minting PO
   Tokens means running YouTube's BotGuard VM. That is out of scope and
   should stay out of scope. Adding QuickJS buys nothing until that changes.

Worth noting for its own sake: yt-dlp no longer interprets that JavaScript
itself. It shells out to an external runtime — "Only deno is enabled by
default" — having retired the hand-written Python interpreter. The approach
this project was never going to take is one yt-dlp has now abandoned too.

So: implement a **client ladder** — an ordered list of client definitions,
tried in turn until one returns playable formats. Start with `visionos`
alone. Keep the client table as *data* in one file, so that burning a client
is a one-line edit and not a refactor.

Treat that as a when, not an if. yt-dlp's own source comments record
`android_vr` dying on 2026-08-17 ("ALL formats ... are 403'd") and HLS
formats narrowing in 2026.07. `visionos` will get the same treatment
eventually. The library must fail cleanly and let the caller fall back, which
for the bot means keeping yt-dlp as a second resolver behind the same
interface, permanently.

Also on the horizon and worth watching rather than solving: YouTube's SABR
streaming, which replaces plain format URLs with a protobuf protocol. yt-dlp
currently only warns about it ("YouTube is forcing SABR streaming for this
client"). If SABR becomes universal, this whole approach needs rethinking.

## Public API

One header, `include/ytres/ytres.h`. No exceptions cross the boundary — a
JNI or C consumer should not have to care — so every call returns a result
carrying a status.

```cpp
namespace ytres {

enum class Error {
    Ok, Cancelled, Timeout, Network, Http, Parse,
    Unavailable,     // deleted, private, removed
    AgeRestricted,
    GeoBlocked,
    LoginRequired,
    NoFormats,       // playable, but nothing usable came back
    PlayerScript,    // base.js fetch or extraction failed (JS tier only)
    BadInput,
    BotCheck,        // "confirm you're not a bot": about the caller, not the video
    Internal,        // a bug or resource failure inside the library
};

struct Status {
    Error code{Error::Ok};
    std::string message;              // UTF-8, safe to show a user
    explicit operator bool() const { return code == Error::Ok; }
};

template <class T>
struct Result {
    Status status;
    T value{};
    explicit operator bool() const { return bool(status); }
};

enum class Track { Audio, Video, Muxed };

struct Format {
    int itag{0};
    Track kind{Track::Audio};
    std::string url;                  // playable as-is
    std::string mimeType;             // audio/webm; codecs="opus"
    std::string codec;                // opus, mp4a.40.2, vp9, avc1
    int bitrate{0};                   // bits per second
    std::int64_t contentLength{0};    // 0 when unknown
    int audioSampleRate{0};
    int audioChannels{0};
    int width{0}, height{0}, fps{0};  // video only
};

struct VideoInfo {
    std::string videoId;
    std::string title;                // UTF-8
    std::string author;
    std::string webpageUrl;
    std::int64_t durationSeconds{0};
    bool isLive{false};
    std::vector<Format> formats;
    std::int64_t expiresAtUnix{0};    // every url above dies at this time

    std::optional<Format> bestAudio() const;
};

// As built in M3.
struct SearchResult {
    std::string videoId;
    std::string title;                // UTF-8
    std::string author;               // the channel's name
    std::int64_t durationSeconds{0};  // 0 when YouTube gives none: live, upcoming
    bool isLive{false};               // streaming now; resolve() answers it with NoFormats
    bool isUpcoming{false};           // a scheduled premiere or stream, not playable yet
};

// https://www.youtube.com/watch?v=<videoId>, built in M3.
std::string watchUrl(std::string_view videoId);

struct PlaylistEntry { std::string videoId, title;         std::int64_t durationSeconds{0}; };

struct Playlist {
    std::string title;
    std::size_t totalCount{0};        // 0 = unknown
    std::vector<PlaylistEntry> entries;
};

// Per-call cancellation and deadline. cancelled may be empty.
struct Request {
    std::function<bool()> cancelled;
    std::chrono::milliseconds deadline{std::chrono::seconds{30}};
};

class Resolver {
public:
    struct Options {
        std::vector<ClientId> clients{ClientId::VisionOS};  // tried in order
        std::string language{"en"}, country{"US"};
        std::shared_ptr<HttpClient> http;                   // null -> built-in libcurl
        std::function<void(LogLevel, std::string_view)> log;
        std::chrono::milliseconds requestTimeout{std::chrono::seconds{10}};
    };

    explicit Resolver(Options options = {});
    ~Resolver();

    Result<VideoInfo>                 resolve(std::string_view urlOrId, const Request& = {});
    // As built in M3: up to max videos, best match first, as the web client
    // with the videos-only filter, following continuations up to ten pages.
    // A later page's failure comes back with the videos read before it.
    Result<std::vector<SearchResult>> search(std::string_view query, std::size_t max, const Request& = {});
    Result<Playlist>                  playlist(std::string_view urlOrId, std::size_t max, const Request& = {});
};

}
```

The library also owns canonical URL parsing (`watch?v=`, `youtu.be/`,
`/shorts/`, `/live/`, playlist ids, bare ids). The bot's
`Helpers/YoutubeInput.h` exists to keep user input off a command line; once
there is no command line, most of it can go.

Returning the whole format list rather than only the best audio costs nothing
and is what a second consumer — a player — would need. The bot calls
`bestAudio()` and ignores the rest.

`expiresAtUnix` matters: these URLs die in a few hours and are bound to the
requesting IP. The bot mostly gets away with ignoring that because it
resolves immediately before playing. A consumer that queues would not.

## Internals

```
Resolver
  |- UrlParse        pure string work, no I/O, trivially testable
  |- InnerTube       request bodies, headers, client table, response walk
  |- FormatPick      itag/codec/bitrate selection
  |- HttpClient      interface; CurlHttpClient is the default implementation
  `- (later) JsTier  base.js cache + QuickJS, only if a client ever needs it
```

`HttpClient` is an interface, not a convenience:

```cpp
struct HttpRequest {
    std::string method, url, body;
    std::vector<std::pair<std::string, std::string>> headers;
    std::chrono::milliseconds timeout;
    std::function<bool()> cancelled;
};

struct HttpResponse { long status{0}; std::string body; };

class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual Result<HttpResponse> send(const HttpRequest&) = 0;
};
```

It earns its place three times over: it is the seam that makes offline
deterministic tests possible (below), it keeps libcurl out of the public
header, and it is what a future Android consumer would swap.

**Cancellation is cooperative and must reach the bottom.** In-process there
is no killing a thread, and the bot cancels resolves constantly — a skip or a
stop kills the running yt-dlp today (`SongPlayer.cpp:243`,
`ResolverWorker.cpp:78`). So `Request::cancelled` is polled from libcurl's
`CURLOPT_XFERINFOFUNCTION` (returning non-zero aborts the transfer) and, if
the JS tier is ever built, from QuickJS's `JS_SetInterruptHandler`. A resolve
must abandon within roughly the bot's 50 ms poll interval.

**Thread safety.** The bot resolves the next song on a worker thread while
one plays, so two resolves can overlap. A `Resolver` must be safe to call
concurrently and any shared cache needs a mutex. Note for the JS tier: a
QuickJS `JSRuntime` may only be used by one thread at a time.

**Errors are specific.** The bot shows one generic "Couldn't get the audio"
because a process exit code is all it gets. With a real error code it can say
the video is private, or age-restricted, or that the network is down. That is
a visible improvement for free.

**UTF-8 everywhere.** `WindowsProcessRunner.cpp:73` carries an `ensureUtf8`
workaround because yt-dlp writes the ANSI code page to the pipe and turns
Polish titles into mojibake. Reading JSON off the wire, that entire class of
bug does not exist. Do not add a code-page conversion anywhere in the core.

## Search and playlists

Both are InnerTube POSTs like the player call — different endpoint, different
body.

- **Search**: `/youtubei/v1/search` with `{"query": ..., "params": ...}`.
  The `params` field is a base64 protobuf filter, and yt-dlp's videos-only
  value is `EgIQAfABAQ==`. This plan first assumed the filter keeps channels
  out; the live probe of 2026-10-01 showed it does not — an artist's name
  still puts a `channelRenderer` first — so the library skips whatever is not
  a `videoRenderer`, which is what makes `firstVideoUrl`'s hand-picking
  unnecessary.
- **Playlists**: `/youtubei/v1/browse` with `browseId = "VL" + playlistId`,
  then follow the continuation among the entries for pages beyond the first
  hundred. Stop as soon as `max` entries are collected — the bot asks for a
  bounded prefix, and a 6000-video playlist must not become sixty requests.
  Drop unplayable entries, matching what the bot already filters:
  `[Private video]`, `[Deleted video]`, and entries with no title. YouTube
  moved playlists to a new layout (`lockupViewModel` entries and
  `continuationItemViewModel`), so the reader takes that and the older
  `playlistVideoRenderer` one.

The response shapes were read off live responses on 2026-10-01 and are
written down in `docs/innertube-notes.md`, "Search and playlists";
`docs/m3-plan.md` and `docs/m4-plan.md` are the plans built on them.

## Testing

The differential harness is the highest-value thing in this project, more so
than any unit test. YouTube breaks this library; you mostly do not.

1. **Fixture tests (offline, deterministic, in CI).** A fake `HttpClient`
   replays recorded responses. The CLI harness gets a `--record` mode that
   saves live responses into `tests/fixtures/`. This covers URL parsing,
   response walking, format selection and error mapping.
2. **Live smoke tests (manual target).** A golden corpus of ids: an ordinary
   video, a Polish title, an age-restricted one, one over an hour, a live
   stream, a deleted one, an auto-generated "- Topic" music track, a playlist
   containing private entries, and a playlist over 100 entries.
3. **Differential harness.** Run yt-dlp and the library on the same input and
   compare title, canonical URL and chosen itag. This is the early-warning
   system for YouTube changing something.
4. **Throughput check**, only if the JS tier is ever built: fetch the first
   few MB and assert the rate. A missed `n` transform looks fine in the URL
   and shows up only as a stuttering bot.

## Milestones

| # | Milestone | Done when |
|---|---|---|
| M0 | Skeleton and seams | CMake, public header, `CurlHttpClient`, JSON, CLI harness with `--record`, test target. No YouTube yet. |
| M1 | Resolve via `visionos` | Player request, playability mapping, format model, `bestAudio()`, expiry. The usable MVP. |
| M2 | Hardening | Client ladder, full error taxonomy, cancellation through curl, timeouts, thread safety, differential harness. |
| M3 | Search | Videos-only filter; one request, no hand-picking. Written 2026-10-01 without a build; see `docs/m3-plan.md`, "Before this is done". |
| M4 | Playlists | Browse plus continuations, bounded by `max`. |
| M5 | JS tier *(deferred)* | Only if a client that needs `base.js` ever becomes necessary. Currently buys nothing. |
| M6 | Bot integration | Separate plan. `IMediaResolver` seam, native first, yt-dlp fallback. |

M1 is the go/no-go. If a `visionos` player request does not yield a playable
audio URL from a plain C++ HTTP client, everything after it is moot, and that
is worth finding out in the first sitting rather than the fifth.

## Build and conventions

C++17, CMake >= 3.14, MSVC, matching the bot. Dependencies through the
existing vcpkg at `C:/vcpkg`: `curl` and `nlohmann-json`. QuickJS only at M5,
vendored as its amalgamation if vcpkg does not carry it.

Targets: `ytres` (static library — no DLL to keep in sync next to the exe,
a cost the bot's `dpp.dll` copy step already demonstrates), `ytres_cli`
(development harness), `ytres_tests`.

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
```

Follow the bot's house style: comments explain *why*, headers carry the
contract, `/W4` and `/we4715`. Plan docs live in `docs/`.

Prerequisite before M0: `git init`, and a branch per milestone.

## Risks

1. **`visionos` gets burned.** Likely, eventually. Mitigated by the client
   ladder, by keeping the client table as editable data, and by the bot
   retaining yt-dlp as a fallback resolver forever.
2. **PO Tokens spread to every client.** Then this approach ends, because
   minting them means running BotGuard. Accept that; do not build toward it.
3. **SABR-only streaming.** Watch yt-dlp's issue tracker.
4. **You become the maintainer.** yt-dlp has a team that patches YouTube
   breakage within hours. A solo port does not. The permanent fallback is not
   pessimism, it is the design.
5. **This document ages.** Everything under "The client ladder" was true on
   2026-09-17 and may not be true when M1 starts. Re-read yt-dlp's `_base.py`
   first.

## Open questions

- Library and namespace name. `ytres` is a placeholder.
- Does anything need age-restricted or region-locked video, or is a clean
  "can't play this" enough? Cookies and sign-in are out of scope, and that
  decision is a large part of what keeps the project small.
- Is the CLI harness worth keeping once the bot links the library, or does it
  become dead weight?
