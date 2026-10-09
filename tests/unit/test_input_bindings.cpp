// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/InputBindings.h"
#include "core/Settings.h"

#include <QCoreApplication>
#include <QSet>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <vector>

using namespace lf;

namespace {

void ensureInputTestApp() {
    if (QCoreApplication::instance())
        return;
    static int argc = 1;
    static char name[] = "lf_unit_tests";
    static char* argv[] = {name, nullptr};
    new QCoreApplication(argc, argv);
}

QString freshInputIni() {
    static QTemporaryDir dir;
    static int counter = 0;
    return dir.filePath(QStringLiteral("input-%1.ini").arg(++counter));
}

// ---- event builders ----
InputEvent keyEv(int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    InputEvent e;
    e.kind = InputKind::Key;
    e.key = key;
    e.modifiers = mods;
    return e;
}
InputEvent wheelEv(int steps, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    InputEvent e;
    e.kind = InputKind::Wheel;
    e.wheelSteps = steps;
    e.modifiers = mods;
    return e;
}
InputEvent kindEv(InputKind k, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    InputEvent e;
    e.kind = k;
    e.modifiers = mods;
    return e;
}
InputEvent swipeEv(int dir) {
    InputEvent e;
    e.kind = InputKind::Swipe;
    e.swipeDirection = dir;
    return e;
}
InputEvent zoomed(InputEvent e) {
    e.zoomed = true;
    return e;
}
InputEvent atEdge(InputEvent e) {
    e.atScrollEdge = true;
    return e;
}
InputEvent animated(InputEvent e) {
    e.animated = true;
    return e;
}
InputEvent multiPage(InputEvent e) {
    e.multiPage = true;
    return e;
}
InputEvent leftHalf(InputEvent e) {
    e.inLeftHalf = true;
    return e;
}
InputEvent culling(InputEvent e) {
    e.inCullingMode = true;
    return e;
}

struct Row {
    const char* slot;
    const char* choice;
    InputEvent ev;
    Action expected;
    const char* what;
};

constexpr auto Cmd = Qt::ControlModifier;  // ⌘ on macOS
constexpr auto Opt = Qt::AltModifier;      // ⌥
constexpr auto Shift = Qt::ShiftModifier;  // ⇧

// The SPEC 4.14 slot table: every slot x every choice with the expected action(s).
std::vector<Row> slotTable() {
    using A = Action;
    return {
        // F-IN-01 wheel (positive steps = wheel down)
        {"wheel", "image", wheelEv(1), A::NextImage, "down"},
        {"wheel", "image", wheelEv(-1), A::PreviousImage, "up"},
        {"wheel", "image", zoomed(wheelEv(1)), A::NextImage, "zoomed down"},
        {"wheel", "zoom", wheelEv(-1), A::ZoomIn, "up"},
        {"wheel", "zoom", wheelEv(2), A::ZoomOut, "down"},
        {"wheel", "scrollOrImage", wheelEv(1), A::NextImage, "not zoomed"},
        {"wheel", "scrollOrImage", wheelEv(-1), A::PreviousImage, "not zoomed up"},
        {"wheel", "scrollOrImage", zoomed(wheelEv(1)), A::ScrollDown, "zoomed"},
        {"wheel", "scrollOrImage", zoomed(wheelEv(-1)), A::ScrollUp, "zoomed up"},
        // F-IN-02 wheel with modifiers
        {"wheel.cmd", "image", wheelEv(1, Cmd), A::NextImage, ""},
        {"wheel.cmd", "zoom", wheelEv(-1, Cmd), A::ZoomIn, ""},
        {"wheel.cmd", "scrollOrImage", zoomed(wheelEv(1, Cmd)), A::ScrollDown, ""},
        {"wheel.cmd", "scrollOrImage", wheelEv(1, Cmd), A::NextImage, ""},
        {"wheel.cmd", "scrollHorizontal", wheelEv(1, Cmd), A::ScrollRight, ""},
        {"wheel.alt", "image", wheelEv(-1, Opt), A::PreviousImage, ""},
        {"wheel.alt", "zoom", wheelEv(1, Opt), A::ZoomOut, ""},
        {"wheel.alt", "scrollOrImage", zoomed(wheelEv(-1, Opt)), A::ScrollUp, ""},
        {"wheel.alt", "scrollOrImage", wheelEv(1, Opt), A::NextImage, ""},
        {"wheel.alt", "scrollHorizontal", wheelEv(-1, Opt), A::ScrollLeft, ""},
        {"wheel.shift", "image", wheelEv(1, Shift), A::NextImage, ""},
        {"wheel.shift", "zoom", wheelEv(-1, Shift), A::ZoomIn, ""},
        {"wheel.shift", "scrollOrImage", zoomed(wheelEv(1, Shift)), A::ScrollDown, ""},
        {"wheel.shift", "scrollOrImage", wheelEv(-1, Shift), A::PreviousImage, ""},
        {"wheel.shift", "scrollHorizontal", wheelEv(1, Shift), A::ScrollRight, ""},
        // F-IN-04 PageUp/PageDown
        {"pageUpDown", "image", keyEv(Qt::Key_PageDown), A::NextImage, ""},
        {"pageUpDown", "image", keyEv(Qt::Key_PageUp), A::PreviousImage, ""},
        {"pageUpDown", "jump", keyEv(Qt::Key_PageDown), A::JumpForward, ""},
        {"pageUpDown", "jump", keyEv(Qt::Key_PageUp), A::JumpBackward, ""},
        {"pageUpDown", "folder", keyEv(Qt::Key_PageDown), A::NextFolder, ""},
        {"pageUpDown", "folder", keyEv(Qt::Key_PageUp), A::PreviousFolder, ""},
        {"pageUpDown", "page", multiPage(keyEv(Qt::Key_PageDown)), A::NextPage, "multi-page"},
        {"pageUpDown", "page", multiPage(keyEv(Qt::Key_PageUp)), A::PreviousPage, "multi-page"},
        {"pageUpDown", "page", keyEv(Qt::Key_PageDown), A::NextImage, "single page"},
        {"pageUpDown", "scroll", zoomed(keyEv(Qt::Key_PageDown)), A::ScrollPageDown, "zoomed"},
        {"pageUpDown", "scroll", zoomed(keyEv(Qt::Key_PageUp)), A::ScrollPageUp, "zoomed"},
        {"pageUpDown", "scroll", keyEv(Qt::Key_PageUp), A::PreviousImage, "not zoomed"},
        // F-IN-05 Home/End
        {"homeEnd", "image", keyEv(Qt::Key_Home), A::FirstImage, ""},
        {"homeEnd", "image", zoomed(keyEv(Qt::Key_End)), A::LastImage, "zoomed"},
        {"homeEnd", "scroll", zoomed(keyEv(Qt::Key_Home)), A::ScrollHome, "zoomed"},
        {"homeEnd", "scroll", zoomed(keyEv(Qt::Key_End)), A::ScrollEnd, "zoomed"},
        {"homeEnd", "scroll", keyEv(Qt::Key_End), A::LastImage, "not zoomed"},
        // F-IN-06 Space / Shift+Space
        {"space", "image", keyEv(Qt::Key_Space), A::NextImage, ""},
        {"space", "image", keyEv(Qt::Key_Space, Shift), A::PreviousImage, ""},
        {"space", "image", animated(keyEv(Qt::Key_Space)), A::NextImage, "animated"},
        {"space", "animationOrImage", animated(keyEv(Qt::Key_Space)), A::ToggleAnimation, "animated"},
        {"space", "animationOrImage", keyEv(Qt::Key_Space), A::NextImage, "still image"},
        {"space", "animationOrImage", keyEv(Qt::Key_Space, Shift), A::PreviousImage, "still image"},
        {"space", "fitActual", keyEv(Qt::Key_Space), A::ToggleFitActualAtCursor, ""},
        {"space", "slideshow", keyEv(Qt::Key_Space), A::ToggleSlideshow, ""},
        // F-IN-07 Left/Right when zoomed
        {"arrowsZoomed", "image", zoomed(keyEv(Qt::Key_Right)), A::NextImage, ""},
        {"arrowsZoomed", "image", zoomed(keyEv(Qt::Key_Left)), A::PreviousImage, ""},
        {"arrowsZoomed", "image", zoomed(keyEv(Qt::Key_Left, Opt)), A::ScrollLeft, "⌥ pans"},
        {"arrowsZoomed", "image", keyEv(Qt::Key_Right), A::NextImage, "not zoomed"},
        {"arrowsZoomed", "scrollThenImage", zoomed(keyEv(Qt::Key_Right)), A::ScrollRight, "inside"},
        {"arrowsZoomed", "scrollThenImage", atEdge(zoomed(keyEv(Qt::Key_Right))), A::NextImage, "at edge"},
        {"arrowsZoomed", "scrollThenImage", atEdge(zoomed(keyEv(Qt::Key_Left))), A::PreviousImage, "at edge"},
        {"arrowsZoomed", "scrollThenImage", keyEv(Qt::Key_Left), A::PreviousImage, "not zoomed"},
        {"arrowsZoomed", "scroll", zoomed(keyEv(Qt::Key_Right)), A::ScrollRight, ""},
        {"arrowsZoomed", "scroll", atEdge(zoomed(keyEv(Qt::Key_Right))), A::ScrollRight, "at edge"},
        {"arrowsZoomed", "scroll", zoomed(keyEv(Qt::Key_Right, Opt)), A::NextImage, "⌥ flips"},
        {"arrowsZoomed", "scroll", zoomed(keyEv(Qt::Key_Left, Opt)), A::PreviousImage, "⌥ flips"},
        {"arrowsZoomed", "scroll", keyEv(Qt::Key_Right), A::NextImage, "not zoomed"},
        // F-IN-08 Up/Down
        {"upDown", "scrollOrImage", zoomed(keyEv(Qt::Key_Down)), A::ScrollDown, "zoomed"},
        {"upDown", "scrollOrImage", zoomed(keyEv(Qt::Key_Up)), A::ScrollUp, "zoomed"},
        {"upDown", "scrollOrImage", keyEv(Qt::Key_Down), A::NextImage, "not zoomed"},
        {"upDown", "scrollOrImage", keyEv(Qt::Key_Up), A::PreviousImage, "not zoomed"},
        {"upDown", "scrollOrNothing", zoomed(keyEv(Qt::Key_Down)), A::ScrollDown, "zoomed"},
        {"upDown", "scrollOrNothing", keyEv(Qt::Key_Down), A::None, "not zoomed"},
        {"upDown", "image", zoomed(keyEv(Qt::Key_Down)), A::NextImage, ""},
        {"upDown", "image", keyEv(Qt::Key_Up), A::PreviousImage, ""},
        {"upDown", "zoom", keyEv(Qt::Key_Up), A::ZoomIn, ""},
        {"upDown", "zoom", keyEv(Qt::Key_Down), A::ZoomOut, ""},
        // F-IN-09 click
        {"click", "none", kindEv(InputKind::Click), A::None, ""},
        {"click", "fitActual", kindEv(InputKind::Click), A::ToggleFitActualAtCursor, ""},
        {"click", "image", kindEv(InputKind::Click), A::NextImage, "right half"},
        {"click", "image", leftHalf(kindEv(InputKind::Click)), A::PreviousImage, "left half"},
        // F-IN-10 double click
        {"doubleClick", "fullscreen", kindEv(InputKind::DoubleClick), A::ToggleFullScreen, ""},
        {"doubleClick", "fitActual", kindEv(InputKind::DoubleClick), A::ToggleFitActualAtCursor, ""},
        {"doubleClick", "close", kindEv(InputKind::DoubleClick), A::CloseWindow, ""},
        // F-IN-11 middle click
        {"middleClick", "none", kindEv(InputKind::MiddleClick), A::None, ""},
        {"middleClick", "fullscreen", kindEv(InputKind::MiddleClick), A::ToggleFullScreen, ""},
        {"middleClick", "close", kindEv(InputKind::MiddleClick), A::CloseWindow, ""},
        {"middleClick", "fitActual", kindEv(InputKind::MiddleClick), A::ToggleFitActualAtCursor, ""},
        {"middleClick", "magnifier", kindEv(InputKind::MiddleClick), A::Magnifier, ""},
        // F-IN-12 back/forward buttons
        {"backForward", "image", kindEv(InputKind::BackButton), A::PreviousImage, ""},
        {"backForward", "image", kindEv(InputKind::ForwardButton), A::NextImage, ""},
        {"backForward", "none", kindEv(InputKind::BackButton), A::None, ""},
        {"backForward", "none", kindEv(InputKind::ForwardButton), A::None, ""},
        // F-IN-13 swipe (-1 = fingers move left = next, like Preview)
        {"swipe", "image", swipeEv(-1), A::NextImage, ""},
        {"swipe", "image", swipeEv(1), A::PreviousImage, ""},
        {"swipe", "image", zoomed(swipeEv(-1)), A::ScrollRight, "zoomed scrolls"},
        {"swipe", "scroll", swipeEv(1), A::ScrollLeft, ""},
        {"swipe", "scroll", zoomed(swipeEv(-1)), A::ScrollRight, ""},
        {"swipe", "off", swipeEv(-1), A::None, ""},
        {"swipe", "off", zoomed(swipeEv(1)), A::None, ""},
        // F-IN-14 drag
        {"drag", "pan", kindEv(InputKind::Drag), A::StartPan, ""},
        {"drag", "pan", kindEv(InputKind::Drag, Opt), A::DragOut, "⌥"},
        {"drag", "dragOut", kindEv(InputKind::Drag), A::DragOut, ""},
        {"drag", "dragOut", kindEv(InputKind::Drag, Opt), A::StartPan, "⌥"},
        // F-IN-15 Esc
        {"esc", "exitMode", keyEv(Qt::Key_Escape), A::ExitModeOrNothing, ""},
        {"esc", "exitModeOrClose", keyEv(Qt::Key_Escape), A::ExitModeOrCloseWindow, ""},
        // rating keys (5.2 conflict rule, F-IN-18 photographer)
        {"ratingKeys", "cullingOnly", keyEv(Qt::Key_1), A::ZoomFit, "outside culling: zoom"},
        {"ratingKeys", "cullingOnly", culling(keyEv(Qt::Key_1)), A::Rate1, "culling: stars"},
        {"ratingKeys", "cullingOnly", keyEv(Qt::Key_P), A::None, "P inactive"},
        {"ratingKeys", "always", keyEv(Qt::Key_1), A::Rate1, ""},
        {"ratingKeys", "always", keyEv(Qt::Key_P), A::RatePick, ""},
    };
}

class InputBindingsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureInputTestApp();
        Settings::useTestingBackend(freshInputIni());
    }

    // Runs every row of `slot` with a fresh binding where only that slot is set.
    void runSlot(const QString& slot) {
        int rows = 0;
        for (const Row& r : slotTable()) {
            if (slot != QLatin1String(r.slot))
                continue;
            ++rows;
            Settings::instance().resetToDefaults();
            InputBindings b;
            b.setChoice(r.slot, r.choice);
            ASSERT_EQ(b.choice(r.slot), QString(r.choice));
            EXPECT_EQ(actionId(b.resolve(r.ev)), actionId(r.expected))
                << r.slot << "=" << r.choice << " (" << r.what << ")";
            // persisted: a newly loaded binding behaves the same
            InputBindings fresh;
            EXPECT_EQ(fresh.resolve(r.ev), r.expected) << r.slot << "=" << r.choice << " reloaded";
        }
        EXPECT_GT(rows, 0) << slot.toStdString();
        // every choice of the slot is exercised
        for (const auto& s : InputBindings::inputSlots()) {
            if (s.id != slot)
                continue;
            for (const auto& c : s.choices) {
                bool found = false;
                for (const Row& r : slotTable())
                    found = found || (slot == QLatin1String(r.slot) && c == QLatin1String(r.choice));
                EXPECT_TRUE(found) << "untested choice " << slot.toStdString() << "=" << c.toStdString();
            }
        }
    }
};

} // namespace

