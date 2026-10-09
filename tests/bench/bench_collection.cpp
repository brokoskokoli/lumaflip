// SPDX-License-Identifier: GPL-3.0-or-later
// P-05: enumerate and sort a folder with 10 000 files (by name) in < 500 ms.
// Measures what the Collection worker does: enumerate, fill platform attributes, sort.

#include "Bench.h"

#include "core/Collection.h"
#include "core/Platform.h"

#include <QElapsedTimer>
#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>

LF_BENCH(P_05_EnumerateAndSort10k, 500.0) {
    QTemporaryDir dir;
    if (!dir.isValid())
        return -1.0;
    // Realistic camera/export style names in random creation order (not timed).
    QRandomGenerator rng(42);
    for (int i = 0; i < 10000; ++i) {
        QString name;
        switch (i % 4) {
        case 0:
            name = QStringLiteral("IMG_%1.JPG").arg(rng.bounded(100000));
            break;
        case 1:
            name = QStringLiteral("DSC%1 (%2).jpg").arg(rng.bounded(10000)).arg(i);
            break;
        case 2:
            name = QStringLiteral("Urlaub %1 Strand %2.heic").arg(rng.bounded(30)).arg(i);
            break;
        default:
            name = QStringLiteral("photo_%1.png").arg(i);
            break;
        }
        QFile f(dir.filePath(QStringLiteral("%1_%2").arg(i).arg(name)));
        if (!f.open(QIODevice::WriteOnly))
            return -1.0;
    }
    auto collator = lf::createNameCollator();
    auto attributes = lf::createFileAttributeProvider();

    QElapsedTimer t;
    t.start();
    bool denied = false;
    QVector<lf::FileEntry> entries = lf::collection::enumerateFolder(dir.path(), &denied);
    for (auto& e : entries)
        attributes->fill(e);
    lf::collection::sortEntries(entries, lf::SortSpec{lf::SortKey::Name, true}, *collator);
    const double ms = double(t.nsecsElapsed()) / 1e6;
    if (entries.size() != 10000)
        return 1e9; // wrong result counts as failure
    return ms;
}
