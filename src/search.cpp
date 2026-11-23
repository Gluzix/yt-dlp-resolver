#include "search.h"

#include "json_read.h"
#include "url_parse.h"
#include "visitor_data.h"

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
using jsonread::readString;
using jsonread::textOf;

// yt-dlp's _SEARCH_PARAMS, which it comments "Videos only": a base64
// protobuf filter. The older EgIQAQ== that circulates is not what it sends.
const char *const VIDEOS_ONLY = "EgIQAfABAQ==";

// The array that holds a page's item sections and its continuation, in the
// container the request expects: on a further page, the first
// appendContinuationItemsAction among onResponseReceivedCommands; on a first
// page, the section list. Null when the answer lacks it. Which request was
// sent decides, not which container is there: a first page that also
// carried an append action would otherwise lose its own videos.
const json *pageItems(const json &root, SearchPageKind kind)
{
    if (kind == SearchPageKind::Further) {
        if (const json *commands = childArray(root, "onResponseReceivedCommands")) {
            for (const json &command : *commands) {
                const json *action = child(command, "appendContinuationItemsAction");
                const json *items = action ? childArray(*action, "continuationItems") : nullptr;
                if (items) {
                    return items;
                }
            }
        }
        return nullptr;
    }
    const json *at = &root;
    for (const char *key : {"contents", "twoColumnSearchResultsRenderer", "primaryContents", "sectionListRenderer"}) {
        at = child(*at, key);
        if (!at) {
            return nullptr;
        }
    }
    return childArray(*at, "contents");
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

// The first time status over the thumbnail; null when there is none.
const json *timeStatusOf(const json &renderer)
{
    if (const json *overlays = childArray(renderer, "thumbnailOverlays")) {
        for (const json &overlay : *overlays) {
            if (const json *timeStatus = child(overlay, "thumbnailOverlayTimeStatusRenderer")) {
                return timeStatus;
            }
        }
    }
    return nullptr;
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
    // Without lengthText, yt-dlp reads the length off the time status over the
    // thumbnail. A live or an upcoming stream has neither, or "LIVE": 0.
    std::string length = textOf(renderer, "lengthText");
    if (length.empty()) {
        const json *timeStatus = timeStatusOf(renderer);
        length = timeStatus ? textOf(*timeStatus, "text") : std::string{};
    }
    result.durationSeconds = durationSeconds(length);
    result.isLive = isLiveNow(renderer);
    result.isUpcoming = child(renderer, "upcomingEventData") != nullptr;
    return result;
}

// The items a first page may hold with no video and still be read right:
// a channel, which the videos-only filter lets through for an artist's
// name, and the promo of a search that found nothing. Not a correction card
// or a shelf: those come beside videos, and alone they may mean the videos
// have moved somewhere the reader does not look.
const char *const KNOWN_SKIPS[] = {"channelRenderer", "backgroundPromoRenderer"};

// What a page held besides its videos, for the first page's alarm.
struct Skipped
{
    bool known{false};   // a channel or the empty search's promo
    bool unknown{false}; // anything else that is not a continuation
};

void noteSkipped(const json &item, Skipped &skipped)
{
    for (const char *key : KNOWN_SKIPS) {
        if (child(item, key)) {
            skipped.known = true;
            return;
        }
    }
    skipped.unknown = true;
}

// The videos of one item section, in its order, and into inner, unless it
// already has one, the first continuation among them.
void appendVideos(const json &section, std::vector<SearchResult> &results, Continuation &inner, Skipped &skipped)
{
    const json *contents = childArray(section, "contents");
    if (!contents) {
        return;
    }
    for (const json &item : *contents) {
        const json *renderer = child(item, "videoRenderer");
        if (!renderer) {
            const Continuation continuation = continuationOf(item);
            if (continuation.token.empty()) {
                noteSkipped(item, skipped); // a channel, a shelf, the empty search's promo
            } else if (inner.token.empty()) {
                inner = continuation;
            }
            continue;
        }
        SearchResult result = readVideo(*renderer);
        if (isVideoId(result.videoId)) {
            results.push_back(std::move(result));
        }
    }
}

// Whether YouTube's estimatedResults says there is anything at all. It
// comes as a digit string ("584471", "0"), but the alarm below must not go
// quiet should it come as "1,234", a float or a number past int64: any
// number but zero, or any string with a digit other than 0, counts.
bool countsResults(const json &root)
{
    const auto it = root.find("estimatedResults");
    if (it == root.end()) {
        return false;
    }
    if (it->is_number()) {
        return it->get<double>() != 0.0;
    }
    if (it->is_string()) {
        const std::string &text = it->get_ref<const std::string &>();
        return text.find_first_of("123456789") != std::string::npos;
    }
    return false;
}

Result<SearchPage> readSearchResponse(const json &root, SearchPageKind kind)
{
    const json *items = pageItems(root, kind);
    if (!items) {
        return {{Error::Parse, "The search response holds no results section"}, {}};
    }
    // Item sections hold the results, and the page's continuation sits beside
    // them. yt-dlp also takes one from inside an item section, so that one
    // serves when there is none beside.
    SearchPage page;
    Continuation inside;
    Skipped skipped;
    for (const json &item : *items) {
        if (const json *section = child(item, "itemSectionRenderer")) {
            appendVideos(*section, page.results, inside, skipped);
            continue;
        }
        const Continuation continuation = continuationOf(item);
        if (continuation.token.empty()) {
            skipped.unknown = true; // a section of a kind the reader does not know
        } else if (page.next.token.empty()) {
            page.next = continuation;
        }
    }
    if (page.next.token.empty()) {
        page.next = std::move(inside);
    }
    // The visitor YouTube took the caller for, which yt-dlp sends with the
    // next page; only when it may travel as a header.
    const json *responseContext = child(root, "responseContext");
    std::string visitorData = responseContext ? readString(*responseContext, "visitorData") : std::string{};
    if (visitorDataRefusal(visitorData).empty()) {
        page.visitorData = std::move(visitorData);
    }
    // YouTube counts results, yet none is a video the library can read: the
    // results may have moved into a renderer it does not know. Saying
    // "nothing found" would keep every caller from falling back to another
    // resolver. Only a first page says so; a further page with nothing in it
    // is the end. A query that matches a channel and no video holds nothing
    // but the channel, which is no alarm; and a page that leads on gets its
    // next page read before the alarm sounds (see SearchPage::unreadable).
    if (kind == SearchPageKind::First && page.results.empty() && countsResults(root)) {
        page.counted = true;
        page.unreadable = skipped.unknown || !skipped.known;
        if (page.unreadable && page.next.token.empty()) {
            return {{Error::Parse, UNREADABLE_SEARCH}, {}};
        }
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

Result<SearchPage> parseSearchResponse(const std::string &body, SearchPageKind kind)
{
    const json root = json::parse(body, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return {{Error::Parse, "The search response is not a JSON object"}, {}};
    }
    // The reads check types as they go; this is the net under them.
    try {
        return readSearchResponse(root, kind);
    } catch (const json::exception &e) {
        return {{Error::Parse, std::string("The search response has an unexpected shape: ") + e.what()}, {}};
    }
}

}
