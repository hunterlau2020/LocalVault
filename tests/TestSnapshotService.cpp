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

#include <QTest>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonDocument>
#include <QSharedPointer>
#include <QTemporaryDir>
#include <QUuid>

#include "config-keepassx-tests.h"
#include "core/Config.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/HistoryRetentionService.h"
#include "core/Metadata.h"
#include "core/SnapshotService.h"
#include "core/ConflictResolver.h"
#include "core/SyncData.h"
#include "core/SyncEngine.h"
#include "core/TimeInfo.h"
#include "crypto/Crypto.h"
#include "util/TemporaryFile.h"

Q_DECLARE_METATYPE(SnapshotRecord)

class TestSnapshotService : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void testSnapshotRecordJsonRoundTrip();
    void testCreateAndListSnapshots();
    void testDeleteSnapshot();
    void testProtectedSnapshotCannotDelete();
    void testUnmarkProtectedThenDelete();
    void testRestoreSnapshot();
    void testEnforceEntryHistoryLimit();
    void testBatchCleanupEntryHistory();
    void testNoTruncationUnderLimit();
    void testDefaultSnapshotDirectory();
    void testSyncEngineAutoSnapshot();
    void testConflictResolverAutoSnapshot();
};

// ---------------------------------------------------------------------------
// Test data file path
// ---------------------------------------------------------------------------

static const QString DB_FILE_NAME = QStringLiteral(KEEPASSX_TEST_DATA_DIR).append("/NewDatabase.kdbx");

// ---------------------------------------------------------------------------
// initTestCase
// ---------------------------------------------------------------------------

void TestSnapshotService::initTestCase()
{
    QVERIFY(Crypto::init());
}

// ---------------------------------------------------------------------------
// Helper: open a copy of the test database
// ---------------------------------------------------------------------------

/**
 * Opens a copy of the test database (NewDatabase.kdbx, password "a").
 * Returns null on failure so caller can skip tests gracefully
 * (though we always QVERIFY success in practice).
 */
static QSharedPointer<Database> openTestDatabaseCopy()
{
    auto* tempFile = new TemporaryFile();
    if (!tempFile->copyFromFile(DB_FILE_NAME)) {
        delete tempFile;
        return {};
    }

    auto db = QSharedPointer<Database>::create();
    auto key = QSharedPointer<CompositeKey>::create();
    key->addKey(QSharedPointer<PasswordKey>::create("a"));

    QString error;
    if (!db->open(tempFile->fileName(), key, &error)) {
        delete tempFile;
        return {};
    }

    // Keep TemporaryFile alive with the Database
    tempFile->setParent(db.data());
    return db;
}

// ---------------------------------------------------------------------------
// Test 1: SnapshotRecord JSON round-trip
// ---------------------------------------------------------------------------

void TestSnapshotService::testSnapshotRecordJsonRoundTrip()
{
    SnapshotRecord rec;
    rec.snapshotId = QUuid::createUuid();
    rec.createdAt = QDateTime(QDate(2026, 5, 18), QTime(10, 30, 0), QTimeZone::UTC);
    rec.reason = QStringLiteral("sync_pre");
    rec.fileSize = 42;
    rec.localPath = QStringLiteral("/tmp/snapshots/snapshot_test.kdbx");
    rec.protected_ = true;

    // To JSON — matches the WithoutBraces format used in SnapshotRecord::toJson()
    QJsonObject json = rec.toJson();
    QCOMPARE(json.value(QStringLiteral("snapshot_id")).toString(), rec.snapshotId.toString(QUuid::WithoutBraces));
    QCOMPARE(json.value(QStringLiteral("reason")).toString(), QStringLiteral("sync_pre"));
    QCOMPARE(json.value(QStringLiteral("file_size")).toInt(), 42);
    QCOMPARE(json.value(QStringLiteral("local_path")).toString(), QStringLiteral("/tmp/snapshots/snapshot_test.kdbx"));
    QCOMPARE(json.value(QStringLiteral("protected")).toBool(), true);

    // From JSON (round trip)
    SnapshotRecord restored = SnapshotRecord::fromJson(json);
    QCOMPARE(restored.snapshotId, rec.snapshotId);
    QCOMPARE(restored.reason, rec.reason);
    QCOMPARE(restored.fileSize, rec.fileSize);
    QCOMPARE(restored.localPath, rec.localPath);
    QCOMPARE(restored.protected_, rec.protected_);
    QVERIFY(restored.createdAt.isValid());
}

