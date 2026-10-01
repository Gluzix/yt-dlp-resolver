# InnerTube notes (read from yt-dlp master on 2026-09-24)

Every fact here was read from yt-dlp's `yt_dlp/extractor/youtube/_base.py`
and `_video.py` at master on the date above, not guessed. This is a snapshot
and it rots: before starting any milestone, re-read those two files and fix
whatever changed here. The "Verified live" section at the bottom is for what
the code actually observed on the wire.

## The client: `visionos`

yt-dlp's defaults for an anonymous user without a JS runtime:

```python
_DEFAULT_CLIENTS        = ('visionos', 'web')
_DEFAULT_JSLESS_CLIENTS = ('visionos',)
```

The client definition, verbatim:

```python
'visionos': {
    'INNERTUBE_CONTEXT': {
        'client': {
            'clientName': 'VISIONOS',
            'clientVersion': '1.02',
            'deviceMake': 'Apple',
            'deviceModel': 'RealityDevice17,1',
            'userAgent': 'Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15',
            'osName': 'visionOS',
            'osVersion': '26.5.23O471',
        },
    },
    'INNERTUBE_CONTEXT_CLIENT_NAME': 101,
    'REQUIRE_JS_PLAYER': False,
},
```

No `GVS_PO_TOKEN_POLICY`, no `PLAYER_PO_TOKEN_POLICY`: it is the one client
that needs neither the JS player nor a PO Token. That is why it is the whole
POC. It will not last forever (yt-dlp's comments record `android_vr` being
cut off on 2026-08-17), so the client table in code must be data that is
trivial to extend.

## The second client: `web`

Added for M2's client ladder. The row, verbatim:

```python
'web': {
    'INNERTUBE_CONTEXT': {
        'client': {
            'clientName': 'WEB',
            'clientVersion': '2.20260708.00.00',
        },
    },
    'INNERTUBE_CONTEXT_CLIENT_NAME': 1,
    'SUPPORTS_COOKIES': True,
    **WEB_PO_TOKEN_POLICIES,
},
```

It needs the JS player and a PO Token, so the library cannot play through
it; it is in the table to exercise the ladder and as the pattern for the
next client that works. Its context has no `userAgent`, so
`_generate_api_headers` drops the `User-Agent` header and yt-dlp's HTTP layer
sends its standard one, from `random_user_agent()` in
`yt_dlp/utils/networking.py`:

```python
USER_AGENT_TMPL = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/{}.0.0.0 Safari/537.36'
CHROME_MAJOR_VERSION_RANGE = (145, 151)
```

The library sends no `userAgent` in the body either, and fixes the header at
Chrome 151, the top of that range, so that a request reads the same every
time.

## The player request

```
POST https://www.youtube.com/youtubei/v1/player?prettyPrint=false
Content-Type: application/json
X-YouTube-Client-Name: 101
X-YouTube-Client-Version: 1.02
Origin: https://www.youtube.com
User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15
```

`X-Goog-Visitor-Id` is sent only when yt-dlp has visitor data; with none, the
header is dropped. The POC starts without it (see "Things the POC skips").
There is no `key=` API key in the URL any more.

Body — these are exactly the keys yt-dlp sends for a JS-less client. `hl`,
`timeZone` and `utcOffsetMinutes` are forced onto the client context by
`_extract_context`; `playbackContext` and the two `...CheckOk` flags come from
`_generate_player_context`. There is no `signatureTimestamp` because that
needs the JS player, and no `serviceIntegrityDimensions` because that is the
PO Token.

```json
{
  "context": {
    "client": {
      "clientName": "VISIONOS",
      "clientVersion": "1.02",
      "deviceMake": "Apple",
      "deviceModel": "RealityDevice17,1",
      "userAgent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15",
      "osName": "visionOS",
      "osVersion": "26.5.23O471",
      "hl": "en",
      "timeZone": "UTC",
      "utcOffsetMinutes": 0
    }
  },
  "videoId": "dQw4w9WgXcQ",
  "playbackContext": {
    "contentPlaybackContext": { "html5Preference": "HTML5_PREF_WANTS" }
  },
  "contentCheckOk": true,
  "racyCheckOk": true
}
```

