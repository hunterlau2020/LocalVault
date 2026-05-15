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

#ifndef KEEPASSXC_SYNCMETADATA_H
#define KEEPASSXC_SYNCMETADATA_H

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QUuid>

class Database;
class Entry;

/**
 * Version vector: maps deviceId → counter.
 * Each device increments its own counter on each modification.
 * Causality is determined by comparing all counters pairwise.
 */
using VersionVector = QMap<QString, int>;

/**
 * Result of comparing two version vectors.
 */
enum class VVCompareResult
{
    Equal,            // identical vectors
    LocalDominates,   // local has all counters >= remote, at least one >
    RemoteDominates,  // remote has all counters >= local, at least one >
    Concurrent        // each has at least one counter > the other
};

/**
 * Identity of a device that has synced this database.
 */
struct DeviceIdentity
{
    QString deviceId;       // UUID v4 as Id128 string
    QString deviceName;     // human-readable label
    QDateTime registeredAt; // UTC ISO-8601
    QDateTime lastSeenAt;   // UTC ISO-8601

    QJsonObject toJson() const;
    static DeviceIdentity fromJson(const QJsonObject& obj);
};

/**
 * Sync baseline for one remote.
 */
struct SyncBaseline
{
    QString remoteId;           // identifies the remote (e.g. path hash)
    QString lastPulledCursor;   // opaque cursor from last pull
    QString lastPushedCursor;   // opaque cursor from last push
    QDateTime lastSuccessSyncAt;

    QJsonObject toJson() const;
    static SyncBaseline fromJson(const QJsonObject& obj);
};

/**
 * Record of a deleted (tombstoned) entry.
 */
struct TombstoneRecord
{
    QUuid entryId;
    QDateTime deletedAt;
    QString deletedBy; // deviceId

    QJsonObject toJson() const;
    static TombstoneRecord fromJson(const QJsonObject& obj);
};

/**
 * Record of a conflict event.
 */
struct ConflictRecord
{
    QUuid entryId;
    QUuid conflictId;       // unique identifier for this conflict event
    QDateTime createdAt;
    bool resolved = false;
    QStringList conflictingFields;

    QJsonObject toJson() const;
    static ConflictRecord fromJson(const QJsonObject& obj);
};

/**
 * SyncMetadataEngine — manages sync metadata stored in KDBX CustomData.
 *
 * Database-level metadata lives in Metadata::customData() under "KPXC_SYNC_METADATA".
 * Entry-level version vectors live in each Entry::customData() under "KPXC_SYNC_VV".
 *
 * Thread-safety: not guaranteed. Use from a single thread only.
 */
class SyncMetadataEngine
{
public:
    explicit SyncMetadataEngine();
    explicit SyncMetadataEngine(QSharedPointer<Database> db);

    void setDatabase(QSharedPointer<Database> db);
    bool hasDatabase() const;

    // --- Load / Save ----------------------------------------------------------
    void loadFromDatabase();
    void saveToDatabase();

    // --- Schema ---------------------------------------------------------------
    int schemaVersion() const;

    // --- Device registry ------------------------------------------------------
    QString registerOrLoadCurrentDevice(const QString& deviceName = QString());
    QString currentDeviceId() const;
    QList<DeviceIdentity> deviceRegistry() const;

    // --- Entry version vectors ------------------------------------------------
    VersionVector getEntryVersionVector(Entry* entry) const;
    void setEntryVersionVector(Entry* entry, const VersionVector& vv);
    VersionVector incrementEntryCounter(Entry* entry);

    // --- Comparison (static, pure logic) --------------------------------------
    static VVCompareResult compareVersionVectors(const VersionVector& local, const VersionVector& remote);
    static VersionVector mergeVersionVectors(const VersionVector& a, const VersionVector& b);

    // --- Tombstones -----------------------------------------------------------
    void addTombstone(const QUuid& entryId);
    bool isTombstone(const QUuid& entryId) const;
    void removeTombstone(const QUuid& entryId);
    QList<TombstoneRecord> tombstones() const;

    // --- Conflicts ------------------------------------------------------------
    void recordConflict(const QUuid& entryId, const QStringList& fields);
    void markConflictResolved(const QUuid& conflictId);
    ConflictRecord conflictById(const QUuid& conflictId) const;
    QList<ConflictRecord> conflicts() const;

    // --- Sync baselines -------------------------------------------------------
    void updateSyncBaseline(const QString& remoteId, const QString& cursor);
    SyncBaseline syncBaseline(const QString& remoteId) const;

    // --- Key constants --------------------------------------------------------
    static const QString DB_METADATA_KEY;  // "KPXC_SYNC_METADATA"
    static const QString ENTRY_VV_KEY;     // "KPXC_SYNC_VV"

private:
    QSharedPointer<Database> m_db;
    int m_schemaVersion = 1;

    // In-memory caches (populated by loadFromDatabase, flushed by saveToDatabase)
    QMap<QString, DeviceIdentity> m_deviceRegistry; // deviceId → record
    QMap<QString, SyncBaseline> m_syncBaselines;    // remoteId → baseline
    QMap<QString, TombstoneRecord> m_tombstones;    // entryId string → record
    QList<ConflictRecord> m_conflicts;

    // Device identity of this local installation (persisted in Config)
    mutable QString m_cachedDeviceId; // lazily loaded
};

#endif // KEEPASSXC_SYNCMETADATA_H
