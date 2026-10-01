#pragma once

#include "continuation.h"
#include "innertube.h"
#include "ytres/http.h"

#include <string>

// A playlist: the request that asks YouTube for one page of a public
// playlist. docs/innertube-notes.md, "Playlists", is the specification, read
// from yt-dlp's _tab.py and probed live.
// =======================================================
// Rules:
// - playlistRequest() sends what yt-dlp sends: the client's context and
//   browseId VL<playlistId> for the first page; the context and the
//   continuation alone, with no browseId, for every page after it. No
//   params: yt-dlp sends wgYCCAA= only to have the unavailable videos listed
//   too, and the library has no use for them.
// =======================================================
namespace ytres::innertube {

// POST /youtubei/v1/browse for the first page of playlistId, through
// apiRequest(). With a continuation token, the request for the page after
// the one the continuation came from instead, which names no playlist:
// the token alone says which.
HttpRequest playlistRequest(const ClientDef &client, const std::string &playlistId, const std::string &language,
                            const std::string &visitorData, const Continuation &continuation = {});

}
