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
};

// ---------------------------------------------------------------------------
// DeviceIdentity
// ---------------------------------------------------------------------------

void TestSyncMetadata::testDeviceIdentityJsonRoundTrip()
{
    DeviceIdentity dev;
    dev.deviceId = QStringLiteral("550e8400e29b41d4a716446655440000");
    dev.deviceName = QStringLiteral("test-device");
    dev.registeredAt = QDateTime(QDate(2026, 5, 10), QTime(14, 30, 0), QTimeZone::UTC);
    dev.lastSeenAt = QDateTime(QDate(2026, 5, 14), QTime(9, 0, 0), QTimeZone::UTC);

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
    bl.lastSuccessSyncAt = QDateTime(QDate(2026, 5, 14), QTime(9, 0, 0), QTimeZone::UTC);

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
    tb.deletedAt = QDateTime(QDate(2026, 5, 13), QTime(18, 0, 0), QTimeZone::UTC);
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
    cr.createdAt = QDateTime(QDate(2026, 5, 13), QTime(12, 0, 0), QTimeZone::UTC);
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
    QCOMPARE(engine.schemaVersion(), 1);
    QVERIFY(engine.deviceRegistry().isEmpty());
    QVERIFY(engine.tombstones().isEmpty());
    QVERIFY(engine.conflicts().isEmpty());

    // Fresh database metadata: deviceId should be empty until registered
    QVERIFY(!engine.hasDatabase() || engine.currentDeviceId().isEmpty());
}

QTEST_GUILESS_MAIN(TestSyncMetadata)
#include "TestSyncMetadata.moc"