## The player response

- `playabilityStatus.status` — yt-dlp accepts `OK`, `LIVE_STREAM_OFFLINE`,
  `AGE_CHECK_REQUIRED` and `AGE_VERIFICATION_REQUIRED`; anything else
  (`ERROR`, `UNPLAYABLE`, `LOGIN_REQUIRED`, `CONTENT_CHECK_REQUIRED`, ...) is
  a failure. `playabilityStatus.reason` is the human-readable message.
- Sanity check yt-dlp does and so must we: `videoDetails.videoId` must equal
  the requested id. YouTube occasionally answers with a different video.
- `videoDetails`: `videoId`, `title`, `author`, `channelId`, `lengthSeconds`
  (a string), `isLive`, `isLiveContent`, `shortDescription`.
- `streamingData.formats` (muxed audio+video) and
  `streamingData.adaptiveFormats` (audio-only and video-only). yt-dlp walks
  both. Per format: `itag`, `url` *or* `signatureCipher`, `mimeType` (e.g.
  `audio/webm; codecs="opus"`), `bitrate`, `averageBitrate`, `contentLength`
  (string), `approxDurationMs` (string), `audioQuality`, `audioSampleRate`
  (string), `audioChannels`, `quality`, `qualityLabel`, `width`, `height`,
  `fps`. yt-dlp reads bitrate as `averageBitrate` falling back to `bitrate`.
- A format with `signatureCipher` and no `url` is unusable without the JS
  player. Skip it. The expectation for this client is that every format
  carries a plain `url` — confirm and record below.
- Expiry: yt-dlp does not read `streamingData.expiresInSeconds`; it takes
  `expire=<unix seconds>` from each format url's query string. Do the same,
  and fall back to now + `expiresInSeconds` if a url has no `expire`.

## Best-audio choice

Among formats whose `mimeType` starts with `audio/`: itag 251 (Opus, ~160k)
first, then 140 (AAC 128k), then whichever has the highest bitrate. For
reference, 250 and 249 are lower-rate Opus, 139 is low AAC, 141 is 256k AAC
(Premium only, will not appear). This matches what the bot gets today from
yt-dlp's `-f bestaudio` on ordinary videos, which is almost always 251.

## Video URL forms to accept

| Input | Id |
|---|---|
| `https://www.youtube.com/watch?v=ID` (any other params, any subdomain) | `v` |
| `https://youtu.be/ID` (optional `?t=` etc.) | path |
| `https://www.youtube.com/shorts/ID`, `/live/ID`, `/embed/ID`, `/v/ID` | path |
| `https://music.youtube.com/watch?v=ID` | `v` |
| bare `ID` | itself |

An id is exactly 11 characters from `[A-Za-z0-9_-]`. The canonical page URL
to report is always `https://www.youtube.com/watch?v=ID`.

## Things the POC skips, and what to do if YouTube objects

