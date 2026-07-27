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

#include "SyncMetadata.h"

#include "core/Database.h"
#include "core/Entry.h"
#include "core/Metadata.h"
#include "core/CustomData.h"
#include "core/Config.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

// ---------------------------------------------------------------------------
// Key constants
// ---------------------------------------------------------------------------

const QString SyncMetadataEngine::DB_METADATA_KEY = QStringLiteral("KPXC_SYNC_METADATA");
const QString SyncMetadataEngine::ENTRY_VV_KEY = QStringLiteral("KPXC_SYNC_VV");

// Parse a UUID stored in either the dashed format or the Id128 (no-dash) format.
// QUuid::fromString does not accept the Id128 format that toJson() emits via
// QUuid::Id128, so re-insert dashes before delegating to fromString. This keeps
// tombstone/conflict records round-trippable (backward compatible with on-disk v1 data).
static QUuid parseUuidLoose(const QString& s)
{
    if (s.length() == 32 && !s.contains(QLatin1Char('-'))) {
        const QString dashed = s.left(8) + QStringLiteral("-") + s.mid(8, 4) + QStringLiteral("-")
                             + s.mid(12, 4) + QStringLiteral("-") + s.mid(16, 4) + QStringLiteral("-")
                             + s.mid(20);
        return QUuid::fromString(dashed);
    }
    return QUuid::fromString(s);
}

// ---------------------------------------------------------------------------
// DeviceIdentity
// ---------------------------------------------------------------------------

QJsonObject DeviceIdentity::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("device_id")] = deviceId;
    obj[QStringLiteral("device_name")] = deviceName;
    obj[QStringLiteral("registered_at")] = registeredAt.toUTC().toString(Qt::ISODate);
    obj[QStringLiteral("last_seen_at")] = lastSeenAt.toUTC().toString(Qt::ISODate);
    return obj;
}

DeviceIdentity DeviceIdentity::fromJson(const QJsonObject& obj)
{
    DeviceIdentity d;
    d.deviceId = obj.value(QStringLiteral("device_id")).toString();
    d.deviceName = obj.value(QStringLiteral("device_name")).toString();
    d.registeredAt = QDateTime::fromString(obj.value(QStringLiteral("registered_at")).toString(), Qt::ISODate);
    d.lastSeenAt = QDateTime::fromString(obj.value(QStringLiteral("last_seen_at")).toString(), Qt::ISODate);
    if (!d.registeredAt.isValid()) {
        d.registeredAt = QDateTime::currentDateTimeUtc();
    }
    if (!d.lastSeenAt.isValid()) {
        d.lastSeenAt = d.registeredAt;
    }
    return d;
}

// ---------------------------------------------------------------------------
// SyncBaseline
// ---------------------------------------------------------------------------

QJsonObject SyncBaseline::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("remote_id")] = remoteId;
    obj[QStringLiteral("last_pulled_cursor")] = lastPulledCursor;
    obj[QStringLiteral("last_pushed_cursor")] = lastPushedCursor;
    if (lastSuccessSyncAt.isValid()) {
        obj[QStringLiteral("last_success_sync_at")] = lastSuccessSyncAt.toUTC().toString(Qt::ISODate);
    }
    return obj;
}

SyncBaseline SyncBaseline::fromJson(const QJsonObject& obj)
{
    SyncBaseline b;
    b.remoteId = obj.value(QStringLiteral("remote_id")).toString();
    b.lastPulledCursor = obj.value(QStringLiteral("last_pulled_cursor")).toString();
    b.lastPushedCursor = obj.value(QStringLiteral("last_pushed_cursor")).toString();
    b.lastSuccessSyncAt = QDateTime::fromString(obj.value(QStringLiteral("last_success_sync_at")).toString(), Qt::ISODate);
    return b;
}

// ---------------------------------------------------------------------------
// TombstoneRecord
// ---------------------------------------------------------------------------

QJsonObject TombstoneRecord::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("entry_id")] = entryId.toString(QUuid::Id128);
    obj[QStringLiteral("deleted_at")] = deletedAt.toUTC().toString(Qt::ISODate);
    obj[QStringLiteral("deleted_by")] = deletedBy;
    return obj;
}

