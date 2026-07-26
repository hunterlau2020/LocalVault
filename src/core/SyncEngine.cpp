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

#include "SyncEngine.h"

#include "core/Database.h"
#include "core/Entry.h"
#include "core/EntryAttachments.h"
#include "core/EntryAttributes.h"
#include "core/EntryDiff.h"
#include "core/EntrySnapshot.h"
#include "core/ExternalChangeDetector.h"
#include "core/Group.h"
#include "core/SnapshotService.h"

#include <QDateTime>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Collect all fields that the diff describes as changed. */
static QStringList diffFieldNames(const EntryDiff& diff)
{
    QStringList names;
    names.reserve(diff.changedFields.size());
    for (const auto& f : diff.changedFields) {
        names.append(f.fieldName);
    }
    return names;
}

// ---------------------------------------------------------------------------
// Metadata engine setup
// ---------------------------------------------------------------------------

void SyncEngine::setMetadataEngine(QSharedPointer<Database> db)
{
    m_metadataEngine.setDatabase(db);
}

SyncMetadataEngine* SyncEngine::metadataEngine()
{
    return &m_metadataEngine;
}

// ---------------------------------------------------------------------------
// Indexing
// ---------------------------------------------------------------------------

QMap<QUuid, Entry*> SyncEngine::indexEntries(QSharedPointer<Database> db)
{
    QMap<QUuid, Entry*> map;
    const auto entries = db->rootGroup()->entriesRecursive(false);
    for (auto* entry : entries) {
        map.insert(entry->uuid(), entry);
    }
    return map;
}

// ---------------------------------------------------------------------------
// Diff analysis
// ---------------------------------------------------------------------------

QStringList SyncEngine::computeTotalDiff(const EntrySnapshot& a, const EntrySnapshot& b)
{
    auto diff = EntryDiff::compute(a, b);
    return diffFieldNames(diff);
}

QStringList SyncEngine::findOverlappingFields(const QStringList& fieldsA, const QStringList& fieldsB)
{
    const QSet<QString> setA(fieldsA.begin(), fieldsA.end());
    QStringList overlap;
    for (const auto& f : fieldsB) {
        if (setA.contains(f)) {
            overlap.append(f);
        }
    }
    return overlap;
}

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

Entry* SyncEngine::findCommonAncestor(Entry* localEntry, Entry* remoteEntry)
{
    if (!localEntry || !remoteEntry) {
        return nullptr;
    }

    QList<Entry*> localVersions;
    localVersions.append(localEntry);
    for (auto* h : localEntry->historyItems()) {
        localVersions.append(h);
    }

    QList<Entry*> remoteVersions;
    remoteVersions.append(remoteEntry);
    for (auto* h : remoteEntry->historyItems()) {
        remoteVersions.append(h);
    }

    Entry* bestAncestor = nullptr;
    VersionVector bestVV;

    for (auto* lVer : localVersions) {
        VersionVector lVV = m_metadataEngine.getEntryVersionVector(lVer);
        if (lVV.isEmpty()) {
            continue;
        }

        for (auto* rVer : remoteVersions) {
            VersionVector rVV = m_metadataEngine.getEntryVersionVector(rVer);
            if (rVV.isEmpty()) {
                continue;
            }

            if (lVV == rVV) {
                if (!bestAncestor) {
                    bestAncestor = lVer;
                    bestVV = lVV;
                } else {
                    auto cmp = SyncMetadataEngine::compareVersionVectors(lVV, bestVV);
                    if (cmp == VVCompareResult::LocalDominates) {
                        bestAncestor = lVer;
                        bestVV = lVV;
                    }
                }
            }
        }
    }

    return bestAncestor;
}

