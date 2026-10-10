# SPDX-License-Identifier: GPL-3.0-or-later
# Homebrew cask template for Lumaflip (SPEC 10.1). Not used by the build.
# For a release: set version, replace sha256 with the contents of
# Lumaflip-<version>.dmg.sha256 and copy the file into the tap
# (brokoskokoli/homebrew-tap: Casks/lumaflip.rb). See docs/RELEASING.md.
cask "lumaflip" do
  version "0.1.0"
  sha256 "@SHA256_OF_DMG@"

  url "https://github.com/brokoskokoli/lumaflip/releases/download/v#{version}/Lumaflip-#{version}.dmg"
  name "Lumaflip"
  desc "Fast image viewer that browses the whole folder in Finder order"
  homepage "https://github.com/brokoskokoli/lumaflip"

  livecheck do
    url :url
    strategy :github_latest
  end

  depends_on arch: :arm64
  depends_on macos: ">= :sequoia"

  app "Lumaflip.app"

  zap trash: [
    "~/Library/Application Support/Lumaflip",
    "~/Library/Caches/Lumaflip",
    "~/Library/Preferences/io.github.brokoskokoli.Lumaflip.plist",
    "~/Library/Saved Application State/io.github.brokoskokoli.lumaflip.savedState",
  ]
end
