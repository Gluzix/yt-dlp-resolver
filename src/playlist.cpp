#include "playlist.h"

#include <nlohmann/json.hpp>

namespace ytres::innertube {

namespace {

using nlohmann::ordered_json;

// A playlist's browse id is its id behind VL, as yt-dlp's tab extractor and
// _reload_with_unavailable_videos both build it.
const char *const PLAYLIST_BROWSE_PREFIX = "VL";

}

HttpRequest playlistRequest(const ClientDef &client, const std::string &playlistId, const std::string &language,
                            const std::string &visitorData, const Continuation &continuation)
{
    // Unlike a search, which repeats its query, a further page of a browse
    // is the continuation and nothing else, as yt-dlp sends it.
    ordered_json fields = ordered_json::object();
    if (continuation.token.empty()) {
        fields["browseId"] = PLAYLIST_BROWSE_PREFIX + playlistId;
    } else {
        addContinuation(fields, continuation);
    }
    return apiRequest(client, "browse", language, visitorData, fields);
}

}
