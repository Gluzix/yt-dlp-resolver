# M4 plan: playlists

Implementation plan for whoever codes it. Everything needed is in this file,
`docs/m3-plan.md` (whose groundwork this milestone stands on) and
`docs/innertube-notes.md`, section "Search and playlists": the protocol
facts, read from yt-dlp's `_tab.py` and `_base.py` at master and probed
against live YouTube on 2026-10-01. Branch: `m4/playlists`, stacked on
`m3/search`. Kamil merges through pull requests; nothing is pushed by anyone
but him.

**Written without a build**, like M3: Kamil asked for no compilation and no
test runs while it was coded. "Before this is done", at the bottom, says what
that leaves open.

## What the bot needs

`listPlaylist(url, maxEntries)` runs `yt-dlp --flat-playlist --playlist-items
:N` today and reads url/title pairs plus the playlist's title and count. The
library's version:

```cpp
ytres::Result<ytres::Playlist> list = resolver.playlist("https://www.youtube.com/playlist?list=PL...", 50);
// list.value.title, list.value.totalCount, list.value.entries[i].videoId / title
```

## Public API (`include/ytres/ytres.h`)

```cpp
struct PlaylistEntry
{
    std::string videoId;
    std::string title;                // UTF-8
    std::int64_t durationSeconds{0};  // 0 when YouTube gives none
};

struct Playlist
{
    std::string playlistId;
    std::string title;                  // empty when YouTube gave none
    std::size_t totalCount{0};          // the videos YouTube says it has; 0 = unknown
    std::vector<PlaylistEntry> entries; // in playlist order, at most max
};

class Resolver
{
    ...
    // The first max videos of a playlist, in its order, with its title and
    // how many videos YouTube says it has. Accepts a youtube.com link that
    // carries list=, or a bare playlist id. One request per hundred videos,
    // and no more requests than max needs: a 6000-video playlist asked for
    // its first 50 costs one. Videos YouTube hides as unavailable are not
    // listed, so entries may be fewer than totalCount says.
    // A playlist that does not exist or is private is Unavailable, with
    // YouTube's words. A mix (RD...), Watch Later, Liked videos and the like
    // are BadInput: they belong to a signed-in viewer. max of 0 is BadInput.
    // Asked as the web client, with cached visitor data when there is some.
    // When a later page fails, the result carries that failure and the
    // entries read so far.
    Result<Playlist> playlist(std::string_view urlOrId, std::size_t max, const Request &request = {});
};
```

## Which playlist (`src/url_parse.h/.cpp`)

```cpp
// The playlist a link names: the list= of a /playlist or /watch link on
// youtube.com or any of its subdomains, of a youtu.be link, or a bare id.
// Only playlists anyone can open: ids starting PL, UU, FL, OLAK5uy_, EC, UL
// or PU followed by ten or more of [A-Za-z0-9_-]. A mix (RD...), WL, LL, LM
// and TL... are BadInput with a message that says they need a signed-in
// viewer; anything else is BadInput.
Result<std::string> parsePlaylistId(std::string_view urlOrId);
```

The prefixes come from yt-dlp's `_PLAYLIST_ID_RE`. Reuse the host and scheme
handling `parseVideoId()` already has, and `queryValue()` for `list=`.

## The request

```
POST https://www.youtube.com/youtubei/v1/browse?prettyPrint=false
(the web client's headers, as for search)

{"context": {...}, "browseId": "VL<playlistId>"}
```

and for each further page

```
{"context": {...}, "continuation": "<token>", "clickTracking": {"clickTrackingParams": "<ctp>"}}
```

Both through `innertube::apiRequest()` and `addContinuation()` from M3. No
`params`: yt-dlp only sends `wgYCCAA=` to make YouTube list the unavailable
videos too, which the bot filters out anyway.

## The response (`src/playlist.h/.cpp`, namespace `ytres::innertube`)

