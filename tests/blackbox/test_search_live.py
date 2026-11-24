"""Searches: the lines of corpus_search.txt through ytres_cli, and the common ones against yt-dlp ytsearchN:.

The comparison with yt-dlp is tolerant: the two do not send YouTube the
same request context, and YouTube orders results a little differently for
each, and from one hour to the next. See the tolerances below and
corpus_search.txt for the numbers behind them.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import TYPE_CHECKING, Any, Callable, Iterator, List, Optional

import pytest

import harness

if TYPE_CHECKING:
    from conftest import Row

pytestmark = pytest.mark.live

CASES = harness.read_search_corpus()


def of_kind(kind: str) -> List[harness.SearchCase]:
    return [case for case in CASES if case.kind == kind]


def ids_of(cases: List[harness.SearchCase]) -> List[str]:
    return [case.query for case in cases]


# ytres's first result must be among yt-dlp's first FIRST_WITHIN, and at
# least SHARED_AT_LEAST of ytres's first COMPARED among yt-dlp's first
# AGAINST. The plan said within ten and at least five; real data showed that
# far too loose to notice anything short of a total breakdown. Seen on
# 2026-11-24 over three rounds of five candidate queries: ytres's first
# result was yt-dlp's first in 13 of 15 cases (2nd and 5th for "billie
# eilish", which was not kept), and 9 or 10 of its first ten were among
# yt-dlp's twenty every time. Within five and at least seven leave room
# for YouTube's day-to-day shuffle and still fail on a search that has
# drifted to other videos. See corpus_search.txt for the queries.
FIRST_WITHIN = 5
COMPARED = 10
AGAINST = 20
SHARED_AT_LEAST = 7

# The pages of a search: the first holds about twenty videos, so 25 takes two.
ONE_PAGE = 5
TWO_PAGES = 25


def said(answer: harness.Answer[List[harness.SearchRow]]) -> str:
    """What ytres_cli said, for the table: how many rows, and the first."""
    rows = answer.value or []
    text = f'{len(rows)} rows' + (f', first {rows[0].video_id}' if rows else '')
    return text if answer.ok else f'{answer.code}, {text}'


def check_rows(rows: List[harness.SearchRow]) -> None:
    """Every id valid and distinct, every row with a title, a channel and a length."""
    ids = [row.video_id for row in rows]
    assert all(ids), 'a row is not https://www.youtube.com/watch?v=<id>: ' + ', '.join(
        row.url for row in rows if not row.video_id)
    assert len(set(ids)) == len(ids), f'a video comes twice: {ids}'
    for row in rows:
        assert row.title.strip(), f'{row.video_id} has no title'
        assert row.channel.strip(), f'{row.video_id} has no channel'
        assert row.length.isdigit() or row.length in ('live', 'upcoming'), f'{row.video_id}: length {row.length!r}'


@pytest.mark.parametrize('max', [ONE_PAGE, TWO_PAGES])
@pytest.mark.parametrize('case', of_kind('common'), ids=ids_of(of_kind('common')))
def test_a_common_query_fills_max(case: harness.SearchCase, max: int, ytres: harness.Ytres,
                                  record: Callable[[str, str], 'Row']) -> None:
    answer = ytres.search(case.query, max)
    if max == TWO_PAGES:
        record('search', case.query).ytres(said(answer), answer.seconds)
    assert answer.ok, f'{case.purpose}: ytres failed: {answer.failure}'
    rows = answer.value or []
    assert len(rows) == max, f'{case.purpose}: {len(rows)} rows for --max {max}'
    check_rows(rows)


@pytest.mark.parametrize('case', of_kind('common'), ids=ids_of(of_kind('common')))
def test_a_common_query_agrees_with_ytdlp(case: harness.SearchCase, ytres: harness.Ytres, ytdlp: harness.YtDlp,
                                          record: Callable[[str, str], 'Row']) -> None:
    ours = ytres.search(case.query, TWO_PAGES)
    theirs = ytdlp.search_ids(case.query, AGAINST)
    row = record('search', case.query)
    row.ytres(said(ours), ours.seconds)
    row.ytdlp(f'{len(theirs.value)} videos, first {theirs.value[0]}' if theirs.value else str(theirs.failure),
              theirs.seconds)
    assert ours.ok and ours.value, f'ytres failed: {ours.failure}'
    assert theirs.ok and theirs.value, f'yt-dlp failed or found nothing: {theirs.failure}'
    mine = [row.video_id for row in ours.value[:COMPARED]]
    other = theirs.value
    both = f'ytres {mine}, yt-dlp {other}'
    assert mine[0] in other[:FIRST_WITHIN], (
        f'{case.purpose}: ytres\'s first result is not among yt-dlp\'s first {FIRST_WITHIN}: {both}')
    shared = sum(video_id in other[:AGAINST] for video_id in mine)
    assert shared >= SHARED_AT_LEAST, (
        f'{case.purpose}: {shared} of ytres\'s first {COMPARED} among yt-dlp\'s {AGAINST}, '
        f'want {SHARED_AT_LEAST}: {both}')


@pytest.mark.parametrize('case', of_kind('first'), ids=ids_of(of_kind('first')))
def test_the_first_result_is_the_known_video(case: harness.SearchCase, ytres: harness.Ytres,
                                             record: Callable[[str, str], 'Row']) -> None:
    answer = ytres.search(case.query, ONE_PAGE)
    record('search', case.query).ytres(said(answer), answer.seconds)
    assert answer.ok and answer.value, f'{case.purpose}: ytres failed or found nothing: {answer.failure}'
    check_rows(answer.value)
    first = answer.value[0]
    assert first.video_id == case.expect, (
        f'{case.purpose}: the first result is {first.video_id} ({first.title!r}), not {case.expect}')


def queries_in(node: Any) -> Iterator[str]:
    """Every string YouTube filed under "query" or "originalQuery" in a response: its echo of the query."""
    if isinstance(node, dict):
        for key, value in node.items():
            if key in ('query', 'originalQuery') and isinstance(value, str):
                yield value
            yield from queries_in(value)
    elif isinstance(node, list):
        for value in node:
            yield from queries_in(value)


@pytest.mark.parametrize('case', of_kind('letters'), ids=ids_of(of_kind('letters')))
def test_a_query_in_polish_letters_arrives_intact(case: harness.SearchCase, ytres: harness.Ytres, dump_file: Path,
                                                  record: Callable[[str, str], 'Row']) -> None:
    answer = ytres.search(case.query, ONE_PAGE, dump=dump_file)
    record('search', case.query).ytres(said(answer), answer.seconds)
    assert answer.ok and answer.value, f'{case.purpose}: ytres failed or found nothing: {answer.failure}'
    first = answer.value[0]
    assert case.expect in first.channel or case.expect in first.title, (
        f'{case.purpose}: the first result, {first.title!r} by {first.channel!r}, does not hold {case.expect!r}')
    # YouTube corrects a mangled query and still finds the right videos, so
    # the results alone could pass with the letters lost on the way: its
    # echo of the query is what shows they arrived.
    echoed: List[str] = list(queries_in(json.loads(dump_file.read_text(encoding='utf-8'))))
    assert case.query in echoed, f'YouTube echoed {echoed!r}, not {case.query!r}'
    mangled: Optional[str] = next((query for query in echoed if '�' in query), None)
    assert mangled is None, f'YouTube got a mangled query: {mangled!r}'


@pytest.mark.parametrize('case', of_kind('nothing'), ids=ids_of(of_kind('nothing')))
def test_a_nonsense_query_finds_nothing(case: harness.SearchCase, ytres: harness.Ytres,
                                        record: Callable[[str, str], 'Row']) -> None:
    answer = ytres.search(case.query, ONE_PAGE)
    record('search', case.query).ytres(said(answer), answer.seconds)
    assert answer.ok, f'{case.purpose}: ytres failed: {answer.failure}'
    assert answer.value == [], f'{case.purpose}: found {[row.title for row in answer.value or []]}'
