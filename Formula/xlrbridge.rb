# Homebrew formula for xlrbridge.
#
# Tap-ready but UNPUBLISHED: this builds from source via the project Makefile.
# To use it from a local checkout:
#
#     brew install --build-from-source ./Formula/xlrbridge.rb
#
# or, once tapped (e.g. `brew tap renatolousan/xlrbridge` pointing a tap repo at
# this formula), `brew install renatolousan/xlrbridge/xlrbridge`.
#
# Update `url`/`sha256` to a released tarball before publishing; the head spec
# lets you install straight from the default branch in the meantime.
class Xlrbridge < Formula
  desc "Route an XLR/pro interface mic into BlackHole so Discord stops chopping it"
  homepage "https://github.com/renatolousan/xlrbridge"
  license "MIT"
  version "1.0.0"

  # Released-tarball spec. Fill in the real sha256 when you cut v1.0.0:
  #   shasum -a 256 xlrbridge-1.0.0.tar.gz
  url "https://github.com/renatolousan/xlrbridge/archive/refs/tags/v1.0.0.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"

  head "https://github.com/renatolousan/xlrbridge.git", branch: "main"

  depends_on :macos
  depends_on xcode: :build

  # BlackHole 2ch is the clean virtual device Discord reads; xlrbridge routes the
  # mic into it. It ships as a cask, so it can't be a Formula `depends_on` — it is
  # documented in the caveats and `xlrbridge setup` instructs the user to install
  # it. (BlackHole is GPL; xlrbridge only talks to it as a separate driver, so
  # xlrbridge stays MIT.)

  def install
    system "make"
    bin.install "xlrbridge"
  end

  def caveats
    <<~EOS
      xlrbridge needs the BlackHole 2ch virtual audio driver. Install it with:

          brew install blackhole-2ch

      Then run:

          xlrbridge setup

      to pick your interface + input channel, write the config, and install the
      login service (a LaunchAgent). xlrbridge needs microphone permission the
      first time it opens the interface — approve the prompt (or grant it under
      System Settings -> Privacy & Security -> Microphone).
    EOS
  end

  test do
    assert_match "xlrbridge 1.0.0", shell_output("#{bin}/xlrbridge --version")
  end
end
