#pragma once

#include "ytres/http.h"
#include "ytres/ytres.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// What the library says to YouTube's InnerTube API and how it reads the
// answer. docs/innertube-notes.md is the specification, read from yt-dlp.
// =======================================================
// Rules:
// - playerRequest() sends exactly the headers and body keys the notes list,
//   nothing more: a field yt-dlp never sends is a fingerprint YouTube can
//   single out.
// - The client table (CLIENTS in innertube.cpp) is data: burning a client or
//   adding one is an edit to a row, not to code.
// - parsePlayerResponse() is pure. The clock comes in as nowUnix, so a
//   recorded response replays the same way every time.
// - Only a format with a plain url is usable. One that carries a
//   signatureCipher instead needs YouTube's player JavaScript, which the
//   library does not run, so parsePlayerResponse() skips it. It also skips
//   what yt-dlp never picks, url or not: live segments, OTF streams, DRM.
// - In parsePlayerResponse() the reason text decides before the status:
//   LOGIN_REQUIRED covers age gates, private videos and the bot check as
//   well as a real sign-in wall. The phrases are English (hl=en); in another
//   language an age gate, a private video or the bot check reads as
//   LoginRequired and a region block as Unavailable.
// - A client YouTube will not serve as asked - web without a PO Token - gets
//   "Video unavailable. The page needs to be reloaded." with a RELOAD_PAGE
//   button. That is NoFormats, a failure of the client, not Unavailable: the
//   ladder must move on rather than give the video up.
// =======================================================
namespace ytres::innertube {

// One row of yt-dlp's INNERTUBE_CLIENTS. A null string is a field the client
// does not send.
struct ClientDef
{
    ClientId id;
    const char *key;               // yt-dlp's name for the client, for logs
    int contextClientName;         // INNERTUBE_CONTEXT_CLIENT_NAME, sent as X-YouTube-Client-Name
    const char *clientName;
    const char *clientVersion;     // also sent as X-YouTube-Client-Version
    const char *deviceMake;
    const char *deviceModel;
    const char *userAgent;         // also the User-Agent header; see userAgentHeader()
    const char *osName;
    const char *osVersion;
    // True when YouTube ciphers the client's formats for its player
    // JavaScript to undo. The library runs none, so such a client can only
    // end in NoFormats.
    bool requireJsPlayer;
};

// The table's row for id; null when it has none.
const ClientDef *findClient(ClientId id);

// The User-Agent header for the client's requests, as yt-dlp sends it: the
// userAgent of its context, or for a client without one (web), the desktop
// Chrome that yt-dlp's HTTP layer poses as.
const char *userAgentHeader(const ClientDef &client);

// POST /youtubei/v1/player for one video, as yt-dlp sends it for a client
// that needs no JS player: no signatureTimestamp, whichever the client, since
// that comes from the player JavaScript. language goes out as hl;
// visitorData, unless empty, as X-Goog-Visitor-Id and as
// context.client.visitorData.
HttpRequest playerRequest(const ClientDef &client, const std::string &videoId, const std::string &language,
                          const std::string &visitorData);

using LogFn = std::function<void(LogLevel, std::string_view)>;

// Reads the player response YouTube gave for videoId: playability, the
// video's details and every format with a plain url. A playability failure
// carries YouTube's reason as its message; an answer about another video is
// a Parse failure.
Result<VideoInfo> parsePlayerResponse(const std::string &body, const std::string &videoId,
                                      std::int64_t nowUnix, const LogFn &log = {});

}
