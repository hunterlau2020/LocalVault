/*
 *  Copyright (C) 2026 KeePassXC Team <team@keepassxc.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 or (at your option)
 *  version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef KEEPASSXC_ENTRYDIFF_H
#define KEEPASSXC_ENTRYDIFF_H

#include <QList>
#include <QString>
#include <QUuid>

#include "core/EntrySnapshot.h"

/**
 * Describes a single field that changed between two entry snapshots.
 */
struct FieldDiff
{
    QString fieldName; // e.g. "title", "username", "custom_fields.MyAttr"
    QString oldValue;
    QString newValue;
};

/**
 * Description of all field-level changes between two EntrySnapshots.
 *
 * Use EntryDiff::compute(before, after) to produce the diff.
 */
struct EntryDiff
{
    /** Entry this diff applies to. */
    QUuid entryId;

    /** List of changed fields with old/new values. */
    QList<FieldDiff> changedFields;

    /** True when no fields changed. */
    bool isEmpty() const;

    /** Convenience: list of changed field names only (no values). */
    QStringList changedFieldNames() const;

    /**
     * Compute the field-level diff between two snapshots of the same entry.
     *
     * Both snapshots should have the same entryId; the result is undefined
     * if they refer to different entries.
     */
    static EntryDiff compute(const EntrySnapshot& before, const EntrySnapshot& after);
};

#endif // KEEPASSXC_ENTRYDIFF_H
