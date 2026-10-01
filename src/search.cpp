#include "search.h"

#include "json_read.h"
#include "url_parse.h"

#include <nlohmann/json.hpp>

#include <initializer_list>
#include <utility>

namespace ytres::innertube {

namespace {

using nlohmann::json;
using nlohmann::ordered_json;

using jsonread::child;
using jsonread::childArray;
using jsonread::durationSeconds;
using jsonread::readInt;
using jsonread::readString;
using jsonread::textOf;

// yt-dlp's _SEARCH_PARAMS, which it comments "Videos only": a base64
// protobuf filter. The older EgIQAQ== that circulates is not what it sends.
const char *const VIDEOS_ONLY = "EgIQAfABAQ==";

// The array that holds a page's item sections and its continuation: on a
// further page, the first appendContinuationItemsAction among
// onResponseReceivedCommands, looked for first since only a continuation
// answer has one; on a first page, the section list. Null when the answer
// has neither. firstPage says which it found.
const json *pageItems(const json &root, bool &firstPage)
{
    firstPage = false;
    if (const json *commands = childArray(root, "onResponseReceivedCommands")) {
        for (const json &command : *commands) {
            const json *action = child(command, "appendContinuationItemsAction");
            const json *items = action ? childArray(*action, "continuationItems") : nullptr;
            if (items) {
                return items;
            }
        }
    }
    const json *at = &root;
    for (const char *key : {"contents", "twoColumnSearchResultsRenderer", "primaryContents", "sectionListRenderer"}) {
        at = child(*at, key);
        if (!at) {
            return nullptr;
        }
    }
    const json *sectionList = childArray(*at, "contents");
    firstPage = sectionList != nullptr;
    return sectionList;
}

// Streaming now, by either of the marks yt-dlp reads: the LIVE badge, or a
// LIVE time status over the thumbnail.
bool isLiveNow(const json &renderer)
{
    if (const json *badges = childArray(renderer, "badges")) {
        for (const json &badge : *badges) {
            const json *metadata = child(badge, "metadataBadgeRenderer");
            if (metadata && readString(*metadata, "style") == "BADGE_STYLE_TYPE_LIVE_NOW") {
                return true;
            }
        }
    }
    if (const json *overlays = childArray(renderer, "thumbnailOverlays")) {
        for (const json &overlay : *overlays) {
            const json *timeStatus = child(overlay, "thumbnailOverlayTimeStatusRenderer");
            if (timeStatus && readString(*timeStatus, "style") == "LIVE") {
                return true;
            }
        }
    }
    return false;
}

SearchResult readVideo(const json &renderer)
{
    SearchResult result;
    result.videoId = readString(renderer, "videoId");
    result.title = textOf(renderer, "title");
    // The channel's name: ownerText where YouTube gives it, else a byline.
    for (const char *key : {"ownerText", "longBylineText", "shortBylineText"}) {
        result.author = textOf(renderer, key);
        if (!result.author.empty()) {
            break;
        }
    }
    // A live or an upcoming stream has no lengthText, and reads as 0.
    result.durationSeconds = durationSeconds(textOf(renderer, "lengthText"));
    result.isLive = isLiveNow(renderer);
    result.isUpcoming = child(renderer, "upcomingEventData") != nullptr;
    return result;
}

// The videos of one item section, in its order.
void appendVideos(const json &section, std::vector<SearchResult> &results)
{
    const json *contents = childArray(section, "contents");
    if (!contents) {
        return;
    }
    for (const json &item : *contents) {
        const json *renderer = child(item, "videoRenderer");
        if (!renderer) {
            continue; // a channel, a shelf, the empty search's promo
        }
        SearchResult result = readVideo(*renderer);
        if (isVideoId(result.videoId)) {
            results.push_back(std::move(result));
        }
    }
}

Result<SearchPage> readSearchResponse(const json &root)
{
    bool firstPage = false;
    const json *items = pageItems(root, firstPage);
    if (!items) {
        return {{Error::Parse, "The search response holds no results section"}, {}};
    }
    // Item sections hold the results; the page's continuation sits beside
    // them, not inside.
    SearchPage page;
    for (const json &item : *items) {
        if (const json *section = child(item, "itemSectionRenderer")) {
            appendVideos(*section, page.results);
        } else if (page.next.token.empty()) {
            page.next = continuationOf(item);
        }
    }
    // YouTube counts results, yet none is a video the library can read: the
    // results have moved into a renderer it does not know. Saying "nothing
    // found" would keep every caller from falling back to another resolver.
    // Only a first page says so; a further page with nothing in it is the end.
    if (firstPage && page.results.empty() && readInt(root, "estimatedResults") > 0) {
        return {{Error::Parse, "The search response counts results but holds no video the library can read"}, {}};
    }
    return {{}, std::move(page)};
}

}

HttpRequest searchRequest(const ClientDef &client, const std::string &query, const std::string &language,
                          const std::string &visitorData, const Continuation &continuation)
{
    // yt-dlp updates one dict from page to page, so a further page repeats
    // query and params, with the continuation after them.
    ordered_json fields = ordered_json::object();
    fields["query"] = query;
    fields["params"] = VIDEOS_ONLY;
    if (!continuation.token.empty()) {
        addContinuation(fields, continuation);
    }
    return apiRequest(client, "search", language, visitorData, fields);
}

Result<SearchPage> parseSearchResponse(const std::string &body)
{
    const json root = json::parse(body, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return {{Error::Parse, "The search response is not a JSON object"}, {}};
    }
    // The reads check types as they go; this is the net under them.
    try {
        return readSearchResponse(root);
    } catch (const json::exception &e) {
        return {{Error::Parse, std::string("The search response has an unexpected shape: ") + e.what()}, {}};
    }
}

}