TombstoneRecord TombstoneRecord::fromJson(const QJsonObject& obj)
{
    TombstoneRecord t;
    t.entryId = parseUuidLoose(obj.value(QStringLiteral("entry_id")).toString());
    t.deletedAt = QDateTime::fromString(obj.value(QStringLiteral("deleted_at")).toString(), Qt::ISODate);
    t.deletedBy = obj.value(QStringLiteral("deleted_by")).toString();
    if (!t.deletedAt.isValid()) {
        t.deletedAt = QDateTime::currentDateTimeUtc();
    }
    return t;
}

// ---------------------------------------------------------------------------
// ConflictRecord
// ---------------------------------------------------------------------------

QJsonObject ConflictRecord::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("entry_id")] = entryId.toString(QUuid::Id128);
    obj[QStringLiteral("conflict_id")] = conflictId.toString(QUuid::Id128);
    obj[QStringLiteral("created_at")] = createdAt.toUTC().toString(Qt::ISODate);
    obj[QStringLiteral("resolved")] = resolved;
    QJsonArray fields;
    for (const auto& f : conflictingFields) {
        fields.append(f);
    }
    obj[QStringLiteral("conflicting_fields")] = fields;
    return obj;
}

ConflictRecord ConflictRecord::fromJson(const QJsonObject& obj)
{
    ConflictRecord c;
    c.entryId = parseUuidLoose(obj.value(QStringLiteral("entry_id")).toString());
    c.conflictId = parseUuidLoose(obj.value(QStringLiteral("conflict_id")).toString());
    c.createdAt = QDateTime::fromString(obj.value(QStringLiteral("created_at")).toString(), Qt::ISODate);
    c.resolved = obj.value(QStringLiteral("resolved")).toBool(false);
    const auto fields = obj.value(QStringLiteral("conflicting_fields")).toArray();
    for (const auto& f : fields) {
        c.conflictingFields.append(f.toString());
    }
    if (!c.createdAt.isValid()) {
        c.createdAt = QDateTime::currentDateTimeUtc();
    }
    return c;
}

// ---------------------------------------------------------------------------
// IntegritySummary
// ---------------------------------------------------------------------------

bool IntegritySummary::isEmpty() const
{
    return metadataRootDigest.isEmpty() && fileSha256.isEmpty();
}

QJsonObject IntegritySummary::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("file_sha256")] = fileSha256;
    obj[QStringLiteral("metadata_root_digest")] = metadataRootDigest;
    if (checkedAtUtc.isValid()) {
        obj[QStringLiteral("checked_at_utc")] = checkedAtUtc.toUTC().toString(Qt::ISODate);
    }
    return obj;
}

IntegritySummary IntegritySummary::fromJson(const QJsonObject& obj)
{
    IntegritySummary s;
    s.fileSha256 = obj.value(QStringLiteral("file_sha256")).toString();
    s.metadataRootDigest = obj.value(QStringLiteral("metadata_root_digest")).toString();
    s.checkedAtUtc = QDateTime::fromString(obj.value(QStringLiteral("checked_at_utc")).toString(), Qt::ISODate);
    return s;
}

// ---------------------------------------------------------------------------
// SyncMetadataEngine
// ---------------------------------------------------------------------------

SyncMetadataEngine::SyncMetadataEngine()
    : m_schemaVersion(2)
{
}

SyncMetadataEngine::SyncMetadataEngine(QSharedPointer<Database> db)
    : m_db(std::move(db))
    , m_schemaVersion(2)
{
    loadFromDatabase();
}

void SyncMetadataEngine::setDatabase(QSharedPointer<Database> db)
{
    m_db = std::move(db);
    m_cachedDeviceId.clear();
    loadFromDatabase();
}

bool SyncMetadataEngine::hasDatabase() const
{
    return !m_db.isNull();
}

// ---------------------------------------------------------------------------
// Load / Save
// ---------------------------------------------------------------------------

