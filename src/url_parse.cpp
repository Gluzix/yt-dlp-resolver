#include "url_parse.h"

#include "ascii.h"

namespace ytres {

namespace {

const std::string_view VIDEO_PATH_PREFIXES[] = {"/shorts/", "/live/", "/embed/", "/v/"};

bool isIdChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
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

}

bool isVideoId(std::string_view text)
{
    if (text.size() != 11) {
        return false;
    }
    for (char c : text) {
        if (!isIdChar(c)) {
            return false;
        }
    }
    return true;
}

Result<std::string> parseVideoId(std::string_view urlOrId)
{
    const std::string_view input = trim(urlOrId);
    if (isVideoId(input)) {
        return {{}, std::string(input)};
    }

    std::string_view rest = input;
    const std::string lowerInput = asciiLower(input);
    if (startsWith(lowerInput, "https://")) {
        rest.remove_prefix(8);
    } else if (startsWith(lowerInput, "http://")) {
        rest.remove_prefix(7);
    }

    // The host ends where the path, query or fragment starts. One carrying
    // a user@ or a :port is no plain YouTube link, however it ends.
    const size_t hostEnd = rest.find_first_of("/?#");
    const std::string host = asciiLower(rest.substr(0, hostEnd));
    const std::string_view path = hostEnd == std::string_view::npos ? std::string_view{} : rest.substr(hostEnd);

    std::string_view id;
    if (host == "youtu.be") {
        id = startsWith(path, "/") ? segmentAfter(path, "/") : std::string_view{};
    } else if (isPlainHost(host) && (host == "youtube.com" || endsWith(host, ".youtube.com"))) {
        id = idInYoutubePath(path);
    }
    if (!isVideoId(id)) {
        return {{Error::BadInput, "Not a YouTube video link or id"}, {}};
    }
    return {{}, std::string(id)};
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
