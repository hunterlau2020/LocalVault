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

#include "core/SyncMetadata.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/Metadata.h"

#include <QTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSharedPointer>
#include <QUuid>
#include <QTimeZone>

Q_DECLARE_METATYPE(VVCompareResult)

class TestSyncMetadata : public QObject
{
    Q_OBJECT

private slots:
    void testDeviceIdentityJsonRoundTrip();
    void testSyncBaselineJsonRoundTrip();
    void testTombstoneJsonRoundTrip();
    void testConflictRecordJsonRoundTrip();
    void testVersionVectorCompare();
    void testVersionVectorMerge();
    void testEmptyEngine();
    // Phase 8: integrity baseline & schema migration
    void testIntegritySummaryJsonRoundTrip();
    void testIntegritySummaryPersistence();
    void testSchemaMigrationV1ToV2();
};

// ---------------------------------------------------------------------------
// DeviceIdentity
// ---------------------------------------------------------------------------

void TestSyncMetadata::testDeviceIdentityJsonRoundTrip()
{
    DeviceIdentity dev;
    dev.deviceId = QStringLiteral("550e8400e29b41d4a716446655440000");
    dev.deviceName = QStringLiteral("test-device");
    dev.registeredAt = QDateTime(QDate(2026, 5, 10), QTime(14, 30, 0), QTimeZone::utc());
    dev.lastSeenAt = QDateTime(QDate(2026, 5, 14), QTime(9, 0, 0), QTimeZone::utc());

    // Round trip
    const QJsonObject json = dev.toJson();
    const DeviceIdentity restored = DeviceIdentity::fromJson(json);

    QCOMPARE(restored.deviceId, dev.deviceId);
    QCOMPARE(restored.deviceName, dev.deviceName);
    QCOMPARE(restored.registeredAt, dev.registeredAt);
    QCOMPARE(restored.lastSeenAt, dev.lastSeenAt);
}

// ---------------------------------------------------------------------------
// SyncBaseline
// ---------------------------------------------------------------------------

void TestSyncMetadata::testSyncBaselineJsonRoundTrip()
{
    SyncBaseline bl;
    bl.remoteId = QStringLiteral("remote-hash-123");
    bl.lastPulledCursor = QStringLiteral("cursor-v3");
    bl.lastPushedCursor = QStringLiteral("cursor-v3");
    bl.lastSuccessSyncAt = QDateTime(QDate(2026, 5, 14), QTime(9, 0, 0), QTimeZone::utc());

    const QJsonObject json = bl.toJson();
    const SyncBaseline restored = SyncBaseline::fromJson(json);

    QCOMPARE(restored.remoteId, bl.remoteId);
    QCOMPARE(restored.lastPulledCursor, bl.lastPulledCursor);
    QCOMPARE(restored.lastPushedCursor, bl.lastPushedCursor);
    QCOMPARE(restored.lastSuccessSyncAt, bl.lastSuccessSyncAt);
}

// ---------------------------------------------------------------------------
// TombstoneRecord
// ---------------------------------------------------------------------------

void TestSyncMetadata::testTombstoneJsonRoundTrip()
{
    TombstoneRecord tb;
    tb.entryId = QUuid::fromString(QStringLiteral("660e8400e29b41d4a716446655440001"));
    tb.deletedAt = QDateTime(QDate(2026, 5, 13), QTime(18, 0, 0), QTimeZone::utc());
    tb.deletedBy = QStringLiteral("dev-123");

    const QJsonObject json = tb.toJson();
    const TombstoneRecord restored = TombstoneRecord::fromJson(json);

    QCOMPARE(restored.entryId, tb.entryId);
    QCOMPARE(restored.deletedAt, tb.deletedAt);
    QCOMPARE(restored.deletedBy, tb.deletedBy);
}

// ---------------------------------------------------------------------------
// ConflictRecord
// ---------------------------------------------------------------------------

