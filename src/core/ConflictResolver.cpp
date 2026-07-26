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

#include "ConflictResolver.h"

#include "core/Database.h"
#include "core/Entry.h"
#include "core/EntryAttachments.h"
#include "core/EntryAttributes.h"
#include "core/Group.h"
#include "core/SnapshotService.h"

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ConflictResolverService::ConflictResolverService(QSharedPointer<Database> localDb,
                                                 SyncMetadataEngine* metadataEngine)
    : m_localDb(std::move(localDb))
    , m_metadataEngine(metadataEngine)
{
}

// ---------------------------------------------------------------------------
// Draft building
// ---------------------------------------------------------------------------

ConflictItem ConflictResolverService::buildConflictDraft(const SyncOperation& op)
{
    ConflictItem item;
    item.entryId = op.entryId;
    item.localEntry = op.localEntry;
    item.remoteEntry = op.remoteEntry;
    item.conflictingFields = op.conflictingFields;

    // Collect field names
    for (const auto& cf : op.conflictingFields) {
        item.conflictingFieldNames.append(cf.fieldName);
    }

    // Extract device IDs from version vectors (if metadata engine available)
    if (m_metadataEngine && m_metadataEngine->hasDatabase()) {
        if (op.localEntry) {
            const VersionVector localVV = m_metadataEngine->getEntryVersionVector(op.localEntry.data());
            item.localDeviceId = m_metadataEngine->currentDeviceId();
        }
        if (op.remoteEntry) {
            const VersionVector remoteVV = m_metadataEngine->getEntryVersionVector(op.remoteEntry.data());
            // Pick first non-local device as the remote device
            for (auto it = remoteVV.begin(); it != remoteVV.end(); ++it) {
                if (it.key() != item.localDeviceId) {
                    item.remoteDeviceId = it.key();
                    break;
                }
            }
        }

        // Check if already resolved
        const auto records = m_metadataEngine->conflicts();
        for (const auto& cr : records) {
            if (cr.entryId == op.entryId && !cr.resolved) {
                item.conflictId = cr.conflictId;
                break;
            }
        }
    }

    // If no conflictId found, create one
    if (item.conflictId.isNull()) {
        item.conflictId = QUuid::createUuid();
    }

    return item;
}

// ---------------------------------------------------------------------------
// Single resolution
// ---------------------------------------------------------------------------

ConflictResolutionResult ConflictResolverService::resolve(const ConflictItem& item,
                                                          const ConflictResolutionCommand& command)
{
    ConflictResolutionResult result;

    if (!m_localDb) {
        result.errorMessage = QStringLiteral("No local database");
        return result;
    }

    // Look up the actual entry in the local database
    Entry* localEntry = m_localDb->rootGroup()->findEntryByUuid(item.entryId);
    if (!localEntry) {
        result.errorMessage = QStringLiteral("Entry not found in local database");
        return result;
    }

    // Apply the chosen strategy
    switch (command.resolutionType) {
    case ConflictResolution::KeepLocal:
        resolveKeepLocal(localEntry, item);
        break;

    case ConflictResolution::KeepRemote:
        resolveKeepRemote(localEntry, item);
        break;

    case ConflictResolution::ManualMerge:
        if (!resolveManualMerge(localEntry, item, command.mergedFieldValues, &result.errorMessage)) {
            result.errorMessage = QStringLiteral("Manual merge failed: %1").arg(result.errorMessage);
            return result;
        }
        break;

    case ConflictResolution::CreateCopy: {
        Entry* copy = resolveCreateCopy(localEntry, item);
        if (copy) {
            result.createdCopyEntryId = copy->uuid();
        }
        break;
    }
    }

    // Update metadata (VV, conflict records)
    finalizeResolution(localEntry, item);

    result.success = true;
    result.conflictMarkedResolved = true;
    return result;
}

// ---------------------------------------------------------------------------
// Batch resolution
// ---------------------------------------------------------------------------

