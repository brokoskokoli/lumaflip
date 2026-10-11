// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-MAC-09: offering to become the default app. When an image opens whose type another app
// opens by default, the window shows an info bar once per type. This is the decision logic
// (no UI): when to ask the system at all, when to show the bar after its answer, and the
// remembered answers (types already asked, the global switch key::OfferDefaultApp).
// The UI part is app/DefaultAppOffer.h.

#include "Platform.h" // DefaultViewerStatus

#include <QString>
#include <QStringList>

namespace lf::defaultapp {

// Settings key: types (DefaultViewerStatus::type) the bar was shown for and answered
// (QStringList). Not a user setting (no entry in the preferences).
inline constexpr auto kAskedTypesKey = "general/defaultAppAskedTypes";
// Environment variable that turns the offer off (test harness, scripted runs).
inline constexpr auto kDisableEnv = "LF_NO_DEFAULT_APP_OFFER";
// The first query waits this long after the first frame (P-01), later ones until an image
// stayed this long (fast flipping asks nothing).
inline constexpr int kStartDelayMs = 3000;
inline constexpr int kSettleMs = 800;

enum class Decision {
    Show,            // after the query: show the bar; before it: go on and ask the system
    Disabled,        // "Nicht mehr fragen" or the preference is off
    Environment,     // kDisableEnv set (tests)
    ContainerMember, // archive member or PDF page (F-OPEN-10)
    FullScreen,      // full screen or slideshow: never
    NotInstalled,    // not the installed app (build tree, disk image, outside /Applications)
    Unknown,         // the query failed or the type is not one of ours
    IsDefault,       // nothing to offer
    AlreadyAsked,    // the type was answered before
};

// What is known on the GUI thread before the system is asked.
struct Situation {
    bool enabled = true;          // key::OfferDefaultApp
    bool environmentOff = false;  // kDisableEnv
    bool containerMember = false; // vpath::isMember()
    bool fullScreen = false;      // full screen (target) or slideshow
};

// Cheap checks before the (slow) query; Decision::Show = ask the system.
Decision beforeQuery(const Situation& situation);
// The answer: Decision::Show = show the bar for status.type.
Decision afterQuery(const Situation& situation, bool installed, const DefaultViewerStatus& status,
                    const QStringList& askedTypes);

// Persistent state (Settings).
bool isEnabled();
void setEnabled(bool on); // "Nicht mehr fragen" = false
QStringList askedTypes();
void markAsked(const QString& type); // dismissed or "Festlegen"
bool disabledByEnvironment();
// The current situation for a file shown in a window (settings, environment, path).
Situation situationFor(const QString& path, bool fullScreen);

// Helpers for the platforms' Shell::isInstalledApp().
// `path` lies inside one of `folders` (string comparison of clean absolute paths, case
// sensitive; "/Applications/Lumaflip.app" is inside "/Applications").
bool isInsideAny(const QString& path, const QStringList& folders);
// `dir` or one of its parents (up to `levels`) holds a CMakeCache.txt: a build tree
// (blocking file system access).
bool isInBuildTree(const QString& dir, int levels = 4);

} // namespace lf::defaultapp
