// SPDX-License-Identifier: GPL-3.0-or-later
// Portable folder watching (F-OPEN-07) via QFileSystemWatcher, ref-counted per folder.

#include "core/Platform.h"

#include <QFileSystemWatcher>
#include <QHash>

namespace lf {

namespace {

class GenericFolderWatcher final : public FolderWatcher {
public:
    explicit GenericFolderWatcher(QObject* parent) : FolderWatcher(parent), m_watcher(this) {
        QObject::connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this,
                         [this](const QString& dir) { emit folderChanged(m_original.value(dir, dir)); });
    }

    void watch(const QString& folderPath) override {
        if (m_refs[folderPath]++ == 0) {
            m_watcher.addPath(folderPath);
            m_original.insert(folderPath, folderPath);
        }
    }

    void unwatch(const QString& folderPath) override {
        auto it = m_refs.find(folderPath);
        if (it == m_refs.end())
            return;
        if (--*it == 0) {
            m_refs.erase(it);
            m_original.remove(folderPath);
            m_watcher.removePath(folderPath);
        }
    }

private:
    QFileSystemWatcher m_watcher;
    QHash<QString, int> m_refs;
    QHash<QString, QString> m_original;
};

} // namespace

std::unique_ptr<FolderWatcher> createFolderWatcher(QObject* parent) {
    return std::make_unique<GenericFolderWatcher>(parent);
}

} // namespace lf