// F-IN-01
TEST_F(InputBindingsTest, F_IN_01_Wheel) { runSlot("wheel"); }
// F-IN-02
TEST_F(InputBindingsTest, F_IN_02_WheelWithModifiers) {
    runSlot("wheel.cmd");
    runSlot("wheel.alt");
    runSlot("wheel.shift");
    // defaults: ⌘ = zoom, ⌥ = scroll, ⇧ = horizontal
    Settings::instance().resetToDefaults();
    InputBindings b;
    EXPECT_EQ(b.resolve(wheelEv(-1, Cmd)), Action::ZoomIn);
    EXPECT_EQ(b.resolve(zoomed(wheelEv(1, Opt))), Action::ScrollDown);
    EXPECT_EQ(b.resolve(wheelEv(1, Shift)), Action::ScrollRight);
    EXPECT_EQ(b.resolve(wheelEv(1)), Action::NextImage);
    EXPECT_EQ(b.resolve(wheelEv(0)), Action::None);
}
// F-IN-03
TEST_F(InputBindingsTest, F_IN_03_WheelTuning) {
    InputBindings b;
    EXPECT_FALSE(b.wheelInverted());
    EXPECT_EQ(b.wheelStepsPerImage(), 1);
    EXPECT_EQ(b.wheelMinIntervalMs(), 120);
    EXPECT_EQ(b.zoomStepPercent(), 15);
    EXPECT_EQ(b.jumpSize(), 10);
    Settings::instance().setValue(key::WheelInverted, true);
    Settings::instance().setValue(key::WheelStepsPerImage, 3);
    Settings::instance().setValue(key::WheelMinIntervalMs, 0);
    Settings::instance().setValue(key::ZoomStepPercent, 50);
    Settings::instance().setValue(key::JumpSize, 25);
    EXPECT_TRUE(b.wheelInverted());
    EXPECT_EQ(b.wheelStepsPerImage(), 3);
    EXPECT_EQ(b.wheelMinIntervalMs(), 0);
    EXPECT_EQ(b.zoomStepPercent(), 50);
    EXPECT_EQ(b.jumpSize(), 25);
    // inverted direction flips the wheel mapping
    EXPECT_EQ(b.resolve(wheelEv(1)), Action::PreviousImage);
    EXPECT_EQ(b.resolve(wheelEv(1, Cmd)), Action::ZoomIn);
    // out of range values are rejected
    Settings::instance().setValue(key::WheelStepsPerImage, 9);
    Settings::instance().setValue(key::WheelMinIntervalMs, 301);
    Settings::instance().setValue(key::ZoomStepPercent, 4);
    EXPECT_EQ(b.wheelStepsPerImage(), 3);
    EXPECT_EQ(b.wheelMinIntervalMs(), 0);
    EXPECT_EQ(b.zoomStepPercent(), 50);
}
// F-IN-04
TEST_F(InputBindingsTest, F_IN_04_PageUpDown) { runSlot("pageUpDown"); }
// F-IN-05
TEST_F(InputBindingsTest, F_IN_05_HomeEnd) { runSlot("homeEnd"); }
// F-IN-06
TEST_F(InputBindingsTest, F_IN_06_Space) { runSlot("space"); }
// F-IN-07
TEST_F(InputBindingsTest, F_IN_07_ArrowsWhenZoomed) { runSlot("arrowsZoomed"); }
// F-IN-08
TEST_F(InputBindingsTest, F_IN_08_UpDown) { runSlot("upDown"); }
// F-IN-09
TEST_F(InputBindingsTest, F_IN_09_Click) { runSlot("click"); }
// F-IN-10
TEST_F(InputBindingsTest, F_IN_10_DoubleClick) { runSlot("doubleClick"); }
// F-IN-11
TEST_F(InputBindingsTest, F_IN_11_MiddleClick) { runSlot("middleClick"); }
// F-IN-12
TEST_F(InputBindingsTest, F_IN_12_BackForwardButtons) { runSlot("backForward"); }
// F-IN-13
TEST_F(InputBindingsTest, F_IN_13_Swipe) { runSlot("swipe"); }
// F-IN-14
TEST_F(InputBindingsTest, F_IN_14_Drag) { runSlot("drag"); }
// F-IN-15
TEST_F(InputBindingsTest, F_IN_15_Esc) { runSlot("esc"); }
TEST_F(InputBindingsTest, RatingKeysSlot) { runSlot("ratingKeys"); }