QList<ConflictResolutionResult>
ConflictResolverService::resolveAll(const SyncResult& result, ConflictResolution strategy)
{
    QList<ConflictResolutionResult> results;

    if (result.conflictCount == 0) {
        return results;
    }

    // Auto-snapshot before batch resolve (best-effort)
    {
        SnapshotService ss(m_localDb);
        ss.createSnapshot(QStringLiteral("conflict_batch_resolve"));
    }

    for (const auto& op : result.operations) {
        if (op.type != SyncOperation::Conflict) {
            continue;
        }

        ConflictItem item = buildConflictDraft(op);
        ConflictResolutionCommand cmd;
        cmd.conflictId = item.conflictId;
        cmd.resolutionType = strategy;

        results.append(resolve(item, cmd));
    }

    return results;
}

QList<ConflictResolutionResult>
ConflictResolverService::resolveAll(const QList<ConflictItem>& items,
                                     const QList<ConflictResolutionCommand>& commands)
{
    QList<ConflictResolutionResult> results;

    if (items.isEmpty()) {
        return results;
    }

    // Auto-snapshot before batch resolve (best-effort)
    {
        SnapshotService ss(m_localDb);
        ss.createSnapshot(QStringLiteral("conflict_batch_resolve"));
    }

    const int count = qMin(items.size(), commands.size());
    for (int i = 0; i < count; ++i) {
        results.append(resolve(items[i], commands[i]));
    }

    // Any items without a matching command get KeepLocal
    for (int i = count; i < items.size(); ++i) {
        ConflictResolutionCommand cmd;
        cmd.conflictId = items[i].conflictId;
        cmd.resolutionType = ConflictResolution::KeepLocal;
        results.append(resolve(items[i], cmd));
    }

    return results;
}

// ---------------------------------------------------------------------------
// Strategy: KeepLocal
// ---------------------------------------------------------------------------

void ConflictResolverService::resolveKeepLocal(Entry* localEntry, const ConflictItem& item)
{
    if (item.remoteEntry) {
        Entry* remoteClone = item.remoteEntry->clone(Entry::CloneDefault);
        remoteClone->setUuid(localEntry->uuid());
        localEntry->addHistoryItem(remoteClone);
        localEntry->truncateHistory();
    }
}

// ---------------------------------------------------------------------------
// Strategy: KeepRemote
// ---------------------------------------------------------------------------

void ConflictResolverService::resolveKeepRemote(Entry* localEntry, const ConflictItem& item)
{
    if (!item.remoteEntry) {
        return;
    }

    localEntry->beginUpdate();
    localEntry->copyDataFrom(item.remoteEntry.data());
    localEntry->endUpdate();
}

// ---------------------------------------------------------------------------
// Strategy: ManualMerge
// ---------------------------------------------------------------------------

bool ConflictResolverService::resolveManualMerge(Entry* localEntry,
                                                  const ConflictItem& item,
                                                  const QMap<QString, QString>& mergedValues,
                                                  QString* errorOut)
{
    QString missingField;
    if (!validateManualMerge(item, mergedValues, &missingField)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Missing merged value for field: %1").arg(missingField);
        }
        return false;
    }

    if (item.remoteEntry) {
        Entry* remoteClone = item.remoteEntry->clone(Entry::CloneDefault);
        remoteClone->setUuid(localEntry->uuid());
        localEntry->addHistoryItem(remoteClone);
        localEntry->truncateHistory();
    }

    localEntry->beginUpdate();
    for (auto it = mergedValues.begin(); it != mergedValues.end(); ++it) {
        setFieldValue(localEntry, it.key(), it.value());
    }
    localEntry->endUpdate();
    return true;
}

// ---------------------------------------------------------------------------
// Helpers — file-local (must come before resolveCreateCopy)
// ---------------------------------------------------------------------------