void SyncMetadataEngine::loadFromDatabase()
{
    // Reset caches
    m_deviceRegistry.clear();
    m_syncBaselines.clear();
    m_tombstones.clear();
    m_conflicts.clear();
    m_integritySummary = IntegritySummary{};
    m_schemaVersion = 2;

    if (!m_db) {
        return;
    }

    const QString raw = m_db->metadata()->customData()->value(DB_METADATA_KEY);
    if (raw.isEmpty()) {
        return; // nothing loaded yet, use defaults
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        // Corrupt data — silently reset (could log in future)
        return;
    }

    const QJsonObject root = doc.object();

    m_schemaVersion = root.value(QStringLiteral("schema_version")).toInt(1);

    // Device registry
    const QJsonArray devArr = root.value(QStringLiteral("device_registry")).toArray();
    for (const auto& val : devArr) {
        const auto dev = DeviceIdentity::fromJson(val.toObject());
        if (!dev.deviceId.isEmpty()) {
            m_deviceRegistry.insert(dev.deviceId, dev);
        }
    }

    // Sync baselines
    const QJsonArray blArr = root.value(QStringLiteral("sync_baselines")).toArray();
    for (const auto& val : blArr) {
        const auto bl = SyncBaseline::fromJson(val.toObject());
        if (!bl.remoteId.isEmpty()) {
            m_syncBaselines.insert(bl.remoteId, bl);
        }
    }

    // Tombstones
    const QJsonArray tbArr = root.value(QStringLiteral("tombstones")).toArray();
    for (const auto& val : tbArr) {
        const auto tb = TombstoneRecord::fromJson(val.toObject());
        if (!tb.entryId.isNull()) {
            m_tombstones.insert(tb.entryId.toString(QUuid::Id128), tb);
        }
    }

    // Conflicts
    const QJsonArray cfArr = root.value(QStringLiteral("conflicts")).toArray();
    for (const auto& val : cfArr) {
        m_conflicts.append(ConflictRecord::fromJson(val.toObject()));
    }

    // Integrity baseline (schema v2; absent in v1 databases → empty summary)
    m_integritySummary =
        IntegritySummary::fromJson(root.value(QStringLiteral("integrity_summary")).toObject());
}

QJsonObject SyncMetadataEngine::digestMetadataJson() const
{
    QJsonObject root;

    // Device registry
    QJsonArray devArr;
    for (const auto& dev : m_deviceRegistry) {
        devArr.append(dev.toJson());
    }
    root[QStringLiteral("device_registry")] = devArr;

    // Sync baselines
    QJsonArray blArr;
    for (const auto& bl : m_syncBaselines) {
        blArr.append(bl.toJson());
    }
    root[QStringLiteral("sync_baselines")] = blArr;

    // Tombstones
    QJsonArray tbArr;
    for (const auto& tb : m_tombstones) {
        tbArr.append(tb.toJson());
    }
    root[QStringLiteral("tombstones")] = tbArr;

    // Conflicts
    QJsonArray cfArr;
    for (const auto& cf : m_conflicts) {
        cfArr.append(cf.toJson());
    }
    root[QStringLiteral("conflicts")] = cfArr;

    return root;
}

void SyncMetadataEngine::saveToDatabase()
{
    if (!m_db) {
        return;
    }

    // v2 introduces the integrity_summary baseline object. Only claim v2 once a
    // baseline has actually been recorded — a database whose integrity_summary is
    // still empty should not be marked v2 just because v2-capable code saved it
    // (review #4: schema_version should reflect actual metadata evolution).
    if (m_schemaVersion < 2 && !m_integritySummary.isEmpty()) {
        m_schemaVersion = 2;
    }

    QJsonObject root = digestMetadataJson();
    root[QStringLiteral("schema_version")] = m_schemaVersion;
    root[QStringLiteral("integrity_summary")] = m_integritySummary.toJson();

    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    m_db->metadata()->customData()->set(DB_METADATA_KEY, QString::fromUtf8(json));
}

// ---------------------------------------------------------------------------
// Integrity baseline (Phase 8)
// ---------------------------------------------------------------------------

IntegritySummary SyncMetadataEngine::integritySummary() const
{
    return m_integritySummary;
}

void SyncMetadataEngine::setIntegritySummary(const IntegritySummary& summary)
{
    m_integritySummary = summary;
}

void SyncMetadataEngine::clearIntegritySummary()
{
    m_integritySummary = IntegritySummary{};
}

void SyncMetadataEngine::clearSyncBaselines()
{
    m_syncBaselines.clear();
}

// ---------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------