// F-IN-16
TEST_F(InputBindingsTest, F_IN_16_EndOfCollectionMirrorsWrapMode) {
    InputBindings b;
    EXPECT_EQ(b.choice("endOfCollection"), "stop");
    for (const char* c : {"wrap", "nextFolder", "stop"}) {
        b.setChoice("endOfCollection", c);
        EXPECT_EQ(Settings::instance().value(key::WrapMode).toString(), QString(c));
        InputBindings fresh;
        EXPECT_EQ(fresh.choice("endOfCollection"), QString(c));
    }
    Settings::instance().setValue(key::WrapMode, "wrap");
    b.reload();
    EXPECT_EQ(b.choice("endOfCollection"), "wrap");
}

TEST_F(InputBindingsTest, SlotTableMatchesSpecDefaults) {
    const QHash<QString, QString> expected = {
        {"wheel", "image"},          {"wheel.cmd", "zoom"},
        {"wheel.alt", "scrollOrImage"}, {"wheel.shift", "scrollHorizontal"},
        {"pageUpDown", "image"},     {"homeEnd", "image"},
        {"space", "animationOrImage"}, {"arrowsZoomed", "image"},
        {"upDown", "scrollOrImage"}, {"click", "none"},
        {"doubleClick", "fullscreen"}, {"middleClick", "fitActual"},
        {"backForward", "image"},    {"swipe", "image"},
        {"drag", "pan"},             {"esc", "exitMode"},
        {"endOfCollection", "stop"}, {"ratingKeys", "cullingOnly"},
    };
    InputBindings b;
    QSet<QString> seen;
    for (const auto& s : InputBindings::inputSlots()) {
        seen.insert(s.id);
        ASSERT_TRUE(expected.contains(s.id)) << s.id.toStdString();
        EXPECT_EQ(s.defaultChoice, expected.value(s.id)) << s.id.toStdString();
        EXPECT_EQ(b.choice(s.id), s.defaultChoice) << s.id.toStdString();
        EXPECT_FALSE(s.label.isEmpty());
        EXPECT_EQ(s.choices.size(), s.choiceLabels.size());
        EXPECT_EQ(s.choices.size(), s.choiceHelp.size());  // F-IN-17 help per choice
        EXPECT_TRUE(s.choices.contains(s.defaultChoice));
    }
    EXPECT_EQ(seen.size(), expected.size());
    // invalid choices are ignored
    b.setChoice("wheel", "teleport");
    EXPECT_EQ(b.choice("wheel"), "image");
}

