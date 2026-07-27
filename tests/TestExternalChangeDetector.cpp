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

#include "core/ExternalChangeDetector.h"
#include "core/IntegrityDigest.h"
#include "core/SyncMetadata.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/Metadata.h"
#include "crypto/Crypto.h"
#include "keys/CompositeKey.h"
#include "keys/PasswordKey.h"

#include <QDir>
#include <QFile>
#include <QSharedPointer>
#include <QUuid>
#include <QTest>

class TestExternalChangeDetector : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void testNoBaselineIsClean();
    void testCleanAfterRecordedBaseline();
    void testContentChangeDetected();
    void testMetadataChangeDetected();
    void testDeepIndexInconsistency();
    void testBuildRepairPlanMapping();
    void testExecuteRebuildIndex();
    void testExecuteResetSyncBaseline();
    void testExecuteMarkNewBranch();
    void testExecuteRepairReestablishesBaseline();
    void testSaveEstablishesPersistentBaseline();
    void testRiskyDirectoryDetected();
    void testRepairFailureDoesNotCorrupt();

private:
    QSharedPointer<Database> makeDb();
};

void TestExternalChangeDetector::initTestCase()
{
    QVERIFY(Crypto::init());
}

QSharedPointer<Database> TestExternalChangeDetector::makeDb()
{
    auto db = QSharedPointer<Database>(new Database());
    db->metadata()->setName(QStringLiteral("detector-test"));

    auto* entry = new Entry();
    entry->setUuid(QUuid::createUuid());
    entry->setTitle(QStringLiteral("E1"));
    entry->setUsername(QStringLiteral("alice"));
    db->rootGroup()->addEntry(entry);

    return db;
}

// ---------------------------------------------------------------------------
// Detection (6.9.1 / 6.9.2)
// ---------------------------------------------------------------------------

void TestExternalChangeDetector::testNoBaselineIsClean()
{
    auto db = makeDb();
    SyncMetadataEngine engine(db);
    ExternalChangeDetector detector(db, &engine);

    const auto r = detector.detectExternalChange();
    QCOMPARE(r.severity, ChangeSeverity::None);
    QVERIFY(!r.fileHashChanged);
    QVERIFY(!r.metadataMismatch);
    QVERIFY(!r.findings.isEmpty()); // "no baseline" note
    QVERIFY(detector.runPreSyncCheck()); // clean → proceed
}

void TestExternalChangeDetector::testCleanAfterRecordedBaseline()
{
    auto db = makeDb();
    SyncMetadataEngine engine(db);
    ExternalChangeDetector detector(db, &engine);

    detector.recordBaseline();
    QVERIFY(!engine.integritySummary().isEmpty());

    const auto r = detector.detectExternalChange();
    QCOMPARE(r.severity, ChangeSeverity::None);
    QVERIFY(detector.runPreSyncCheck());
}

void TestExternalChangeDetector::testContentChangeDetected()
{
    auto db = makeDb();
    SyncMetadataEngine engine(db);
    ExternalChangeDetector detector(db, &engine);

    detector.recordBaseline();
    db->rootGroup()->entries().at(0)->setNotes(QStringLiteral("externally modified"));

    const auto r = detector.detectExternalChange();
    QVERIFY(r.fileHashChanged);
    QCOMPARE(r.severity, ChangeSeverity::High);
    QVERIFY(!detector.runPreSyncCheck()); // blocked
}

void TestExternalChangeDetector::testMetadataChangeDetected()
{
    auto db = makeDb();
    SyncMetadataEngine engine(db);
    ExternalChangeDetector detector(db, &engine);

    detector.recordBaseline();
    engine.addTombstone(QUuid::createUuid()); // tombstone for a non-existent entry

    const auto r = detector.detectExternalChange();
    QVERIFY(r.metadataMismatch);
    QCOMPARE(r.severity, ChangeSeverity::Medium);
    QVERIFY(!detector.runPreSyncCheck()); // blocked
}

void TestExternalChangeDetector::testDeepIndexInconsistency()
{
    auto db = makeDb();
    SyncMetadataEngine engine(db);
    ExternalChangeDetector detector(db, &engine);

    detector.recordBaseline();
    const auto liveUuid = db->rootGroup()->entries().at(0)->uuid();
    engine.addTombstone(liveUuid); // tombstone references a LIVE entry

    const auto r = detector.detectExternalChange(true);
    QVERIFY(r.indexInconsistency);
    QVERIFY(r.metadataMismatch);
}

