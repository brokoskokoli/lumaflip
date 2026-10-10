// SPDX-License-Identifier: GPL-3.0-or-later
// Real NSEvents for integration tests (see NativeEvents.h).

#include "NativeEvents.h"

#include <QAction>
#include <QGuiApplication>
#include <QWidget>
#include <QWindow>

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>

namespace lf::test {

namespace {

NSWindow* nativeWindow(QWidget* widget) {
    QWidget* top = widget ? widget->window() : nullptr;
    if (!top || !top->windowHandle() || !top->windowHandle()->handle())
        return nil;
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(top->windowHandle()->winId());
    return view.window;
}

// The virtual key code that types `c` (unshifted, or shifted if `shifted`) on the current
// keyboard layout, so Qt's layout-based key mapping sees the same key a user would press.
int keyCodeForCharacter(QChar c, bool* shifted) {
    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    if (!source)
        return -1;
    int found = -1;
    auto* data = static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
    if (data) {
        const auto* layout = reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(data));
        for (UInt32 shift : {0u, 1u}) {
            for (UInt16 code = 0; code < 128 && found < 0; ++code) {
                UInt32 dead = 0;
                UniChar chars[4];
                UniCharCount len = 0;
                const UInt32 mods = shift ? (shiftKey >> 8) & 0xff : 0;
                if (UCKeyTranslate(layout, code, kUCKeyActionDown, mods, LMGetKbdType(),
                                   kUCKeyTranslateNoDeadKeysBit, &dead, 4, &len, chars) == noErr &&
                    len == 1 && chars[0] == c.unicode()) {
                    found = code;
                    *shifted = shift != 0;
                }
            }
            if (found >= 0)
                break;
        }
    }
    CFRelease(source);
    return found;
}

struct NativeKey {
    unsigned short code = 0;
    QString chars; // charactersIgnoringModifiers (Shift applied)
    bool needsShift = false;
};

bool nativeKeyFor(Qt::Key key, bool shift, NativeKey* out) {
    auto fn = [&](unsigned short code, unichar ch) {
        out->code = code;
        out->chars = QString(QChar(ch));
        return true;
    };
    switch (key) {
    case Qt::Key_Left:
        return fn(kVK_LeftArrow, NSLeftArrowFunctionKey);
    case Qt::Key_Right:
        return fn(kVK_RightArrow, NSRightArrowFunctionKey);
    case Qt::Key_Up:
        return fn(kVK_UpArrow, NSUpArrowFunctionKey);
    case Qt::Key_Down:
        return fn(kVK_DownArrow, NSDownArrowFunctionKey);
    case Qt::Key_Home:
        return fn(kVK_Home, NSHomeFunctionKey);
    case Qt::Key_End:
        return fn(kVK_End, NSEndFunctionKey);
    case Qt::Key_PageUp:
        return fn(kVK_PageUp, NSPageUpFunctionKey);
    case Qt::Key_PageDown:
        return fn(kVK_PageDown, NSPageDownFunctionKey);
    case Qt::Key_Backspace:
        return fn(kVK_Delete, NSDeleteCharacter);
    case Qt::Key_Delete:
        return fn(kVK_ForwardDelete, NSDeleteFunctionKey);
    case Qt::Key_Return:
        return fn(kVK_Return, NSCarriageReturnCharacter);
    case Qt::Key_Escape:
        return fn(kVK_Escape, 0x1b);
    case Qt::Key_Space:
        return fn(kVK_Space, ' ');
    case Qt::Key_F2:
        return fn(kVK_F2, NSF2FunctionKey);
    default:
        break;
    }
    if (key < 0x20 || key > 0x7e)
        return false;
    QChar c = QChar(char16_t(key));
    if (c.isLetter())
        c = c.toLower();
    bool shifted = false;
    const int code = keyCodeForCharacter(c, &shifted);
    if (code < 0)
        return false;
    out->code = static_cast<unsigned short>(code);
    out->needsShift = shifted;
    out->chars = (shift || shifted) && c.isLetter() ? QString(c.toUpper()) : QString(c);
    return true;
}

} // namespace

