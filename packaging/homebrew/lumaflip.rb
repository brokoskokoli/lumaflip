# SPDX-License-Identifier: GPL-3.0-or-later
# Homebrew cask for Lumaflip (SPEC 10.1). Not used by the build. The live copy is
# Casks/lumaflip.rb in https://github.com/brokoskokoli/homebrew-tap (without these comment
# lines); the release workflow (job "homebrew-tap") updates version and sha256 there after
# every release. Edit both when a stanza changes (zap paths, requirements). See
# docs/RELEASING.md, section "Homebrew Cask".
cask "lumaflip" do
  version "1.2.1"
  sha256 "e4f8dc0d65f1911f04535358287acde0b20f788a9aed58522b01383e589b7265"

  url "https://github.com/brokoskokoli/lumaflip/releases/download/v#{version}/Lumaflip-#{version}.dmg"
  name "Lumaflip"
  desc "Fast image viewer that browses the whole folder in Finder order"
  homepage "https://brokoskokoli.github.io/lumaflip/"

  livecheck do
    url "https://brokoskokoli.github.io/lumaflip/appcast.xml"
    strategy :sparkle, &:short_version
  end

  auto_updates true
  depends_on arch: :arm64
  depends_on macos: :sequoia

  app "Lumaflip.app"

  uninstall quit: "io.github.brokoskokoli.lumaflip"

  zap trash: [
    "~/Library/Application Support/Lumaflip",
    "~/Library/Caches/io.github.brokoskokoli.lumaflip",
    "~/Library/Caches/Lumaflip",
    "~/Library/HTTPStorages/io.github.brokoskokoli.lumaflip",
    "~/Library/Preferences/io.github.brokoskokoli.lumaflip.plist",
    "~/Library/Saved Application State/io.github.brokoskokoli.lumaflip.savedState",
  ]
end