// ---------------------------------------------------------------------------
// Repair plan mapping
// ---------------------------------------------------------------------------

void TestExternalChangeDetector::testBuildRepairPlanMapping()
{
    auto db = makeDb();
    SyncMetadataEngine engine(db);
    ExternalChangeDetector detector(db, &engine);

    ExternalChangeReport high;
    high.severity = ChangeSeverity::High;
    QCOMPARE(detector.buildRepairPlan(high).planType, RepairPlanType::MarkNewBranch);

    ExternalChangeReport medium;
    medium.severity = ChangeSeverity::Medium;
    QCOMPARE(detector.buildRepairPlan(medium).planType, RepairPlanType::RebuildIndex);

    ExternalChangeReport low;
    low.severity = ChangeSeverity::Low;
    QCOMPARE(detector.buildRepairPlan(low).planType, RepairPlanType::ResetSyncBaseline);

    ExternalChangeReport none;
    none.severity = ChangeSeverity::None;
    QCOMPARE(detector.buildRepairPlan(none).planType, RepairPlanType::None);
    QVERIFY(!detector.buildRepairPlan(none).requireUserConfirmation);
}

// ---------------------------------------------------------------------------
// Repair execution (6.9.3)
// ---------------------------------------------------------------------------

void TestExternalChangeDetector::testExecuteRebuildIndex()
{
    auto db = makeDb();
    SyncMetadataEngine engine(db);
    engine.registerOrLoadCurrentDevice(QStringLiteral("test-device"));
    ExternalChangeDetector detector(db, &engine);
    detector.recordBaseline();

    // Inconsistent state: tombstone for a live entry + conflict for an unknown entry.
    const auto liveUuid = db->rootGroup()->entries().at(0)->uuid();
    engine.addTombstone(liveUuid);
    engine.recordConflict(QUuid::createUuid(), {QStringLiteral("url")});
    QCOMPARE(engine.tombstones().size(), 1);
    QCOMPARE(engine.conflicts().size(), 1);

    RepairPlan plan;
    plan.planType = RepairPlanType::RebuildIndex;
    QString err;
    QVERIFY(detector.executeRepairPlan(plan, &err));

    // Tombstone for the live entry purged; conflict for unknown entry resolved (kept as history).
    QCOMPARE(engine.tombstones().size(), 0);
    QCOMPARE(engine.conflicts().size(), 1);
    QVERIFY(engine.conflicts().at(0).resolved);
}

void TestExternalChangeDetector::testExecuteResetSyncBaseline()
{
    auto db = makeDb();
    SyncMetadataEngine engine(db);
    ExternalChangeDetector detector(db, &engine);
    detector.recordBaseline();

    engine.updateSyncBaseline(QStringLiteral("remote1"), QStringLiteral("cursor1"));
    QVERIFY(!engine.syncBaseline(QStringLiteral("remote1")).remoteId.isEmpty());

    RepairPlan plan;
    plan.planType = RepairPlanType::ResetSyncBaseline;
    QVERIFY(detector.executeRepairPlan(plan));

    QVERIFY(engine.syncBaseline(QStringLiteral("remote1")).remoteId.isEmpty());
}

void TestExternalChangeDetector::testExecuteMarkNewBranch()
{
    auto db = makeDb();
    SyncMetadataEngine engine(db);
    engine.registerOrLoadCurrentDevice(QStringLiteral("test-device"));
    const QString devId = engine.currentDeviceId();
    QVERIFY(!devId.isEmpty());
    ExternalChangeDetector detector(db, &engine);
    detector.recordBaseline();

    auto* entry = db->rootGroup()->entries().at(0);
    const int before = engine.getEntryVersionVector(entry).value(devId, 0);

    RepairPlan plan;
    plan.planType = RepairPlanType::MarkNewBranch;
    QVERIFY(detector.executeRepairPlan(plan));

    const int after = engine.getEntryVersionVector(entry).value(devId, 0);
    QCOMPARE(after, before + 1);
}

void TestExternalChangeDetector::testExecuteRepairReestablishesBaseline()
{
    auto db = makeDb();
    SyncMetadataEngine engine(db);
    engine.registerOrLoadCurrentDevice(QStringLiteral("test-device"));
    ExternalChangeDetector detector(db, &engine);
    detector.recordBaseline();

    // Introduce a content change → should be detected and block sync.
    db->rootGroup()->entries().at(0)->setNotes(QStringLiteral("changed"));
    QVERIFY(!detector.runPreSyncCheck());

    // Auto-repair using the suggested plan.
    const auto report = detector.detectExternalChange();
    const auto plan = detector.buildRepairPlan(report);
    QVERIFY(detector.executeRepairPlan(plan));

    // Baseline re-established to the current (changed) state → now clean.
    const auto after = detector.detectExternalChange();
    QCOMPARE(after.severity, ChangeSeverity::None);
    QVERIFY(detector.runPreSyncCheck());
}