// ---------------------------------------------------------------------------
// Test 2: Create and list snapshots
// ---------------------------------------------------------------------------

void TestSnapshotService::testCreateAndListSnapshots()
{
    QTemporaryDir snapDir;
    QVERIFY(snapDir.isValid());

    Config::instance()->set(Config::Snapshot_Directory, snapDir.path());

    auto db = openTestDatabaseCopy();
    QVERIFY(db);
    SnapshotService service(db);

    // Initially no snapshots
    QVERIFY(service.listSnapshots().isEmpty());

    // Create a snapshot
    QVERIFY(service.createSnapshot(QStringLiteral("manual")));

    // List snapshots — should have 1
    auto snapshots = service.listSnapshots();
    QCOMPARE(snapshots.size(), 1);
    QCOMPARE(snapshots[0].reason, QStringLiteral("manual"));
    QVERIFY(!snapshots[0].snapshotId.isNull());
    QVERIFY(snapshots[0].fileSize > 0);
    QVERIFY(QFile::exists(snapshots[0].localPath));

    // Create a second snapshot
    QVERIFY(service.createSnapshot(QStringLiteral("sync_pre")));
    QCOMPARE(service.listSnapshots().size(), 2);

    // Verify the file exists on disk
    for (const auto& s : service.listSnapshots()) {
        QVERIFY2(QFile::exists(s.localPath), qPrintable(s.localPath));
    }

    Config::instance()->remove(Config::Snapshot_Directory);
}

// ---------------------------------------------------------------------------
// Test 3: Delete snapshot
// ---------------------------------------------------------------------------

void TestSnapshotService::testDeleteSnapshot()
{
    QTemporaryDir snapDir;
    QVERIFY(snapDir.isValid());

    Config::instance()->set(Config::Snapshot_Directory, snapDir.path());

    auto db = openTestDatabaseCopy();
    QVERIFY(db);
    SnapshotService service(db);

    QVERIFY(service.createSnapshot(QStringLiteral("manual")));
    auto snapshots = service.listSnapshots();
    QCOMPARE(snapshots.size(), 1);

    QUuid snapId = snapshots[0].snapshotId;
    QString snapPath = snapshots[0].localPath;
    QVERIFY(QFile::exists(snapPath));

    // Delete
    QVERIFY(service.deleteSnapshot(snapId));
    QVERIFY(!QFile::exists(snapPath));
    QVERIFY(service.listSnapshots().isEmpty());

    // Delete non-existent snapshot returns false
    QVERIFY(!service.deleteSnapshot(QUuid::createUuid()));

    Config::instance()->remove(Config::Snapshot_Directory);
}

// ---------------------------------------------------------------------------
// Test 4: Protected snapshot cannot be deleted
// ---------------------------------------------------------------------------

void TestSnapshotService::testProtectedSnapshotCannotDelete()
{
    QTemporaryDir snapDir;
    QVERIFY(snapDir.isValid());

    Config::instance()->set(Config::Snapshot_Directory, snapDir.path());

    auto db = openTestDatabaseCopy();
    QVERIFY(db);
    SnapshotService service(db);

    QVERIFY(service.createSnapshot(QStringLiteral("manual")));
    auto snapshots = service.listSnapshots();
    QCOMPARE(snapshots.size(), 1);
    QUuid snapId = snapshots[0].snapshotId;

    // Mark as protected
    QVERIFY(service.markProtected(snapId));

    // Protected snapshot cannot be deleted
    QVERIFY(!service.deleteSnapshot(snapId));

    // Snapshot still exists
    snapshots = service.listSnapshots();
    QCOMPARE(snapshots.size(), 1);

    Config::instance()->remove(Config::Snapshot_Directory);
}

// ---------------------------------------------------------------------------
// Test 5: Unmark protected then delete
// ---------------------------------------------------------------------------

void TestSnapshotService::testUnmarkProtectedThenDelete()
{
    QTemporaryDir snapDir;
    QVERIFY(snapDir.isValid());

    Config::instance()->set(Config::Snapshot_Directory, snapDir.path());

    auto db = openTestDatabaseCopy();
    QVERIFY(db);
    SnapshotService service(db);

    QVERIFY(service.createSnapshot(QStringLiteral("manual")));
    auto snapshots = service.listSnapshots();
    QUuid snapId = snapshots[0].snapshotId;

    // Mark and verify protected
    QVERIFY(service.markProtected(snapId));

    // Look up the record to verify protected flag
    auto protectedRec = service.snapshotById(snapId);
    QVERIFY(protectedRec.protected_);

    // Unmark
    QVERIFY(service.unmarkProtected(snapId));

    // Verify not protected
    auto unprotectedRec = service.snapshotById(snapId);
    QVERIFY(!unprotectedRec.protected_);

    // Now delete should succeed
    QVERIFY(service.deleteSnapshot(snapId));
    QVERIFY(service.listSnapshots().isEmpty());

    // Unmark on non-existent returns false
    QVERIFY(!service.unmarkProtected(QUuid::createUuid()));

    Config::instance()->remove(Config::Snapshot_Directory);
}