int SyncMetadataEngine::schemaVersion() const
{
    return m_schemaVersion;
}

// ---------------------------------------------------------------------------
// Device registry
// ---------------------------------------------------------------------------

QString SyncMetadataEngine::registerOrLoadCurrentDevice(const QString& deviceName)
{
    // Check Config for existing device identity
    const QString existingId = Config::instance()->get(Config::Sync_LocalDeviceId).toString();
    if (!existingId.isEmpty()) {
        // Update lastSeenAt in registry
        auto it = m_deviceRegistry.find(existingId);
        if (it != m_deviceRegistry.end()) {
            it->lastSeenAt = QDateTime::currentDateTimeUtc();
        } else {
            // Registry entry missing — re-create from Config
            DeviceIdentity dev;
            dev.deviceId = existingId;
            dev.deviceName = Config::instance()->get(Config::Sync_LocalDeviceName).toString();
            if (dev.deviceName.isEmpty()) {
                dev.deviceName = deviceName.isEmpty() ? QStringLiteral("Local") : deviceName;
            }
            dev.registeredAt = QDateTime::currentDateTimeUtc();
            dev.lastSeenAt = dev.registeredAt;
            m_deviceRegistry.insert(existingId, dev);
        }
        m_cachedDeviceId = existingId;
        return existingId;
    }

    // First registration — generate new device ID
    const QString newId = QUuid::createUuid().toString(QUuid::Id128);
    const QString name = deviceName.isEmpty() ? QStringLiteral("Local") : deviceName;

    Config::instance()->set(Config::Sync_LocalDeviceId, newId);
    Config::instance()->set(Config::Sync_LocalDeviceName, name);

    DeviceIdentity dev;
    dev.deviceId = newId;
    dev.deviceName = name;
    dev.registeredAt = QDateTime::currentDateTimeUtc();
    dev.lastSeenAt = dev.registeredAt;
    m_deviceRegistry.insert(newId, dev);

    m_cachedDeviceId = newId;
    return newId;
}

QString SyncMetadataEngine::currentDeviceId() const
{
    if (m_cachedDeviceId.isEmpty()) {
        m_cachedDeviceId = Config::instance()->get(Config::Sync_LocalDeviceId).toString();
    }
    return m_cachedDeviceId;
}

QList<DeviceIdentity> SyncMetadataEngine::deviceRegistry() const
{
    return m_deviceRegistry.values();
}

// ---------------------------------------------------------------------------
// Entry version vectors
// ---------------------------------------------------------------------------

VersionVector SyncMetadataEngine::getEntryVersionVector(Entry* entry) const
{
    if (!entry) {
        return {};
    }

    const QString raw = entry->customData()->value(ENTRY_VV_KEY);
    if (raw.isEmpty()) {
        return {};
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return {};
    }

    const QJsonObject obj = doc.object();
    const QJsonObject vvObj = obj.value(QStringLiteral("vv")).toObject();

    VersionVector vv;
    for (auto it = vvObj.begin(); it != vvObj.end(); ++it) {
        vv.insert(it.key(), it.value().toInt(0));
    }
    return vv;
}

void SyncMetadataEngine::setEntryVersionVector(Entry* entry, const VersionVector& vv)
{
    if (!entry) {
        return;
    }

    QJsonObject vvObj;
    for (auto it = vv.begin(); it != vv.end(); ++it) {
        vvObj[it.key()] = it.value();
    }

    QJsonObject root;
    root[QStringLiteral("vv")] = vvObj;

    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    entry->customData()->set(ENTRY_VV_KEY, QString::fromUtf8(json));
}

VersionVector SyncMetadataEngine::incrementEntryCounter(Entry* entry)
{
    VersionVector vv = getEntryVersionVector(entry);
    const QString myId = currentDeviceId();
    if (!myId.isEmpty()) {
        vv[myId] = vv.value(myId, 0) + 1;
    }
    setEntryVersionVector(entry, vv);
    return vv;
}

// ---------------------------------------------------------------------------
// Comparison (static)
// ---------------------------------------------------------------------------

