#pragma once

#include "ytres/ytres.h"

#include <optional>
#include <string>
#include <string_view>

// Reads video ids, playlist ids and query parameters out of YouTube links.
// Pure string work: no I/O, so every form is checked offline.
namespace ytres {

// Exactly 11 characters from [A-Za-z0-9_-].
bool isVideoId(std::string_view text);

// The id in a /watch?v=, youtu.be/, /shorts/, /live/, /embed/ or /v/ link on
// youtube.com or any of its subdomains, or a bare id. The scheme may be
// https, http or left out. Anything else is BadInput.
Result<std::string> parseVideoId(std::string_view urlOrId);

// The playlist a link names: the list= of a /playlist or /watch link on
// youtube.com or any of its subdomains, of a youtu.be link, or a bare id.
// The scheme may be https, http or left out, as for parseVideoId().
// Only playlists anyone can open: ids starting PL, UU, FL, OLAK5uy_, EC, UL
// or PU followed by ten or more of [A-Za-z0-9_-]. A mix (RD...), WL, LL, LM
// and TL... are BadInput with a message that says they need a signed-in
// viewer; anything else is BadInput.
Result<std::string> parsePlaylistId(std::string_view urlOrId);

// https://www.youtube.com/watch?v=<id>, the only page url the library reports.
// The public watchUrl() in ytres.h hands out the same.
std::string canonicalWatchUrl(std::string_view videoId);

// The value of one query parameter, still percent-encoded; nullopt when the
// url has no such parameter.
std::optional<std::string_view> queryValue(std::string_view url, std::string_view name);

}