yt-dlp fetches the watch page first (with the `web` client's user agent) to
read `ytcfg`, and lifts visitor data from `VISITOR_DATA` or
`INNERTUBE_CONTEXT.client.visitorData` there, which then travels as
`X-Goog-Visitor-Id`. The POC tries the bare player request first. If YouTube
answers with a 4xx, an empty `streamingData`, or only ciphered formats, the
next thing to try — in that order — is:

1. Add the visitor data: GET `https://www.youtube.com/watch?v=ID` with the
   client user agent, find `ytcfg.set({...})` in the HTML, pull
   `INNERTUBE_CONTEXT.client.visitorData`, send it as `X-Goog-Visitor-Id`
   and also as `context.client.visitorData` in the body.
   yt-dlp's `_initialize_consent` also puts `SOCS=CAI`, a consent already
   given, in its cookie jar for `.youtube.com`, so that no EU consent
   interstitial stands in for the page. Since M2 the library sends it as a
   `Cookie` header on the watch page; yt-dlp's jar sends it on the player
   request too, and the library does not.
2. Report back. Do not go beyond this in the POC — adding more clients is a
   M2 decision, and PO Tokens are out of scope for good.

yt-dlp also retries on 5xx. The POC does not need to.

## Verified live

2026-09-24, from the development PC in Poland, through `ytres_cli` (libcurl
8.18.0 on Schannel), with yt-dlp 2026.08.19 for comparison.

- **The bare request is not enough.** Without visitor data the player request
  answered `dQw4w9WgXcQ` with `OK` and formats, but the nine other videos
  tried (`jNQXAC9IVRw`, `9bZkp7q19f0`, `kJQP7kiw5Fk`, `JGwWNGJdvx8`,
  `fJ9rUzIMcZQ`, `hTWKbfoikeg`, `YQHsXMglC9A`, `OPf0YbXqDm0`, `60ItHLz5WEA`)
  came back HTTP 200 with `LOGIN_REQUIRED`, reason "Sign in to confirm you’re
  not a bot", and no `streamingData`. `jNQXAC9IVRw` failed that way three
  times with `dQw4w9WgXcQ` still answering in between, so it is no blanket
  block on the IP.
- **Step 1 fixed all of them.** With the watch page's visitor data sent as
  `X-Goog-Visitor-Id` and as `context.client.visitorData`, all ten came back
  `OK`, and the library now does this on every resolve. The watch page GET
  with the visionos user agent got a 200 straight away, no consent redirect.
  Its first `ytcfg.set(` takes a string; the first object form sits about
  10 KB in and carries `INNERTUBE_CONTEXT.client.visitorData`. There is no
  top-level `VISITOR_DATA` in it.
- **The page is the cost.** It is about 1.3 MB for a real video and took 0.7
  to 1.1 s to fetch, most of a resolve: 1.1 to 1.7 s with it, against 0.3 s
  for the one bare request that worked.
- **Every format had a plain `url`.** No `signatureCipher` in any `OK`
  answer, and no url carried an `n` parameter. `serverAbrStreamingUrl`
  (SABR) and `hlsManifestUrl` came alongside the plain formats.
- **No muxed formats.** `streamingData.formats` was absent. `dQw4w9WgXcQ` had
  27 entries in `adaptiveFormats`: 22 video, 5 audio (139, 140, 249, 250,
  251).
- **`bestAudio()` picked 251 for all ten.** For `dQw4w9WgXcQ`: Opus,
  `averageBitrate` 128930 (`bitrate` 136544), 3433755 bytes. A HEAD request
  on its url answered 200, `audio/webm`, with that Content-Length.
- **`expire=` was present** in every url, one value per response, about six
  hours out; `expiresInSeconds` was 21540.
- **`00000000000`** answered `ERROR`, "This video is unavailable", with no
  `videoDetails`.
- **yt-dlp agrees.** Its `-f bestaudio` on `dQw4w9WgXcQ` printed the same
  title and page url and format 251, from a url also marked `c=VISIONOS`, in
  4.8 s.

The responses in `tests/fixtures/` come from this run, with the requesting
IP replaced by 203.0.113.7 and the visitor data by "FIXTURE".

M2, the same day and line:

- **The visitor data outlives its video.** Cached from `dQw4w9WgXcQ`'s page,
  it served `jNQXAC9IVRw`, `9bZkp7q19f0`, `kJQP7kiw5Fk` and `fJ9rUzIMcZQ`
  with no bot check. A warm resolve, the player request alone, took 118 to
  187 ms; the POC's two-request resolves took 0.84 to 1.31 s the same
  afternoon.
- **The `web` client is refused, not ciphered.** Asked for `dQw4w9WgXcQ`
  with visitor data and without a PO Token, it answered `UNPLAYABLE`, reason
  "Video unavailable", subreason "The page needs to be reloaded.", a Reload
  button whose command signals `RELOAD_PAGE`, `videoDetails`, and no
  `streamingData` at all. It is YouTube turning the client away (yt-dlp's
  own `web` clients met the same answer in March 2026), so the library
  reads it as `NoFormats` and the ladder moves on, where `Unavailable`
  would have given the video up. `tests/fixtures/player_web_dQw4w9WgXcQ.json`
  is that answer.
- **The `web` answer names the visitor twice.** Besides
  `responseContext.visitorData`, which it lacked, it carries the visitor id
  as the `GFEEDBACK` tracking parameter `visitor_data`: base64 protobuf that
  also encodes the country (`PL`). The dump scrubber replaces both.
- **Cancellation now lands in 20 to 27 ms.** A cancel 100 ms into a connect
  to the unroutable 10.255.255.1 returned after 1000 to 1015 ms through
  `curl_easy_perform`, which polls its progress callback about once a
  second while connecting, and after 120 to 127 ms through the multi
  interface's 20 ms polls.

## Search and playlists

Read from yt-dlp's `_search.py`, `_tab.py` and `_base.py` at master and
probed against live YouTube on 2026-10-01, with the `web` client and no
visitor data. Both endpoints answered the bare request with HTTP 200; neither
asked for a PO Token or met a bot check. The `visionos` and `web` rows of
yt-dlp's client table were unchanged from 2026-09-24.

yt-dlp makes both kinds of request as the `web` client (`default_client='web'`
in `_search_results` and `_extract_response`), through `_call_api`: the
client's context first, then the query's own fields, with the headers the
player request has.

### Search

```
POST https://www.youtube.com/youtubei/v1/search?prettyPrint=false

{"context": {"client": {"clientName": "WEB", "clientVersion": "2.20260708.00.00",
                        "hl": "en", "timeZone": "UTC", "utcOffsetMinutes": 0}},
 "query": "Dawid Podsiadło",
 "params": "EgIQAfABAQ=="}
```

- `EgIQAfABAQ==` is yt-dlp's `_SEARCH_PARAMS`, commented "Videos only". The
  older `EgIQAQ==` that circulates is not what yt-dlp sends now.
- The filter does **not** keep channels out. For an artist's name the first
  item of the first page was a `channelRenderer`, followed by 19
  `videoRenderer`s. Whoever reads the results still has to skip what is not
  a video.
- First page: `contents.twoColumnSearchResultsRenderer.primaryContents.sectionListRenderer.contents[]`
  held one `itemSectionRenderer` (the results, in its `contents[]`) and one
  `continuationItemRenderer`, as siblings.
- The continuation token is at
  `continuationItemRenderer.continuationEndpoint.continuationCommand.token`;
  the endpoint also carries `clickTrackingParams`.
- A further page: yt-dlp keeps `query` and `params` in the body and adds
  `"continuation": <token>` and `"clickTracking": {"clickTrackingParams": ...}`
  (`_build_api_continuation_query`). The answer has no `contents`; the items
  are at `onResponseReceivedCommands[0].appendContinuationItemsAction.continuationItems[]`,
  again an `itemSectionRenderer` (20 videos) and a `continuationItemRenderer`.
- A `videoRenderer` has `videoId`, `title.runs[].text`,
  `lengthText.simpleText` ("4:36"), `ownerText.runs[0].text`,
  `longBylineText`, `shortBylineText`, `thumbnailOverlays[]`,
  `publishedTimeText`, `viewCountText`.
- A live stream's `videoRenderer` has no `lengthText` and a badge:
  `badges[].metadataBadgeRenderer.style` is `BADGE_STYLE_TYPE_LIVE_NOW`.
  yt-dlp also reads `thumbnailOverlays[].thumbnailOverlayTimeStatusRenderer.style`
  being `LIVE`, and takes `upcomingEventData` to mean an upcoming one.
- A search with no results: `estimatedResults` is `"0"` and the item section
  holds one `backgroundPromoRenderer`. No continuation.
- Sizes: a first page is about 265 KB of JSON (24 KB on the wire with gzip),
  0.5 s.
- Every result carries preview-thumbnail urls with `&ip=<the requesting
  address>`: 57 occurrences in one page. The dump scrubber's `ip=` rule
  covers them.
- `estimatedResults` is a string on every page, first or further:
  `"584471"` for the artist, `"0"` for the empty search. The library takes
  a first page that counts results but holds no `videoRenderer` as a sign
  that the results moved into a renderer it does not read, and fails it
  with `Parse` rather than report nothing found.

Read in the M3 review of yt-dlp's `_search_results` and its helpers, not
probed again:

- **What yt-dlp does that the library deliberately does not.** Before
  searching, yt-dlp downloads www.youtube.com's ytcfg (`_download_ytcfg`;
  only `--extractor-args youtube:skip=webpage` skips it) and builds the
  request from the page's `INNERTUBE_CONTEXT`, so its context carries more
  client fields than the bare one above, the page's visitor data among
  them. The library fetches no page for a search: YouTube answered the bare
  request on 2026-10-01, and the page would cost as much as the search. It
  sends the visitor data a resolve has cached, when there is some.
