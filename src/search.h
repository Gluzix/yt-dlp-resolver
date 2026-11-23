#pragma once

#include "continuation.h"
#include "innertube.h"
#include "ytres/http.h"
#include "ytres/ytres.h"

#include <string>
#include <vector>

// A search: the request that asks YouTube for the videos matching a query,
// and the reader of each page of the answer. docs/innertube-notes.md,
// "Search", is the specification, read from yt-dlp's _search_results and
// probed live.
// =======================================================
// Rules:
// - searchRequest() sends what yt-dlp sends: the client's context, the
//   query and the videos-only filter, and for a further page the same again
//   with the continuation added.
// - A page's visitor data is read under the same rule as the watch page's
//   before it can become the next page's X-Goog-Visitor-Id.
// - Only a videoRenderer is a result. The videos-only filter still lets a
//   channel through for an artist's name, and an empty search holds a
//   promo; both are read past, as are shelves and whatever YouTube adds.
//   A result without a valid video id is dropped rather than handed on to
//   fail in resolve().
// - A first page with no video it can read, while YouTube's
//   estimatedResults says there are some, is Parse, not an empty list: the
//   results have moved into a renderer the library does not know, and
//   "nothing found" would keep the caller from falling back to another
//   resolver. Two such pages are not: one whose items are all a channel or
//   the empty search's promo, which the reader knows and skips, and one
//   that offers a continuation, which the Resolver follows once before it
//   decides (SearchPage::counted). A further page with nothing readable is
//   only the end.
// - parseSearchResponse() is pure, like every reader of YouTube's answers.
// =======================================================
namespace ytres::innertube {

// POST /youtubei/v1/search for query, as yt-dlp's _search_results sends it,
// through apiRequest(). With a continuation token, the request for the page
// after the one the continuation came from.
HttpRequest searchRequest(const ClientDef &client, const std::string &query, const std::string &language,
                          const std::string &visitorData, const Continuation &continuation = {});

// The Parse message of a first page that counts results but holds none the
// reader can read, from parseSearchResponse() or, after the page it leads
// to, from the Resolver.
inline const char *const UNREADABLE_SEARCH = "The search response counts results but holds no video the library can read";

// Which answer a body is: the first page's, to a request with no
// continuation, or a further page's, to one with a token.
enum class SearchPageKind { First, Further };

struct SearchPage
{
    std::vector<SearchResult> results;  // in YouTube's order, best match first
    Continuation next;                  // no token on the last page
    // responseContext.visitorData, which yt-dlp sends with the next page;
    // empty when the page has none, or none that may go out as a header
    // (visitor_data.h).
    std::string visitorData;
    // A first page only: YouTube counts results, yet there is no video
    // here, and the page leads on. The Resolver follows it once rather than
    // stop at a page that brought nothing.
    bool counted{false};
    // ... and what is here is not only items the reader knows and skips, so
    // the results may have moved into a renderer it does not know: when the
    // next page brings no video either, the search is Parse.
    bool unreadable{false};
};

// Reads one answer of /youtubei/v1/search, a first page or a continuation,
// as kind says the request was: the item sections of
//   contents.twoColumnSearchResultsRenderer.primaryContents.sectionListRenderer.contents[]
// on a first page, of
//   onResponseReceivedCommands[].appendContinuationItemsAction.continuationItems[]
// on a further one, and the continuation beside them - or, when none is
// there, the first one inside an item section's contents[], where yt-dlp
// looks too. Only the container kind expects is read: an answer that holds
// both is read as what was asked for. Pure. Not JSON, JSON without the
// container kind expects, or a first page whose estimatedResults is above 0
// but which holds no readable video, nothing but known items to skip, nor
// a continuation, is Parse; any other page with no videos in it is not.
Result<SearchPage> parseSearchResponse(const std::string &body, SearchPageKind kind);

}
