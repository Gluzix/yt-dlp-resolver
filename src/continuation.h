#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>

// What "the next page" is in an InnerTube answer, and how a request asks for
// it. A search and a playlist both come a page at a time, each page ending
// in a continuation whose token fetches the next. docs/innertube-notes.md,
// "Search and playlists", has the shapes as YouTube sent them.
// =======================================================
// Rules:
// - Pure, like everything that reads YouTube's answers: no I/O, and nothing
//   throws on a surprising shape. What is not a continuation reads as one
//   with an empty token.
// - Which item of a page is the continuation to follow is the caller's
//   business: a search's sits beside its item sections, a playlist's among
//   its entries.
// =======================================================
namespace ytres::innertube {

struct Continuation
{
    std::string token;                // empty: there is no next page
    std::string clickTrackingParams;  // sent back as clickTracking when present
};

// The continuation an item of a contents array carries, if it is one:
//   continuationItemRenderer.continuationEndpoint
//   continuationItemRenderer.button.buttonRenderer.command
//   continuationItemViewModel.continuationCommand.innertubeCommand
// each of which is a command holding continuationCommand.token, or a
// commandExecutorCommand whose commands[] hold one. Empty for anything else.
// clickTrackingParams comes from the command the token was found in, as in
// yt-dlp's _extract_continuation_ep_data.
Continuation continuationOf(const nlohmann::json &item);

// The fields of a continuation request, as yt-dlp's
// _build_api_continuation_query makes them: "continuation", and
// "clickTracking": {"clickTrackingParams": ...} when there are some.
void addContinuation(nlohmann::ordered_json &fields, const Continuation &continuation);

}
