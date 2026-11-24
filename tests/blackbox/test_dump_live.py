"""Dumps: ytres_cli --dump for a video, a search and a playlist writes what README promises.

A dump is a fixture to commit, so it must parse as JSON and hold no address
but the placeholder 203.0.113.7 and no visitor data but FIXTURE. The video's
dump is the regression test for the scrubber crash of 2026-11-24: a real
player response holds JSON strings far longer than the 1000 characters on
which the old regular expressions threw, and ytres_cli aborted.

The tests count what they find; they never print it, since what a scrubber
missed is the very address nothing may show.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import TYPE_CHECKING, Any, Callable

import pytest

import harness

if TYPE_CHECKING:
    from conftest import Row

pytestmark = pytest.mark.live

# One of each from the corpora, so that nothing here names an input twice.
VIDEO = next(case for case in harness.read_video_corpus() if case.expected == 'Ok')
QUERY = next(case for case in harness.read_search_corpus() if case.kind == 'common')
PLAYLIST = next(case for case in harness.read_playlist_corpus() if case.expected == 'Ok')

# The length of value the old scrubber threw on.
LONG_VALUE = 1000


def check_dump(answer: harness.Answer[Any], dump: Path) -> Any:
    """The dump parsed, once it is shown to be scrubbed; fails the test otherwise."""
    assert answer.ok, f'ytres failed: {answer.failure}'
    assert 'No response to dump' not in answer.completed.stderr
    assert dump.is_file(), 'ytres_cli exited 0 but wrote no dump'
    text = dump.read_text(encoding='utf-8')
    try:
        response = json.loads(text)
    except json.JSONDecodeError as error:
        pytest.fail(f'the dump is not JSON: {error.msg} at line {error.lineno}, column {error.colno}')
    left_ipv4 = len(harness.foreign_ipv4_in(text))
    assert left_ipv4 == 0, f'the dump holds {left_ipv4} IPv4 address(es) other than {harness.PLACEHOLDER_ADDRESS}'
    left_parameters = len(harness.foreign_address_parameters_in(text))
    assert left_parameters == 0, f'the dump holds {left_parameters} ip= or /ip/ value(s) other than the placeholder'
    visitor = harness.visitor_data_in(response)
    unscrubbed = sum(value != harness.PLACEHOLDER_VISITOR_DATA for value in visitor)
    assert unscrubbed == 0, f'{unscrubbed} of {len(visitor)} visitor data value(s) are not FIXTURE'
    return response


def test_a_video_dump_is_scrubbed(ytres: harness.Ytres, dump_file: Path, record: Callable[[str, str], 'Row']) -> None:
    row = record('dump', f'video {VIDEO.video_id}')
    answer = ytres.resolve(VIDEO.video_id, dump=dump_file)
    row.ytres(answer.code, answer.seconds)
    response = check_dump(answer, dump_file)
    # The condition of the crash is there, so the test still tests it.
    longest = harness.longest_string_in(response)
    assert longest >= LONG_VALUE, f'the longest value is {longest} characters; the crash needed {LONG_VALUE}'
    # The run's output is whole beside the dump.
    assert answer.value is not None and answer.value.page_url.endswith(VIDEO.video_id)
    # A player response names its address in its stream urls: the scrubber
    # had something to do, and did it.
    assert harness.PLACEHOLDER_ADDRESS in dump_file.read_text(encoding='utf-8')


def test_a_search_dump_is_scrubbed(ytres: harness.Ytres, dump_file: Path,
                                   record: Callable[[str, str], 'Row']) -> None:
    row = record('dump', f'search {QUERY.query}')
    answer = ytres.search(QUERY.query, 5, dump=dump_file)
    row.ytres(answer.code, answer.seconds)
    check_dump(answer, dump_file)


def test_a_playlist_dump_is_scrubbed(ytres: harness.Ytres, dump_file: Path,
                                     record: Callable[[str, str], 'Row']) -> None:
    row = record('dump', f'playlist {PLAYLIST.target}')
    answer = ytres.playlist(PLAYLIST.target, 5, dump=dump_file)
    row.ytres(answer.code, answer.seconds)
    check_dump(answer, dump_file)