SyncOperation SyncEngine::classifyEntry(Entry* localEntry, Entry* remoteEntry, const QUuid& uuid, const SyncMetadataEngine& remoteMetadata)
{
    SyncOperation op;
    op.entryId = uuid;

    const bool isLocalTombstone = m_metadataEngine.hasDatabase() && m_metadataEngine.isTombstone(uuid);
    const bool isRemoteTombstone = remoteMetadata.hasDatabase() && remoteMetadata.isTombstone(uuid);

    // --- Both deleted or tombstoned ------------------------------------------
    if (isLocalTombstone && isRemoteTombstone) {
        op.type = SyncOperation::Skipped;
        return op;
    }

    // --- Deleted on one side, but exists on the other -------------------------
    if (isLocalTombstone && !isRemoteTombstone && remoteEntry) {
        // Deleted locally, exists remotely
        const auto localTombstone = m_metadataEngine.getTombstone(uuid);
        const QDateTime remoteTime = remoteEntry->timeInfo().lastModificationTime();
        
        // Did remote modify it after local deletion?
        if (remoteTime > localTombstone.deletedAt) {
            // Remote modification is newer, revive local entry
            op.type = SyncOperation::DirectApply;
            op.sourceEntry.reset(remoteEntry->clone(Entry::CloneIncludeHistory));
            op.changedFields = QStringList{QStringLiteral("[revived entry]")};
            return op;
        } else {
            // Deletion wins, keep it deleted locally
            op.type = SyncOperation::Skipped;
            return op;
        }
    }

    if (!isLocalTombstone && isRemoteTombstone && localEntry) {
        // Exists locally, deleted remotely
        const auto remoteTombstone = remoteMetadata.getTombstone(uuid);
        const QDateTime localTime = localEntry->timeInfo().lastModificationTime();

        // Did local modify it after remote deletion?
        if (localTime > remoteTombstone.deletedAt) {
            // Local modification is newer, keep it local (no action on local DB)
            op.type = SyncOperation::Skipped;
            return op;
        } else {
            // Remote deletion wins, delete it locally
            op.type = SyncOperation::DeleteLocally;
            op.changedFields = QStringList{QStringLiteral("[deleted entry]")};
            return op;
        }
    }

    // --- Only in one database ------------------------------------------------
    if (localEntry && !remoteEntry) {
        op.type = SyncOperation::Skipped;
        return op;
    }

    if (!localEntry && remoteEntry) {
        op.type = SyncOperation::DirectApply;
        op.sourceEntry.reset(remoteEntry->clone(Entry::CloneIncludeHistory));
        op.changedFields = QStringList{QStringLiteral("[new entry]")};
        return op;
    }

    // --- Both exist — compare ------------------------------------------------
    const auto localSnap = EntrySnapshot::capture(localEntry);
    const auto remoteSnap = EntrySnapshot::capture(remoteEntry);

    const QStringList totalDiff = computeTotalDiff(localSnap, remoteSnap);
    if (totalDiff.isEmpty()) {
        op.type = SyncOperation::Skipped;
        return op;
    }

    // --- Try version vector comparison first ----------------------------------
    if (m_metadataEngine.hasDatabase()) {
        const VersionVector localVV = m_metadataEngine.getEntryVersionVector(localEntry);
        const VersionVector remoteVV = m_metadataEngine.getEntryVersionVector(remoteEntry);

        if (!localVV.isEmpty() || !remoteVV.isEmpty()) {
            const auto vvResult = SyncMetadataEngine::compareVersionVectors(localVV, remoteVV);

            switch (vvResult) {
            case VVCompareResult::Equal:
                break;
            case VVCompareResult::LocalDominates:
                op.type = SyncOperation::Skipped;
                return op;
            case VVCompareResult::RemoteDominates:
                op.type = SyncOperation::DirectApply;
                op.sourceEntry.reset(remoteEntry->clone(Entry::CloneIncludeHistory));
                op.changedFields = totalDiff;
                return op;
            case VVCompareResult::Concurrent:
                break; // fall through to field-overlap check
            }
        }
    }

    // --- Fallback: last modification time (legacy entries without VVs) ---------
    {
        const QDateTime localTime = localEntry->timeInfo().lastModificationTime();
        const QDateTime remoteTime = remoteEntry->timeInfo().lastModificationTime();

        const qint64 diffMs = localTime.msecsTo(remoteTime);
        constexpr qint64 SKEW_MS = 1000;

        if (diffMs < -SKEW_MS) {
            op.type = SyncOperation::Skipped;
            return op;
        }

        if (diffMs > SKEW_MS) {
            op.type = SyncOperation::DirectApply;
            op.sourceEntry.reset(remoteEntry->clone(Entry::CloneIncludeHistory));
            op.changedFields = totalDiff;
            return op;
        }
    }

    // --- Concurrent (same time window or VVs concurrent) — check field overlap -
    Entry* ancestor = findCommonAncestor(localEntry, remoteEntry);
    if (ancestor) {
        const auto ancestorSnap = EntrySnapshot::capture(ancestor);
        const auto localDiff = computeTotalDiff(ancestorSnap, localSnap);
        const auto remoteDiff = computeTotalDiff(ancestorSnap, remoteSnap);

        const auto overlap = findOverlappingFields(localDiff, remoteDiff);
        if (overlap.isEmpty()) {
            // No overlap! Auto merge.
            op.type = SyncOperation::AutoMerge;
            op.sourceEntry.reset(remoteEntry->clone(Entry::CloneIncludeHistory));
            op.changedFields = remoteDiff;
            return op;
        }
    }

    // Fallback: Conflict
    EntryDiff localVsRemote = EntryDiff::compute(localSnap, remoteSnap);
    QStringList allChanged = diffFieldNames(localVsRemote);

    QList<SyncFieldConflict> conflicts;
    for (const auto& fd : localVsRemote.changedFields) {
        conflicts.append({fd.fieldName, fd.oldValue, fd.newValue});
    }

    op.type = SyncOperation::Conflict;
    op.localEntry.reset(localEntry->clone(Entry::CloneIncludeHistory));
    op.remoteEntry.reset(remoteEntry->clone(Entry::CloneIncludeHistory));
    op.conflictingFields = conflicts;
    op.changedFields = allChanged;

    return op;
}

