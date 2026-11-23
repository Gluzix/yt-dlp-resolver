# M4 plan: playlists

Implementation plan for whoever codes it. Everything needed is in this file,
`docs/m3-plan.md` (whose groundwork this milestone stands on) and
`docs/innertube-notes.md`, section "Search and playlists": the protocol
facts, read from yt-dlp's `_tab.py` and `_base.py` at master and probed
against live YouTube on 2026-10-01. Branch: `m4/playlists`, stacked on
`m3/search`. Kamil merges through pull requests; nothing is pushed by anyone
but him.

Coded without a build, like M3, as Kamil asked; first built and verified on
2026-11-23 (see "Verified", at the bottom).

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

Also accepted since 2026-11-23: `/embed/videoseries?list=`, the link of an
embedded playlist player, on a YouTube host or on `youtube-nocookie.com`,
as yt-dlp's tab regex accepts it. The path stays case-sensitive, as
YouTube's paths are.

Changed on 2026-11-23, after the first build: `RDCLAK5uy_` followed by ten
or more id characters is public too. It is YouTube Music's curated playlists
("The Hit List"), which users share from music.youtube.com; they start like a
mix but are the same for every viewer, and browse `VL<id>` lists them as the
web client like any other playlist (52 of 52, the same order as yt-dlp). The
public prefixes are checked first, so every other `RD...` stays a mix.

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
  and in it an `itemSectionRenderer`, whose `contents[]` are
  - new layout: `lockupViewModel` items, followed by the page's
    `continuationItemViewModel` when there are more;
  - old layout: one `playlistVideoListRenderer`, whose own `contents[]` are
    `playlistVideoRenderer` items followed by a `continuationItemRenderer`.

  Every page probed held one item section, with the playlist id as its
  `targetId`. Settled in the M4 review, so that another populated section
  YouTube might put ahead of it (a shelf of other videos) cannot pass for
  the playlist, the reader takes, in this order: the
  `playlistVideoListRenderer`'s `contents[]`, in whichever item section it
  is; the item section whose `targetId` is the playlist id, which
  `parsePlaylistResponse(body, playlistId)` takes as an optional second
  argument and the Resolver passes for every page; the first item section
  that holds a `lockupViewModel` or `playlistVideoRenderer`; else the first
  item section, which the rule for a layout the reader does not know then
  judges.
- Continuation: `onResponseReceivedActions[].appendContinuationItemsAction.continuationItems[]`
  (also look under `onResponseReceivedEndpoints`), holding the same item
  kinds directly. A continuation answer also has a `contents` key, but it is
  a stub with no entries: when the answer has an `appendContinuationItemsAction`,
  read that and nothing else. The action carries the playlist id as its
  `targetId` too; the reader does not ask for it.

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
  `header.pageHeaderRenderer.pageTitle`. The first and the last are plain
  JSON strings, so they are read with `readString`; `textOf` answers a bare
  string with nothing.
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
failures. Settled in the M4 review: the ERROR alert is looked for in
whatever renderer an alert holds, as yt-dlp does, and it also makes a first
page `Unavailable` when the page has a section list but no entries, before
the rule below and whatever the count says. yt-dlp fails any answer with an
error alert; the library keeps an answer that has entries to show.

