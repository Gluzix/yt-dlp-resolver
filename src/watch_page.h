#pragma once

#include "innertube.h"
#include "ytres/http.h"

#include <string>
#include <string_view>

// The video's watch page, fetched only for the visitor data in its ytcfg.
// Without visitor data YouTube answers most anonymous player requests with
// "Sign in to confirm you're not a bot" (docs/innertube-notes.md, Verified
// live), so every resolve asks for the page first.
namespace ytres::watchpage {

// GET https://www.youtube.com/watch?v=<id> with the client's user agent and
// no other header, as the notes say.
HttpRequest request(const innertube::ClientDef &client, const std::string &videoId);

// INNERTUBE_CONTEXT.client.visitorData out of the first ytcfg.set({...}) in
// the page that has it; empty when none does. Pure, like everything that
// reads YouTube's answers.
std::string visitorData(std::string_view html);

}