// ---------------------------------------------------------------------------
// Full analysis
// ---------------------------------------------------------------------------

SyncResult SyncEngine::analyzeDiffs(QSharedPointer<Database> local, QSharedPointer<Database> remote)
{
    SyncResult result;

    if (!local || !remote) {
        result.success = false;
        result.errorMessage = QStringLiteral("Invalid database pointer(s)");
        return result;
    }

    // Phase 8: abort sync if the local database shows signs of external modification.
    // The metadata engine must be bound to the local DB (SyncCommand calls
    // setMetadataEngine); an unbound engine makes this check a harmless no-op.
    {
        ExternalChangeDetector detector(local, &m_metadataEngine);
        ExternalChangeReport report;
        if (!detector.runPreSyncCheck(&report)) {
            result.success = false;
            result.errorMessage = QStringLiteral(
                "Sync aborted: the local database appears to have been modified outside "
                "LocalVault. Run `keepassxc-cli db-check` / `repair` before syncing. "
                "Findings: %1")
                    .arg(report.findings.join(QStringLiteral("; ")));
            return result;
        }
        if (report.severity == ChangeSeverity::Low) {
            result.warnings.append(report.findings);
        }
    }

    // Auto-snapshot before sync (best-effort: snapshot creation failure
    // does not block the sync itself).
    {
        SnapshotService ss(local);
        ss.createSnapshot(QStringLiteral("sync_pre"));
    }

    // Import deletions from Database::deletedObjects() to metadata tombstones
    if (m_metadataEngine.hasDatabase()) {
        for (const auto& del : local->deletedObjects()) {
            if (!m_metadataEngine.isTombstone(del.uuid)) {
                m_metadataEngine.addTombstone(del.uuid, del.deletionTime);
            }
        }
    }
    SyncMetadataEngine remoteMetadata(remote);
    for (const auto& del : remote->deletedObjects()) {
        if (!remoteMetadata.isTombstone(del.uuid)) {
            remoteMetadata.addTombstone(del.uuid, del.deletionTime);
        }
    }

    const auto localEntries = indexEntries(local);
    const auto remoteEntries = indexEntries(remote);

    // Collect all UUIDs (including deleted ones from tombstones)
    QSet<QUuid> allUuids;
    for (const auto& uuid : localEntries.keys()) {
        allUuids.insert(uuid);
    }
    for (const auto& uuid : remoteEntries.keys()) {
        allUuids.insert(uuid);
    }
    if (m_metadataEngine.hasDatabase()) {
        for (const auto& tb : m_metadataEngine.tombstones()) {
            allUuids.insert(tb.entryId);
        }
    }
    for (const auto& tb : remoteMetadata.tombstones()) {
        allUuids.insert(tb.entryId);
    }

    // Classify each entry
    for (const auto& uuid : allUuids) {
        auto* localEntry = localEntries.value(uuid, nullptr);
        auto* remoteEntry = remoteEntries.value(uuid, nullptr);

        auto op = classifyEntry(localEntry, remoteEntry, uuid, remoteMetadata);
        op.entryId = uuid;
        result.operations.append(op);

        switch (op.type) {
        case SyncOperation::DirectApply:
            if (!localEntry && remoteEntry) {
                // Remote Only → will be added to local
                ++result.addedCount;
            } else if (localEntry && remoteEntry) {
                // Both exist, one side is newer → update local
                ++result.updatedCount;
            }
            break;
        case SyncOperation::AutoMerge:
            ++result.updatedCount;
            break;
        case SyncOperation::Conflict:
            ++result.conflictCount;
            break;
        case SyncOperation::Skipped:
            ++result.skippedCount;
            break;
        case SyncOperation::DeleteLocally:
            ++result.deletedCount;
            break;
        }
    }

    result.success = true;
    return result;
}

