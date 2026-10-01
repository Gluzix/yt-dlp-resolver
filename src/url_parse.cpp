#include "url_parse.h"

#include "ascii.h"

#include <cstddef>

namespace ytres {

namespace {

const std::string_view VIDEO_PATH_PREFIXES[] = {"/shorts/", "/live/", "/embed/", "/v/"};

// The playlists anyone can open, from yt-dlp's _PLAYLIST_ID_RE: a playlist
// (PL), a channel's uploads (UU), an album (OLAK5uy_) and their kin, each
// followed by MIN_PLAYLIST_ID_TAIL or more id characters.
const std::string_view PUBLIC_PLAYLIST_PREFIXES[] = {"PL", "UU", "FL", "OLAK5uy_", "EC", "UL", "PU"};
const std::size_t MIN_PLAYLIST_ID_TAIL = 10;

// The rest of _PLAYLIST_ID_RE: lists YouTube makes for one viewer. A mix
// (RD..., RDMM among them) is generated per viewer, TL... is a viewer's
// queue, and WL, LL... and LM are a signed-in viewer's Watch Later, Liked
// videos and liked music. Nobody else can list them.
const std::string_view PERSONAL_PLAYLIST_PREFIXES[] = {"RD", "TL", "LL"};
const std::string_view PERSONAL_PLAYLIST_IDS[] = {"WL", "LM"};

bool isIdChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

// One or more characters, all from [A-Za-z0-9_-].
bool isIdText(std::string_view text)
{
    if (text.empty()) {
        return false;
    }
    for (char c : text) {
        if (!isIdChar(c)) {
            return false;
        }
    }
    return true;
}

bool startsWith(std::string_view text, std::string_view prefix)
{
    return text.substr(0, prefix.size()) == prefix;
}

bool endsWith(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// Lower-case letters, digits, dots and dashes, as a lowered host name has.
bool isPlainHost(std::string_view host)
{
    for (char c : host) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view text)
{
    const std::string_view spaces = " \t\r\n";
    const size_t first = text.find_first_not_of(spaces);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(spaces) - first + 1);
}

// The path segment right after prefix, up to the next '/', '?' or '#'.
std::string_view segmentAfter(std::string_view path, std::string_view prefix)
{
    const std::string_view rest = path.substr(prefix.size());
    return rest.substr(0, rest.find_first_of("/?#"));
}

std::string_view idInYoutubePath(std::string_view path)
{
    if (path.substr(0, path.find_first_of("?#")) == "/watch") {
        return queryValue(path, "v").value_or(std::string_view{});
    }
    for (std::string_view prefix : VIDEO_PATH_PREFIXES) {
        if (startsWith(path, prefix)) {
            return segmentAfter(path, prefix);
        }
    }
    return {};
}

// A link as parseVideoId() and parsePlaylistId() both read it: the host,
// lowered, and what follows it.
struct Link
{
    std::string host;
    std::string_view path; // from the first '/', '?' or '#' on; empty when there is none
};

// The scheme may be https, http or left out.
Link splitLink(std::string_view input)
{
    std::string_view rest = input;
    const std::string lowerInput = asciiLower(input);
    if (startsWith(lowerInput, "https://")) {
        rest.remove_prefix(8);
    } else if (startsWith(lowerInput, "http://")) {
        rest.remove_prefix(7);
    }
    // The host ends where the path, query or fragment starts.
    const size_t hostEnd = rest.find_first_of("/?#");
    Link link;
    link.host = asciiLower(rest.substr(0, hostEnd));
    link.path = hostEnd == std::string_view::npos ? std::string_view{} : rest.substr(hostEnd);
    return link;
}

// youtube.com or one of its subdomains. A host carrying a user@ or a :port
// is no plain YouTube link, however it ends.
bool isYoutubeHost(const std::string &host)
{
    return isPlainHost(host) && (host == "youtube.com" || endsWith(host, ".youtube.com"));
}

bool isPublicPlaylistId(std::string_view id)
{
    if (!isIdText(id)) {
        return false;
    }
    for (std::string_view prefix : PUBLIC_PLAYLIST_PREFIXES) {
        if (startsWith(id, prefix) && id.size() >= prefix.size() + MIN_PLAYLIST_ID_TAIL) {
            return true;
        }
    }
    return false;
}

bool isPersonalPlaylistId(std::string_view id)
{
    if (!isIdText(id)) {
        return false;
    }
    for (std::string_view personal : PERSONAL_PLAYLIST_IDS) {
        if (id == personal) {
            return true;
        }
    }
    for (std::string_view prefix : PERSONAL_PLAYLIST_PREFIXES) {
        if (startsWith(id, prefix)) {
            return true;
        }
    }
    return false;
}

}

bool isVideoId(std::string_view text)
{
    return text.size() == 11 && isIdText(text);
}

Result<std::string> parseVideoId(std::string_view urlOrId)
{
    const std::string_view input = trim(urlOrId);
    if (isVideoId(input)) {
        return {{}, std::string(input)};
    }

    const Link link = splitLink(input);
    std::string_view id;
    if (link.host == "youtu.be") {
        id = startsWith(link.path, "/") ? segmentAfter(link.path, "/") : std::string_view{};
    } else if (isYoutubeHost(link.host)) {
        id = idInYoutubePath(link.path);
    }
    if (!isVideoId(id)) {
        return {{Error::BadInput, "Not a YouTube video link or id"}, {}};
    }
    return {{}, std::string(id)};
}

Result<std::string> parsePlaylistId(std::string_view urlOrId)
{
    const std::string_view input = trim(urlOrId);

    // A bare id is all id characters; a link never is, having a '.' or a '/'.
    std::string_view id;
    if (isIdText(input)) {
        id = input;
    } else {
        const Link link = splitLink(input);
        const std::string_view page = link.path.substr(0, link.path.find_first_of("?#"));
        if (link.host == "youtu.be" || (isYoutubeHost(link.host) && (page == "/playlist" || page == "/watch"))) {
            id = queryValue(link.path, "list").value_or(std::string_view{});
        }
    }
    if (isPublicPlaylistId(id)) {
        return {{}, std::string(id)};
    }
    if (isPersonalPlaylistId(id)) {
        return {{Error::BadInput, "A mix, Watch Later, Liked videos or another personal list needs a signed-in viewer; "
                                  "only public playlists can be listed"},
                {}};
    }
    return {{Error::BadInput, "Not a YouTube playlist link or id"}, {}};
}

std::string canonicalWatchUrl(std::string_view videoId)
{
    return "https://www.youtube.com/watch?v=" + std::string(videoId);
}

std::string watchUrl(std::string_view videoId)
{
    return canonicalWatchUrl(videoId);
}

std::optional<std::string_view> queryValue(std::string_view url, std::string_view name)
{
    const size_t question = url.find('?');
    if (question == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view query = url.substr(question + 1);
    query = query.substr(0, query.find('#'));
    for (;;) {
        const size_t ampersand = query.find('&');
        const std::string_view pair = query.substr(0, ampersand);
        const size_t equals = pair.find('=');
        if (pair.substr(0, equals) == name) {
            return equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);
        }
        if (ampersand == std::string_view::npos) {
            return std::nullopt;
        }
        query.remove_prefix(ampersand + 1);
    }
}

}