/** Recursively search the group tree for the group containing \p entry. */
static Group* findEntryGroup(Group* root, Entry* entry)
{
    if (root->entries().contains(entry)) {
        return root;
    }
    for (auto* child : root->children()) {
        Group* found = findEntryGroup(child, entry);
        if (found) {
            return found;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Strategy: CreateCopy
// ---------------------------------------------------------------------------

Entry* ConflictResolverService::resolveCreateCopy(Entry* localEntry, const ConflictItem& item)
{
    if (!item.remoteEntry || !localEntry) {
        return nullptr;
    }

    // Clone the remote entry with a new UUID, reset timestamps, include history
    Entry* copy = item.remoteEntry->clone(Entry::CloneNewUuid | Entry::CloneResetTimeInfo | Entry::CloneIncludeHistory | Entry::CloneRenameTitle);

    // Place the copy in the same group as the local entry.
    // NOTE: localEntry->group() cannot be relied upon — Group::addEntry() does
    // not set Entry::m_group.  Instead, search the group tree.
    Group* parentGroup = findEntryGroup(m_localDb->rootGroup(), localEntry);
    if (!parentGroup) {
        parentGroup = m_localDb->rootGroup();
    }
    parentGroup->addEntry(copy);

    // Initialize version vector for the copy
    if (m_metadataEngine && m_metadataEngine->hasDatabase()) {
        m_metadataEngine->incrementEntryCounter(copy);
    }

    return copy;
}

// ---------------------------------------------------------------------------
// Metadata finalization
// ---------------------------------------------------------------------------

void ConflictResolverService::finalizeResolution(Entry* localEntry, const ConflictItem& item)
{
    if (!m_metadataEngine || !m_metadataEngine->hasDatabase()) {
        return;
    }

    // Get the pre-resolution local VV from the conflict item clone.
    // KeepRemote calls copyDataFrom() which overwrites CustomData including
    // the entry's version vector, so reading VV from localEntry after
    // resolution would give the remote VV, not the local one.
    VersionVector localVV = m_metadataEngine->getEntryVersionVector(item.localEntry.data());

    // Advance local counter
    const QString myId = m_metadataEngine->currentDeviceId();
    if (!myId.isEmpty()) {
        localVV[myId] = localVV.value(myId, 0) + 1;
    }

    // Merge remote VV so both devices' counters are reflected
    if (item.remoteEntry) {
        const VersionVector remoteVV = m_metadataEngine->getEntryVersionVector(item.remoteEntry.data());
        localVV = SyncMetadataEngine::mergeVersionVectors(localVV, remoteVV);
    }

    m_metadataEngine->setEntryVersionVector(localEntry, localVV);

    // Mark the conflict record as resolved
    if (!item.conflictId.isNull()) {
        m_metadataEngine->markConflictResolved(item.conflictId);
    }
}

bool ConflictResolverService::validateManualMerge(const ConflictItem& item,
                                                    const QMap<QString, QString>& mergedValues,
                                                    QString* missingField)
{
    for (const auto& field : item.conflictingFieldNames) {
        if (!mergedValues.contains(field)) {
            if (missingField) {
                *missingField = field;
            }
            return false;
        }
    }
    return true;
}

void ConflictResolverService::setFieldValue(Entry* entry, const QString& field, const QString& value)
{
    if (field == QStringLiteral("title")) {
        entry->setTitle(value);
    } else if (field == QStringLiteral("username")) {
        entry->setUsername(value);
    } else if (field == QStringLiteral("password")) {
        entry->setPassword(value);
    } else if (field == QStringLiteral("url")) {
        entry->setUrl(value);
    } else if (field == QStringLiteral("notes")) {
        entry->setNotes(value);
    } else if (field.startsWith(QStringLiteral("custom_fields."))) {
        const QString key = field.mid(QStringLiteral("custom_fields.").length());
        entry->attributes()->set(key, value);
    } else if (field.startsWith(QStringLiteral("attachment."))) {
        // Binary data not supported via string value — skip in ManualMerge
    } else if (field == QStringLiteral("icon")) {
        entry->setIcon(value.toInt());
    } else if (field == QStringLiteral("tags")) {
        entry->setTags(value);
    } else if (field == QStringLiteral("autoTypeEnabled")) {
        entry->setAutoTypeEnabled(value == QStringLiteral("true"));
    } else if (field == QStringLiteral("autoTypeSequence")) {
        entry->setDefaultAutoTypeSequence(value);
    }
    // NOTE: expiry, custom_data fields skipped in V1 (same as SyncEngine::applyFieldValues)
}

QString ConflictResolverService::formatVersionId(const VersionVector& vv, const QString& deviceId)
{
    if (vv.isEmpty()) {
        return QStringLiteral("v0");
    }
    const int counter = vv.value(deviceId, 0);
    return QStringLiteral("v%1-%2").arg(counter).arg(deviceId.left(8));
}