// F-IN-18
TEST_F(InputBindingsTest, F_IN_18_Presets) {
    InputBindings::applyPreset("photographer");
    {
        InputBindings b;
        EXPECT_EQ(b.resolve(wheelEv(-1)), Action::ZoomIn);       // wheel = zoom
        EXPECT_EQ(b.resolve(keyEv(Qt::Key_3)), Action::Rate3);   // digits = stars
        EXPECT_EQ(b.resolve(keyEv(Qt::Key_0)), Action::Rate0);
        EXPECT_EQ(b.resolve(keyEv(Qt::Key_P)), Action::RatePick);
        EXPECT_EQ(b.resolve(keyEv(Qt::Key_X)), Action::RateReject);
        EXPECT_EQ(b.resolve(keyEv(Qt::Key_U)), Action::RateClear);
        EXPECT_EQ(b.resolve(keyEv(Qt::Key_0, Cmd)), Action::ZoomActual);  // ⌘0 still zooms
    }
    InputBindings::applyPreset("windows");
    {
        InputBindings b;
        EXPECT_EQ(b.resolve(wheelEv(1)), Action::NextImage);
        EXPECT_EQ(b.resolve(keyEv(Qt::Key_PageDown)), Action::NextImage);
        EXPECT_EQ(b.resolve(keyEv(Qt::Key_Return)), Action::ToggleFullScreen);
        EXPECT_EQ(b.resolve(keyEv(Qt::Key_F2)), Action::Rename);
        EXPECT_EQ(b.resolve(keyEv(Qt::Key_Escape)), Action::ExitModeOrCloseWindow);
        EXPECT_EQ(b.resolve(keyEv(Qt::Key_1)), Action::ZoomFit);
        EXPECT_TRUE(b.conflicts().isEmpty());
    }
    InputBindings::applyPreset("mac");
    {
        InputBindings b;
        for (const auto& s : InputBindings::inputSlots())
            EXPECT_EQ(b.choice(s.id), s.defaultChoice) << s.id.toStdString();
        EXPECT_EQ(b.resolve(keyEv(Qt::Key_Return)), Action::Rename);
        EXPECT_EQ(b.resolve(keyEv(Qt::Key_Escape)), Action::ExitModeOrNothing);
        EXPECT_TRUE(b.conflicts().isEmpty());
    }
    // unknown preset: no change
    InputBindings::applyPreset("amiga");
    EXPECT_EQ(InputBindings().choice("wheel"), "image");
}

