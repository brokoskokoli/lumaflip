// SPDX-License-Identifier: GPL-3.0-or-later
#include "NavigationOrder.h"

#include <QRandomGenerator>
#include <QVector>

#include <algorithm>

namespace lf {

namespace nav {

std::optional<int> parseGoTo(const QString& text, int current, int count) {
    if (count <= 0)
        return std::nullopt;
    QString t = text.trimmed();
    t.remove(QLatin1Char(' '));
    if (t.isEmpty())
        return std::nullopt;
    const QChar sign = t.front();
    const bool relative = sign == QLatin1Char('+') || sign == QLatin1Char('-') || sign == QChar(0x2212);
    if (relative)
        t.remove(0, 1);
    bool ok = false;
    const qlonglong n = t.toLongLong(&ok);
    if (!ok || n < 0)
        return std::nullopt;
    qlonglong index = 0;
    if (relative)
        index = qlonglong(std::max(0, current)) + (sign == QLatin1Char('+') ? n : -n);
    else
        index = n - 1; // "1" = first image
    return int(std::clamp<qlonglong>(index, 0, count - 1));
}

} // namespace nav

namespace {

quint64 splitmix64(quint64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

} // namespace

ShuffleOrder::ShuffleOrder(quint64 seed) : m_seed(seed) {}

quint64 ShuffleOrder::sessionSeed() {
    static const quint64 seed = QRandomGenerator::global()->generate64();
    return seed;
}

quint64 ShuffleOrder::keyOf(const QString& path) const {
    // FNV-1a over the UTF-16 code units, mixed with the seed (stable across processes).
    quint64 h = 0xCBF29CE484222325ull ^ splitmix64(m_seed);
    for (const QChar c : path) {
        h ^= c.unicode();
        h *= 0x100000001B3ull;
    }
    return splitmix64(h);
}

void ShuffleOrder::setPaths(const QStringList& paths) {
    QVector<std::pair<quint64, QString>> keyed;
    keyed.reserve(paths.size());
    for (const QString& p : paths)
        keyed.append({keyOf(p), p});
    std::sort(keyed.begin(), keyed.end());
    m_order.clear();
    m_pos.clear();
    m_order.reserve(keyed.size());
    for (const auto& [k, p] : keyed) {
        if (m_pos.contains(p))
            continue;
        m_pos.insert(p, int(m_order.size()));
        m_order.append(p);
    }
}

int ShuffleOrder::positionOf(const QString& path) const {
    return m_pos.value(path, -1);
}

QString ShuffleOrder::step(const QString& from, int delta, bool wrap, bool* hitEnd) const {
    if (hitEnd)
        *hitEnd = false;
    const int n = int(m_order.size());
    if (n == 0)
        return {};
    int pos = positionOf(from);
    if (pos < 0)
        return delta >= 0 ? m_order.first() : m_order.last();
    qlonglong to = qlonglong(pos) + delta;
    if (wrap) {
        to = ((to % n) + n) % n;
    } else if (to < 0 || to >= n) {
        if (hitEnd)
            *hitEnd = true;
        to = std::clamp<qlonglong>(to, 0, n - 1);
    }
    return m_order.at(int(to));
}

} // namespace lf