// ---------------------------------------------------------------------------
// Test 6: Restore snapshot
// ---------------------------------------------------------------------------

void TestSnapshotService::testRestoreSnapshot()
{
    QTemporaryDir snapDir;
    QVERIFY(snapDir.isValid());

    Config::instance()->set(Config::Snapshot_Directory, snapDir.path());

    auto db = openTestDatabaseCopy();
    QVERIFY(db);
    SnapshotService service(db);

    // Modify the database so we have a change
    const QString originalName = db->metadata()->name();
    db->metadata()->setName(QStringLiteral("ModifiedName"));
    QVERIFY(db->isModified());

    // Create snapshot (which saves the current state)
    QVERIFY(service.createSnapshot(QStringLiteral("manual")));
    auto snapshots = service.listSnapshots();
    QCOMPARE(snapshots.size(), 1);
    QUuid snapId = snapshots[0].snapshotId;
    QString snapPath = snapshots[0].localPath;

    // Verify snapshot file exists
    QVERIFY(QFile::exists(snapPath));

    // Modify the database metadata again (will be rolled back by restore)
    db->metadata()->setName(QStringLiteral("AnotherName"));

    // Restore the snapshot
    QVERIFY(service.restoreSnapshot(snapId));

    // After restore, a protective snapshot was created automatically
    QCOMPARE(service.listSnapshots().size(), 2);

    // Verify the database file still exists (restore copies back)
    QVERIFY(QFile::exists(db->filePath()));

    // Need to reload the database to verify the restored content
    // (restoreSnapshot copies file back but doesn't reload in-memory state)
    auto db2 = QSharedPointer<Database>::create();
    auto key = QSharedPointer<CompositeKey>::create();
    key->addKey(QSharedPointer<PasswordKey>::create("a"));
    QString error;
    QVERIFY2(db2->open(db->filePath(), key, &error), error.toLatin1());

    // The restored database should have the "After" protective snapshot entry
    // in its CustomData (because restoreSnapshot creates a protective snapshot
    // which writes to CustomData, and the file copy brings that along).
    // We check that the original database file is valid and readable.
    QCOMPARE(db2->metadata()->name(), QStringLiteral("ModifiedName"));

    // Restore non-existent snapshot returns false
    QVERIFY(!service.restoreSnapshot(QUuid::createUuid()));

    Config::instance()->remove(Config::Snapshot_Directory);
}

// ---------------------------------------------------------------------------
// Test 7: Enforce entry history limit
// ---------------------------------------------------------------------------

void TestSnapshotService::testEnforceEntryHistoryLimit()
{
    Entry entry;
    entry.setUuid(QUuid::createUuid());
    entry.setTitle("TestEntry");

    // Add 15 history items
    for (int i = 0; i < 15; ++i) {
        auto* historyEntry = new Entry();
        historyEntry->setTitle(QStringLiteral("History %1").arg(i));
        entry.addHistoryItem(historyEntry);
    }

    QCOMPARE(entry.historyItems().size(), 15);

    // Enforce limit of 10
    HistoryRetentionService::enforceEntryHistoryLimit(&entry, 10);
    QCOMPARE(entry.historyItems().size(), 10);

    // Verify the 10 most recent items remain (oldest were removed)
    const auto remaining = entry.historyItems();
    // After removing 5 oldest, the first remaining should be "History 5"
    QCOMPARE(remaining[0]->title(), QStringLiteral("History 5"));
    QCOMPARE(remaining[9]->title(), QStringLiteral("History 14"));
}

// ---------------------------------------------------------------------------
// Test 8: Batch cleanup entry history
// ---------------------------------------------------------------------------