- **Visitor data across pages.** yt-dlp sends the previous page's
  `responseContext.visitorData` as `X-Goog-Visitor-Id` on page 2 and later.
  The library does the same when it has no cached visitor data, after the
  same check the watch page's value passes (`src/visitor_data.h`), and does
  not cache it.
- **Length.** When a `videoRenderer` has no `lengthText`, yt-dlp reads the
  text of its `thumbnailOverlays[].thumbnailOverlayTimeStatusRenderer`; so
  does the library, taking the first.
- **Continuation.** yt-dlp also finds a page's continuation inside an item
  section's own `contents[]`. The library takes the one beside the item
  sections first, and one inside only when there is none beside.

### Playlists

```
POST https://www.youtube.com/youtubei/v1/browse?prettyPrint=false

{"context": {...as above...}, "browseId": "VLPLrAXtmErZgOdP_8GztsuKi9nrraNbKKp4"}
```

- `browseId` is `VL` followed by the playlist id, as
  `_reload_with_unavailable_videos` builds it (the tab extractor's own first
  request is the web page or `navigation/resolve_url`; see below). yt-dlp
  adds `"params": "wgYCCAA="` only to make YouTube list unavailable videos
  too; without it they are left out.
- Playlist ids, from yt-dlp's `_PLAYLIST_ID_RE`:
  `(?:(?:PL|LL|EC|UU|FL|RD|UL|TL|PU|OLAK5uy_)[0-9A-Za-z-_]{10,}|RDMM|WL|LL|LM)`.
  `RD...` are mixes generated per viewer, `WL`, `LL` and `LM` are a signed-in
  viewer's own lists.