```cpp
struct PlaylistPage
{
    std::string title;          // first page only
    std::size_t totalCount{0};  // first page only; 0 = unknown
    std::vector<PlaylistEntry> entries;
    Continuation next;
};

// Reads one answer of /youtubei/v1/browse for a playlist, a first page or a
// continuation. Pure. An ERROR alert with no contents is Unavailable with
// the alert's text; not JSON, or JSON with nothing recognisable, is Parse.
Result<PlaylistPage> parsePlaylistResponse(const std::string &body);
```

YouTube serves two layouts, and the reader takes both. On 2026-10-01 every
playlist probed came in the new one; the old one is what yt-dlp still
handles alongside it.

**Where the entries are.**

- First page: `contents.twoColumnBrowseResultsRenderer.tabs[0].tabRenderer.content.sectionListRenderer.contents[]`,
  and in it the first `itemSectionRenderer`; its `contents[]` are
  - new layout: `lockupViewModel` items, followed by the page's
    `continuationItemViewModel` when there are more;
  - old layout: one `playlistVideoListRenderer`, whose own `contents[]` are
    `playlistVideoRenderer` items followed by a `continuationItemRenderer`.
- Continuation: `onResponseReceivedActions[].appendContinuationItemsAction.continuationItems[]`
  (also look under `onResponseReceivedEndpoints`), holding the same item
  kinds directly. A continuation answer also has a `contents` key, but it is
  a stub with no entries: when the answer has an `appendContinuationItemsAction`,
  read that and nothing else.

**Which continuation.** The one that is a sibling of the entries, in the same
array. The `sectionListRenderer.contents[]` of a first page holds a second
`continuationItemViewModel` next to the item section; it loads something
else and must not be followed. (`playlist_long_first.json` has both, with
different tokens, so a test can tell.) This is the opposite of search, where
the continuation is a sibling of the item sections.

**An entry.**

| Field | New layout (`lockupViewModel`) | Old layout (`playlistVideoRenderer`) |
|---|---|---|
| is a video | `contentType` is `LOCKUP_CONTENT_TYPE_VIDEO` | always |
| `videoId` | `contentId` | `videoId` |
| `title` | `metadata.lockupMetadataViewModel.title.content` | `textOf(renderer, "title")` |
| `durationSeconds` | the first `thumbnailBadgeViewModel.text` under `contentImage.thumbnailViewModel.overlays[]`, in either `thumbnailBottomOverlayViewModel.badges[]` or `thumbnailOverlayBadgeViewModel.thumbnailBadges[]`, through `durationSeconds()` | `readInt(renderer, "lengthSeconds")`, else `durationSeconds(textOf(renderer, "lengthText"))` |
| skip when | not a video, no valid `videoId`, or an unplayable title (below) | `isPlayable` is present and false, no valid `videoId`, or an unplayable title |

An unplayable title is an empty one, `[Private video]` or `[Deleted video]`:
what the bot filters today. With the default request YouTube leaves such
videos out by itself; the check is for the day it does not.

**The playlist's own facts**, first page only:

- `title`: `metadata.playlistMetadataRenderer.title`; else
  `textOf(header.playlistHeaderRenderer, "title")`; else
  `header.pageHeaderRenderer.pageTitle`.
- `totalCount`: the digits of the first stat in
  `sidebar.playlistSidebarRenderer.items[].playlistSidebarPrimaryInfoRenderer.stats[0]`
  (`textOf` gives "447 episodes", "7 videos", "6,000 videos"; keep the
  digits, drop the separators; "No videos" is 0); else the same from
  `header.playlistHeaderRenderer.numVideosText`; else 0.

**Failures.** A playlist that does not exist answers HTTP 200 with no
`contents` and `alerts[0].alertRenderer` of `type` `ERROR` and text "The
playlist does not exist." That is `Unavailable` with the alert's text
(`textOf(alertRenderer, "text")`). Alerts of other types beside real
contents (YouTube uses one to say unavailable videos are hidden) are not
failures.

## The loop (`Resolver::Impl::playlist` in `src/resolver.cpp`)

