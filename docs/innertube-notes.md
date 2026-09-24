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
IP replaced by 203.0.113.7.
