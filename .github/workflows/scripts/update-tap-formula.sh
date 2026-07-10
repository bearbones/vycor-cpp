#!/usr/bin/env bash
# Updates Formula/*.rb in the bearbones/homebrew-vycor-cpp tap to point at
# the macOS assets of the GitHub Release that just published.
#
# Invoked by .github/workflows/release.yml's update-homebrew-tap job. Expects
# these environment variables:
#   GH_TOKEN        -- a token with contents:write on the tap repo
#                      (secrets.HOMEBREW_TAP_TOKEN; the default GITHUB_TOKEN
#                      can't push cross-repo)
#   VYCOR_VERSION   -- bare SemVer, e.g. "0.1.0"
#   RELEASE_TAG     -- the pushed tag, e.g. "v0.1.0" or "v0.1.0-llvm18"
#
# Only touches the LLVM majors this release actually built (parsed from
# RELEASE_TAG the same way release.yml's determine-matrix job does), so a
# single-major backport release doesn't touch formulas for other majors.

set -euo pipefail

TAP_REPO="bearbones/homebrew-vycor-cpp"
SOURCE_REPO="bearbones/vycor-cpp"

if [[ "$RELEASE_TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+-llvm([0-9]+)$ ]]; then
  RELEASED_MAJORS="${BASH_REMATCH[1]}"
elif [[ "$RELEASE_TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  RELEASED_MAJORS=$(grep -oP 'set\(VYCOR_SUPPORTED_LLVM_VERSIONS\s+\K[0-9 ]+' CMakeLists.txt)
else
  echo "error: RELEASE_TAG '$RELEASE_TAG' doesn't match the expected grammar" >&2
  exit 1
fi

# Newest-first list from CMakeLists.txt; the first token is the major that
# the unsuffixed `vycor-cpp.rb` alias should track.
NEWEST_MAJOR=$(grep -oP 'set\(VYCOR_SUPPORTED_LLVM_VERSIONS\s+\K[0-9]+' CMakeLists.txt | head -1)

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

echo "Cloning $TAP_REPO ..."
git clone --depth 1 "https://x-access-token:${GH_TOKEN}@github.com/${TAP_REPO}.git" "$WORKDIR/tap"
mkdir -p "$WORKDIR/tap/Formula"

asset_url() {
  local os_label="$1" major="$2"
  gh release view "$RELEASE_TAG" --repo "$SOURCE_REPO" --json assets \
    --jq ".assets[] | select(.name == \"vycor-cpp-v${VYCOR_VERSION}-${os_label}-llvm${major}.tar.gz\") | .url"
}

sha256_of_url() {
  local url="$1"
  curl -sL "$url" -o "$WORKDIR/asset.tar.gz"
  sha256sum "$WORKDIR/asset.tar.gz" | cut -d' ' -f1
}

write_formula() {
  local major="$1" class_name="$2" out_path="$3"
  local arm64_url x86_64_url arm64_sha x86_64_sha

  arm64_url=$(asset_url "macos-arm64" "$major")
  x86_64_url=$(asset_url "macos-x86_64" "$major")

  if [[ -z "$arm64_url" || -z "$x86_64_url" ]]; then
    echo "error: could not find both macOS assets for llvm${major} on $RELEASE_TAG" >&2
    exit 1
  fi

  arm64_sha=$(sha256_of_url "$arm64_url")
  x86_64_sha=$(sha256_of_url "$x86_64_url")

  cat > "$out_path" <<EOF
class ${class_name} < Formula
  desc "Clang LibTooling backend for C++ static analysis (LLVM ${major} build)"
  homepage "https://github.com/${SOURCE_REPO}"
  version "${VYCOR_VERSION}"
  license "Apache-2.0"
  depends_on "llvm@${major}"

  on_macos do
    if Hardware::CPU.arm?
      url "${arm64_url}"
      sha256 "${arm64_sha}"
    else
      url "${x86_64_url}"
      sha256 "${x86_64_sha}"
    end
  end

  def install
    bin.install "bin/vycor-cpp"
  end

  test do
    assert_match "vycor-cpp version #{version}", shell_output("#{bin}/vycor-cpp --version")
  end
end
EOF
}

for major in $RELEASED_MAJORS; do
  class_name="VycorCppAT${major}"
  write_formula "$major" "$class_name" "$WORKDIR/tap/Formula/vycor-cpp@${major}.rb"
  echo "Wrote Formula/vycor-cpp@${major}.rb"

  if [[ "$major" == "$NEWEST_MAJOR" ]]; then
    write_formula "$major" "VycorCpp" "$WORKDIR/tap/Formula/vycor-cpp.rb"
    echo "Wrote Formula/vycor-cpp.rb (tracks llvm${major}, the newest supported major)"
  fi
done

cd "$WORKDIR/tap"
git config user.name "vycor-cpp release bot"
git config user.email "actions@github.com"
git add Formula/
if git diff --cached --quiet; then
  echo "No formula changes to commit."
else
  git commit -m "vycor-cpp ${VYCOR_VERSION}: update formula(s) for ${RELEASED_MAJORS}"
  git push origin HEAD
fi
