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

#ifndef KEEPASSXC_SYNCDATA_H
#define KEEPASSXC_SYNCDATA_H

#include <QList>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QUuid>

class Entry;

/**
 * Describes a single field-level conflict between local and remote versions
 * of the same entry.  Used when concurrent modifications touch the same field.
 */
struct SyncFieldConflict
{
    QString fieldName;
    QString localValue;
    QString remoteValue;
};

/**
 * Classification of how an entry should be handled during sync.
 */
struct SyncOperation
{
    enum Type
    {
        DirectApply,  // one side has changes, the other doesn't — copy as-is
        AutoMerge,    // both sides changed but fields don't overlap — field-level merge
        Conflict,     // both sides changed with overlapping fields — needs user resolution
        Skipped,      // no changes on either side
        DeleteLocally // delete entry locally (since remote has deleted it)
    };

    Type type = Skipped;
    QUuid entryId;

    // For DirectApply / AutoMerge — the "source" entry to take changes from
    QSharedPointer<Entry> sourceEntry;

    // For Conflict — the two diverged copies
    QSharedPointer<Entry> localEntry;
    QSharedPointer<Entry> remoteEntry;
    QList<SyncFieldConflict> conflictingFields;

    // Human-readable summary
    QStringList changedFields; // all fields that changed on the "winning" side
};

/**
 * Summary of one sync run — what happened and which entries need attention.
 */
struct SyncResult
{
    bool success = false;
    QString errorMessage;

    int addedCount = 0;
    int updatedCount = 0;
    int deletedCount = 0;
    int conflictCount = 0;
    int skippedCount = 0;

    QList<SyncOperation> operations;

    QStringList warnings;

    bool hasConflicts() const
    {
        return conflictCount > 0;
    }

    QString summaryText() const
    {
        QString s;
        s += QStringLiteral("Sync %1\n").arg(success ? QStringLiteral("succeeded") : QStringLiteral("FAILED"));
        if (!errorMessage.isEmpty()) {
            s += QStringLiteral("  Error: %1\n").arg(errorMessage);
        }
        s += QStringLiteral("  Added:   %1\n").arg(addedCount);
        s += QStringLiteral("  Updated: %1\n").arg(updatedCount);
        s += QStringLiteral("  Deleted: %1\n").arg(deletedCount);
        s += QStringLiteral("  Skipped: %1\n").arg(skippedCount);
        s += QStringLiteral("  Conflicts: %1\n").arg(conflictCount);
        if (hasConflicts()) {
            s += QStringLiteral("\nConflicting entries:\n");
            for (const auto& op : operations) {
                if (op.type == SyncOperation::Conflict) {
                    s += QStringLiteral("  - %1 (%2)\n")
                             .arg(op.entryId.toString(QUuid::Id128))
                             .arg(op.changedFields.join(QStringLiteral(", ")));
                }
            }
        }
        if (!warnings.isEmpty()) {
            s += QStringLiteral("\nWarnings:\n");
            for (const auto& w : warnings) {
                s += QStringLiteral("  - %1\n").arg(w);
            }
        }
        return s;
    }
};

#endif // KEEPASSXC_SYNCDATA_H
