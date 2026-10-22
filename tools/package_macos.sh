#!/bin/sh
# Build and package the macOS universal AU + VST3 as a ZIP with a SHA-256 companion.
# Run from a clean, committed tree after a Release build in build-release.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/build-release"
DIST="$BUILD/dist"
VERSION=$(sed -n 's/^project(CodyPitch VERSION \([0-9][0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")
BUNDLE_ID=$(sed -n 's/^[[:space:]]*BUNDLE_ID "\([^"]*\)".*/\1/p' "$ROOT/CMakeLists.txt")
MANUFACTURER=$(sed -n 's/^[[:space:]]*PLUGIN_MANUFACTURER_CODE \([^[:space:]]*\).*/\1/p' "$ROOT/CMakeLists.txt")
PLUGIN_CODE=$(sed -n 's/^[[:space:]]*PLUGIN_CODE \([^[:space:]]*\).*/\1/p' "$ROOT/CMakeLists.txt")
test -n "$VERSION" && test -n "$BUNDLE_ID" && test -n "$MANUFACTURER" && test -n "$PLUGIN_CODE" || {
    echo "Could not determine CMake project version/identity" >&2; exit 1;
}

# Refuse to package anything that is not an exact commit.
if [ -n "$(git -C "$ROOT" status --porcelain)" ]; then
    echo "Working tree is not clean; commit before packaging." >&2; exit 1
fi
COMMIT=$(git -C "$ROOT" rev-parse --verify HEAD)

# Bring the package's binaries up to date with this exact commit.
cmake --build "$BUILD" --target CodyPitch_AU CodyPitch_VST3 -j3
NAME="Tuna-Tuner-$VERSION-macos-universal"
STAGE="$DIST/$NAME"
ARCHIVE="$DIST/$NAME.zip"
AU="$BUILD/CodyPitch_artefacts/Release/AU/Tuna Tuner.component"
VST3="$BUILD/CodyPitch_artefacts/Release/VST3/Tuna Tuner.vst3"

# JUCE in use: the local checkout, else the fetched source.
if [ -f "$ROOT/third_party/JUCE/LICENSE.md" ]; then
    JUCE_DIR="$ROOT/third_party/JUCE"
elif [ -f "$BUILD/_deps/juce-src/LICENSE.md" ]; then
    JUCE_DIR="$BUILD/_deps/juce-src"
else
    echo "Could not find the JUCE checkout used for this build" >&2; exit 1
fi
SDK="$JUCE_DIR/modules/juce_audio_processors/format_types/VST3_SDK"
WORLD_LICENSE="$BUILD/_deps/world-src/LICENSE.txt"
[ -f "$WORLD_LICENSE" ] || WORLD_LICENSE="$ROOT/licenses/WORLD-LICENSE.txt"

for path in "$AU" "$VST3"; do
    test -d "$path" || { echo "Missing built plug-in: $path" >&2; exit 1; }
    plist="$path/Contents/Info.plist"
    for key in CFBundleVersion CFBundleShortVersionString; do
        actual=$(plutil -extract "$key" raw -o - "$plist")
        test "$actual" = "$VERSION" || { echo "Stale $key in $path: $actual (expected $VERSION)" >&2; exit 1; }
    done
    actual=$(plutil -extract CFBundleIdentifier raw -o - "$plist")
    test "$actual" = "$BUNDLE_ID" || { echo "Stale bundle identity in $path: $actual (expected $BUNDLE_ID)" >&2; exit 1; }
done
actual=$(plutil -extract AudioComponents.0.manufacturer raw -o - "$AU/Contents/Info.plist")
test "$actual" = "$MANUFACTURER" || { echo "Stale AU manufacturer: $actual" >&2; exit 1; }
actual=$(plutil -extract AudioComponents.0.subtype raw -o - "$AU/Contents/Info.plist")
test "$actual" = "$PLUGIN_CODE" || { echo "Stale AU subtype: $actual" >&2; exit 1; }
for path in "$AU/Contents/MacOS/Tuna Tuner" "$VST3/Contents/MacOS/Tuna Tuner"; do
    arches=$(lipo -archs "$path")
    printf '%s\n' "$arches" | grep -qw arm64 || { echo "Missing arm64 in $path; got $arches" >&2; exit 1; }
    printf '%s\n' "$arches" | grep -qw x86_64 || { echo "Missing x86_64 in $path; got $arches" >&2; exit 1; }
done
codesign --verify --deep --strict "$AU"
codesign --verify --deep --strict "$VST3"

rm -rf "$STAGE"
mkdir -p "$STAGE/licenses" "$STAGE/docs"
ditto "$AU" "$STAGE/Tuna Tuner.component"
ditto "$VST3" "$STAGE/Tuna Tuner.vst3"
cp "$ROOT/README.md" "$ROOT/THIRD_PARTY_NOTICES.md" "$STAGE/"
cp "$ROOT/docs/editor.png" "$STAGE/docs/"
cp "$ROOT/licenses/"* "$STAGE/licenses/"
cp "$ROOT/Resources/fonts/Inter-LICENSE.txt" "$ROOT/Resources/fonts/JetBrainsMono-LICENSE.txt" "$STAGE/licenses/"
cp "$JUCE_DIR/LICENSE.md" "$STAGE/licenses/JUCE-LICENSE.md"
cp "$SDK/LICENSE.txt" "$STAGE/licenses/VST3-SDK-LICENSE.txt"
cp "$SDK/base/LICENSE.txt" "$STAGE/licenses/VST3-SDK-base-LICENSE.txt"
cp "$SDK/pluginterfaces/LICENSE.txt" "$STAGE/licenses/VST3-SDK-pluginterfaces-LICENSE.txt"
cp "$SDK/public.sdk/LICENSE.txt" "$STAGE/licenses/VST3-SDK-public-sdk-LICENSE.txt"
cp "$SDK/VST3_License_Agreement.pdf" "$STAGE/licenses/"
cp "$WORLD_LICENSE" "$STAGE/licenses/WORLD-LICENSE.txt"
codesign --force --deep --sign - "$STAGE/Tuna Tuner.component"
codesign --force --deep --sign - "$STAGE/Tuna Tuner.vst3"
codesign --verify --deep --strict "$STAGE/Tuna Tuner.component"
codesign --verify --deep --strict "$STAGE/Tuna Tuner.vst3"

cat > "$STAGE/INSTALL.txt" <<TXT
Tuna Tuner $VERSION - macOS universal

This package contains AU and VST3 bundles for arm64 and x86_64 Macs.
Copy Tuna Tuner.component to ~/Library/Audio/Plug-Ins/Components/ and
Tuna Tuner.vst3 to ~/Library/Audio/Plug-Ins/VST3/, then rescan plug-ins in
your DAW. The bundles are ad-hoc signed, not Developer ID signed or notarized.

Bundle filenames: the original development copy of Tuna Tuner uses the same
bundle filenames (Tuna Tuner.component and Tuna Tuner.vst3) as this release,
although the two have different plug-in identities. If you have the original
installed, move or rename its bundles first (for example, Tuna Tuner Dev.component
and Tuna Tuner Dev.vst3) and leave their Contents untouched, or copying this
package will replace them. Existing sessions keep identifying the original
plug-in; this release does not replace those instances or migrate sessions.

See README.md for controls and known limits, and BUILD-INFO.txt for build
provenance.
TXT

cat > "$STAGE/BUILD-INFO.txt" <<TXT
Product: Tuna Tuner
Version: $VERSION
Source commit: $COMMIT
Build: macOS arm64 and x86_64; AU and VST3
Signing: ad-hoc signatures verified; not Developer ID signed or notarized
Windows: not built by this package
Identity: $BUNDLE_ID; manufacturer $MANUFACTURER; plug-in code $PLUGIN_CODE
Known limits: 320 ms latency; monophonic; no formant correction; phrase drift can cause forced seams. Real-time timing has not been qualified on a quiet host or in a DAW.
TXT

rm -f "$ARCHIVE" "$ARCHIVE.sha256"
ditto -c -k --norsrc --keepParent "$STAGE" "$ARCHIVE"
unzip -t "$ARCHIVE" >/dev/null
VERIFY="$DIST/.verify-$NAME"
rm -rf "$VERIFY"
mkdir -p "$VERIFY"
ditto -x -k "$ARCHIVE" "$VERIFY"
EXTRACTED="$VERIFY/$NAME"
for bundle in "Tuna Tuner.component" "Tuna Tuner.vst3"; do
    binary="$EXTRACTED/$bundle/Contents/MacOS/Tuna Tuner"
    test -x "$binary" || { echo "Archived executable missing or not executable: $binary" >&2; exit 1; }
    arches=$(lipo -archs "$binary")
    printf '%s\n' "$arches" | grep -qw arm64 || { echo "Archived binary missing arm64: $binary" >&2; exit 1; }
    printf '%s\n' "$arches" | grep -qw x86_64 || { echo "Archived binary missing x86_64: $binary" >&2; exit 1; }
    codesign --verify --deep --strict "$EXTRACTED/$bundle"
done
for f in THIRD_PARTY_NOTICES.md INSTALL.txt BUILD-INFO.txt docs/editor.png licenses/JUCE-LICENSE.md \
    licenses/Inter-LICENSE.txt licenses/JetBrainsMono-LICENSE.txt licenses/WORLD-LICENSE.txt \
    licenses/VST3-SDK-LICENSE.txt licenses/VST3_License_Agreement.pdf; do
    test -s "$EXTRACTED/$f" || { echo "Archive is missing $f" >&2; exit 1; }
done
(cd "$DIST" && shasum -a 256 "$NAME.zip" > "$NAME.zip.sha256")
rm -rf "$VERIFY"
printf 'Created %s\n' "$ARCHIVE"
