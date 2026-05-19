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

#ifndef KEEPASSXC_LIFECYCLEMANAGER_H
#define KEEPASSXC_LIFECYCLEMANAGER_H

#include <QDateTime>
#include <QList>
#include <QSharedPointer>
#include <QString>

class Database;
class Entry;

/**
 * Policy governing automatic cleanup operations.
 */
struct LifecyclePolicy
{
    int maxSnapshotCount = 20;   // max snapshots to keep (0 = unlimited)
    int maxSnapshotDays = 90;    // max age of snapshots in days (0 = unlimited)
    int entryHistoryLimit = 10;  // max history items per entry (0 = unlimited)
    bool tombstoneCleanupEnabled = false; // tombstone cleanup is manual by default
};

/**
 * Report summarizing what was cleaned up.
 */
struct CleanupExecutionReport
{
    int cleanedSnapshots = 0;
    int cleanedHistoryRecords = 0;
    int cleanedTombstones = 0;
    qint64 reclaimedBytes = 0;
};

class SnapshotService;
class SyncMetadataEngine;

/**
 * LifecycleManager — coordinates cleanup of snapshots, entry history,
 * and tombstones according to a LifecyclePolicy.
 *
 * Reuses SnapshotService for snapshot file/index management and
 * HistoryRetentionService for entry history truncation.  Tombstone
 * cleanup is delegated to a simple age-based filter.
 *
 * Thread-safety: not guaranteed. Use from a single thread only.
 */
class LifecycleManager
{
public:
    explicit LifecycleManager(QSharedPointer<Database> db,
                              SyncMetadataEngine* metadataEngine = nullptr);

    /**
     * Run all cleanup operations: snapshots → entry history → tombstones.
     * Creates an auto-snapshot ("cleanup_pre") before any cleanup.
     *
     * Returns a report with counts of items cleaned and bytes reclaimed.
     */
    CleanupExecutionReport runCleanup(const LifecyclePolicy& policy = LifecyclePolicy{});

    /**
     * Clean up old/excess snapshots using SnapshotService.
     * Returns the number of snapshots removed.
     */
    int cleanupSnapshots(SnapshotService& snapshotService, const LifecyclePolicy& policy);

    /**
     * Enforce the entry history limit across all entries in the database.
     * Returns the total number of history items removed.
     */
    int cleanupEntryHistory(const LifecyclePolicy& policy);

    /**
     * Remove tombstones that are not referenced by any unresolved conflict.
     * Returns the number of tombstones removed.
     */
    int cleanupTombstones(const LifecyclePolicy& policy);

private:
    /** Collect all entries in the database (recursive). */
    QList<Entry*> allEntries() const;

    QSharedPointer<Database> m_db;
    SyncMetadataEngine* m_metadataEngine;
};

#endif // KEEPASSXC_LIFECYCLEMANAGER_H