- **The layout changed.** yt-dlp still reads the old one,
  `playlistVideoListRenderer.contents[]` of `playlistVideoRenderer` with a
  `continuationItemRenderer` at the end, but both playlists probed (an
  ordinary one of 7 videos and a podcast of 447) came as
  `contents.twoColumnBrowseResultsRenderer.tabs[0].tabRenderer.content.sectionListRenderer.contents[]`
  = one `itemSectionRenderer` and one `continuationItemViewModel`, and inside
  the item section's `contents[]`: up to 100 `lockupViewModel`s and, when
  there are more, a `continuationItemViewModel`. No `playlistVideoListRenderer`
  anywhere. yt-dlp handles both (`_extract_lockup_view_model`,
  `continuationItemViewModel` in `_extract_continuation`).
- A `lockupViewModel` for a video: `contentType` is
  `LOCKUP_CONTENT_TYPE_VIDEO`, `contentId` is the video id, the title is at
  `metadata.lockupMetadataViewModel.title.content`, and the duration text
  ("2:31:48") at `contentImage.thumbnailViewModel.overlays[].thumbnailBottomOverlayViewModel.badges[].thumbnailBadgeViewModel.text`.
  yt-dlp also looks under `thumbnailOverlayBadgeViewModel.thumbnailBadges[]`,
  reads `badgeStyle` `THUMBNAIL_OVERLAY_BADGE_STYLE_LIVE` as live and the
  text "Upcoming" as upcoming.
