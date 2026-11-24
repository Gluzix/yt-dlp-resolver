"""Videos: every line of tests/corpus.txt through ytres_cli, and the ones that play against yt-dlp -f bestaudio.

A failure message shows both sides, redacted, so that "YouTube changed" can
be told from "we broke something" at a glance.
"""

from __future__ import annotations

import time
from typing import TYPE_CHECKING, Callable

import pytest

import harness

if TYPE_CHECKING:
    from conftest import Row

pytestmark = pytest.mark.live

CASES = harness.read_video_corpus()
PLAYING = [case for case in CASES if case.expected == 'Ok']


def said(answer: harness.Answer[harness.Video]) -> str:
    """What ytres_cli said, for the table: the itag it picked, or its Error."""
    if answer.ok and answer.value is not None:
        return f'itag {answer.value.itag}'
    return answer.code


@pytest.mark.parametrize('case', CASES, ids=[case.video_id for case in CASES])
def test_ytres_answers_as_the_corpus_expects(case: harness.VideoCase, ytres: harness.Ytres,
                                             record: Callable[[str, str], Row]) -> None:
    row = record('video', case.video_id)
    answer = ytres.resolve(case.video_id)
    row.ytres(said(answer), answer.seconds)
    assert answer.code == case.expected, (
        f'{case.purpose}: ytres said {answer.failure or "Ok"}, the corpus expects {case.expected}')
    if case.expected != 'Ok':
        # A refusal is "<Error>: <message>" as the last line of stderr, and nothing on stdout.
        assert answer.completed.exit_code == 1
        assert answer.completed.stdout == ''
        assert harness.lines_of(answer.completed.stderr)[-1] == f'{answer.failure.code}: {answer.failure.message}'
        return
    video = answer.value
    assert video is not None
    assert video.title.strip(), 'ytres printed an empty title'
    assert video.page_url == f'https://www.youtube.com/watch?v={case.video_id}'
    # Checked as booleans, so that pytest has no stream url to show.
    on_media_host = harness.host_of(video.stream_url).endswith('.googlevideo.com')
    assert on_media_host, f'the stream url is not on YouTube\'s media host: {harness.redact(video.stream_url)}'
    assert video.itag and video.itag.isdigit(), f'the stream url has no itag: {harness.redact(video.stream_url)}'
    expire = harness.expire_of(video.stream_url)
    assert expire is not None and expire > time.time(), f'the stream url expires {expire}, not in the future'


@pytest.mark.parametrize('case', PLAYING, ids=[case.video_id for case in PLAYING])
def test_ytres_picks_what_ytdlp_picks(case: harness.VideoCase, ytres: harness.Ytres, ytdlp: harness.YtDlp,
                                      record: Callable[[str, str], Row]) -> None:
    row = record('video', case.video_id)
    ours = ytres.resolve(case.video_id)
    theirs = ytdlp.best_audio(case.video_id)
    row.ytres(said(ours), ours.seconds)
    row.ytdlp(f'format {theirs.value.format_id}' if theirs.value else str(theirs.failure), theirs.seconds)
    assert ours.ok and ours.value is not None, f'{case.purpose}: ytres failed: {ours.failure}'
    assert theirs.ok and theirs.value is not None, f'{case.purpose}: yt-dlp failed: {theirs.failure}'
    differing = [f'{name}: ytres {mine!r}, yt-dlp {other!r}'
                 for name, mine, other in (('title', ours.value.title, theirs.value.title),
                                           ('page url', ours.value.page_url, theirs.value.page_url),
                                           ('itag', ours.value.itag, theirs.value.itag))
                 if mine != other]
    assert not differing, harness.redact(f'{case.purpose}: ' + '; '.join(differing))