// ---- default keyboard shortcuts (SPEC 5.2). ⌘ = Qt::ControlModifier, ⌃ = Qt::MetaModifier ----

// F-NAV-01
TEST_F(InputBindingsTest, F_NAV_01_ArrowKeysFlip) {
    InputBindings b;
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_Right)), Action::NextImage);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_Left)), Action::PreviousImage);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_Space)), Action::NextImage);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_Space, Shift)), Action::PreviousImage);
    EXPECT_EQ(b.resolve(animated(keyEv(Qt::Key_Space))), Action::ToggleAnimation);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_PageDown)), Action::NextImage);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_PageUp)), Action::PreviousImage);
    EXPECT_EQ(b.resolve(kindEv(InputKind::ForwardButton)), Action::NextImage);
    EXPECT_EQ(b.resolve(wheelEv(1)), Action::NextImage);
}

// F-NAV-02
TEST_F(InputBindingsTest, F_NAV_02_FirstLast) {
    InputBindings b;
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_Home)), Action::FirstImage);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_End)), Action::LastImage);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_Up, Cmd)), Action::FirstImage);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_Down, Cmd)), Action::LastImage);
}

// F-NAV-05
TEST_F(InputBindingsTest, F_NAV_05_DefaultMouseMapping) {
    InputBindings b;
    EXPECT_EQ(b.resolve(kindEv(InputKind::Click)), Action::None);
    EXPECT_EQ(b.resolve(kindEv(InputKind::DoubleClick)), Action::ToggleFullScreen);
    EXPECT_EQ(b.resolve(kindEv(InputKind::MiddleClick)), Action::ToggleFitActualAtCursor);
    EXPECT_EQ(b.resolve(kindEv(InputKind::BackButton)), Action::PreviousImage);
}