- The continuation token of the new layout:
  `continuationItemViewModel.continuationCommand.innertubeCommand.continuationCommand.token`,
  with `clickTrackingParams` on the `innertubeCommand`.
- **Two continuations, one to follow.** The one among the entries loads the
  next hundred. The one at section-list level (present even for the 7-video
  playlist) loads something else; yt-dlp follows it only when it finds no
  other and expects nothing from it.
- A further page: `{"context": ..., "continuation": <token>, "clickTracking": ...}`
  to the same endpoint. The answer's entries are at
  `onResponseReceivedActions[0].appendContinuationItemsAction.continuationItems[]`:
  100 `lockupViewModel`s and the next `continuationItemViewModel`. The answer
  also has a `contents` key, but it is a stub (a `tabRenderer` with no
  `content`) and holds no entries.
- The playlist's title is at `metadata.playlistMetadataRenderer.title`. The
  header is a `pageHeaderRenderer` now; `playlistHeaderRenderer` was absent.
- The count is the first stat of
  `sidebar.playlistSidebarRenderer.items[].playlistSidebarPrimaryInfoRenderer.stats[0]`:
  `{"runs": [{"text": "447"}, {"text": " episodes"}]}`, or `" videos"`.
  yt-dlp reads the same place, falling back to the old header's
  `numVideosText` and `byline`.
- A playlist that does not exist: HTTP 200, no `contents`, and
  `alerts[0].alertRenderer` = `{"type": "ERROR", "text": {"runs": [{"text":
  "The playlist does not exist."}]}}`.
- Sizes: about 12 KB of JSON per entry. A full page of 100 is 1.25 MB before
  gzip, well under the HTTP client's 8 MB cap; 0.2 to 0.4 s.
- The entries carry the same `&ip=` preview urls as search results.

Read in the M4 review of yt-dlp's tab extractor (`_real_extract` and its
helpers in `_tab.py`), not probed again:

- **What yt-dlp does that the library deliberately does not.** For a
  playlist link, yt-dlp first downloads the playlist's own page,
  `https://www.youtube.com/playlist?list=<id>` (`_extract_data`,
  `_extract_webpage`), and takes the first page's answer from its
  `ytInitialData` and the request context from its ytcfg. Only
  `--extractor-args youtubetab:skip=webpage`, or a page that yields no data,
  makes it ask the API instead, in two requests: `navigation/resolve_url`
  with the link, then `browse` with the endpoint that answers
  (`_extract_tab_endpoint`). Either way, once the answer is a playlist
  (`metadata.playlistMetadataRenderer` or `header.playlistHeaderRenderer`),
  it asks `browse` again with `{"params": "wgYCCAA=", "browseId": "VL<id>"}`
  (`_reload_with_unavailable_videos`, which
  `--compat-options no-youtube-unavailable-videos` skips) and reads the
  first page's entries from that answer, unavailable videos among them.
  That reload is the one place yt-dlp builds `VL<id>` itself; the pages
  after the first come from `browse` with the continuation alone
  (`_entries`). The library fetches no page, resolves no url and makes no
  reload: one `browse` request with `VL<id>` and no `params`, which YouTube
  answered on 2026-10-01, leaving out the unavailable videos the bot drops
  anyway. It sends the visitor data a resolve has cached, when there is
  some.
- **Alerts.** yt-dlp fails an answer that carries any alert of type
  `error`, whatever else it holds, with the last such alert's text
  (`_report_alerts` in `_base.py`). The library fails an answer with no
  section list or no entries, with the first `ERROR` alert's text.
- **Looping.** yt-dlp's `_entries` stops when a continuation token comes
  round again ("Detected YouTube feed looping") and has no page limit;
  `MAX_PLAYLIST_PAGES` (200) and the stop after three empty pages in a row
  are the library's own.

The fixtures `tests/fixtures/search_*.json` and `playlist_*.json` are these
answers, scrubbed and cut down to a few entries each.
