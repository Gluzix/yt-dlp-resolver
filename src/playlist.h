#pragma once

#include "continuation.h"
#include "innertube.h"
#include "ytres/http.h"
#include "ytres/ytres.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// A playlist: the request that asks YouTube for one page of a public
// playlist, and the reader of each page of the answer.
// docs/innertube-notes.md, "Playlists", is the specification, read from
// yt-dlp's _tab.py and probed live.
// =======================================================
// Rules:
// - playlistRequest() sends, for the first page, the request yt-dlp's
//   _reload_with_unavailable_videos sends, without its params: the
//   client's context and browseId VL<playlistId>. yt-dlp itself starts from
//   the playlist's web page; the library fetches none. Its params,
//   wgYCCAA=, only have the unavailable videos listed too, which the
//   library has no use for. Every page after the first is the context and
//   the continuation alone, with no browseId, as yt-dlp's _entries asks.
// - YouTube serves two layouts and the reader takes both: lockupViewModel
//   entries, in which every playlist probed on 2026-10-01 came, and the
//   playlistVideoRenderer entries of a playlistVideoListRenderer, which
//   yt-dlp still reads.
// - The continuation to follow is the one among the entries, in the same
//   array. A first page holds a second one beside its item section, which
//   loads something else and is never followed: the opposite of a search,
//   whose continuation sits beside its item sections.
// - A further page is read from its appendContinuationItemsAction alone:
//   its contents key is a stub, and the title and the count it repeats
//   belong to the first page.
// - Only a video with a valid id and a playable title is an entry: what is
//   not a video, says it cannot be played, or is titled [Private video] or
//   [Deleted video] is read past, as the bot filters today.
// - A first page with no entry it can read, while the playlist counts
//   videos, is Parse, not an empty playlist: the entries have moved into a
//   renderer the library does not know, and "empty" would keep the caller
//   from falling back to another resolver. A further page with nothing
//   readable is only the end.
// - A page's visitor data is read under the same rule as the watch page's
//   before it can become the next page's X-Goog-Visitor-Id.
// - parsePlaylistResponse() is pure, like every reader of YouTube's answers.
// =======================================================
namespace ytres::innertube {

// POST /youtubei/v1/browse for the first page of playlistId, through
// apiRequest(). With a continuation token, the request for the page after
// the one the continuation came from instead, which names no playlist:
// the token alone says which.
HttpRequest playlistRequest(const ClientDef &client, const std::string &playlistId, const std::string &language,
                            const std::string &visitorData, const Continuation &continuation = {});

struct PlaylistPage
{
    std::string title;                   // first page only; empty when YouTube gave none
    std::size_t totalCount{0};           // first page only: the videos YouTube says it has; 0 = unknown
    std::vector<PlaylistEntry> entries;  // in playlist order
    Continuation next;                   // no token on the last page
    // responseContext.visitorData, which yt-dlp sends with the next page;
    // empty when the page has none, or none that may go out as a header
    // (visitor_data.h).
    std::string visitorData;
};

// Reads one answer of /youtubei/v1/browse for a playlist, a first page or a
// continuation: on a first page, the entries in the first item section of
//   contents.twoColumnBrowseResultsRenderer.tabs[].tabRenderer.content.sectionListRenderer.contents[]
// (or in the playlistVideoListRenderer inside it), with the title and the
// count; on a further one, the entries of the first
//   onResponseReceivedActions[].appendContinuationItemsAction.continuationItems[]
// (or the same under onResponseReceivedEndpoints); and the continuation
// among the entries. Pure. A page with no section list that carries an
// ERROR alert - a playlist that does not exist - is Unavailable with the
// alert's text. Not JSON, JSON with none of the above, or a first page that
// counts videos but holds none the library can read, is Parse.
Result<PlaylistPage> parsePlaylistResponse(const std::string &body);

// The count at the start of a playlist's stat, as YouTube writes it with
// hl=en: "447 episodes" is 447, "6,000 videos" 6000, "No videos" 0. The
// separators between the digits are dropped. Text that starts with no
// digit, or has more than nine of them, is 0: unknown.
std::size_t videoCount(std::string_view text);

}