void TestSyncMetadata::testConflictRecordJsonRoundTrip()
{
    ConflictRecord cr;
    cr.entryId = QUuid::fromString(QStringLiteral("770e8400e29b41d4a716446655440002"));
    cr.conflictId = QUuid::fromString(QStringLiteral("880e8400e29b41d4a716446655440003"));
    cr.createdAt = QDateTime(QDate(2026, 5, 13), QTime(12, 0, 0), QTimeZone::utc());
    cr.resolved = false;
    cr.conflictingFields << QStringLiteral("url") << QStringLiteral("notes");

    const QJsonObject json = cr.toJson();
    const ConflictRecord restored = ConflictRecord::fromJson(json);

    QCOMPARE(restored.entryId, cr.entryId);
    QCOMPARE(restored.conflictId, cr.conflictId);
    QCOMPARE(restored.createdAt, cr.createdAt);
    QCOMPARE(restored.resolved, cr.resolved);
    QCOMPARE(restored.conflictingFields, cr.conflictingFields);
}

// ---------------------------------------------------------------------------
// Version vector comparison
// ---------------------------------------------------------------------------

void TestSyncMetadata::testVersionVectorCompare()
{
    // Equal
    QCOMPARE(SyncMetadataEngine::compareVersionVectors({}, {}), VVCompareResult::Equal);
    QCOMPARE(SyncMetadataEngine::compareVersionVectors({{QStringLiteral("A"), 1}}, {{QStringLiteral("A"), 1}}), VVCompareResult::Equal);

    // Local dominates
    QCOMPARE(SyncMetadataEngine::compareVersionVectors({{QStringLiteral("A"), 2}}, {{QStringLiteral("A"), 1}}), VVCompareResult::LocalDominates);
    QCOMPARE(SyncMetadataEngine::compareVersionVectors({{QStringLiteral("A"), 2}, {QStringLiteral("B"), 1}}, {{QStringLiteral("A"), 1}}), VVCompareResult::LocalDominates);

    // Remote dominates
    QCOMPARE(SyncMetadataEngine::compareVersionVectors({{QStringLiteral("A"), 1}}, {{QStringLiteral("A"), 2}}), VVCompareResult::RemoteDominates);
    QCOMPARE(SyncMetadataEngine::compareVersionVectors({{QStringLiteral("A"), 1}}, {{QStringLiteral("A"), 2}, {QStringLiteral("B"), 1}}), VVCompareResult::RemoteDominates);

    // Concurrent
    QCOMPARE(SyncMetadataEngine::compareVersionVectors({{QStringLiteral("A"), 2}}, {{QStringLiteral("B"), 1}}), VVCompareResult::Concurrent);
    QCOMPARE(SyncMetadataEngine::compareVersionVectors({{QStringLiteral("A"), 2}, {QStringLiteral("B"), 1}}, {{QStringLiteral("A"), 1}, {QStringLiteral("B"), 2}}), VVCompareResult::Concurrent);
}

// ---------------------------------------------------------------------------
// Version vector merge
// ---------------------------------------------------------------------------

void TestSyncMetadata::testVersionVectorMerge()
{
    // Basic merge
    const VersionVector a{{QStringLiteral("A"), 3}, {QStringLiteral("B"), 1}};
    const VersionVector b{{QStringLiteral("A"), 1}, {QStringLiteral("B"), 2}};
    const VersionVector merged = SyncMetadataEngine::mergeVersionVectors(a, b);

    QCOMPARE(merged.value(QStringLiteral("A")), 3);
    QCOMPARE(merged.value(QStringLiteral("B")), 2);

    // Merge with disjoint devices
    const VersionVector c{{QStringLiteral("A"), 1}};
    const VersionVector d{{QStringLiteral("B"), 1}};
    const VersionVector merged2 = SyncMetadataEngine::mergeVersionVectors(c, d);

    QCOMPARE(merged2.value(QStringLiteral("A")), 1);
    QCOMPARE(merged2.value(QStringLiteral("B")), 1);

    // Merge empty
    QVERIFY(SyncMetadataEngine::mergeVersionVectors({}, {}).isEmpty());
    QCOMPARE(SyncMetadataEngine::mergeVersionVectors(a, {}).value(QStringLiteral("A")), 3);
}

