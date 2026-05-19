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

#include "SnapshotService.h"

#include "core/Database.h"
#include "core/Metadata.h"
#include "core/CustomData.h"
#include "core/Config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

// ---------------------------------------------------------------------------
// Key constant
// ---------------------------------------------------------------------------

const QString SnapshotService::SNAPSHOT_INDEX_KEY = QStringLiteral("KPXC_SNAPSHOT_INDEX");

// ---------------------------------------------------------------------------
// SnapshotRecord serialization
// ---------------------------------------------------------------------------

QJsonObject SnapshotRecord::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("snapshot_id")] = snapshotId.toString(QUuid::WithoutBraces);
    obj[QStringLiteral("created_at")] = createdAt.toUTC().toString(Qt::ISODate);
    obj[QStringLiteral("reason")] = reason;
    obj[QStringLiteral("file_size")] = fileSize;
    obj[QStringLiteral("local_path")] = localPath;
    obj[QStringLiteral("protected")] = protected_;
    return obj;
}

SnapshotRecord SnapshotRecord::fromJson(const QJsonObject& obj)
{
    SnapshotRecord r;
    r.snapshotId = QUuid::fromString(obj.value(QStringLiteral("snapshot_id")).toString());
    r.createdAt = QDateTime::fromString(obj.value(QStringLiteral("created_at")).toString(), Qt::ISODate);
    r.reason = obj.value(QStringLiteral("reason")).toString();
    r.fileSize = obj.value(QStringLiteral("file_size")).toInt(0);
    r.localPath = obj.value(QStringLiteral("local_path")).toString();
    r.protected_ = obj.value(QStringLiteral("protected")).toBool(false);

    if (!r.createdAt.isValid()) {
        r.createdAt = QDateTime::currentDateTimeUtc();
    }

    return r;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SnapshotService::SnapshotService()
{
}

SnapshotService::SnapshotService(QSharedPointer<Database> db)
    : m_db(std::move(db))
{
    loadFromDatabase();
}

void SnapshotService::setDatabase(QSharedPointer<Database> db)
{
    m_db = std::move(db);
    m_snapshots.clear();
    loadFromDatabase();
}

bool SnapshotService::hasDatabase() const
{
    return !m_db.isNull();
}

// ---------------------------------------------------------------------------
// Index management
// ---------------------------------------------------------------------------

void SnapshotService::loadFromDatabase()
{
    m_snapshots.clear();

    if (!m_db) {
        return;
    }

    const QString raw = m_db->metadata()->customData()->value(SNAPSHOT_INDEX_KEY);
    if (raw.isEmpty()) {
        return;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return;
    }

    const QJsonObject root = doc.object();
    const QJsonArray arr = root.value(QStringLiteral("snapshots")).toArray();
    for (const auto& val : arr) {
        auto rec = SnapshotRecord::fromJson(val.toObject());
        if (!rec.snapshotId.isNull()) {
            m_snapshots.insert(rec.snapshotId.toString(QUuid::Id128), rec);
        }
    }
}

void SnapshotService::saveToDatabase()
{
    if (!m_db) {
        return;
    }

    QJsonArray arr;
    for (const auto& rec : m_snapshots) {
        arr.append(rec.toJson());
    }

    QJsonObject root;
    root[QStringLiteral("snapshots")] = arr;

    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    m_db->metadata()->customData()->set(SNAPSHOT_INDEX_KEY, QString::fromUtf8(json));
}

// ---------------------------------------------------------------------------
// Snapshot operations
// ---------------------------------------------------------------------------

bool SnapshotService::createSnapshot(const QString& reason)
{
    if (!m_db || m_db->filePath().isEmpty()) {
        return false;
    }

    const QUuid snapshotId = QUuid::createUuid();
    const QString snapshotDir = resolveSnapshotDirectory();
    const QString snapshotPath = snapshotDir + QStringLiteral("/") + snapshotFileName(snapshotId);

    if (!ensureSnapshotDirectoryExists()) {
        return false;
    }

    // Save in-memory state to the original file first
    const QString originalPath = m_db->filePath();
    if (m_db->isModified()) {
        QString error;
        if (!m_db->save(Database::Atomic, QString(), &error)) {
            return false;
        }
    }

    // Copy the KDBX file to create the snapshot
    QFile::remove(snapshotPath);
    if (!QFile::copy(originalPath, snapshotPath)) {
        return false;
    }
    QFile::setPermissions(snapshotPath, QFile::ReadOwner | QFile::WriteOwner);

    // Record the snapshot
    SnapshotRecord record;
    record.snapshotId = snapshotId;
    record.createdAt = QDateTime::currentDateTimeUtc();
    record.reason = reason;
    record.fileSize = QFileInfo(snapshotPath).size();
    record.localPath = snapshotPath;
    record.protected_ = false;

    m_snapshots.insert(snapshotId.toString(QUuid::Id128), record);
    saveToDatabase();

    return true;
}

QList<SnapshotRecord> SnapshotService::listSnapshots() const
{
    return m_snapshots.values();
}

SnapshotRecord SnapshotService::snapshotById(const QUuid& snapshotId) const
{
    return m_snapshots.value(snapshotId.toString(QUuid::Id128));
}

bool SnapshotService::deleteSnapshot(const QUuid& snapshotId)
{
    const QString key = snapshotId.toString(QUuid::Id128);
    auto it = m_snapshots.find(key);
    if (it == m_snapshots.end()) {
        return false;
    }

    // Protected snapshots cannot be deleted
    if (it->protected_) {
        return false;
    }

    // Remove the file from disk
    QFile::remove(it->localPath);

    // Remove from index
    m_snapshots.erase(it);
    saveToDatabase();

    return true;
}

bool SnapshotService::restoreSnapshot(const QUuid& snapshotId)
{
    const QString key = snapshotId.toString(QUuid::Id128);
    auto it = m_snapshots.find(key);
    if (it == m_snapshots.end()) {
        return false;
    }

    // Verify the snapshot file exists on disk
    if (!QFile::exists(it->localPath)) {
        return false;
    }

    // Create a protective snapshot before restoring
    createSnapshot(QStringLiteral("Before restore of snapshot %1").arg(snapshotId.toString(QUuid::Id128)));

    // Re-read our record (createSnapshot may have modified m_snapshots)
    it = m_snapshots.find(key);
    if (it == m_snapshots.end()) {
        return false;
    }

    // Copy the snapshot file back to the database path
    const QString originalPath = m_db->filePath();
    QFile::remove(originalPath);
    if (!QFile::copy(it->localPath, originalPath)) {
        return false;
    }
    QFile::setPermissions(originalPath, QFile::ReadOwner | QFile::WriteOwner);

    return true;
}

bool SnapshotService::markProtected(const QUuid& snapshotId)
{
    const QString key = snapshotId.toString(QUuid::Id128);
    auto it = m_snapshots.find(key);
    if (it == m_snapshots.end()) {
        return false;
    }

    it->protected_ = true;
    saveToDatabase();
    return true;
}

bool SnapshotService::unmarkProtected(const QUuid& snapshotId)
{
    const QString key = snapshotId.toString(QUuid::Id128);
    auto it = m_snapshots.find(key);
    if (it == m_snapshots.end()) {
        return false;
    }

    it->protected_ = false;
    saveToDatabase();
    return true;
}

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

QString SnapshotService::defaultSnapshotDirectory(const QString& databasePath)
{
    if (databasePath.isEmpty()) {
        return QDir::homePath() + QStringLiteral("/LocalVault/snapshots/");
    }

    QFileInfo fi(databasePath);
    return fi.absolutePath() + QStringLiteral("/LocalVault/snapshots/");
}

QString SnapshotService::snapshotFileName(const QUuid& snapshotId)
{
    return QStringLiteral("snapshot_%1.kdbx").arg(snapshotId.toString(QUuid::Id128));
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

QString SnapshotService::resolveSnapshotDirectory() const
{
    // Check config for custom snapshot directory
    const QString configured = Config::instance()->get(Config::Snapshot_Directory).toString();
    if (!configured.isEmpty()) {
        return configured;
    }

    // Fall back to database-relative default
    return defaultSnapshotDirectory(m_db ? m_db->filePath() : QString());
}

bool SnapshotService::ensureSnapshotDirectoryExists() const
{
    const QDir dir(resolveSnapshotDirectory());
    if (!dir.exists()) {
        return QDir().mkpath(dir.absolutePath());
    }
    return true;
}
