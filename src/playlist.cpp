#include "playlist.h"

#include "json_read.h"
#include "url_parse.h"
#include "visitor_data.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytres::innertube {

namespace {

using nlohmann::json;
using nlohmann::ordered_json;

using jsonread::child;
using jsonread::childArray;
using jsonread::durationSeconds;
using jsonread::labelText;
using jsonread::readInt;
using jsonread::readString;
using jsonread::textOf;

// A playlist's browse id is its id behind VL, as yt-dlp's
// _reload_with_unavailable_videos builds it.
const char *const PLAYLIST_BROWSE_PREFIX = "VL";

// The titles YouTube puts in place of a video nobody may watch, which the bot
// filters today along with an empty title. With the default request YouTube
// leaves such videos out by itself; this is for the day it does not.
const char *const UNPLAYABLE_TITLES[] = {"[Private video]", "[Deleted video]"};

bool isPlayableTitle(const std::string &title)
{
    if (title.empty()) {
        return false;
    }
    for (const char *unplayable : UNPLAYABLE_TITLES) {
        if (title == unplayable) {
            return false;
        }
    }
    return true;
}

// A key that is there and false. An old-layout entry is dropped only when
// YouTube says outright that it cannot be played; one that says nothing is
// kept.
bool isFalse(const json &object, const char *key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && !it->get<bool>();
}

// The first badge text over a lockup's thumbnail - "2:31:48", or "LIVE" and
// the like - in either overlay yt-dlp reads it from. Empty when there is none.
std::string badgeText(const json &lockup)
{
    const json *image = child(lockup, "contentImage");
    const json *thumbnail = image ? child(*image, "thumbnailViewModel") : nullptr;
    const json *overlays = thumbnail ? childArray(*thumbnail, "overlays") : nullptr;
    if (!overlays) {
        return {};
    }
    const std::pair<const char *, const char *> badgeHolders[] = {
        {"thumbnailBottomOverlayViewModel", "badges"},
        {"thumbnailOverlayBadgeViewModel", "thumbnailBadges"},
    };
    for (const json &overlay : *overlays) {
        for (const auto &[holderKey, badgesKey] : badgeHolders) {
            const json *holder = child(overlay, holderKey);
            const json *badges = holder ? childArray(*holder, badgesKey) : nullptr;
            if (!badges) {
                continue;
            }
            for (const json &badge : *badges) {
                const json *viewModel = child(badge, "thumbnailBadgeViewModel");
                std::string text = viewModel ? readString(*viewModel, "text") : std::string{};
                if (!text.empty()) {
                    return text;
                }
            }
        }
    }
    return {};
}

// A lockupViewModel, the new layout's entry; nothing unless it is a video.
std::optional<PlaylistEntry> fromLockup(const json &lockup)
{
    if (readString(lockup, "contentType") != "LOCKUP_CONTENT_TYPE_VIDEO") {
        return std::nullopt; // a playlist, a channel, whatever YouTube adds
    }
    PlaylistEntry entry;
    entry.videoId = readString(lockup, "contentId");
    const json *metadata = child(lockup, "metadata");
    const json *lockupMetadata = metadata ? child(*metadata, "lockupMetadataViewModel") : nullptr;
    entry.title = lockupMetadata ? textOf(*lockupMetadata, "title") : std::string{};
    entry.durationSeconds = durationSeconds(badgeText(lockup));
    return entry;
}

// A playlistVideoRenderer, the old layout's entry; nothing when YouTube says
// it cannot be played.
std::optional<PlaylistEntry> fromRenderer(const json &renderer)
{
    if (isFalse(renderer, "isPlayable")) {
        return std::nullopt;
    }
    PlaylistEntry entry;
    entry.videoId = readString(renderer, "videoId");
    entry.title = textOf(renderer, "title");
    entry.durationSeconds = readInt(renderer, "lengthSeconds");
    if (entry.durationSeconds <= 0) {
        entry.durationSeconds = durationSeconds(textOf(renderer, "lengthText"));
    }
    return entry;
}

// The entries of one array of a page, in its order, and into next, unless it
// already has one, the first continuation among them: the one that loads the
// entries after these.
void appendEntries(const json &items, std::vector<PlaylistEntry> &entries, Continuation &next)
{
    for (const json &item : items) {
        std::optional<PlaylistEntry> entry;
        if (const json *lockup = child(item, "lockupViewModel")) {
            entry = fromLockup(*lockup);
        } else if (const json *renderer = child(item, "playlistVideoRenderer")) {
            entry = fromRenderer(*renderer);
        } else {
            if (next.token.empty()) {
                next = continuationOf(item);
            }
            continue;
        }
        // An entry without a valid id is dropped rather than handed on to
        // fail in resolve().
        if (entry && isVideoId(entry->videoId) && isPlayableTitle(entry->title)) {
            entries.push_back(std::move(*entry));
        }
    }
}

// A further page's items: the first appendContinuationItemsAction among
// onResponseReceivedActions, or among onResponseReceivedEndpoints, where
// yt-dlp looks too. Null on a first page, which has neither.
const json *appendedItems(const json &root)
{
    for (const char *key : {"onResponseReceivedActions", "onResponseReceivedEndpoints"}) {
        const json *actions = childArray(root, key);
        if (!actions) {
            continue;
        }
        for (const json &action : *actions) {
            const json *append = child(action, "appendContinuationItemsAction");
            const json *items = append ? childArray(*append, "continuationItems") : nullptr;
            if (items) {
                return items;
            }
        }
    }
    return nullptr;
}

// A first page's section list: the contents[] of the sectionListRenderer of
// the first tab that has one. There is one tab, and a further page's stub
// tab has none. Null when the page has no section list.
const json *sectionListOf(const json &root)
{
    const json *contents = child(root, "contents");
    const json *browse = contents ? child(*contents, "twoColumnBrowseResultsRenderer") : nullptr;
    const json *tabs = browse ? childArray(*browse, "tabs") : nullptr;
    if (!tabs) {
        return nullptr;
    }
    for (const json &tab : *tabs) {
        const json *at = &tab;
        for (const char *key : {"tabRenderer", "content", "sectionListRenderer"}) {
            at = child(*at, key);
            if (!at) {
                break;
            }
        }
        const json *sections = at ? childArray(*at, "contents") : nullptr;
        if (sections) {
            return sections;
        }
    }
    return nullptr;
}

// The array a first page's entries are in: the contents[] of the first item
// section of the section list, or, in the old layout, the contents[] of the
// playlistVideoListRenderer inside it. Null when there is no item section.
const json *entriesIn(const json &sections)
{
    for (const json &section : sections) {
        const json *itemSection = child(section, "itemSectionRenderer");
        const json *items = itemSection ? childArray(*itemSection, "contents") : nullptr;
        if (!items) {
            continue; // the continuation beside the item section, among others
        }
        for (const json &item : *items) {
            const json *list = child(item, "playlistVideoListRenderer");
            const json *listItems = list ? childArray(*list, "contents") : nullptr;
            if (listItems) {
                return listItems;
            }
        }
        return items;
    }
    return nullptr;
}

// The playlist's title: the metadata's, else the old header's, else the page
// header's. The first and the last are plain strings, not labels.
std::string titleOf(const json &root)
{
    const json *metadata = child(root, "metadata");
    const json *playlistMetadata = metadata ? child(*metadata, "playlistMetadataRenderer") : nullptr;
    std::string title = playlistMetadata ? readString(*playlistMetadata, "title") : std::string{};
    const json *header = child(root, "header");
    if (title.empty() && header) {
        const json *oldHeader = child(*header, "playlistHeaderRenderer");
        title = oldHeader ? textOf(*oldHeader, "title") : std::string{};
    }
    if (title.empty() && header) {
        const json *pageHeader = child(*header, "pageHeaderRenderer");
        title = pageHeader ? readString(*pageHeader, "pageTitle") : std::string{};
    }
    return title;
}

// How many videos YouTube says the playlist has: the first stat in its
// sidebar's primary info, else the old header's numVideosText, as yt-dlp
// reads them; 0 when neither says.
std::size_t totalCountOf(const json &root)
{
    std::string text;
    const json *sidebar = child(root, "sidebar");
    const json *sidebarRenderer = sidebar ? child(*sidebar, "playlistSidebarRenderer") : nullptr;
    const json *items = sidebarRenderer ? childArray(*sidebarRenderer, "items") : nullptr;
    if (items) {
        for (const json &item : *items) {
            const json *primary = child(item, "playlistSidebarPrimaryInfoRenderer");
            const json *stats = primary ? childArray(*primary, "stats") : nullptr;
            if (stats && !stats->empty()) {
                text = labelText(stats->front());
                break;
            }
        }
    }
    if (text.empty()) {
        const json *header = child(root, "header");
        const json *oldHeader = header ? child(*header, "playlistHeaderRenderer") : nullptr;
        text = oldHeader ? textOf(*oldHeader, "numVideosText") : std::string{};
    }
    return videoCount(text);
}

// The text of the first ERROR alert, which is how YouTube answers for a
// playlist that does not exist; empty when there is none. Like yt-dlp, it
// reads whatever renderer an alert holds - alertRenderer,
// alertWithButtonRenderer or one YouTube names later.
std::string errorAlert(const json &root)
{
    const json *alerts = childArray(root, "alerts");
    if (!alerts) {
        return {};
    }
    for (const json &alert : *alerts) {
        // A range-for over a JSON primitive would visit the value itself.
        if (!alert.is_object()) {
            continue;
        }
        for (const json &renderer : alert) {
            if (renderer.is_object() && readString(renderer, "type") == "ERROR") {
                std::string text = textOf(renderer, "text");
                return text.empty() ? std::string("YouTube says the playlist cannot be shown") : text;
            }
        }
    }
    return {};
}

Result<PlaylistPage> readPlaylistResponse(const json &root)
{
    PlaylistPage page;
    // The visitor YouTube took the caller for, which yt-dlp sends with the
    // next page; only when it may travel as a header.
    const json *responseContext = child(root, "responseContext");
    std::string visitorData = responseContext ? readString(*responseContext, "visitorData") : std::string{};
    if (visitorDataRefusal(visitorData).empty()) {
        page.visitorData = std::move(visitorData);
    }

    // A further page: its appended entries and nothing else. Its contents
    // key is a stub, and the title and the count it repeats are the first
    // page's business.
    if (const json *appended = appendedItems(root)) {
        appendEntries(*appended, page.entries, page.next);
        return {{}, std::move(page)};
    }

    // A playlist that does not exist has no contents and says so in an
    // ERROR alert: YouTube's words, for the caller to pass on. yt-dlp fails
    // any answer with an error alert; the library fails one that has no
    // entries to show for it. Alerts beside real entries - YouTube uses one
    // to say it hides unavailable videos - are no failure.
    const json *sections = sectionListOf(root);
    if (!sections) {
        std::string alert = errorAlert(root);
        if (!alert.empty()) {
            return {{Error::Unavailable, std::move(alert)}, {}};
        }
        return {{Error::Parse, "The playlist response holds no section list"}, {}};
    }
    page.title = titleOf(root);
    page.totalCount = totalCountOf(root);
    // The continuation to follow is among the entries; the one beside the
    // item section is never looked at.
    if (const json *items = entriesIn(*sections)) {
        appendEntries(*items, page.entries, page.next);
    }
    // A section list with nothing in it and an ERROR alert beside it is
    // YouTube's refusal, not an empty playlist, whatever the count says.
    if (page.entries.empty()) {
        std::string alert = errorAlert(root);
        if (!alert.empty()) {
            return {{Error::Unavailable, std::move(alert)}, {}};
        }
    }
    // YouTube counts videos, yet none is one the library can read: the
    // entries have moved into a renderer it does not know. Saying "empty"
    // would keep every caller from falling back to another resolver. Only a
    // first page says so; a further page with nothing in it is the end.
    if (page.entries.empty() && page.totalCount > 0) {
        return {{Error::Parse, "The playlist counts videos but holds none the library can read"}, {}};
    }
    return {{}, std::move(page)};
}

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

Result<PlaylistPage> parsePlaylistResponse(const std::string &body)
{
    const json root = json::parse(body, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return {{Error::Parse, "The playlist response is not a JSON object"}, {}};
    }
    // The reads check types as they go; this is the net under them.
    try {
        return readPlaylistResponse(root);
    } catch (const json::exception &e) {
        return {{Error::Parse, std::string("The playlist response has an unexpected shape: ") + e.what()}, {}};
    }
}

std::size_t videoCount(std::string_view text)
{
    // Nine digits keep the count inside a 32-bit size_t whatever comes.
    const std::size_t MAX_DIGITS = 9;
    std::size_t count = 0;
    std::size_t digits = 0;
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            if (++digits > MAX_DIGITS) {
                return 0;
            }
            count = count * 10 + static_cast<std::size_t>(c - '0');
        } else if (digits == 0 || (c != ',' && c != '.')) {
            break; // the count has ended, or there is none
        }
        // Otherwise a separator between the digits, "6,000": dropped.
    }
    return count;
}

}