// F-VIEW-02
TEST_F(InputBindingsTest, F_VIEW_02_ZoomShortcuts) {
    InputBindings b;
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_0, Cmd)), Action::ZoomActual);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_9, Cmd)), Action::ZoomFit);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_Plus, Cmd)), Action::ZoomIn);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_Equal, Cmd)), Action::ZoomIn);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_Minus, Cmd)), Action::ZoomOut);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_Plus)), Action::ZoomIn);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_Plus, Shift)), Action::ZoomIn);  // "+" typed with ⇧
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_Minus)), Action::ZoomOut);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_1)), Action::ZoomFit);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_2)), Action::ZoomFillWidth);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_3)), Action::ZoomActual);
    // conflict rule: in culling mode digits are stars
    EXPECT_EQ(b.resolve(culling(keyEv(Qt::Key_1))), Action::Rate1);
    EXPECT_EQ(b.resolve(culling(keyEv(Qt::Key_3))), Action::Rate3);
    EXPECT_EQ(b.resolve(culling(keyEv(Qt::Key_0, Cmd))), Action::ZoomActual);
}

TEST_F(InputBindingsTest, Keys_ZoomToggleAndLock) {
    InputBindings b;
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_Z)), Action::ToggleFitActualAtCursor);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_Z, Shift)), Action::ToggleZoomLock);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_Z, Cmd)), Action::Undo);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_Z, Cmd | Shift)), Action::Redo);
}

