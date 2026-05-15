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

#ifndef KEEPASSXC_CONFLICTRESOLVER_H
#define KEEPASSXC_CONFLICTRESOLVER_H

#include <QMap>
#include <QSharedPointer>
#include <QString>
#include <QUuid>

#include "core/SyncData.h"  // SyncFieldConflict
#include "core/SyncMetadata.h" // SyncMetadataEngine, VersionVector

class Database;
class Entry;
class Group;

/**
 * Conflict resolution strategies.
 */
enum class ConflictResolution
{
    KeepLocal,    // Discard remote changes, keep local as-is
    KeepRemote,   // Overwrite local entry with remote values
    ManualMerge,  // Apply caller-specified merged field values
    CreateCopy    // Keep local, clone remote entry with new UUID
};

/**
 * A conflict item with full context for resolution.
 */
struct ConflictItem
{
    QUuid conflictId;                              // matches ConflictRecord.conflictId
    QUuid entryId;                                 // the entry in conflict
    QSharedPointer<Entry> localEntry;              // snapshot of local at analysis time
    QSharedPointer<Entry> remoteEntry;             // snapshot of remote at analysis time
    QList<SyncFieldConflict> conflictingFields;    // field-level conflict details
    QStringList conflictingFieldNames;             // convenience: just field names
    QString localDeviceId;                         // device that made local changes
    QString remoteDeviceId;                        // device that made remote changes
    bool resolved = false;
};

/**
 * A command telling the resolver what to do with a conflict.
 */
struct ConflictResolutionCommand
{
    QUuid conflictId;
    ConflictResolution resolutionType = ConflictResolution::KeepLocal;

    /** For ManualMerge: fieldName -> merged value */
    QMap<QString, QString> mergedFieldValues;
};

/**
 * The result of resolving one conflict.
 */
struct ConflictResolutionResult
{
    bool success = false;
    QString errorMessage;

    QUuid newVersionId;                // first local counter after resolution
    QList<QUuid> archivedVersionIds;   // history entries created for pre-resolution state
    QUuid createdCopyEntryId;          // non-null only for CreateCopy
    bool conflictMarkedResolved = false;
};

/**
 * ConflictResolverService — resolves sync conflicts detected by SyncEngine.
 *
 * Consumes SyncOperation::Conflict entries from SyncResult and applies
 * one of four resolution strategies directly to the local database.
 *
 * Thread-safety: not guaranteed. Use from a single thread only.
 */
class ConflictResolverService
{
public:
    /**
     * @param localDb  The local database to apply resolutions to
     * @param metadataEngine  Optional SyncMetadataEngine for VV/conflict-record updates
     */
    explicit ConflictResolverService(QSharedPointer<Database> localDb,
                                     SyncMetadataEngine* metadataEngine = nullptr);

    /**
     * Build a ConflictItem from a SyncOperation::Conflict.
     * Enriches it with device IDs from version vectors.
     */
    ConflictItem buildConflictDraft(const SyncOperation& op);

    /**
     * Resolve a single conflict using the given command.
     * Modifies the local database directly (but does NOT save it).
     * Caller must save() afterwards.
     */
    ConflictResolutionResult resolve(const ConflictItem& item, const ConflictResolutionCommand& command);

    /**
     * Resolve all conflicts in a SyncResult using the same strategy.
     * ManualMerge is not supported by this overload (use the command-list overload).
     */
    QList<ConflictResolutionResult> resolveAll(const SyncResult& result, ConflictResolution strategy);

    /**
     * Resolve conflicts with per-conflict commands (required for ManualMerge).
     */
    QList<ConflictResolutionResult> resolveAll(const QList<ConflictItem>& items,
                                                const QList<ConflictResolutionCommand>& commands);

    // --- Strategy implementations ------------------------------------------

    /** Keep local entry unchanged (discard remote). */
    void resolveKeepLocal(Entry* localEntry, const ConflictItem& item);

    /** Overwrite local entry with remote values. */
    void resolveKeepRemote(Entry* localEntry, const ConflictItem& item);

    /** Apply caller-specified merged field values. */
    bool resolveManualMerge(Entry* localEntry,
                            const ConflictItem& item,
                            const QMap<QString, QString>& mergedValues,
                            QString* errorOut = nullptr);

    /** Keep local, create a copy of remote with a new UUID. Returns the copy. */
    Entry* resolveCreateCopy(Entry* localEntry, const ConflictItem& item);

    // --- Helpers -----------------------------------------------------------

    /** Validate that mergedValues covers all conflicting fields. */
    static bool validateManualMerge(const ConflictItem& item,
                                     const QMap<QString, QString>& mergedValues,
                                     QString* missingField = nullptr);

    /** Set a single field value on an entry by field name. */
    static void setFieldValue(Entry* entry, const QString& fieldName, const QString& value);

    /** Build a version-id string from a VV for audit purposes. */
    static QString formatVersionId(const VersionVector& vv, const QString& deviceId);

private:
    /** After resolving (any strategy), update VV + mark conflict resolved. */
    void finalizeResolution(Entry* localEntry, const ConflictItem& item);

    QSharedPointer<Database> m_localDb;
    SyncMetadataEngine* m_metadataEngine;
};

#endif // KEEPASSXC_CONFLICTRESOLVER_H
