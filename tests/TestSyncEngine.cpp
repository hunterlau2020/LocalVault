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
#include <QSharedPointer>

#include "core/Database.h"
#include "core/Entry.h"
#include "core/ExternalChangeDetector.h"
#include "core/Group.h"
#include "core/Metadata.h"
#include "core/TimeInfo.h"
#include "core/SyncData.h"
#include "core/SyncEngine.h"
#include "crypto/Crypto.h"

class TestSyncEngine : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void testBothIdentical();
    void testRemoteOnlyEntry();
    void testDifferentFields();
    void testApplyMerge();
    void testSyncBlockedByExternalChange();
};

// Minimal helper: create an in-memory database with one entry
static QSharedPointer<Database> makeDb(const QString& name, const QString& entryTitle,
                                       const QString& username, const QString& url, const QString& notes)
{
    auto db = QSharedPointer<Database>(new Database());
    db->metadata()->setName(name);

    auto* entry = new Entry();
    entry->setUuid(QUuid::createUuid());
    entry->setTitle(entryTitle);
    entry->setUsername(username);
    entry->setUrl(url);
    entry->setNotes(notes);
    db->rootGroup()->addEntry(entry);

    return db;
}

void TestSyncEngine::initTestCase()
{
    QVERIFY(Crypto::init());
}

void TestSyncEngine::testBothIdentical()
{
    auto local = makeDb("Local", "Entry1", "alice", "https://example.com", "Hello");
    auto remote = makeDb("Remote", "Entry1", "alice", "https://example.com", "Hello");

    // Need same UUID for both entries
    QUuid sharedUuid = QUuid::createUuid();
    local->rootGroup()->entries()[0]->setUuid(sharedUuid);
    remote->rootGroup()->entries()[0]->setUuid(sharedUuid);

    SyncEngine engine;
    auto result = engine.analyzeDiffs(local, remote);

    QVERIFY(result.success);
    QCOMPARE(result.skippedCount, 1);
    QCOMPARE(result.addedCount, 0);
    QCOMPARE(result.conflictCount, 0);
}

void TestSyncEngine::testRemoteOnlyEntry()
{
    auto local = makeDb("Local", "LocalEntry", "alice", "", "");
    auto remote = makeDb("Remote", "RemoteEntry", "bob", "https://remote.com", "New");

    QUuid localUuid = QUuid::createUuid();
    QUuid remoteUuid = QUuid::createUuid();
    local->rootGroup()->entries()[0]->setUuid(localUuid);
    remote->rootGroup()->entries()[0]->setUuid(remoteUuid);

    SyncEngine engine;
    auto result = engine.analyzeDiffs(local, remote);

    QVERIFY(result.success);
    QCOMPARE(result.addedCount, 1); // Remote Only → added to local
    // The reworked engine classifies the local-only entry (LocalEntry) as Skipped
    // (no action needed when merging remote→local), so it is counted here.
    QCOMPARE(result.skippedCount, 1);
    QCOMPARE(result.conflictCount, 0);
}

void TestSyncEngine::testDifferentFields()
{
    auto local = makeDb("Local", "Common", "alice", "https://local.com", "Notes local");
    auto remote = makeDb("Remote", "Common", "alice", "https://remote.com", "Notes remote");

    QUuid sharedUuid = QUuid::createUuid();
    local->rootGroup()->entries()[0]->setUuid(sharedUuid);
    remote->rootGroup()->entries()[0]->setUuid(sharedUuid);

    SyncEngine engine;
    auto result = engine.analyzeDiffs(local, remote);

    QVERIFY(result.success);
    // With identical timestamps this should be a conflict
    QCOMPARE(result.conflictCount, 1);
}

void TestSyncEngine::testApplyMerge()
{
    // Simulate: remote modified 3 seconds after local (DirectApply: remote wins)
    auto local = makeDb("Local", "Target", "alice", "https://local.com", "Original notes");
    auto remote = makeDb("Remote", "Target", "alice", "https://remote.com", "Modified notes");

    QUuid sharedUuid = QUuid::createUuid();
    local->rootGroup()->entries()[0]->setUuid(sharedUuid);
    remote->rootGroup()->entries()[0]->setUuid(sharedUuid);

    // Manipulate timestamps: remote is 3s newer than local
    TimeInfo localTime = local->rootGroup()->entries()[0]->timeInfo();
    TimeInfo remoteTime = remote->rootGroup()->entries()[0]->timeInfo();
    localTime.setLastModificationTime(QDateTime::currentDateTimeUtc().addSecs(-3));
    remoteTime.setLastModificationTime(QDateTime::currentDateTimeUtc());
    local->rootGroup()->entries()[0]->setTimeInfo(localTime);
    remote->rootGroup()->entries()[0]->setTimeInfo(remoteTime);

    SyncEngine engine;
    auto result = engine.analyzeDiffs(local, remote);

    QVERIFY(result.success);
    QCOMPARE(result.updatedCount, 1);
    QCOMPARE(result.conflictCount, 0);

    QVERIFY(result.operations.size() >= 1);
    QCOMPARE(result.operations[0].type, SyncOperation::DirectApply);
    QVERIFY(result.operations[0].sourceEntry != nullptr);

    // Verify source entry values
    auto* src = result.operations[0].sourceEntry.data();
    QCOMPARE(src->url(), QStringLiteral("https://remote.com"));
    QCOMPARE(src->notes(), QStringLiteral("Modified notes"));

    // Apply merges
    bool ok = engine.applyMerges(result, local, remote);
    QVERIFY(ok);

    // Verify local entry was updated with remote's values
    auto* entry = local->rootGroup()->findEntryByUuid(sharedUuid);
    QVERIFY(entry != nullptr);

    QCOMPARE(entry->url(), QStringLiteral("https://remote.com"));
    QCOMPARE(entry->notes(), QStringLiteral("Modified notes"));
    QCOMPARE(entry->username(), QStringLiteral("alice")); // unchanged
}

// ---------------------------------------------------------------------------
// Phase 8: sync must be blocked when the local DB was modified externally.
// ---------------------------------------------------------------------------

void TestSyncEngine::testSyncBlockedByExternalChange()
{
    auto local = makeDb("Local", "Entry1", "alice", "https://example.com", "Hello");
    auto remote = makeDb("Remote", "Entry1", "alice", "https://example.com", "Hello");

    const QUuid sharedUuid = QUuid::createUuid();
    local->rootGroup()->entries()[0]->setUuid(sharedUuid);
    remote->rootGroup()->entries()[0]->setUuid(sharedUuid);

    SyncEngine engine;
    engine.setMetadataEngine(local); // bind the metadata engine (as SyncCommand does)

    // Record the integrity baseline on the current (clean) local state.
    {
        ExternalChangeDetector detector(local, engine.metadataEngine());
        detector.recordBaseline();
    }

    // Simulate an external modification of local content after the baseline was recorded.
    local->rootGroup()->entries()[0]->setNotes(QStringLiteral("externally tampered"));

    const auto result = engine.analyzeDiffs(local, remote);

    // Sync must be blocked: no success, an error message, and no classification performed.
    QVERIFY(!result.success);
    QVERIFY(!result.errorMessage.isEmpty());
    QVERIFY(result.operations.isEmpty());
}

QTEST_GUILESS_MAIN(TestSyncEngine)
#include "TestSyncEngine.moc"
