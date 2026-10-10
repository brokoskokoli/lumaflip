// SPDX-License-Identifier: GPL-3.0-or-later
// No file tags on this platform (F-INFO-06): reading gives an empty list, writing fails.

#include "core/Platform.h"

#include <QCoreApplication>

namespace lf {

namespace {

class GenericTagService final : public TagService {
public:
    bool isSupported() const override { return false; }
    std::optional<QStringList> tags(const QString&, QString*) override { return QStringList(); }
    bool setTags(const QString&, const QStringList&, QString* error) override {
        if (error)
            *error = QCoreApplication::translate("lf::TagService",
                                                 "Tags werden auf diesem System nicht unterstützt.");
        return false;
    }
};

} // namespace

std::unique_ptr<TagService> createTagService() {
    return std::make_unique<GenericTagService>();
}

} // namespace lf
