#pragma once

#include "ytres/http.h"

#include <cstddef>

// The HttpClient a Resolver builds when it is given none: libcurl.
// =======================================================
// Rules:
// - send() makes its own easy and multi handles and shares nothing with
//   other calls, so any number of threads can send at once. CURLOPT_NOSIGNAL
//   keeps libcurl off SIGALRM, which it would otherwise use for DNS timeouts
//   and which is not thread-safe on the platforms that have it.
// - curl_global_init runs once per process, from the first send(), and is
//   never undone: curl_global_cleanup is unsafe while any other thread might
//   still be using libcurl, and the process exit reclaims it anyway.
// - Certificate verification stays on. libcurl verifies peer and host by
//   default, and on Windows its Schannel backend checks against the system
//   certificate store, so there is no CA bundle to ship.
// - A cancel lands within about POLL_MS: 20 to 27 ms, measured on Windows.
//   send() drives its one transfer through a multi handle, waiting at most
//   POLL_MS at a time and checking cancelled in between, and the progress
//   callback checks it while data flows. The callback alone would not do:
//   libcurl calls it only about once a second while it waits for DNS, a
//   connection or the first byte.
// - CURLOPT_QUICK_EXIT lets a cancel or a timeout during a threaded DNS
//   lookup return at once: libcurl detaches the lookup thread instead of
//   waiting for it. That thread runs on until getaddrinfo returns - seconds,
//   on a resolver that hangs - and then cleans up after itself. A few such
//   threads at a time are acceptable for the bot.
// - Redirects are not followed: nothing the resolver asks for redirects, and
//   a 3xx comes back as it is.
// - A cancel check that throws is the caller's bug: the transfer stops as
//   for a cancel, and send() reports Internal, not Cancelled.
// - A Status message is fixed English: curl_easy_strerror's,
//   curl_multi_strerror's, one of ours, or "HTTP <n>". Never the error
//   buffer: Schannel fills that through FormatMessage in the local code
//   page. The start of an error page is left in value for the caller to
//   log.
// - send() asks for gzip or deflate, as yt-dlp's HTTP layer does: the watch
//   page shrinks from 1.3 MB to about 150 KB on the wire. MAX_BODY_BYTES
//   bounds what it keeps once decoded.
// =======================================================
namespace ytres {

class CurlHttpClient : public HttpClient
{
public:
    Result<HttpResponse> send(const HttpRequest &request) override;

    // Far past anything the resolver asks for: the watch page is 1.3 MB, a
    // player response 90 KB. More than this is no answer worth reading.
    static const size_t MAX_BODY_BYTES = 8 * 1024 * 1024;

    // The longest send() waits between two looks at the cancel check. The
    // bot polls its own stop flag every 50 ms.
    static const int POLL_MS = 20;
};

}