The same shape as `Impl::search()`: the shared opening, `max == 0` is
`BadInput`, `parsePlaylistId()`, the first page, then continuations while
fewer than `max` entries are held and the page gave a continuation. Two
guards against a feed that loops, both from yt-dlp's `_entries`: stop when a
token repeats, and stop after `MAX_PLAYLIST_PAGES` (200). Truncate to `max`
at the end. Title and count come from the first page; a failure on a later
page returns that failure with the playlist as read so far.

## CLI

`ytres_cli --playlist <url-or-id> [--max N] [--dump <file>]`, `--max`
defaulting to 50. First line: the title, a tab, the total count. Then one
line per entry: the watch url, a tab, the seconds, a tab, the title.
`--dump` writes the first browse response, scrubbed.

## Tests (`tests/playlist_test.cpp`)

Real responses from 2026-10-01, scrubbed and trimmed, already in
`tests/fixtures/`:

| Fixture | What it holds | Expect |
|---|---|---|
| `playlist_small.json` | `PLC2bGavj05vj1BDIQbhCDiXywCU4_P9Pk`, new layout, 7 videos, one page | title `Our favourite Lex Fridman Podcast Episodes`, count 7, 7 entries: `DxREm3s1scA` (9108 s), `XW0QZmtbjvs` (10921 s), `4dC_nRYIDZU` (11840 s), `Fx0G6DHMfXM` (6750 s), `Iau6W5pjy9Y` (5180 s), `hGRNUw559SE` (15320 s), `KOwm7GUjcg8` (7499 s); no continuation, although the section list holds a `continuationItemViewModel` |
| `playlist_long_first.json` | `PLrAXtmErZgOdP_8GztsuKi9nrraNbKKp4`, 447 videos, first page cut to 3 entries and its continuation | title `Lex Fridman Podcast`, count 447, entries `s7d2d8FhevU` (13016 s), `NYFGCESmikA` (18951 s), `l6USUAIKJls` (11578 s); the continuation token starts `4qmFsgKBARIk` (the one to follow), not `4qmFsgJbEiRW` (the section-level one) |
| `playlist_long_continuation.json` | its second page cut to 3 entries and the next continuation | `2yHr9DPnSzk` (5942 s), `r4wLXNydzeY` (12402 s), `JN3KPFbWCy8` (8207 s); a token starting `4qmFsgJ_EiRW`; no title, count 0 |
| `playlist_missing.json` | a playlist that does not exist | `Unavailable`, "The playlist does not exist." |

The old layout cannot be recorded any more, so its test writes a small JSON
by hand from the table above (a `playlistVideoListRenderer` with two
`playlistVideoRenderer`s, one unplayable one and a `continuationItemRenderer`,
plus a `playlistHeaderRenderer` with `numVideosText`), and says in a comment
that it is synthetic.

Cover at least: `parsePlaylistId` on every accepted form and every refused
one; the browse request and the continuation request pinned field for field;
each fixture with the table's values; the old layout; an unplayable title in
either layout; through the Resolver and the fake — `max` 2 on
`playlist_long_first.json` sends one request and returns two entries; `max`
5 sends the continuation and returns five of the six; a repeated token stops
the loop; a second page that fails returns the failure with the first
page's entries, title and count; `playlist_missing.json` is `Unavailable`;
`max` 0 and a mix id are `BadInput` with nothing sent.

## Documentation

README (the playlist API and `--playlist`), `docs/resolver-plan.md` (the API
sketch as built, and the milestones table), `docs/walkthrough.md` (a short
playlist section next to M3's search section).

## Out of scope

Mixes and personal lists. Channel tabs. Listing unavailable videos. A page
size other than YouTube's hundred.

## Before this is done

Nothing here was compiled or run. When Kamil lifts that: build with 0
warnings, `ctest`, `ytres_live_tests`, then live runs of
`ytres_cli --playlist PLrAXtmErZgOdP_8GztsuKi9nrraNbKKp4 --max 150` (two
pages) and of a playlist with fewer than a hundred videos, and a comparison
with `yt-dlp --flat-playlist` on the same two.