// ---------------------------------------------------------------------------
// Step 7: the Database::writeDatabase save hook must persist a baseline that
// matches the reloaded logical content (self-consistent across a save cycle).
// ---------------------------------------------------------------------------

void TestExternalChangeDetector::testSaveEstablishesPersistentBaseline()
{
    // A database carrying sync metadata (so the writeDatabase hook activates).
    auto db = QSharedPointer<Database>::create();
    db->metadata()->setName(QStringLiteral("hook-test"));
    auto* entry = new Entry();
    entry->setUuid(QUuid::createUuid());
    entry->setTitle(QStringLiteral("E1"));
    db->rootGroup()->addEntry(entry);
    {
        SyncMetadataEngine engine(db);
        engine.registerOrLoadCurrentDevice(QStringLiteral("test-device"));
        engine.saveToDatabase(); // creates KPXC_SYNC_METADATA in customData
    }

    auto key = QSharedPointer<CompositeKey>::create();
    key->addKey(QSharedPointer<PasswordKey>::create(QStringLiteral("a")));
    QVERIFY(db->setKey(key));

    const QString path = QDir::tempPath() + QStringLiteral("/lv_phase8_test_")
                       + QUuid::createUuid().toString(QUuid::Id128) + QStringLiteral(".kdbx");
    db->setFilePath(path);

    QString err;
    QVERIFY2(db->save(Database::Atomic, {}, &err), qPrintable(err)); // hook fires → baseline on disk

    // Reload the saved file fresh from disk and detect.
    auto db2 = QSharedPointer<Database>::create();
    QVERIFY2(db2->open(path, key, &err), qPrintable(err));
    SyncMetadataEngine engine2(db2);
    ExternalChangeDetector detector2(db2, &engine2);

    QVERIFY2(!engine2.integritySummary().isEmpty(), "baseline was not persisted by the save hook");
    const auto r = detector2.detectExternalChange();
    QCOMPARE(r.severity, ChangeSeverity::None);

    QFile::remove(path);
}

// ---------------------------------------------------------------------------
// Review #6: risky-directory heuristic is reported (Low, non-blocking).
// ---------------------------------------------------------------------------

void TestExternalChangeDetector::testRiskyDirectoryDetected()
{
    auto db = makeDb();
    SyncMetadataEngine engine(db);
    ExternalChangeDetector detector(db, &engine);
    detector.recordBaseline();

    // Path matching the risky-directory heuristic (cloud-synced folder).
    db->setFilePath(QStringLiteral("C:/Users/me/OneDrive/LocalVault/test.kdbx"));

    const auto r = detector.detectExternalChange();
    QVERIFY(r.riskyDirectoryDetected);
    QCOMPARE(r.severity, ChangeSeverity::Low);
    QVERIFY(detector.runPreSyncCheck()); // Low severity does not block sync
}

// ---------------------------------------------------------------------------
// Review #6 / §6.9.3: a failed repair must not corrupt the database.
// ---------------------------------------------------------------------------

void TestExternalChangeDetector::testRepairFailureDoesNotCorrupt()
{
    auto db = makeDb();
    SyncMetadataEngine engine(db);
    engine.registerOrLoadCurrentDevice(QStringLiteral("test-device"));
    ExternalChangeDetector detector(db, &engine);
    detector.recordBaseline();

    const QString contentBefore = IntegrityDigest::contentDigest(*db, engine);

    // A detector without a metadata engine cannot execute a repair and must
    // fail cleanly rather than partially mutate the database.
    ExternalChangeDetector noEngineDetector(db, nullptr);
    RepairPlan plan;
    plan.planType = RepairPlanType::RebuildIndex;
    QString err;
    QVERIFY(!noEngineDetector.executeRepairPlan(plan, &err));
    QVERIFY(!err.isEmpty());

    // Logical content must be unchanged (no partial / corrupting mutation).
    QCOMPARE(IntegrityDigest::contentDigest(*db, engine), contentBefore);
}

QTEST_GUILESS_MAIN(TestExternalChangeDetector)
#include "TestExternalChangeDetector.moc"
