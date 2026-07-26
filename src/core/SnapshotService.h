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

#ifndef KEEPASSXC_SNAPSHOTSERVICE_H
#define KEEPASSXC_SNAPSHOTSERVICE_H

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QSharedPointer>
#include <QString>
#include <QUuid>

class Database;

/**
 * Record of one database snapshot stored on disk as a full KDBX copy.
 */
struct SnapshotRecord
{
    QUuid snapshotId;
    QDateTime createdAt;
    QString reason;    // e.g. "sync_pre", "manual", "pre_repair", "pre_cleanup"
    qint64 fileSize = 0;
    QString localPath; // absolute path to the snapshot KDBX file
    bool protected_ = false;

    QJsonObject toJson() const;
    static SnapshotRecord fromJson(const QJsonObject& obj);
};

/**
 * SnapshotService — manages database snapshots stored as full KDBX copies.
 *
 * Snapshot files are saved to a configurable local directory (default:
 * <database_dir>/LocalVault/snapshots/) as complete encrypted KDBX files.
 * The snapshot index is persisted in the database's CustomData under the
 * key "KPXC_SNAPSHOT_INDEX".  Only metadata travels with the KDBX, never
 * the snapshot file data itself.
 *
 * Thread-safety: not guaranteed. Use from a single thread only.
 */
class SnapshotService
{
public:
    explicit SnapshotService();
    explicit SnapshotService(QSharedPointer<Database> db);

    void setDatabase(QSharedPointer<Database> db);
    bool hasDatabase() const;

    // --- Index management ----------------------------------------------------
    void loadFromDatabase();
    void saveToDatabase();

    // --- Snapshot operations -------------------------------------------------
    bool createSnapshot(const QString& reason);
    QList<SnapshotRecord> listSnapshots() const;
    SnapshotRecord snapshotById(const QUuid& snapshotId) const;
    bool deleteSnapshot(const QUuid& snapshotId);
    /**
     * Restore the database file to the state of the snapshot.
     * Performs atomic replacement of the file to prevent data loss on copy failure.
     * NOTE: The caller MUST close and reload the Database object after calling this
     * method because the underlying file has been replaced.
     */
    bool restoreSnapshot(const QUuid& snapshotId);
    bool markProtected(const QUuid& snapshotId);
    bool unmarkProtected(const QUuid& snapshotId);

    // --- Config helpers ------------------------------------------------------
    static QString defaultSnapshotDirectory(const QString& databasePath);
    static QString snapshotFileName(const QUuid& snapshotId);

    // --- Key constant --------------------------------------------------------
    static const QString SNAPSHOT_INDEX_KEY;

private:
    bool ensureSnapshotDirectoryExists() const;
    QString resolveSnapshotDirectory() const;

    QSharedPointer<Database> m_db;
    QMap<QString, SnapshotRecord> m_snapshots;
};

#endif // KEEPASSXC_SNAPSHOTSERVICE_H