**A layout the reader does not know** must not pass for an empty playlist,
for the reason M3's search reader fails a first page that counts results it
cannot read: the bot falls back to yt-dlp on a failure, never on an empty
list. A first page that has `contents`, yields no entry in either layout,
and whose count is above 0 is `Parse` ("The playlist counts videos but holds
none the library can read"). A count of 0 with no entries is an empty
playlist and Ok. A continuation page with nothing readable is the end of the
list, not a failure. One exception, recorded in the M4 review: an
`appendContinuationItemsAction` with no `continuationItems` is `Parse`.
Without the array the reader cannot tell the answer from a first page, finds
no section list in the stub `contents`, and fails it, where yt-dlp would take
it for the end of the list.

Narrowed on 2026-11-23, after the first build: "yields no entry" meant "keeps
no entry", so a first page whose videos were all dropped as `[Private video]`,
`[Deleted video]` or unplayable was `Parse`, and its continuation, lost with
it, was never followed. The rule now asks whether the page held a video in a
layout the reader knows (a `lockupViewModel` of `LOCKUP_CONTENT_TYPE_VIDEO`,
or a `playlistVideoRenderer`) with a valid video id, kept or dropped. A video
whose id is missing or invalid does not count, so a moved or renamed id field
still gives `Parse`. Such a page is Ok, with its
continuation, and `MAX_EMPTY_PLAYLIST_PAGES` bounds what follows. A first page
of nothing but lockups that are no video (playlists, channels) under a count
above 0 stays `Parse`: a playlist of videos would not hold only those, and a
renamed content type would look exactly like it.

**Visitor data across pages**, as M3 does for search and yt-dlp's `_entries`
does for every feed: `PlaylistPage` carries the page's
`responseContext.visitorData` when it passes `visitorDataRefusal()`
(`src/visitor_data.h`), and the loop sends it with the next page only when
the cache holds nothing. It is never written into the cache.

## The loop (`Resolver::Impl::playlist` in `src/resolver.cpp`)

The same shape as `Impl::search()`: the shared opening, `max == 0` is
`BadInput`, `parsePlaylistId()`, the first page, then continuations while
fewer than `max` entries are held and the page gave a continuation. Two
guards against a feed that loops: stop when a token repeats, as yt-dlp's
`_entries` does, and stop after `MAX_PLAYLIST_PAGES` (200), the library's
own, with a warning, since a channel's uploads (`UU...`) can run longer. A
page that brings no entry does not end the list, since it may hold nothing
but videos nobody may watch; `MAX_EMPTY_PLAYLIST_PAGES` (3) such pages in a
row do, with a warning, so that a broken feed cannot cost two hundred
requests. Truncate to `max` at the end. Title and count come from the first
page; a failure on a later page returns that failure with the playlist as
read so far.

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
| `playlist_music_curated.json` | added 2026-11-23: `RDCLAK5uy_kmPRjHDECIcuVwnKsx2Ng7fyNgFKWNJFs`, YouTube Music's "The Hit List", 52 videos on one page, cut to 3 | title `The Hit List`, count 52, `78wrful9cVU` (251 s), `mw3kSNIxjqo` (313 s), `nUsrYVxrDwI` (422 s); no continuation |

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
`max` 0 and a mix id are `BadInput` with nothing sent; a first page with a
count above 0 and only an unknown item kind is `Parse`, with a count of 0 it
is Ok and empty; with an empty cache the second page carries the first
page's visitor data, with a cached value the cached one goes on both.

One live case joins `tests/live_test.cpp` (never run by `ctest`):
`PLrAXtmErZgOdP_8GztsuKi9nrraNbKKp4` with `max` 150 is Ok, has a title, a
count above 150, exactly 150 entries with valid ids and titles, and took two
requests. It is what notices YouTube changing the playlist layout again.

## Documentation

README (the playlist API and `--playlist`), `docs/resolver-plan.md` (the API
sketch as built, and the milestones table), `docs/walkthrough.md` (a short
playlist section next to M3's search section).

## Out of scope

Mixes and personal lists. Channel tabs. Listing unavailable videos. A page
size other than YouTube's hundred.

## Verified, 2026-11-23

What this section once listed as still to do, done on the first build: all
targets built with 0 warnings; `ctest` passed (167 cases before the fix
round, 187 after it) and so did `ytres_live_tests`, whose playlist case
listed 150 of the long playlist's videos, now 450 of them (447 when the
fixtures were recorded), in two requests in about 1.5 s.
`ytres_cli --playlist PLrAXtmErZgOdP_8GztsuKi9nrraNbKKp4 --max 150` gave 150
distinct entries and `PLC2bGavj05vj1BDIQbhCDiXywCU4_P9Pk` its 7, title and
count right; both matched `yt-dlp --flat-playlist` id for id and in order
(150 of 150, 7 of 7), as did a channel's uploads and an album. A missing
playlist was `Unavailable`, "The playlist does not exist.", and a mix id
`BadInput`.

The probing that followed the build found, and the fix round fixed: YouTube
Music's curated `RDCLAK5uy_` lists were refused as mixes; `/embed/videoseries`
links were refused; a first page whose videos were all private or deleted
was `Parse`; and a malformed length badge read as hours. The dated notes
under "Which playlist" and "A layout the reader does not know" say what
changed.
