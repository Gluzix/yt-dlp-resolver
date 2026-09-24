#pragma once

#include "ytres/ytres.h"

#include <optional>
#include <string>
#include <string_view>

// Reads video ids and query parameters out of YouTube links. Pure string
// work: no I/O, so every form is checked offline.
namespace ytres {

// Exactly 11 characters from [A-Za-z0-9_-].
bool isVideoId(std::string_view text);

// The id in a /watch?v=, youtu.be/, /shorts/, /live/, /embed/ or /v/ link on
// youtube.com or any of its subdomains, or a bare id. The scheme may be
// https, http or left out. Anything else is BadInput.
Result<std::string> parseVideoId(std::string_view urlOrId);

// https://www.youtube.com/watch?v=<id>, the only page url the library reports.
std::string canonicalWatchUrl(std::string_view videoId);

// The value of one query parameter, still percent-encoded; nullopt when the
// url has no such parameter.
std::optional<std::string_view> queryValue(std::string_view url, std::string_view name);

}
