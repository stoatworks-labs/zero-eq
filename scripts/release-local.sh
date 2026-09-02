#!/usr/bin/env bash
# release-local.sh — cut a full Zero EQ release from this Mac.
#
# GitHub Actions minutes are exhausted, so releases are built here.
#
#   macOS    universal (arm64 + x86_64). CMAKE_OSX_ARCHITECTURES must be set
#            before project() or the "universal" build is silently arm64-only,
#            so it is passed on the configure line and then checked with lipo.
#   Windows  x64 and ARM64, built in the Parallels guest — JUCE links the MSVC
#            runtime and cannot be cross-compiled from macOS.
#   Linux    NOT BUILDABLE HERE. Needs ALSA/X11/GTK dev headers on a Linux
#            host; there is no Linux machine and no container runtime.
#
# A plugin does not install to one place, so the macOS installer is a
# multi-part .pkg: VST3 -> /Library/Audio/Plug-Ins/VST3, AU -> .../Components,
# and the standalone app -> /Applications. The .dmg holds all three for people
# who would rather drag them themselves.
#
#   scripts/release-local.sh                  build into dist-release/
#   scripts/release-local.sh --version 0.8.0  set an explicit version
#   scripts/release-local.sh --no-vm          skip the Windows builds
#   scripts/release-local.sh --upload         tag and publish the GitHub release
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo"
source "$repo/scripts/release-lib.sh"

out="$repo/dist-release"
upload=0; use_vm=1; version=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --upload)  upload=1 ;;
    --no-vm)   use_vm=0 ;;
    --version) version="$2"; shift ;;
    *) echo "unknown flag: $1" >&2; exit 2 ;;
  esac
  shift
done

# CMakeLists has drifted from the tags (it still said 0.1.0 at v0.7.0), and it
# is the CMake value the DAW reports, so prefer the git tag as the source of
# truth and write it back.
current="$(git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//')"
[[ -z "$current" ]] && current="$(sed -n 's/^project(ZeroEQ VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
[[ -z "$version" ]] && version="$(awk -F. '{printf "%d.%d.%d", $1, $2, $3+1}' <<<"$current")"
cmake_ver="$(sed -n 's/^project(ZeroEQ VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
tag="v${version}"
echo "==> Zero EQ ${current} -> ${version} (CMakeLists said ${cmake_ver})"
sed -i '' "s/^project(ZeroEQ VERSION ${cmake_ver}/project(ZeroEQ VERSION ${version}/" CMakeLists.txt

rl_init "Zero EQ" zero-eq "$version" com.stoatworks.zero-eq "$out"
rm -rf "$out"; mkdir -p "$out"

# ------------------------------------------------------------------ macOS ---

echo "==> configuring macOS (universal)"
cmake -B build-release -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" >/dev/null
echo "==> building macOS"
cmake --build build-release --config Release --parallel "$(sysctl -n hw.ncpu)" >/dev/null

art="build-release/ZeroEQ_artefacts/Release"
vst3="$art/VST3/Zero EQ.vst3"
au="$art/AU/Zero EQ.component"
app="$art/Standalone/Zero EQ.app"

# Trust lipo, not the build log — a single-tone test cannot tell you the
# architecture and neither can a successful compile.
for b in "$vst3/Contents/MacOS/Zero EQ" "$au/Contents/MacOS/Zero EQ" "$app/Contents/MacOS/Zero EQ"; do
  [[ -f "$b" ]] || continue
  a="$(lipo -archs "$b")"
  echo "    $(basename "$(dirname "$(dirname "$b")")"): ${a}"
  [[ "$a" == *arm64* && "$a" == *x86_64* ]] \
    || { echo "expected universal, got ${a} for $b" >&2; exit 1; }
done

for b in "$vst3" "$au" "$app"; do [[ -d "$b" ]] && rl_adhoc_sign "$b"; done

stage="$out/.stage-mac"
rm -rf "$stage"; mkdir -p "$stage"
[[ -d "$vst3" ]] && cp -R "$vst3" "$stage/"
[[ -d "$au"   ]] && cp -R "$au"   "$stage/"
[[ -d "$app"  ]] && cp -R "$app"  "$stage/"
cp README.md LICENSE "$stage/"
rl_dmg macos-universal "$stage"
rl_pkg_multi macos-universal \
  "$vst3:/Library/Audio/Plug-Ins/VST3" \
  "$au:/Library/Audio/Plug-Ins/Components" \
  "$app:/Applications"
rm -rf "$stage"

# ---------------------------------------------------------------- Windows ---

if (( use_vm )) && command -v prlctl >/dev/null 2>&1 \
   && prlctl list -a 2>/dev/null | grep -q "running.*Windows 11"; then
  echo "==> Windows (Parallels VM)"
  bash "$repo/scripts/release-windows-vm-cmake.sh" "$repo" zero-eq "'*.vst3','*.exe'" \
    || rl_skip "Windows builds (VM build failed)"

  for label in x86_64 aarch64; do
    src="$out/win-${label}"
    if [[ -d "$src" ]] && [[ -n "$(ls -A "$src" 2>/dev/null)" ]]; then
      wstage="$out/.stage-win-${label}"
      rm -rf "$wstage"; mkdir -p "$wstage"
      cp -R "$src"/* "$wstage/"
      cp README.md LICENSE "$wstage/"
      rl_zip  "windows-${label}" "$wstage"
      # A plugin installer has no shortcut to make; --plain keeps it to files.
      rl_nsis "windows-${label}" "$wstage" --plain
      rm -rf "$wstage" "$src"
    else
      rl_skip "Windows ${label} (no artefacts produced)"
      rm -rf "$src"
    fi
  done
else
  rl_skip "Windows builds (VM not running or --no-vm)"
fi

rl_skip "Linux builds (JUCE needs a Linux host; no container runtime here)"

rl_summary

cat <<'NOTE'

    Unsigned. macOS users must run
      xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/Zero EQ.vst3"
    (and the same for the .component) or the DAW's plugin scan will reject them.

    Note: CMakeLists sets COPY_PLUGIN_AFTER_BUILD TRUE, so building here also
    installs Zero EQ into this Mac's own plugin folders.
NOTE

if (( upload )); then
  echo "==> tagging ${tag}"
  git add -A
  git commit -m "release: ${tag}" || true
  git tag -a "$tag" -m "Zero EQ ${version}" || true
  git push origin HEAD --tags
  gh release create "$tag" --title "Zero EQ ${version}" \
     --notes "Local build — GitHub Actions minutes are exhausted, so these artefacts were cut on a Mac. Unsigned: see the README for the macOS quarantine step." \
     "$out"/* \
    || gh release upload "$tag" "$out"/* --clobber
fi