// ---------------------------------------------------------------------------
// Empty engine
// ---------------------------------------------------------------------------

void TestSyncMetadata::testEmptyEngine()
{
    auto db = QSharedPointer<Database>(new Database());
    db->metadata()->setName(QStringLiteral("test"));

    SyncMetadataEngine engine(db);
    QCOMPARE(engine.schemaVersion(), 2); // v2-capable engine (Phase 8 schema bump)
    QVERIFY(engine.deviceRegistry().isEmpty());
    QVERIFY(engine.tombstones().isEmpty());
    QVERIFY(engine.conflicts().isEmpty());

    // Fresh database metadata: deviceId should be empty until registered
    QVERIFY(!engine.hasDatabase() || engine.currentDeviceId().isEmpty());
}

// ---------------------------------------------------------------------------
// IntegritySummary (Phase 8)
// ---------------------------------------------------------------------------

void TestSyncMetadata::testIntegritySummaryJsonRoundTrip()
{
    IntegritySummary s;
    s.fileSha256 = QStringLiteral("abc123def");
    s.fileSize = 4096;
    s.fileMtimeUtc = QDateTime(QDate(2026, 7, 26), QTime(12, 0, 0), QTimeZone::utc());
    s.metadataRootDigest = QStringLiteral("feedface");
    s.checkedAtUtc = QDateTime(QDate(2026, 7, 26), QTime(12, 5, 0), QTimeZone::utc());

    QVERIFY(!s.isEmpty());
    const QJsonObject json = s.toJson();
    const IntegritySummary restored = IntegritySummary::fromJson(json);

    QCOMPARE(restored.fileSha256, s.fileSha256);
    QCOMPARE(restored.fileSize, s.fileSize);
    QCOMPARE(restored.fileMtimeUtc, s.fileMtimeUtc);
    QCOMPARE(restored.metadataRootDigest, s.metadataRootDigest);
    QCOMPARE(restored.checkedAtUtc, s.checkedAtUtc);
    QVERIFY(!restored.isEmpty());

    // Default-constructed summary is empty.
    QVERIFY(IntegritySummary{}.isEmpty());
}

void TestSyncMetadata::testIntegritySummaryPersistence()
{
    auto db = QSharedPointer<Database>(new Database());
    db->metadata()->setName(QStringLiteral("integrity"));

    SyncMetadataEngine engine(db);
    QVERIFY(engine.integritySummary().isEmpty());

    IntegritySummary s;
    s.fileSha256 = QStringLiteral("deadbeef");
    s.metadataRootDigest = QStringLiteral("cafef00d");
    s.fileSize = 1024;
    engine.setIntegritySummary(s);
    engine.saveToDatabase();

    // Reload from the same DB and verify round-trip.
    SyncMetadataEngine reloaded(db);
    QVERIFY(!reloaded.integritySummary().isEmpty());
    QCOMPARE(reloaded.integritySummary().fileSha256, QStringLiteral("deadbeef"));
    QCOMPARE(reloaded.integritySummary().metadataRootDigest, QStringLiteral("cafef00d"));
    QCOMPARE(reloaded.integritySummary().fileSize, 1024);

    // clearIntegritySummary + save → reloaded is empty again.
    engine.clearIntegritySummary();
    engine.saveToDatabase();
    SyncMetadataEngine afterClear(db);
    QVERIFY(afterClear.integritySummary().isEmpty());
}

// ---------------------------------------------------------------------------
// Schema migration v1 → v2 (Phase 8)
// ---------------------------------------------------------------------------