TEST_F(InputBindingsTest, Keys_OptionArrowsPanWhenZoomed) {
    InputBindings b;
    EXPECT_EQ(b.resolve(zoomed(keyEv(Qt::Key_Right, Opt))), Action::ScrollRight);
    EXPECT_EQ(b.resolve(zoomed(keyEv(Qt::Key_Down))), Action::ScrollDown);
}

TEST_F(InputBindingsTest, Keys_FullScreen) {
    InputBindings b;
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_F, Cmd | Qt::MetaModifier)), Action::ToggleFullScreen);  // ⌃⌘F
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_F)), Action::ToggleFullScreen);
    EXPECT_EQ(b.resolve(kindEv(InputKind::DoubleClick)), Action::ToggleFullScreen);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_Escape)), Action::ExitModeOrNothing);
}

TEST_F(InputBindingsTest, SPEC_5_2_DefaultShortcutTable) {
    InputBindings b;
    struct K {
        int key;
        Qt::KeyboardModifiers mods;
        Action a;
    };
    const K table[] = {
        {Qt::Key_N, Cmd, Action::NewWindow},
        {Qt::Key_N, Cmd | Opt, Action::DuplicateWindow},
        {Qt::Key_W, Cmd, Action::CloseWindow},
        {Qt::Key_W, Cmd | Opt, Action::CloseAllWindows},
        {Qt::Key_F, Cmd | Shift, Action::ToggleSlideshow},
        {Qt::Key_S, {}, Action::ToggleSlideshow},
        {Qt::Key_L, Cmd, Action::RotateLeft},
        {Qt::Key_R, Cmd, Action::RotateRight},
        {Qt::Key_H, {}, Action::FlipHorizontal},
        {Qt::Key_H, Shift, Action::FlipVertical},
        {Qt::Key_I, Cmd, Action::ToggleInfoPanel},
        {Qt::Key_I, {}, Action::ToggleInfoPanel},
        {Qt::Key_T, {}, Action::ToggleFilmstrip},
        {Qt::Key_G, {}, Action::ToggleGrid},
        {Qt::Key_Backspace, Cmd, Action::Trash},
        {Qt::Key_Delete, {}, Action::Trash},
        {Qt::Key_Return, {}, Action::Rename},
        {Qt::Key_F2, {}, Action::Rename},
        {Qt::Key_R, Cmd | Shift, Action::RevealInFinder},
        {Qt::Key_C, Cmd, Action::CopyImage},
        {Qt::Key_C, Cmd | Opt, Action::CopyPath},
        {Qt::Key_E, {}, Action::OpenInEditor},
        {Qt::Key_Period, {}, Action::NextFrame},
        {Qt::Key_Comma, {}, Action::PreviousFrame},
        {Qt::Key_Down, Opt, Action::NextPage},
        {Qt::Key_Up, Opt, Action::PreviousPage},
        {Qt::Key_Comma, Cmd, Action::OpenPreferences},
        {Qt::Key_Question, Shift, Action::ShowShortcuts},
        {Qt::Key_Question, {}, Action::ShowShortcuts},
        {Qt::Key_L, {}, Action::Magnifier},
    };
    for (const K& k : table)
        EXPECT_EQ(actionId(b.resolve(keyEv(k.key, k.mods))), actionId(k.a))
            << QKeySequence(QKeyCombination(k.mods, Qt::Key(k.key))).toString().toStdString();
    // ⌃ (Meta) is not ⌘: ⌃N does nothing
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_N, Qt::MetaModifier)), Action::None);
    // ⌘0 in Qt notation is "Ctrl+0"
    EXPECT_EQ(b.shortcut(Action::ZoomActual), QKeySequence("Ctrl+0"));
    EXPECT_TRUE(b.conflicts().isEmpty()) << "default table must be conflict free";
}

