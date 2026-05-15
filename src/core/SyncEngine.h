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

#ifndef KEEPASSXC_SYNCENGINE_H
#define KEEPASSXC_SYNCENGINE_H

#include <QMap>
#include <QSharedPointer>
#include <QUuid>

#include "core/SyncData.h"
#include "core/SyncMetadata.h"

class Database;
class Entry;
struct EntrySnapshot;

/**
 * SyncEngine drives the manual sync main chain:
 *
 *   analyzeDiffs() → classify entries → applyMerges() → SyncResult
 *
 * Uses version vectors from SyncMetadataEngine for causality tracking.
 * Falls back to lastModificationTime when VVs are absent (legacy entries).
 */
class SyncEngine
{
public:
    SyncEngine() = default;

    /**
     * Initialize the metadata engine with the local database.
     * Must be called before analyzeDiffs() if the sync metadata engine
     * should be used for version-vector-based classification.
     */
    void setMetadataEngine(QSharedPointer<Database> db);

    /**
     * Access the internal SyncMetadataEngine (e.g. for ConflictResolverService).
     */
    SyncMetadataEngine* metadataEngine();

    /**
     * Analyze differences between two databases and produce a SyncResult
     * with all operations (add, update, conflict, skip).
     *
     * Does NOT write anything to either database — use applyMerges() for that.
     */
    SyncResult analyzeDiffs(QSharedPointer<Database> local, QSharedPointer<Database> remote);

    /**
     * Apply the auto-merge and direct-apply operations from \p result
     * into \p local (the "local" database).
     *
     * \p remote is needed to disambiguate "Remote Only" entries.
     * Does NOT save the database — caller must call local->save() after.
     *
     * Returns false if any operation failed (partial apply may have occurred).
     */
    bool applyMerges(const SyncResult& result,
                     QSharedPointer<Database> local,
                     QSharedPointer<Database> remote = {});

private:
    /** Build entryId → Entry* map for all entries in a database. */
    QMap<QUuid, Entry*> indexEntries(QSharedPointer<Database> db);

    /** Classify a pair of entries (one or both may be nullptr). */
    SyncOperation classifyEntry(Entry* localEntry, Entry* remoteEntry);

    /** Compute all fields where two snapshots differ. */
    QStringList computeTotalDiff(const EntrySnapshot& a, const EntrySnapshot& b);

    /** Find which fields in a diff list overlap between two sets. */
    QStringList findOverlappingFields(const QStringList& fieldsA, const QStringList& fieldsB);

    /** Copy all field values from \p src to \p dst (full overwrite). */
    bool copyEntryValues(Entry* dst, const Entry* src);

    /** Field-level merge: copy values only for \p fields from \p src into \p dst. */
    bool applyFieldValues(Entry* dst, const Entry* src, const QStringList& fields);

    /** Sync metadata engine for version-vector-based classification. */
    SyncMetadataEngine m_metadataEngine;
};

#endif // KEEPASSXC_SYNCENGINE_H
