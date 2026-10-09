// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEMPORARY link shim - delete this file once Collection.cpp is merged.
//
// AUTOMOC compiles the meta objects of all core headers into one object file. Any
// binary that uses ImageProvider/FileOps/FolderWatcher pulls that object in, which
// references Collection's constructor and destructor (implemented by the collection
// module, developed in parallel). These weak placeholders only make the link work;
// they are never meant to run.

#include "Collection.h"

namespace lf {

__attribute__((weak)) Collection::Collection(QObject* parent) : QObject(parent), d(nullptr) {}
__attribute__((weak)) Collection::~Collection() = default;

} // namespace lf
