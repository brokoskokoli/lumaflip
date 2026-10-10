// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// EDR capabilities of the displays (F-COL-03, SPEC 8.4): how far above SDR white the screen
// of a window can show right now (NSScreen.maximumExtendedDynamicRangeColorComponentValue,
// changes with the brightness and while EDR content is on screen) and at most
// (maximumPotentialExtendedDynamicRangeColorComponentValue; > 1 = EDR capable).

#include "core/HdrMapping.h"

#include <QObject>

#include <functional>

class QWidget;

namespace lf {

// Screen of `widget`'s window (main screen without a native window). {1, 1} on platforms
// or screens without EDR.
hdr::DisplayState edrDisplayState(const QWidget* widget);
// Any connected screen can show EDR (decode HDR content with extended range, F-COL-03).
bool anyScreenSupportsEdr();
// The extended range presentation can be used at all (macOS with the Cocoa platform; not
// with the offscreen platform of the tests).
bool edrPresentationAvailable();

// Calls `changed` (GUI thread) when the screen configuration changes (displays added or
// removed, arrangement, EDR capability). Brightness changes have no notification: the view
// polls edrDisplayState() while it shows HDR content.
class EdrDisplayWatcher : public QObject {
    Q_OBJECT
public:
    explicit EdrDisplayWatcher(std::function<void()> changed, QObject* parent = nullptr);
    ~EdrDisplayWatcher() override;

private:
    void* m_token = nullptr;
};

} // namespace lf