void TestSnapshotService::testBatchCleanupEntryHistory()
{
    const int MAX_HISTORY = 3;

    // Create entries with history
    QList<Entry*> entries;

    for (int e = 0; e < 3; ++e) {
        auto* entry = new Entry();
        entry->setUuid(QUuid::createUuid());
        entry->setTitle(QStringLiteral("Entry %1").arg(e));

        for (int i = 0; i < 10; ++i) {
            auto* historyEntry = new Entry();
            historyEntry->setTitle(QStringLiteral("E%1-H%2").arg(e).arg(i));
            entry->addHistoryItem(historyEntry);
        }

        QCOMPARE(entry->historyItems().size(), 10);
        entries.append(entry);
    }

    // Batch cleanup
    HistoryRetentionPolicy policy;
    policy.entryHistoryLimit = MAX_HISTORY;
    HistoryRetentionService::cleanupEntryHistory(entries, policy);

    // Verify each entry has been truncated to MAX_HISTORY
    for (int e = 0; e < 3; ++e) {
        QCOMPARE(entries[e]->historyItems().size(), MAX_HISTORY);
        // The oldest remaining should be "E{e}-H{10-MAX_HISTORY}"
        const auto remaining = entries[e]->historyItems();
        QCOMPARE(remaining[0]->title(), QStringLiteral("E%1-H%2").arg(e).arg(10 - MAX_HISTORY));
    }

    qDeleteAll(entries);
}

// ---------------------------------------------------------------------------
// Test 9: No truncation when under limit
// ---------------------------------------------------------------------------

void TestSnapshotService::testNoTruncationUnderLimit()
{
    Entry entry;
    entry.setUuid(QUuid::createUuid());
    entry.setTitle("TestEntry");

    // Add 3 history items
    for (int i = 0; i < 3; ++i) {
        auto* historyEntry = new Entry();
        historyEntry->setTitle(QStringLiteral("History %1").arg(i));
        entry.addHistoryItem(historyEntry);
    }

    QCOMPARE(entry.historyItems().size(), 3);

    // Enforce limit of 10 — no truncation needed
    HistoryRetentionService::enforceEntryHistoryLimit(&entry, 10);
    QCOMPARE(entry.historyItems().size(), 3);

    // Enforce limit of 0 (unlimited) — no truncation
    HistoryRetentionService::enforceEntryHistoryLimit(&entry, 0);
    QCOMPARE(entry.historyItems().size(), 3);

    // Null entry — no crash
    HistoryRetentionService::enforceEntryHistoryLimit(nullptr, 10);

    // Negative limit — no crash
    HistoryRetentionService::enforceEntryHistoryLimit(&entry, -1);
    QCOMPARE(entry.historyItems().size(), 3);
}

// ---------------------------------------------------------------------------
// Test 10: Default snapshot directory path resolution
// ---------------------------------------------------------------------------

void TestSnapshotService::testDefaultSnapshotDirectory()
{
    // Database path — verify it appends LocalVault/snapshots/ to the parent dir
    QString dbPath = QStringLiteral("/home/user/Documents/passwords.kdbx");
    QString dir1 = SnapshotService::defaultSnapshotDirectory(dbPath);
    QVERIFY(dir1.endsWith(QStringLiteral("/LocalVault/snapshots/")));
    QVERIFY(dir1.contains(QStringLiteral("Documents")));

    // Database path with Windows-style separator
    QString dbPath2 = QStringLiteral("C:\\Users\\test\\passwords.kdbx");
    QString dir2 = SnapshotService::defaultSnapshotDirectory(dbPath2);
    QFileInfo fi(dbPath2);
    QString expected = QDir(fi.absolutePath()).absolutePath() + QStringLiteral("/LocalVault/snapshots/");
    QCOMPARE(QDir(dir2).absolutePath(), QDir(expected).absolutePath());

    // Empty database path — fallback to home directory
    QString dir3 = SnapshotService::defaultSnapshotDirectory(QString());
    QVERIFY(dir3.endsWith(QStringLiteral("/LocalVault/snapshots/")));
    QVERIFY(dir3.contains(QDir::homePath()));

    // Snapshot file name format
    QUuid testId = QUuid::createUuid();
    QString fileName = SnapshotService::snapshotFileName(testId);
    QCOMPARE(fileName, QStringLiteral("snapshot_%1.kdbx").arg(testId.toString(QUuid::Id128)));
}

// ---------------------------------------------------------------------------
// Test 11: SyncEngine auto-snapshot integration
// ---------------------------------------------------------------------------