void TestSyncMetadata::testSchemaMigrationV1ToV2()
{
    auto db = QSharedPointer<Database>(new Database());
    db->metadata()->setName(QStringLiteral("v1-migration"));

    // Simulate a legacy v1 database: schema_version=1, the four old fields, NO integrity_summary.
    const QString v1Json = QStringLiteral(
        "{\"schema_version\":1,"
        "\"device_registry\":[{\"device_id\":\"dev1\",\"device_name\":\"d\","
        "\"registered_at\":\"2026-05-10T14:30:00Z\",\"last_seen_at\":\"2026-05-10T14:30:00Z\"}],"
        "\"sync_baselines\":[{\"remote_id\":\"r1\",\"last_pulled_cursor\":\"c\",\"last_pushed_cursor\":\"c\"}],"
        "\"tombstones\":[{\"entry_id\":\"660e8400e29b41d4a716446655440001\","
        "\"deleted_at\":\"2026-05-13T18:00:00Z\",\"deleted_by\":\"dev1\"}],"
        "\"conflicts\":[{\"entry_id\":\"770e8400e29b41d4a716446655440002\","
        "\"conflict_id\":\"880e8400e29b41d4a716446655440003\",\"created_at\":\"2026-05-13T12:00:00Z\","
        "\"resolved\":false,\"conflicting_fields\":[\"url\"]}]}");
    db->metadata()->customData()->set(SyncMetadataEngine::DB_METADATA_KEY, v1Json);

    SyncMetadataEngine engine(db);

    // v1 loads honestly as v1; integrity_summary absent → empty; old fields preserved.
    QCOMPARE(engine.schemaVersion(), 1);
    QVERIFY(engine.integritySummary().isEmpty());
    QCOMPARE(engine.deviceRegistry().size(), 1);
    QCOMPARE(engine.syncBaseline(QStringLiteral("r1")).remoteId, QStringLiteral("r1"));
    QCOMPARE(engine.tombstones().size(), 1);
    QCOMPARE(engine.conflicts().size(), 1);

    // Saving with an EMPTY integrity_summary does NOT claim v2 (review #4): the
    // schema must reflect that no baseline has been recorded yet. integrity_summary
    // is still written (empty), and the old fields are preserved non-destructively.
    engine.saveToDatabase();
    QCOMPARE(engine.schemaVersion(), 1);

    {
        const QString raw = db->metadata()->customData()->value(SyncMetadataEngine::DB_METADATA_KEY);
        const auto doc = QJsonDocument::fromJson(raw.toUtf8());
        QVERIFY(doc.isObject());
        const QJsonObject root = doc.object();
        QCOMPARE(root.value(QStringLiteral("schema_version")).toInt(), 1);
        QVERIFY(root.contains(QStringLiteral("integrity_summary"))); // present but empty
        QCOMPARE(root.value(QStringLiteral("device_registry")).toArray().size(), 1);
        QCOMPARE(root.value(QStringLiteral("sync_baselines")).toArray().size(), 1);
        QCOMPARE(root.value(QStringLiteral("tombstones")).toArray().size(), 1);
        QCOMPARE(root.value(QStringLiteral("conflicts")).toArray().size(), 1);
    }

    // Once a real baseline is recorded, the schema upgrades to v2.
    IntegritySummary summary;
    summary.metadataRootDigest = QStringLiteral("deadbeef");
    summary.fileSha256 = QStringLiteral("cafef00d");
    engine.setIntegritySummary(summary);
    engine.saveToDatabase();
    QCOMPARE(engine.schemaVersion(), 2);

    // A freshly-loaded engine on the upgraded DB reports v2 with the recorded summary.
    SyncMetadataEngine reloaded(db);
    QCOMPARE(reloaded.schemaVersion(), 2);
    QVERIFY(!reloaded.integritySummary().isEmpty());
    QCOMPARE(reloaded.integritySummary().metadataRootDigest, QStringLiteral("deadbeef"));
    QCOMPARE(reloaded.deviceRegistry().size(), 1);
    QCOMPARE(reloaded.conflicts().size(), 1);
}

QTEST_GUILESS_MAIN(TestSyncMetadata)
#include "TestSyncMetadata.moc"
