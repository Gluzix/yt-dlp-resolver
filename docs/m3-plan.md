# M3 plan: search

Implementation plan for whoever codes it. Everything needed is in this file
and in `docs/innertube-notes.md`, section "Search and playlists", which holds
the protocol facts: read from yt-dlp's `_search.py`, `_tab.py` and `_base.py`
at master and probed against live YouTube on 2026-10-01. Branch: `m3/search`
off `master`. Kamil merges through pull requests; nothing is pushed by anyone
but him.

**Written without a build.** Kamil asked for no compilation and no test runs
while this milestone was coded, so nothing in it has been compiled or
executed. The first thing to do when that is lifted is at the bottom, under
"Before this is done".

## What the bot needs

Today `firstVideoUrl(query)` runs `yt-dlp ytsearch5:` with `--flat-playlist`
and takes the first entry yt-dlp calls a plain video, because a band name
ranks the artist's channel first. The library answers the same question with
one HTTP request:

```cpp
ytres::Result<std::vector<ytres::SearchResult>> found = resolver.search("Dawid Podsiadło", 5);
// the first result that is neither live nor upcoming is the one to play
```

## Public API (`include/ytres/ytres.h`)

```cpp
struct SearchResult
{
    std::string videoId;
    std::string title;                // UTF-8
    std::string author;               // the channel's name
    std::int64_t durationSeconds{0};  // 0 when YouTube gives none: live, upcoming
    bool isLive{false};               // streaming now; resolve() answers it with NoFormats
    bool isUpcoming{false};           // a scheduled premiere or stream, not playable yet
};

// https://www.youtube.com/watch?v=<videoId>: the page of a search result or
// a playlist entry, and a valid resolve() target.
std::string watchUrl(std::string_view videoId);

class Resolver
{
    ...
    // Up to max videos for query, best match first; an empty list when
    // YouTube finds none. One request for the first twenty or so, one more
    // per further page. Channels, playlists and shelves in the results are
    // skipped. max of 0, or a query that is empty or all spaces, is BadInput.
    // Asked as the web client whatever Options::clients says: that list is
    // the player's ladder. Cached visitor data is sent when there is some,
    // but none is fetched for a search.
    // When a later page fails, the result carries that failure and the
    // videos read so far.
    Result<std::vector<SearchResult>> search(std::string_view query, std::size_t max, const Request &request = {});
};
```

`watchUrl()` is `canonicalWatchUrl()` from `src/url_parse.h` made public; M4
needs it too. A failure with a partial value is deliberate: `Result` already
carries both, and a caller that asked for sixty videos and was cancelled on
page three can still use the first forty.

## Groundwork this milestone lays for M4

1. **`src/json_read.h`** — the type-checked JSON readers move out of
   `innertube.cpp`'s anonymous namespace into an internal header, as `inline`
   functions in namespace `ytres::jsonread`: `child`, `childArray`,
   `readString`, `readBool`, `isTruthy`, `toInt64`, `readInt`, moved verbatim.
   One new reader joins them:

   ```cpp
   // InnerTube writes a label three ways: {"simpleText": ...}, {"runs":
   // [{"text": ...}, ...]} to be joined, or a view model's {"content": ...}.
   // Empty when object has no such key or the value is none of the three.
   std::string textOf(const json &object, const char *key);
   ```

   `innertube.cpp` includes the header and pulls the names in with
   using-declarations; its behaviour must not change, and `subreasonOf()`
   stays as it is. This is a cut and paste, not a redesign.

2. **`innertube::apiRequest()`** — one builder for every InnerTube POST that
   is not the player's:

   ```cpp
   // POST /youtubei/v1/<endpoint> as yt-dlp's _call_api sends it: the
   // client's context first, then fields in the order given. The same
   // headers as playerRequest().
   HttpRequest apiRequest(const ClientDef &client, const char *endpoint, const std::string &language,
                          const std::string &visitorData, const nlohmann::ordered_json &fields);
   ```

   The client context and the header list are today built inline in
   `playerRequest()`. Lift each into a file-local helper
   (`clientContext(client, language, visitorData)` and
   `apiHeaders(client, visitorData)`) used by both builders, so they cannot
   drift apart. `playerRequest()`'s output must stay byte for byte what it
   is: `tests/player_request_test.cpp` pins it.

3. **`src/continuation.h/.cpp`** — what "the next page" is, in every shape
   YouTube uses:

   ```cpp
   namespace ytres::innertube {

   struct Continuation
   {
       std::string token;                // empty: there is no next page
       std::string clickTrackingParams;  // sent back as clickTracking when present
   };

   // The continuation an item of a contents array carries, if it is one:
   //   continuationItemRenderer.continuationEndpoint
   //   continuationItemRenderer.button.buttonRenderer.command
   //   continuationItemViewModel.continuationCommand.innertubeCommand
   // each of which is a command holding continuationCommand.token, or a
   // commandExecutorCommand whose commands[] hold one. Empty for anything else.
   Continuation continuationOf(const nlohmann::json &item);

   // The fields of a continuation request, as yt-dlp's
   // _build_api_continuation_query makes them: "continuation", and
   // "clickTracking": {"clickTrackingParams": ...} when there are some.
   void addContinuation(nlohmann::ordered_json &fields, const Continuation &continuation);

   }
   ```

