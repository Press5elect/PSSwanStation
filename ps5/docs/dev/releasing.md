# Making a release

A release is made only when the owner asks for one. He creates the tag and publishes the release
in the browser; from an agent's session, tag and release creation are refused (403).

## Never in a release

Games, BIOS files, firmware, keys, SBI data, saves, memory cards, states, a `network.cfg`, a
settings file, a log, the chtdb files. No game names in the notes. Console addresses masked.
`ps5/tools/release.py` refuses a folder that carries any of these, a path of the build machine, or
a word listed in `.release-private`.

## Steps

1. Check for newer upstreams and dependencies, and say what changed ([`workflow.md`](workflow.md)).
2. Bump `BuildNumber` in `ps5/src/main.cpp` (the console shows it as `01.0NN.000`).
3. Write the notes: `ps5/releases/build<N>.md`, laid out as the last one (Added, Changed, Fixed;
   Source with the `{{COMMIT}}`, `{{SOURCES}}`, `{{ZIP}}`, `{{ZIP_SHA256}}` fields release.py fills
   in). Short, laid out like PSFlyCast v1.3.5's (Added / Changed / Fixed, Installation, Reporting
   problems, Source and licensing, Build versions). The README's notes list only what was added,
   changed and fixed; `ps5/CHANGELOG.txt` keeps every build's.
4. Build from committed source: `RELEASE=1 bash ps5/tools/build.sh`, then
   ```bash
   python3 ps5/tools/release.py --notes ps5/releases/build<N>.md --zip-only \
       --source-at lapy=https://github.com/Press5elect/PSSwanStation/releases/download/build24/lapy-c3bdfe3a3993.tar.gz
   ```
   `--zip-only`: the page carries the ZIP and its `.sha256` alone (`build-ps5/release-build<N>/attach/`);
   `--source-at`: Lapy's repository is gone, its source is build 24's archive. release.py refuses
   a revision GitHub does not have, so push first or expect only `psswanstation` to be named as
   not pushed yet. It also refuses a build machine's path in any file: a library whose `__FILE__`
   gets in needs `-ffile-prefix-map` for every form of its directory (as given, without `..`,
   with links resolved; mbedTLS in `ps5/CMakeLists.txt`).
5. Push `ps5-port` (on the owner's word), then he tags the commit `build<N>` and publishes.
6. **Attach the ZIP and its `.zip.sha256` only.** release.py also writes source archives; they
   are not attached: the sources are public at the revisions the notes name, and our commits on
   other projects are in [`../../patches`](../../patches/README.md).
7. Read the published release back (its assets and their digests) and check the README's
   "Latest release" line.
