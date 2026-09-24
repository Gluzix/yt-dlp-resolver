# ytres

A C++17 library that turns a YouTube link into the video's title, its
canonical page URL and a direct audio stream URL, by asking YouTube's
InnerTube API the way yt-dlp does. It exists to replace the `yt-dlp.exe`
process the Discord bot starts for every track: the bot links the library
instead, and the one to two seconds of process startup per song go away.

It is a library. `ytres_cli` is a development harness for trying it against
live YouTube and recording test fixtures, not the product.

This is the proof of concept, milestones M0 and M1 of
[docs/resolver-plan.md](docs/resolver-plan.md): one video at a time, through
the `visionos` InnerTube client, which needs neither YouTube's player
JavaScript nor a PO Token. Search and playlists come later.
[docs/innertube-notes.md](docs/innertube-notes.md) has the exact request and
what YouTube answered to it.

## Requirements

Windows 10 or 11, Visual Studio 2022 (MSVC), CMake 3.14+ and vcpkg at
`C:/vcpkg`. libcurl, nlohmann-json and doctest come through the `vcpkg.json`
manifest; the first configure builds libcurl from source and takes a few
minutes.

## Building

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

The tests never touch the network: they replay responses recorded in
`tests/fixtures/`.

## Running

```
build\Debug\ytres_cli.exe https://www.youtube.com/watch?v=dQw4w9WgXcQ
```

prints the title, the canonical page URL and the best audio stream URL, one
per line: the three lines the bot reads from yt-dlp today. On failure it
prints the error code and YouTube's reason to stderr and exits with 1.

- `--formats` lists every usable format instead.
- `--dump <file>` also writes YouTube's player response to `<file>`; that is
  how fixtures are recorded. The dump is scrubbed of the requesting IP and
  visitor data automatically.

Stream URLs expire after a few hours and work only from the IP address that
asked for them.
