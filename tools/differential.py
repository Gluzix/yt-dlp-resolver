#!/usr/bin/env python3
"""Differential check of ytres_cli against yt-dlp on the live corpus.

For every video in tests/corpus.txt, run ytres_cli and yt-dlp on it and
compare the title, the page URL and the itag each picks as the best audio.
Prints one row per video with both timings, then exits non-zero if any row
is a mismatch. Standard library only; it touches live YouTube.

A video the corpus expects ytres to refuse - a live stream (NoFormats), an
age gate, a deleted video - passes when ytres refuses it with that code.
yt-dlp's answer is shown beside it for information only: yt-dlp may well
play a live stream through its HLS manifest, which the library does not read.

    python tools/differential.py [--yt-dlp PATH] [--ytres PATH] [--corpus PATH]
"""

import argparse
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_YTRES = ROOT / 'build' / 'Debug' / ('ytres_cli.exe' if sys.platform == 'win32' else 'ytres_cli')
DEFAULT_CORPUS = ROOT / 'tests' / 'corpus.txt'
TIMEOUT_SECONDS = 120


def read_corpus(path):
    """(id, the Error ytres should report, what the video is for) per line."""
    entries = []
    for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        body, _, comment = line.partition('#')
        fields = body.split()
        if not fields:
            continue
        if len(fields) != 2:
            sys.exit(f'{path}:{number}: expected an id and an Error, got: {line}')
        entries.append((fields[0], fields[1], comment.strip()))
    return entries


def run(command):
    """The finished process, or None if it ran out of time, and the seconds it took."""
    start = time.monotonic()
    try:
        done = subprocess.run(command, capture_output=True, timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        done = None
    return done, time.monotonic() - start


def last_line(data):
    lines = data.decode('utf-8', 'replace').strip().splitlines()
    return lines[-1] if lines else ''


def ask_ytres(cli, video_id):
    """ytres_cli prints title, page URL and stream URL, or '<Error>: <message>' on stderr."""
    done, took = run([str(cli), video_id])
    if done is None:
        return {'error': 'Timeout', 'detail': f'no answer in {TIMEOUT_SECONDS} s'}, took
    lines = done.stdout.decode('utf-8', 'replace').splitlines()
    if done.returncode == 0 and len(lines) == 3:
        itag = re.search(r'[?&]itag=(\d+)', lines[2])
        return {'title': lines[0], 'url': lines[1], 'itag': itag.group(1) if itag else '?'}, took
    detail = last_line(done.stderr) or f'exit code {done.returncode}'
    return {'error': detail.split(':', 1)[0], 'detail': detail}, took


def ask_ytdlp(exe, video_id):
    """yt-dlp's -f bestaudio: title, page URL, format id and stream URL, in UTF-8."""
    done, took = run([exe, '--no-playlist', '--no-warnings', '--encoding', 'utf-8', '-f', 'bestaudio',
                      '--print', 'title', '--print', 'webpage_url', '--print', 'format_id', '--print', 'urls',
                      'https://www.youtube.com/watch?v=' + video_id])
    if done is None:
        return {'error': 'refused', 'detail': f'no answer in {TIMEOUT_SECONDS} s'}, took
    lines = done.stdout.decode('utf-8', 'replace').splitlines()
    if done.returncode == 0 and len(lines) >= 4:
        # 251-drc and 251-1 are itag 251 too; the format id shows which.
        return {'title': lines[0], 'url': lines[1], 'format': lines[2], 'itag': lines[2].split('-')[0]}, took
    return {'error': 'refused', 'detail': last_line(done.stderr) or f'exit code {done.returncode}'}, took


def mismatch(expected, ours, theirs):
    """What differs, or None when the row passes."""
    if expected != 'Ok':
        got = ours.get('error', 'Ok')
        return None if got == expected else f'ytres gave {got}, the corpus expects {expected}'
    if 'error' in ours:
        return 'ytres failed: ' + ours['detail']
    if 'error' in theirs:
        return 'yt-dlp failed: ' + theirs['detail']
    differing = [field for field in ('title', 'url', 'itag') if ours[field] != theirs[field]]
    if not differing:
        return None
    return '; '.join(f'{field}: ytres {ours[field]!r}, yt-dlp {theirs[field]!r}' for field in differing)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--yt-dlp', dest='ytdlp', default=shutil.which('yt-dlp'),
                        help='yt-dlp executable (default: the one on PATH)')
    parser.add_argument('--ytres', type=Path, default=DEFAULT_YTRES, help=f'ytres_cli (default: {DEFAULT_YTRES})')
    parser.add_argument('--corpus', type=Path, default=DEFAULT_CORPUS, help=f'the corpus (default: {DEFAULT_CORPUS})')
    args = parser.parse_args()

    # Titles are UTF-8 on both sides; the Windows console's code page must not
    # get in the way of printing them.
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    if not args.ytdlp:
        sys.exit('yt-dlp is not on PATH; pass --yt-dlp, such as --yt-dlp C:\\ytdlp\\yt-dlp.exe')
    if not args.ytres.is_file():
        sys.exit(f'{args.ytres} not found; build it, or pass --ytres')

    print(f'{"video":<11}  {"ytres":<14} {"took":>7}   {"yt-dlp":<10} {"took":>7}   verdict')
    videos = 0
    failures = 0
    for video_id, expected, purpose in read_corpus(args.corpus):
        ours, ours_took = ask_ytres(args.ytres, video_id)
        theirs, theirs_took = ask_ytdlp(args.ytdlp, video_id)
        problem = mismatch(expected, ours, theirs)
        videos += 1
        failures += problem is not None
        if problem:
            verdict = 'MISMATCH'
        else:
            verdict = 'same' if expected == 'Ok' else 'refused, as expected'
        ours_said = ours.get('itag') or ours['error']
        theirs_said = theirs.get('format') or theirs['error']
        print(f'{video_id:<11}  {ours_said:<14} {ours_took:5.1f} s   {theirs_said:<10} {theirs_took:5.1f} s   {verdict}',
              flush=True)
        if problem:
            print(f'{"":13}{purpose}\n{"":13}{problem}', flush=True)

    print(f'\n{videos} videos, {failures} mismatched')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
