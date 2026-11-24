"""Playlists: the lines of corpus_playlists.txt through ytres_cli, and the ones that list against yt-dlp.

The comparison with yt-dlp --flat-playlist is exact for an ordinary
playlist: the same ids in the same order, the same title and the same
count. Both read the same browse answers; the only difference allowed is
the one the library makes on purpose, leaving out the videos YouTube lists
as private or deleted. A curated playlist (its corpus line says so) is
served a little differently from one request to the next, so its ids are
compared as a long common start and a large overlap instead.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Callable, Dict, List, Optional

import pytest

import harness

if TYPE_CHECKING:
    from conftest import Row

pytestmark = pytest.mark.live

CASES = harness.read_playlist_corpus()
LISTING = [case for case in CASES if case.expected == 'Ok']


def ids_of(cases: List[harness.PlaylistCase]) -> List[str]:
    return [case.target for case in cases]


# yt-dlp is asked once per playlist, for the most entries any line of it
# wants: the long playlist and its watch link share one run.
YTDLP_MAX: Dict[str, int] = {}
for _case in LISTING:
    _list_id = harness.list_id_of(_case.target)
    YTDLP_MAX[_list_id] = max(YTDLP_MAX.get(_list_id, 0), _case.max)


def first_difference(mine: List[Optional[str]], theirs: List[str]) -> Optional[int]:
    """The index of the first place two lists of ids differ, a missing id included; None when they are equal."""
    for index in range(max(len(mine), len(theirs))):
        if index >= len(mine) or index >= len(theirs) or mine[index] != theirs[index]:
            return index
    return None


# A curated line's ids: as many as yt-dlp's, the first CURATED_SAME_ORDER in
# the same order, and at least CURATED_SHARED of them shared. The Hit List
# was seen on 2026-11-24 with its 50th of 52 slots holding another video for
# each program; a dropped, added or reordered video near its top still fails.
CURATED_SAME_ORDER = 40
CURATED_SHARED = 0.95


def curated_difference(mine: List[Optional[str]], theirs: List[str]) -> Optional[str]:
    """Why two listings of a curated playlist differ beyond what YouTube's re-curating explains, or None."""
    if len(mine) != len(theirs):
        return f'{len(mine)} ids, yt-dlp {len(theirs)}'
    differ = first_difference(mine[:CURATED_SAME_ORDER], theirs[:CURATED_SAME_ORDER])
    if differ is not None:
        return (f'the ids differ from entry {differ + 1}, within the first {CURATED_SAME_ORDER}: '
                f'ytres {mine[differ:differ + 5]}, yt-dlp {theirs[differ:differ + 5]}')
    shared = len(set(mine) & set(theirs))
    if shared < CURATED_SHARED * len(theirs):
        return f'only {shared} of {len(theirs)} ids shared, want {CURATED_SHARED:.0%}'
    return None


def said(answer: harness.Answer[harness.Listing]) -> str:
    """What ytres_cli said, for the table: the count and how many entries, or its Error."""
    listing = answer.value
    text = f'count {listing.count}, {len(listing.entries)} listed' if listing else 'nothing listed'
    return text if answer.ok else f'{answer.code}, {text}'


@pytest.mark.parametrize('case', CASES, ids=ids_of(CASES))
def test_ytres_lists_as_the_corpus_expects(case: harness.PlaylistCase, ytres: harness.Ytres,
                                           record: Callable[[str, str], 'Row']) -> None:
    row = record('playlist', case.target)
    answer = ytres.playlist(case.target, case.max)
    row.ytres(said(answer), answer.seconds)
    assert answer.code == case.expected, (
        f'{case.purpose}: ytres said {answer.failure or "Ok"}, the corpus expects {case.expected}')
    if case.expected != 'Ok':
        assert answer.completed.stdout == '', 'a playlist refused on its first page prints nothing'
        assert answer.failure is not None
        if case.expected == 'Unavailable':
            assert 'does not exist' in answer.failure.message, f'{case.purpose}: {answer.failure}'
        return
    listing = answer.value
    assert listing is not None
    assert listing.title.strip(), 'ytres printed an empty playlist title'
    assert case.count.holds(listing.count), f'{case.purpose}: count {listing.count}, the corpus wants {case.count}'
    least = min(case.max, case.count.value) if case.count.value is not None else 1
    assert least <= len(listing.entries) <= case.max, (
        f'{case.purpose}: {len(listing.entries)} entries for --max {case.max}, want at least {least}')
    ids = listing.ids
    assert all(ids), 'an entry is not https://www.youtube.com/watch?v=<id>: ' + ', '.join(
        entry.url for entry in listing.entries if not entry.video_id)
    assert len(set(ids)) == len(ids), f'a video comes twice: {ids}'
    untitled = [entry.video_id for entry in listing.entries if not entry.title.strip()]
    assert not untitled, f'entries without a title: {untitled}'


@pytest.mark.parametrize('case', LISTING, ids=ids_of(LISTING))
def test_ytres_lists_what_ytdlp_lists(case: harness.PlaylistCase, ytres: harness.Ytres, ytdlp: harness.YtDlp,
                                      record: Callable[[str, str], 'Row']) -> None:
    list_id = harness.list_id_of(case.target)
    row = record('playlist', case.target)
    ours = ytres.playlist(case.target, case.max)
    theirs = ytdlp.playlist(harness.playlist_url(list_id), YTDLP_MAX[list_id])
    row.ytres(said(ours), ours.seconds)
    if theirs.value is not None:
        row.ytdlp(f'count {theirs.value.count}, {len(theirs.value.ids)} listed', theirs.seconds)
    else:
        row.ytdlp(str(theirs.failure), theirs.seconds)
    assert ours.ok and ours.value is not None, f'{case.purpose}: ytres failed: {ours.failure}'
    assert theirs.ok and theirs.value is not None, f'{case.purpose}: yt-dlp failed: {theirs.failure}'
    mine = ours.value
    other = theirs.value
    # yt-dlp was asked for as many entries as the line of this playlist that
    # wants the most; this line wants the first case.max playable ones.
    their_ids: List[str] = other.playable_ids[:case.max]
    my_ids = mine.ids
    if len(their_ids) < case.max and len(other.ids) >= YTDLP_MAX[list_id]:
        # Hidden videos took places in yt-dlp's window: ytres went on past
        # them, so only as many as yt-dlp saw can be compared.
        my_ids = my_ids[:len(their_ids)]
    if case.compare == 'curated':
        why = curated_difference(my_ids, their_ids)
        if why is not None:
            pytest.fail(f'{case.purpose}: {why}')
    else:
        differ = first_difference(my_ids, their_ids)
        if differ is not None:
            pytest.fail(f'{case.purpose}: the ids differ from entry {differ + 1}: ytres {my_ids[differ:differ + 5]}, '
                        f'yt-dlp {their_ids[differ:differ + 5]} ({len(my_ids)} and {len(their_ids)} ids)')
    assert mine.title == other.title, f'title: ytres {mine.title!r}, yt-dlp {other.title!r}'
    if other.count is not None:
        assert mine.count == other.count, f'count: ytres {mine.count}, yt-dlp {other.count}'