VVCompareResult SyncMetadataEngine::compareVersionVectors(const VersionVector& local, const VersionVector& remote)
{
    // Collect all device IDs from both vectors
    QSet<QString> allDevices;
    for (auto it = local.begin(); it != local.end(); ++it) {
        allDevices.insert(it.key());
    }
    for (auto it = remote.begin(); it != remote.end(); ++it) {
        allDevices.insert(it.key());
    }

    if (allDevices.isEmpty()) {
        return VVCompareResult::Equal;
    }

    bool localNewer = false;
    bool remoteNewer = false;

    for (const auto& deviceId : allDevices) {
        const int lv = local.value(deviceId, 0);
        const int rv = remote.value(deviceId, 0);

        if (lv > rv) {
            localNewer = true;
        } else if (rv > lv) {
            remoteNewer = true;
        }
    }

    if (!localNewer && !remoteNewer) {
        return VVCompareResult::Equal;
    }
    if (localNewer && !remoteNewer) {
        return VVCompareResult::LocalDominates;
    }
    if (!localNewer && remoteNewer) {
        return VVCompareResult::RemoteDominates;
    }
    return VVCompareResult::Concurrent;
}

VersionVector SyncMetadataEngine::mergeVersionVectors(const VersionVector& a, const VersionVector& b)
{
    VersionVector result = a;
    for (auto it = b.begin(); it != b.end(); ++it) {
        const int existing = result.value(it.key(), 0);
        if (it.value() > existing) {
            result[it.key()] = it.value();
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Tombstones
// ---------------------------------------------------------------------------

void SyncMetadataEngine::addTombstone(const QUuid& entryId, const QDateTime& deletedAt, const QString& deletedBy)
{
    if (entryId.isNull()) {
        return;
    }

    TombstoneRecord tb;
    tb.entryId = entryId;
    tb.deletedAt = deletedAt.isValid() ? deletedAt : QDateTime::currentDateTimeUtc();
    tb.deletedBy = !deletedBy.isEmpty() ? deletedBy : currentDeviceId();
    m_tombstones.insert(entryId.toString(QUuid::Id128), tb);
}

TombstoneRecord SyncMetadataEngine::getTombstone(const QUuid& entryId) const
{
    return m_tombstones.value(entryId.toString(QUuid::Id128));
}

bool SyncMetadataEngine::isTombstone(const QUuid& entryId) const
{
    return m_tombstones.contains(entryId.toString(QUuid::Id128));
}

void SyncMetadataEngine::removeTombstone(const QUuid& entryId)
{
    m_tombstones.remove(entryId.toString(QUuid::Id128));
}

QList<TombstoneRecord> SyncMetadataEngine::tombstones() const
{
    return m_tombstones.values();
}

// ---------------------------------------------------------------------------
// Conflicts
// ---------------------------------------------------------------------------

void SyncMetadataEngine::recordConflict(const QUuid& entryId, const QStringList& fields)
{
    ConflictRecord cr;
    cr.entryId = entryId;
    cr.conflictId = QUuid::createUuid();
    cr.createdAt = QDateTime::currentDateTimeUtc();
    cr.resolved = false;
    cr.conflictingFields = fields;
    m_conflicts.append(cr);
}

void SyncMetadataEngine::markConflictResolved(const QUuid& conflictId)
{
    for (auto& cr : m_conflicts) {
        if (cr.conflictId == conflictId) {
            cr.resolved = true;
            return;
        }
    }
}

ConflictRecord SyncMetadataEngine::conflictById(const QUuid& conflictId) const
{
    for (const auto& cr : m_conflicts) {
        if (cr.conflictId == conflictId) {
            return cr;
        }
    }
    return {};
}

QList<ConflictRecord> SyncMetadataEngine::conflicts() const
{
    return m_conflicts;
}

// ---------------------------------------------------------------------------
// Sync baselines
// ---------------------------------------------------------------------------

void SyncMetadataEngine::updateSyncBaseline(const QString& remoteId, const QString& cursor)
{
    SyncBaseline& bl = m_syncBaselines[remoteId]; // default-constructs if missing
    bl.remoteId = remoteId;
    bl.lastPulledCursor = cursor;
    bl.lastPushedCursor = cursor;
    bl.lastSuccessSyncAt = QDateTime::currentDateTimeUtc();
}

SyncBaseline SyncMetadataEngine::syncBaseline(const QString& remoteId) const
{
    return m_syncBaselines.value(remoteId);
}