4. **`durationSeconds()`** in `src/json_read.h` or next to it:
   `"4:36"` → 276, `"2:31:48"` → 9108, anything that is not colon-separated
   digits → 0.

5. **The fake HTTP client** (`tests/fake_http_client.h`) learns the two other
   endpoints. Today every request that is not the player's gets the watch
   page. Add, keyed by the endpoint name in the url (`"search"`, `"browse"`):

   ```cpp
   std::map<std::string, std::deque<std::string>> apiBodies;   // answered in turn
   std::map<std::string, ytres::Status> apiFailure;            // not Ok: returned instead
   std::map<std::string, long> apiStatus;                      // default 200
   ```

   A request to `/youtubei/v1/search` or `/youtubei/v1/browse` takes the next
   body of its queue; an empty queue is a test bug, answered with
   `Error::Internal` and a message that says so. Player and page behaviour
   stays exactly as it is.

## The request

From the notes. First page:

```
POST https://www.youtube.com/youtubei/v1/search?prettyPrint=false
(the web client's headers: X-YouTube-Client-Name 1, version 2.20260708.00.00,
 Origin, the Chrome User-Agent, X-Goog-Visitor-Id when known)

{"context": {"client": {"clientName": "WEB", "clientVersion": "2.20260708.00.00",
                        "hl": "en", "timeZone": "UTC", "utcOffsetMinutes": 0}},
 "query": "<the query>",
 "params": "EgIQAfABAQ=="}
```

`EgIQAfABAQ==` is yt-dlp's `_SEARCH_PARAMS`, "videos only". A further page
repeats `query` and `params` and adds the continuation fields, exactly as
yt-dlp's `_search_results` does (it updates the same dict):

```
{"context": ..., "query": ..., "params": "EgIQAfABAQ==",
 "continuation": "<token>", "clickTracking": {"clickTrackingParams": "<ctp>"}}
```

The client is always the `web` row of the table (`findClient(ClientId::Web)`).
Visitor data: whatever `visitorCache.get(now).value` holds, fresh or stale;
when it holds nothing the request goes out bare, which YouTube answered fine
on 2026-10-01. No watch page is fetched for a search.

## The response (`src/search.h/.cpp`, namespace `ytres::innertube`)

```cpp
struct SearchPage
{
    std::vector<SearchResult> results;
    Continuation next;
};

// Reads one answer of /youtubei/v1/search, a first page or a continuation.
// Pure. Not JSON, or JSON with neither container below, is Parse.
Result<SearchPage> parseSearchResponse(const std::string &body);
```

The items live in one of two places:

- first page: `contents.twoColumnSearchResultsRenderer.primaryContents.sectionListRenderer.contents[]`
- continuation: `onResponseReceivedCommands[].appendContinuationItemsAction.continuationItems[]`
  (take the first command that has one)

Each element of that array is either an `itemSectionRenderer`, whose
`contents[]` hold the results, or the page's continuation
(`continuationItemRenderer`, read with `continuationOf()`). So for search the
continuation is a **sibling of the item sections**, not inside them.

Inside an item section, only `videoRenderer` is a result. Everything else is
skipped: on 2026-10-01 the videos-only filter still put a `channelRenderer`
first for an artist's name, and an empty search holds one
`backgroundPromoRenderer`. From a `videoRenderer`:

| Field | From |
|---|---|
| `videoId` | `videoId`; skip the result if it is not a valid id (`isVideoId`) |
| `title` | `textOf(renderer, "title")` |
| `author` | `textOf(renderer, "ownerText")`, else `longBylineText`, else `shortBylineText` |
| `durationSeconds` | `durationSeconds(textOf(renderer, "lengthText"))` |
| `isLive` | any `badges[].metadataBadgeRenderer.style` equal to `BADGE_STYLE_TYPE_LIVE_NOW`, or any `thumbnailOverlays[].thumbnailOverlayTimeStatusRenderer.style` equal to `LIVE` |
| `isUpcoming` | `upcomingEventData` is present |

## The loop (`Resolver::Impl::search` in `src/resolver.cpp`)

1. The same opening as `resolve()`: the moved-from guard and the catch-all
   in the public method, then `requestTimeout` must be positive, then the
   deadline clamped to 0–24 h. Lift the clamp into a small helper both use
   rather than writing it twice.
2. `max == 0`, or a query that is empty after trimming ASCII spaces, is
   `BadInput`.