TEST_F(InputBindingsTest, Keys_RatingKeysInCullingMode) {
    InputBindings b;
    const std::pair<int, Action> keys[] = {
        {Qt::Key_0, Action::Rate0}, {Qt::Key_1, Action::Rate1}, {Qt::Key_2, Action::Rate2},
        {Qt::Key_3, Action::Rate3}, {Qt::Key_4, Action::Rate4}, {Qt::Key_5, Action::Rate5},
        {Qt::Key_P, Action::RatePick}, {Qt::Key_X, Action::RateReject}, {Qt::Key_U, Action::RateClear},
    };
    for (const auto& [k, a] : keys)
        EXPECT_EQ(b.resolve(culling(keyEv(k))), a);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_X)), Action::None);
}

// F-SET-02
TEST_F(InputBindingsTest, F_SET_02_RebindAndConflicts) {
    InputBindings b;
    EXPECT_TRUE(b.conflicts().isEmpty());
    b.setShortcut(Action::ToggleGrid, QKeySequence("T"));  // collides with filmstrip
    auto c = b.conflicts();
    EXPECT_TRUE(c.contains(Action::ToggleGrid));
    EXPECT_TRUE(c.contains(Action::ToggleFilmstrip));
    EXPECT_EQ(b.conflictsWith(Action::ToggleGrid), QVector<Action>{Action::ToggleFilmstrip});
    // rating digits vs zoom digits are not a conflict (different contexts)
    EXPECT_FALSE(c.contains(Action::Rate1));
    EXPECT_FALSE(c.contains(Action::ZoomFit));
    // fix it
    b.setShortcut(Action::ToggleGrid, QKeySequence("Shift+G"));
    EXPECT_TRUE(b.conflicts().isEmpty());
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_G, Shift)), Action::ToggleGrid);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_G)), Action::None);
    // multiple alternatives
    b.setShortcuts(Action::RotateLeft, {QKeySequence("Ctrl+L"), QKeySequence("["), QKeySequence("[")});
    EXPECT_EQ(b.shortcuts(Action::RotateLeft).size(), 2);
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_BracketLeft)), Action::RotateLeft);
    // a rebound key overrides the fixed slot keys
    b.setShortcut(Action::Trash, QKeySequence("Space"));
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_Space)), Action::Trash);
    // clear
    b.setShortcut(Action::OpenInEditor, QKeySequence());
    EXPECT_TRUE(b.shortcuts(Action::OpenInEditor).isEmpty());
    EXPECT_EQ(b.resolve(keyEv(Qt::Key_E)), Action::None);
    // persisted
    InputBindings fresh;
    EXPECT_EQ(fresh.shortcut(Action::ToggleGrid), QKeySequence("Shift+G"));
    EXPECT_TRUE(fresh.shortcuts(Action::OpenInEditor).isEmpty());
    EXPECT_EQ(fresh.resolve(keyEv(Qt::Key_Space)), Action::Trash);
    // reset to the standard table
    fresh.resetShortcuts();
    EXPECT_EQ(fresh.shortcut(Action::ToggleGrid), QKeySequence("G"));
    EXPECT_EQ(fresh.shortcuts(Action::OpenInEditor), InputBindings::defaultShortcuts(Action::OpenInEditor));
    EXPECT_TRUE(fresh.conflicts().isEmpty());
}

TEST_F(InputBindingsTest, ActionIdRoundtrip) {
    QSet<QString> ids;
    for (Action a : InputBindings::allActions()) {
        const QString id = actionId(a);
        EXPECT_FALSE(id.isEmpty());
        EXPECT_FALSE(ids.contains(id)) << "duplicate id " << id.toStdString();
        ids.insert(id);
        EXPECT_EQ(actionFromId(id), a) << id.toStdString();
        EXPECT_FALSE(actionLabel(a).isEmpty());
    }
    EXPECT_EQ(actionFromId("doesNotExist"), Action::None);
    EXPECT_EQ(actionId(Action::None), "none");
    EXPECT_EQ(actionId(Action::NextImage), "nextImage");
    // every enum value except None is listed
    EXPECT_EQ(InputBindings::allActions().size(), int(Action::CloseAllWindows));
}
