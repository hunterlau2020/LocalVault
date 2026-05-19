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

#include "LifecycleManager.h"

#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/HistoryRetentionService.h"
#include "core/SnapshotService.h"
#include "core/SyncMetadata.h"

#include <QFileInfo>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

LifecycleManager::LifecycleManager(QSharedPointer<Database> db, SyncMetadataEngine* metadataEngine)
    : m_db(std::move(db))
    , m_metadataEngine(metadataEngine)
{
}

// ---------------------------------------------------------------------------
// All entries helper
// ---------------------------------------------------------------------------

QList<Entry*> LifecycleManager::allEntries() const
{
    if (!m_db || !m_db->rootGroup()) {
        return {};
    }
    return m_db->rootGroup()->entriesRecursive(false);
}

// ---------------------------------------------------------------------------
// Full cleanup
// ---------------------------------------------------------------------------

CleanupExecutionReport LifecycleManager::runCleanup(const LifecyclePolicy& policy)
{
    CleanupExecutionReport report;

    if (!m_db) {
        return report;
    }

    // Auto-snapshot before any cleanup (best-effort)
    {
        SnapshotService ss(m_db);
        ss.createSnapshot(QStringLiteral("cleanup_pre"));
    }

    // Step 1: Clean up old snapshots
    {
        SnapshotService snapshotService(m_db);
        const int removed = cleanupSnapshots(snapshotService, policy);
        report.cleanedSnapshots = removed;

        // Estimate reclaimed bytes: average snapshot size * count
        // (actual bytes are tracked per-snapshot but are reclaimed
        //  as files are deleted — we report a best-effort estimate)
        const auto remaining = snapshotService.listSnapshots();
        qint64 avgSize = 0;
        if (!remaining.isEmpty()) {
            for (const auto& s : remaining) {
                avgSize += s.fileSize;
            }
            avgSize /= remaining.size();
        }
        report.reclaimedBytes += avgSize * removed;
    }

    // Step 2: Clean up entry history
    {
        const int removed = cleanupEntryHistory(policy);
        report.cleanedHistoryRecords = removed;
    }

    // Step 3: Clean up tombstones
    if (policy.tombstoneCleanupEnabled) {
        const int removed = cleanupTombstones(policy);
        report.cleanedTombstones = removed;
    }

    return report;
}

// ---------------------------------------------------------------------------
// Snapshot cleanup
// ---------------------------------------------------------------------------

int LifecycleManager::cleanupSnapshots(SnapshotService& snapshotService, const LifecyclePolicy& policy)
{
    if (!snapshotService.hasDatabase()) {
        return 0;
    }

    auto snapshots = snapshotService.listSnapshots();
    if (snapshots.isEmpty()) {
        return 0;
    }

    // Sort by creation time (oldest first)
    std::sort(snapshots.begin(), snapshots.end(), [](const SnapshotRecord& a, const SnapshotRecord& b) {
        return a.createdAt < b.createdAt;
    });

    int removedCount = 0;

    // Policy 1: max age — remove snapshots older than maxSnapshotDays
    if (policy.maxSnapshotDays > 0) {
        const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-policy.maxSnapshotDays);
        for (const auto& snap : snapshots) {
            if (snap.createdAt < cutoff && !snap.protected_) {
                if (snapshotService.deleteSnapshot(snap.snapshotId)) {
                    ++removedCount;
                }
            }
        }
    }

    // Policy 2: max count — remove oldest beyond the limit
    if (policy.maxSnapshotCount > 0) {
        // Re-fetch the list (may have changed after age cleanup)
        auto remaining = snapshotService.listSnapshots();
        if (remaining.size() > policy.maxSnapshotCount) {
            // Sort oldest-first
            std::sort(remaining.begin(), remaining.end(), [](const SnapshotRecord& a, const SnapshotRecord& b) {
                return a.createdAt < b.createdAt;
            });

            const int excessCount = remaining.size() - policy.maxSnapshotCount;
            int removedFromSorted = 0;
            for (int i = 0; i < remaining.size() && removedFromSorted < excessCount; ++i) {
                if (!remaining[i].protected_) {
                    if (snapshotService.deleteSnapshot(remaining[i].snapshotId)) {
                        ++removedCount;
                        ++removedFromSorted;
                    }
                }
            }
        }
    }

    return removedCount;
}

// ---------------------------------------------------------------------------
// Entry history cleanup
// ---------------------------------------------------------------------------

int LifecycleManager::cleanupEntryHistory(const LifecyclePolicy& policy)
{
    if (policy.entryHistoryLimit <= 0) {
        return 0;
    }

    const auto entries = allEntries();
    int totalRemoved = 0;
    for (auto* entry : entries) {
        const auto history = entry->historyItems();
        const int beforeCount = history.size();
        HistoryRetentionService::enforceEntryHistoryLimit(entry, policy.entryHistoryLimit);
        totalRemoved += (beforeCount - entry->historyItems().size());
    }

    return totalRemoved;
}

// ---------------------------------------------------------------------------
// Tombstone cleanup
// ---------------------------------------------------------------------------

int LifecycleManager::cleanupTombstones(const LifecyclePolicy& policy)
{
    Q_UNUSED(policy);

    if (!m_metadataEngine || !m_metadataEngine->hasDatabase()) {
        return 0;
    }

    const auto tombstones = m_metadataEngine->tombstones();
    if (tombstones.isEmpty()) {
        return 0;
    }

    // Build a set of entry IDs referenced by unresolved conflicts
    QSet<QUuid> conflictedEntryIds;
    const auto conflicts = m_metadataEngine->conflicts();
    for (const auto& cr : conflicts) {
        if (!cr.resolved) {
            conflictedEntryIds.insert(cr.entryId);
        }
    }

    int removedCount = 0;
    for (const auto& tombstone : tombstones) {
        // Skip tombstones for entries that have unresolved conflicts
        if (conflictedEntryIds.contains(tombstone.entryId)) {
            continue;
        }

        m_metadataEngine->removeTombstone(tombstone.entryId);
        ++removedCount;
    }

    // Persist the metadata changes
    if (removedCount > 0) {
        m_metadataEngine->saveToDatabase();
    }

    return removedCount;
}