void TestSnapshotService::testSyncEngineAutoSnapshot()
{
    QTemporaryDir snapDir;
    QVERIFY(snapDir.isValid());

    // Point snapshots to a temp directory
    Config::instance()->set(Config::Snapshot_Directory, snapDir.path());

    // Open a file-based database (required for snapshot creation)
    auto local = openTestDatabaseCopy();
    QVERIFY(local);

    // Create a remote database with a conflicting entry (same UUID, different values)
    auto remote = QSharedPointer<Database>(new Database());
    const QUuid sharedUuid = local->rootGroup()->entries()[0]->uuid();
    auto* remoteEntry = new Entry();
    remoteEntry->setUuid(sharedUuid);
    remoteEntry->setTitle(local->rootGroup()->entries()[0]->title());
    remoteEntry->setUsername(local->rootGroup()->entries()[0]->username());
    remoteEntry->setUrl(QStringLiteral("https://remote.com"));
    remoteEntry->setNotes(QStringLiteral("Remote notes"));
    remote->rootGroup()->addEntry(remoteEntry);

    // Run analyzeDiffs — should trigger auto-snapshot
    SyncEngine engine;
    auto result = engine.analyzeDiffs(local, remote);

    QVERIFY(result.success);

    // Verify auto-snapshot was created
    SnapshotService ss(local);
    auto snapshots = ss.listSnapshots();
    bool found = false;
    for (const auto& s : snapshots) {
        if (s.reason == QStringLiteral("sync_pre")) {
            found = true;
            QVERIFY(QFile::exists(s.localPath));
            break;
        }
    }
    QVERIFY2(found, "No sync_pre snapshot found after analyzeDiffs");

    Config::instance()->remove(Config::Snapshot_Directory);
}

// ---------------------------------------------------------------------------
// Test 12: ConflictResolver auto-snapshot integration
// ---------------------------------------------------------------------------

void TestSnapshotService::testConflictResolverAutoSnapshot()
{
    QTemporaryDir snapDir;
    QVERIFY(snapDir.isValid());

    Config::instance()->set(Config::Snapshot_Directory, snapDir.path());

    // Open a file-based database
    auto local = openTestDatabaseCopy();
    QVERIFY(local);
    auto* localEntry = local->rootGroup()->entries()[0];

    // Create a remote with conflicting entry (same UUID, different fields)
    auto remote = QSharedPointer<Database>(new Database());
    const QUuid sharedUuid = localEntry->uuid();
    auto* remoteEntry = new Entry();
    remoteEntry->setUuid(sharedUuid);
    remoteEntry->setTitle(localEntry->title());
    remoteEntry->setUsername(localEntry->username());
    remoteEntry->setUrl(QStringLiteral("https://remote.com"));
    remoteEntry->setNotes(QStringLiteral("Remote notes"));
    remote->rootGroup()->addEntry(remoteEntry);

    // Align timestamps so SyncEngine sees concurrent modifications
    // (otherwise the old file timestamp would make remote dominate)
    const QDateTime concurrentTime = QDateTime::currentDateTimeUtc();
    TimeInfo ti;
    ti.setLastModificationTime(concurrentTime);
    localEntry->setTimeInfo(ti);
    remoteEntry->setTimeInfo(ti);

    // Get conflicts from SyncEngine
    SyncEngine engine;
    auto result = engine.analyzeDiffs(local, remote);
    QVERIFY(result.success);
    QVERIFY(result.conflictCount > 0);

    // Resolve all via ConflictResolver — should trigger auto-snapshot
    ConflictResolverService resolver(local, nullptr);
    auto resResults = resolver.resolveAll(result, ConflictResolution::KeepLocal);

    QCOMPARE(resResults.size(), result.conflictCount);
    for (const auto& r : resResults) {
        QVERIFY2(r.success, qPrintable(r.errorMessage));
    }

    // Verify auto-snapshot was created
    SnapshotService ss(local);
    auto snapshots = ss.listSnapshots();
    bool foundSyncPre = false;
    bool foundConflictBatch = false;
    for (const auto& s : snapshots) {
        if (s.reason == QStringLiteral("sync_pre")) {
            foundSyncPre = true;
        }
        if (s.reason == QStringLiteral("conflict_batch_resolve")) {
            foundConflictBatch = true;
            QVERIFY(QFile::exists(s.localPath));
        }
    }
    QVERIFY2(foundConflictBatch, "No conflict_batch_resolve snapshot found after resolveAll");

    Config::instance()->remove(Config::Snapshot_Directory);
}

// ---------------------------------------------------------------------------

QTEST_GUILESS_MAIN(TestSnapshotService)
#include "TestSnapshotService.moc"
