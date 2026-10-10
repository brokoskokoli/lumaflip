// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Process-wide objects owned by the application object (so their worker threads are
// joined before Qt shuts down). A raw static pointer to such an object dangles once the
// application object is destroyed (several in one test process, a host that recreates
// it); AppScoped keeps a QPointer and creates the object again, for the current
// application object, the next time it is asked for.
//
//   static AppScoped<Foo> s([](QObject* parent) { return new Foo(parent); });
//   Foo& foo = s.get();
//
// get() may be called from any thread; the object itself lives on the thread of its parent.

#include <QCoreApplication>
#include <QMutex>
#include <QObject>
#include <QPointer>

#include <functional>

namespace lf {

template <class T> class AppScoped {
public:
    using Factory = std::function<T*(QObject* parent)>;
    explicit AppScoped(Factory factory) : m_factory(std::move(factory)) {}
    AppScoped(const AppScoped&) = delete;
    AppScoped& operator=(const AppScoped&) = delete;

    // The object owned by `parent` (default: the application object); created if there is
    // none yet or the previous one was destroyed with its parent.
    T& get(QObject* parent = QCoreApplication::instance()) {
        QMutexLocker lock(&m_mutex);
        if (!m_object || m_parent != parent) {
            if (m_object && m_object->parent() == m_parent)
                m_object->deleteLater(); // parent changed while the old one lives: retire it
            m_object = m_factory(parent);
            m_parent = parent;
        }
        return *m_object;
    }
    // The current object, or nullptr (never creates one).
    T* existing() const {
        QMutexLocker lock(&m_mutex);
        return m_object.data();
    }

private:
    Factory m_factory;
    mutable QMutex m_mutex;
    QPointer<T> m_object;
    QPointer<QObject> m_parent;
};

} // namespace lf
