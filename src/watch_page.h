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

struct VisitorData
{
    std::string value;   // empty when the page has none fit to send
    std::string refused; // why a value the page did carry was not used; empty if none was refused
};

// INNERTUBE_CONTEXT.client.visitorData out of the first ytcfg.set({...}) in
// the page that has a value fit to send. It goes out as a header, so it must
// be 1 to 4096 characters of [A-Za-z0-9%_=+/-]. A value that is not gets
// refused, and refused says why: were YouTube's token to outgrow the limit,
// the log would say so instead of every resolve quietly meeting the bot
// check again. Pure, like everything that reads YouTube's answers.
VisitorData visitorData(std::string_view html);

}
