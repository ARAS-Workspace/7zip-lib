# Distribution

## Branch

```
main    ip7z/7zip releases + our changes + release-mac    mac-v*  →  lib7zip_apple.a (arm64 + x86_64)
```

`main` is the only branch: 7-Zip's own history from `ip7z/7zip`, one commit per release, with this fork's changes on top. A `dev/<topic>` branch is opened only when a change needs one.

This fork's own files are `7zip-apple/` (the C API in `include/7zip_apple.h`, its sources in `src/`, `makefile.gcc`, `exports.txt`, `build.sh` and the round-trip test in `test/roundtrip.c`), `.github/` (the release line), `VERSION` and this file. The files of 7-Zip it changes are listed below.

`VERSION` holds `<7-Zip release>+<Consumer>.Patch.<n>`: the 7-Zip release the tree carries, the app the round serves, and the round. The first release, `26.05+MacRAR.Patch.1`, served MacRAR; the rounds since serve Phantom-Archiver. Its base must equal `MY_VERSION_NUMBERS` in `C/7zVersion.h`; `bump.sh`, `release-mac.sh` and the release workflow all refuse a mismatch.

## Set up

```bash
W=/path/to/7zip-lib

git clone git@github.com:ARAS-Workspace/7zip-lib.git "$W"
git -C "$W" remote add upstream https://github.com/ip7z/7zip.git
git -C "$W" remote set-url --push upstream DISABLED
git -C "$W" fetch upstream --tags
```

## Build and test

```bash
"$W/7zip-apple/build.sh"
```

It builds the library for arm64 and x86_64 (`MACOSX_DEPLOYMENT_TARGET` 13.0 unless set), joins the slices into `7zip-apple/build/universal/lib7zip_apple.a`, refuses any exported symbol outside `sevenzip_*`, and runs the round-trip test against each slice; x86_64 runs under Rosetta and is skipped where it cannot. The test expects the library to report the version `VERSION` holds.

## Take an upstream release

7-Zip publishes one commit per release on `ip7z/7zip` `main`, tagged with the version.

```bash
git -C "$W" fetch upstream --tags
git -C "$W" cherry-pick -x 26.06
```

Resolve the conflicts in the files this fork changes, check their change notices (below), build and test, then start the new base:

```bash
(cd "$W" && .github/scripts/bump.sh 26.06+PhantomArchiver.Patch.1)
git -C "$W" push origin main
```

## Changes to 7-Zip's own files

7-Zip is under the GNU LGPL 2.1, whose section 2(b) asks that a changed file carry a prominent notice of the change and its date. Every file of 7-Zip this fork changes says so under its first line, one dated line per change:

```
// Common/StringConvert.cpp
// Changed by ARAS-Workspace (https://github.com/ARAS-Workspace/7zip-lib) from 7-Zip 26.05:
//   2026-10-13: On Apple systems, CodePageToUnicodeString() reads ...
```

The files changed so far: `CPP/7zip/Archive/Zip/ZipItem.cpp`, `CPP/Common/StringConvert.cpp`, `CPP/Common/StringConvert.h`. A commit that changes one of 7-Zip's files adds it here. When an upstream release is taken, check that each notice survived the cherry-pick and that its "from 7-Zip" names the new release. The release archive carries `DOC/License.txt` as `LICENSE` and the full LGPL text as `copying.txt`.

7-Zip's own console, `7zz`, still builds from this tree with 7-Zip's makefiles, for comparing the library with it. The code page reader links CoreFoundation, and the x64 makefile's 10.15 target is older than current SDKs accept:

```bash
make -C CPP/7zip/Bundles/Alone2 -f ../../cmpl_mac_arm64.mak O=/tmp/7zz-arm64 MY_LIBS="-framework CoreFoundation"
make -C CPP/7zip/Bundles/Alone2 -f ../../cmpl_mac_x64.mak O=/tmp/7zz-x64 MY_LIBS="-framework CoreFoundation" \
     MY_ARCH="-arch x86_64 -mmacosx-version-min=13.0"
```

## Release

```bash
(cd "$W" && .github/scripts/bump.sh)
git -C "$W" push origin main
(cd "$W" && .github/scripts/release-mac.sh)
```

`bump.sh` without an argument takes the next round for the consumer `VERSION` names; a new 7-Zip base or a new consumer is given explicitly (`bump.sh 26.05+PhantomArchiver.Patch.1`). `release-mac.sh` pushes the tag `mac-v<version>`. The workflow `release-mac.yaml` checks the tag against `VERSION` and `C/7zVersion.h`, builds arm64 and x86_64 and runs the round-trip test (`7zip-apple/build.sh`), and publishes `7zip-apple_<version>_macos_universal.tar.gz`: the universal library, its header, `LICENSE`, `copying.txt` and `CHECKSUM.sha256`.

## Verify

```bash
cat "$W/VERSION"
gh run list --repo ARAS-Workspace/7zip-lib --workflow release-mac.yaml --limit 5
gh release list --repo ARAS-Workspace/7zip-lib
```

## Consume

```bash
# Phantom-Archiver (MacRAR takes 26.05+MacRAR.Patch.1 the same way)
V=26.05+PhantomArchiver.Patch.1
gh release download "mac-v$V" --repo ARAS-Workspace/7zip-lib --dir /tmp/sza
tar -xzf /tmp/sza/*.tar.gz -C Libraries/SevenZipKit/
(cd Libraries/SevenZipKit && shasum -a 256 -c CHECKSUM.sha256)
```

`Libraries/SevenZipKit/SOURCE` then names the release, its workflow run and this repository, where the library's source is.