3. Send the first page through `Impl::send()` (it applies the cancel check
   and what is left of the deadline). `Impl::Call` gets an empty `videoId`
   here; say so in its comment. A status outside 200–299 is `Http`, as in
   `askPlayer()`.
4. Append results until `max`. While fewer than `max`, the page gave a
   continuation, the page added at least one result, and fewer than
   `MAX_SEARCH_PAGES` (10) were read: send the continuation and repeat.
5. A failure on the first page is that failure with an empty list. A failure
   on a later page is that failure with what was read so far.

Log one Debug line per page ("search page N: K videos").

## CLI (`cli/main.cpp`)

`ytres_cli --search "<query>" [--max N] [--dump <file>]`, `--max` defaulting
to 5. One line per result on stdout: the watch url, a tab, the duration in
seconds (or `live` / `upcoming`), a tab, the author, a tab, the title.
Failures as today. `--dump` writes the first search response, scrubbed; teach
`RecordingHttpClient` to keep the first `/youtubei/v1/search` body. The
scrubber already covers what search responses carry: preview-thumbnail urls
with `&ip=<address>` (57 of them in one page on 2026-10-01) and both visitor
fields.

## Tests (`tests/search_test.cpp`, added to `tests/CMakeLists.txt`)

The fixtures are real responses recorded on 2026-10-01 with the web client,
scrubbed (address → 203.0.113.7, visitor data → `FIXTURE`) and trimmed to a
few results each. They are already in `tests/fixtures/`:

| Fixture | What it holds | Expect |
|---|---|---|
| `search_videos.json` | "Dawid Podsiadło", first page: a `channelRenderer` first, then 4 videos, then the continuation | 4 results; the first is `MxWXAIWsppY`, title `Dawid Podsiadło "na błysk"`, author `Dawid Podsiadło`, 276 s, not live; the second `oCZugu1ea18`, 291 s; the third `2DiP0mMeaT8`, title `Dawid Podsiadło - mori (Official Video)`, 194 s; a non-empty continuation token |
| `search_continuation.json` | the second page: 3 videos and the next continuation | `g4UDeQTjMYk` (247 s), `5rNXe7Z1qN0` (244 s), `c9B4Z_HRAcI` (224 s); a non-empty token |
| `search_empty.json` | a query with no results | Ok, no results, no continuation |
| `search_live.json` | "lofi girl live": 3 live streams | `rFZHOHl-L8A`, `JD-kMIpDfnY`, `E2vONfzoyRI`; each `isLive`, 0 s, author `Lofi Girl` |

Write the non-ASCII expectations as `\u` escapes inside JSON literals or as
`u8` byte strings the way `tests/player_response_test.cpp` already does; the
sources are compiled with `/utf-8`.

Cover at least:

- `searchRequest`: url, headers and body pinned field for field, with and
  without visitor data; the continuation body repeats `query` and `params`
  and adds `continuation` and `clickTracking`.
- `parseSearchResponse` on each fixture, with the table's values.
- `continuationOf` on each of its three shapes and on the
  `commandExecutorCommand` form, plus an item that is not a continuation.
- `durationSeconds`: `"4:36"`, `"2:31:48"`, `"0:07"`, `""`, `"LIVE"`, `"1:2x"`.
- Through the Resolver and the fake: `max` smaller than a page sends one
  request and returns `max` results; `max` larger than a page sends the
  continuation and stops when satisfied; a first page of `search_empty.json`
  is Ok and empty; a 429 is `Http`; a body that is not JSON is `Parse`; a
  second page that fails returns that failure **with** the first page's
  results; `max` 0 and a blank query are `BadInput` with nothing sent; cached
  visitor data is sent as `X-Goog-Visitor-Id` and none is fetched (no request
  to a watch page); a cancel before the first request is `Cancelled`.
- `watchUrl("dQw4w9WgXcQ")`.

`player_request_test.cpp` and the rest must keep passing unchanged: the
groundwork is a refactor.

## Documentation

- README: the search API, the CLI's `--search`, and the new fixture names.
- `docs/resolver-plan.md`: add `SearchResult`, `watchUrl()` and `search()` to
  the API sketch as built.
- `docs/walkthrough.md`: a short section on search after section 3.

## Out of scope

Playlists (M4). Search filters other than videos-only, sorting, music search.
Fetching visitor data for a search. De-duplicating results across pages
(yt-dlp does not either).

## Before this is done

Nothing here was compiled or run. When Kamil lifts that:

1. Build all targets; fix whatever the compiler says, with 0 warnings at
   `/W4 /permissive-` as before.
2. `ctest`, then `ytres_live_tests`.
3. One live search through `ytres_cli --search "Dawid Podsiadło" --max 25`
   (two pages) and one with `--dump`, comparing the dump's shape with
   `search_videos.json`.
4. Only then is M3 reviewable as done.
