// SPDX-License-Identifier: GPL-3.0-or-later
// Helper of tools/make_finder_fixtures.sh (JavaScript for Automation, Foundation only -
// controls no app, needs no Automation permission):
//
//   osascript -l JavaScript tools/finder_fixture_meta.js <folder>
//
// Prints JSON with the visible files of <folder> (name, logical size, modification,
// creation and added date, localized kind) and, for every sort key, the file order in
// both directions as Finder's rule computes it: primary key, then the name ascending
// with localizedStandardCompare (Finder's name comparison, F-SORT-01).
ObjC.import('Foundation');

function run(argv) {
    const folder = argv[0];
    const keys = [$.NSURLNameKey, $.NSURLFileSizeKey, $.NSURLContentModificationDateKey,
                  $.NSURLCreationDateKey, $.NSURLAddedToDirectoryDateKey,
                  $.NSURLLocalizedTypeDescriptionKey, $.NSURLIsRegularFileKey];
    const url = $.NSURL.fileURLWithPathIsDirectory(folder, true);
    const urls = $.NSFileManager.defaultManager.contentsOfDirectoryAtURLIncludingPropertiesForKeysOptionsError(
        url, $(keys), $.NSDirectoryEnumerationSkipsHiddenFiles, $());
    if (urls.isNil())
        throw new Error('cannot list ' + folder);
    const ms = (d) => (d.isNil() ? null : Math.round(d.timeIntervalSince1970 * 1000));
    const files = [];
    for (let i = 0; i < urls.count; ++i) {
        const v = urls.objectAtIndex(i).resourceValuesForKeysError($(keys), $());
        if (!ObjC.unwrap(v.objectForKey($.NSURLIsRegularFileKey)))
            continue;
        files.push({
            name: ObjC.unwrap(v.objectForKey($.NSURLNameKey)),
            size: ObjC.unwrap(v.objectForKey($.NSURLFileSizeKey)),
            modifiedMs: ms(v.objectForKey($.NSURLContentModificationDateKey)),
            createdMs: ms(v.objectForKey($.NSURLCreationDateKey)),
            addedMs: ms(v.objectForKey($.NSURLAddedToDirectoryDateKey)),
            kind: ObjC.unwrap(v.objectForKey($.NSURLLocalizedTypeDescriptionKey)),
        });
    }
    const byName = (a, b) => $(a.name).localizedStandardCompare($(b.name));
    const field = {DateModified: 'modifiedMs', DateCreated: 'createdMs', DateAdded: 'addedMs', Size: 'size'};
    const orders = {};
    for (const key of ['Name', 'Kind', 'DateModified', 'DateCreated', 'DateAdded', 'Size']) {
        const primary = (a, b) => {
            if (key === 'Name')
                return byName(a, b);
            if (key === 'Kind')
                return $(a.kind).localizedStandardCompare($(b.kind));
            const x = a[field[key]], y = b[field[key]];
            return x < y ? -1 : x > y ? 1 : 0;
        };
        const sorted = (dir) => files.slice().sort((a, b) => dir * primary(a, b) || byName(a, b)).map((f) => f.name);
        orders[key] = {asc: sorted(1), desc: sorted(-1)};
    }
    for (const f of files) {
        for (const k of ['modified', 'created', 'added'])
            f[k] = f[k + 'Ms'] === null ? null : new Date(f[k + 'Ms']).toISOString();
    }
    return JSON.stringify({files: files, orders: orders}, null, 1);
}
