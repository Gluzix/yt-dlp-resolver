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
//   resolver. A further page with nothing readable is only the end.
// - parseSearchResponse() is pure, like every reader of YouTube's answers.
// =======================================================
namespace ytres::innertube {

// POST /youtubei/v1/search for query, as yt-dlp's _search_results sends it,
// through apiRequest(). With a continuation token, the request for the page
// after the one the continuation came from.
HttpRequest searchRequest(const ClientDef &client, const std::string &query, const std::string &language,
                          const std::string &visitorData, const Continuation &continuation = {});

struct SearchPage
{
    std::vector<SearchResult> results;  // in YouTube's order, best match first
    Continuation next;                  // no token on the last page
    // responseContext.visitorData, which yt-dlp sends with the next page;
    // empty when the page has none, or none that may go out as a header
    // (visitor_data.h).
    std::string visitorData;
};

// Reads one answer of /youtubei/v1/search, a first page or a continuation:
// the item sections of
//   contents.twoColumnSearchResultsRenderer.primaryContents.sectionListRenderer.contents[]
// on a first page, of
//   onResponseReceivedCommands[].appendContinuationItemsAction.continuationItems[]
// on a further one, and the continuation beside them - or, when none is
// there, the first one inside an item section's contents[], where yt-dlp
// looks too. Pure. Not JSON, JSON with neither container, or a first page
// whose estimatedResults is above 0 but which holds no readable video, is
// Parse; any other page with no videos in it is not.
Result<SearchPage> parseSearchResponse(const std::string &body);

}