bool nativeEventsAvailable() {
    return QGuiApplication::platformName() == QLatin1String("cocoa") && NSApp != nil;
}

bool sendNativeKey(QWidget* window, Qt::Key key, Qt::KeyboardModifiers mods) {
    if (!nativeEventsAvailable())
        return false;
    @autoreleasepool {
        NSWindow* w = nativeWindow(window);
        NativeKey nk;
        if (!w || !nativeKeyFor(key, mods & Qt::ShiftModifier, &nk))
            return false;
        if (!w.isKeyWindow)
            [w makeKeyAndOrderFront:nil]; // key events go to the key window
        NSEventModifierFlags flags = 0;
        if ((mods & Qt::ShiftModifier) || nk.needsShift)
            flags |= NSEventModifierFlagShift;
        if (mods & Qt::ControlModifier) // ⌘
            flags |= NSEventModifierFlagCommand;
        if (mods & Qt::MetaModifier) // ⌃
            flags |= NSEventModifierFlagControl;
        if (mods & Qt::AltModifier)
            flags |= NSEventModifierFlagOption;
        if (key >= Qt::Key_Left && key <= Qt::Key_Down) // arrows: like the real keyboard
            flags |= NSEventModifierFlagFunction | NSEventModifierFlagNumericPad;
        else if (key >= Qt::Key_Home && key <= Qt::Key_PageDown)
            flags |= NSEventModifierFlagFunction;
        NSString* ignoring = nk.chars.toNSString();
        NSString* chars = ignoring;
        if ((flags & NSEventModifierFlagControl) && nk.chars.size() == 1 && nk.chars.at(0).isLetter())
            chars = QString(QChar(char16_t(nk.chars.at(0).toLower().unicode() & 0x1f))).toNSString();
        for (NSEventType type : {NSEventTypeKeyDown, NSEventTypeKeyUp}) {
            NSEvent* ev = [NSEvent keyEventWithType:type
                                           location:NSZeroPoint
                                      modifierFlags:flags
                                          timestamp:[NSProcessInfo processInfo].systemUptime
                                       windowNumber:w.windowNumber
                                            context:nil
                                         characters:chars
                        charactersIgnoringModifiers:ignoring
                                          isARepeat:NO
                                            keyCode:nk.code];
            if (!ev)
                return false;
            [NSApp sendEvent:ev];
        }
        return true;
    }
}

bool sendNativeShortcut(QWidget* window, const QKeySequence& seq) {
    if (seq.isEmpty())
        return false;
    const QKeyCombination k = seq[0];
    return sendNativeKey(window, k.key(), k.keyboardModifiers());
}

namespace {

NSMenuItem* findItem(NSMenu* menu, NSString* title) {
    for (NSMenuItem* item in menu.itemArray) {
        if ([item.title isEqualToString:title])
            return item;
        if (item.submenu)
            if (NSMenuItem* found = findItem(item.submenu, title))
                return found;
    }
    return nil;
}

} // namespace

QString nativeKeyEquivalent(QAction* action) {
    if (!action || !nativeEventsAvailable())
        return QStringLiteral("<no native item>");
    @autoreleasepool {
        QString title = action->text();
        title.remove(QLatin1Char('&'));
        NSMenuItem* item = findItem(NSApp.mainMenu, title.toNSString());
        if (!item)
            return QStringLiteral("<no native item>");
        QString out;
        const NSEventModifierFlags m = item.keyEquivalentModifierMask;
        if (m & NSEventModifierFlagControl)
            out += QStringLiteral("⌃");
        if (m & NSEventModifierFlagOption)
            out += QStringLiteral("⌥");
        if (m & NSEventModifierFlagShift)
            out += QStringLiteral("⇧");
        if (m & NSEventModifierFlagCommand)
            out += QStringLiteral("⌘");
        return item.keyEquivalent.length ? out + QString::fromNSString(item.keyEquivalent) : QString();
    }
}

} // namespace lf::test