// ---------------------------------------------------------------------------
// Apply merges
// ---------------------------------------------------------------------------

bool SyncEngine::copyEntryValues(Entry* dst, const Entry* src)
{
    if (!dst || !src) {
        return false;
    }

    dst->beginUpdate();
    dst->copyDataFrom(src);
    dst->endUpdate();
    return true;
}

bool SyncEngine::applyFieldValues(Entry* dst, const Entry* src, const QStringList& fields)
{
    if (!dst || !src) {
        return false;
    }

    dst->beginUpdate();

    for (const QString& field : fields) {
        if (field == QStringLiteral("title")) {
            dst->setTitle(src->title());
        } else if (field == QStringLiteral("username")) {
            dst->setUsername(src->username());
        } else if (field == QStringLiteral("password")) {
            dst->setPassword(src->password());
        } else if (field == QStringLiteral("url")) {
            dst->setUrl(src->url());
        } else if (field == QStringLiteral("notes")) {
            dst->setNotes(src->notes());
        } else if (field.startsWith(QStringLiteral("custom_fields."))) {
            // Extract the key name after "custom_fields."
            const QString key = field.mid(QStringLiteral("custom_fields.").length());
            if (src->attributes()->hasKey(key)) {
                dst->setDefaultAttribute(key, src->attributes()->value(key));
            }
        } else if (field.startsWith(QStringLiteral("attachment."))) {
            const QString key = field.mid(QStringLiteral("attachment.").length());
            QByteArray data;
            // Check existence rather than direct value access (avoids creating
            // missing-key entries). The "is absent" check is internal to EntryAttachments.
            if (src->attachments()->hasKey(key)) {
                data = src->attachments()->value(key);
                dst->attachments()->set(key, data);
            } else {
                dst->attachments()->remove(key);
            }
        } else if (field == QStringLiteral("icon")) {
            dst->setIcon(src->iconNumber());
        } else if (field == QStringLiteral("tags")) {
            dst->setTags(src->tags());
        } else if (field == QStringLiteral("autoTypeEnabled")) {
            dst->setAutoTypeEnabled(src->autoTypeEnabled());
        } else if (field == QStringLiteral("autoTypeSequence")) {
            dst->setDefaultAutoTypeSequence(src->defaultAutoTypeSequence());
        }
        // NOTE: expiry and custom_data fields could be added here.
        // They are omitted in V1 for simplicity; the full-sync fallback
        // (DirectApply) catches them when timestamps diverge.
    }

    // Suppress "not modified" warning — we intentionally only apply partial fields
    // but the entry WAS modified.
    dst->endUpdate();
    return true;
}

bool SyncEngine::applyMerges(const SyncResult& result, QSharedPointer<Database> local, QSharedPointer<Database> remote)
{
    if (!local) {
        return false;
    }

    bool allOk = true;

    for (const auto& op : result.operations) {
        if (op.type == SyncOperation::Skipped || op.type == SyncOperation::Conflict) {
            continue;
        }

        if (op.type == SyncOperation::DeleteLocally) {
            auto* localEntry = local->rootGroup()->findEntryByUuid(op.entryId);
            if (localEntry) {
                delete localEntry;
            }
            continue;
        }

        if (!op.sourceEntry) {
            allOk = false;
            continue;
        }

        // Find if this entry already exists in local
        auto* localEntry = local->rootGroup()->findEntryByUuid(op.entryId);
        const bool existsLocally = (localEntry != nullptr);

        if (op.type == SyncOperation::DirectApply) {
            if (!existsLocally) {
                // New entry from remote (Remote Only) — clone into local
                Entry* cloned = op.sourceEntry->clone(Entry::CloneNewUuid | Entry::CloneResetTimeInfo | Entry::CloneIncludeHistory);
                cloned->setUuid(op.entryId);
                local->rootGroup()->addEntry(cloned);

                // Initialize version vector for the new entry
                if (m_metadataEngine.hasDatabase()) {
                    m_metadataEngine.incrementEntryCounter(cloned);
                }
            } else if (remote && remote->rootGroup()->findEntryByUuid(op.entryId)) {
                // Entry exists in both — remote version wins
                copyEntryValues(localEntry, op.sourceEntry.data());

                // Advance local version vector after accepting remote changes
                if (m_metadataEngine.hasDatabase()) {
                    m_metadataEngine.incrementEntryCounter(localEntry);
                }
            }
            // Local Only entries: no action needed
        }

        if (op.type == SyncOperation::AutoMerge) {
            if (existsLocally && op.sourceEntry) {
                applyFieldValues(localEntry, op.sourceEntry.data(), op.changedFields);

                // Advance local version vector after merging
                if (m_metadataEngine.hasDatabase()) {
                    VersionVector localVV = m_metadataEngine.getEntryVersionVector(localEntry);
                    if (remote) {
                        auto* remoteEntry = remote->rootGroup()->findEntryByUuid(op.entryId);
                        if (remoteEntry) {
                            VersionVector remoteVV = m_metadataEngine.getEntryVersionVector(remoteEntry);
                            localVV = SyncMetadataEngine::mergeVersionVectors(localVV, remoteVV);
                        }
                    }
                    m_metadataEngine.setEntryVersionVector(localEntry, localVV);
                    m_metadataEngine.incrementEntryCounter(localEntry);
                }
            }
        }
    }

    // Replicate tombstones from remote to local
    if (m_metadataEngine.hasDatabase() && remote) {
        SyncMetadataEngine remoteMetadata(remote);
        for (const auto& tb : remoteMetadata.tombstones()) {
            if (!m_metadataEngine.isTombstone(tb.entryId)) {
                m_metadataEngine.addTombstone(tb.entryId, tb.deletedAt, tb.deletedBy);
            }
        }
    }

    // Persist any metadata changes (tombstones, baselines, etc.)
    if (m_metadataEngine.hasDatabase()) {
        m_metadataEngine.saveToDatabase();
    }

    return allOk;
}
