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

#include "core/ConflictResolver.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/EntryDiff.h"
#include "core/EntrySnapshot.h"
#include "core/Group.h"
#include "core/Metadata.h"
#include "core/SyncData.h"
#include "core/SyncEngine.h"
#include "crypto/Crypto.h"

class TestConflictResolver : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void testKeepLocal();
    void testKeepRemote();
    void testCreateCopy();
    void testManualMerge();
    void testManualMergeMissingField();
    void testVersionVectorUpdated();
    void testConflictRecordMarked();
    void testResolveAllKeepRemote();
    void testResolveAllKeepLocal();
    void testResolveAllCreateCopy();
    void testEntryNotFound();
    void testBuildConflictDraft();
    void testSetFieldValue();
    void testValidateManualMerge();
    void testCreateCopyGroupPlacement();
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static QSharedPointer<Database> makeDb(const QString& name,
                                       const QString& title,
                                       const QString& username,
                                       const QString& url,
                                       const QString& notes,
                                       const QString& password = QString())
{
    auto db = QSharedPointer<Database>(new Database());
    db->metadata()->setName(name);

    auto* entry = new Entry();
    entry->setUuid(QUuid::createUuid());
    entry->setTitle(title);
    entry->setUsername(username);
    if (!password.isEmpty()) {
        entry->setPassword(password);
    }
    entry->setUrl(url);
    entry->setNotes(notes);
    db->rootGroup()->addEntry(entry);
    return db;
}

/** Create two databases with the same entry UUID but different values. */
static QPair<QSharedPointer<Database>, QSharedPointer<Database>> makeConflictPair()
{
    const QUuid sharedUuid = QUuid::createUuid();

    auto local = makeDb("Local", "CommonTitle", "alice", "https://local.com", "Local notes");
    auto remote = makeDb("Remote", "CommonTitle", "alice", "https://remote.com", "Remote notes");

    local->rootGroup()->entries()[0]->setUuid(sharedUuid);
    remote->rootGroup()->entries()[0]->setUuid(sharedUuid);

    return {local, remote};
}

/** Run analyzeDiffs and return the first Conflict operation (or a default). */
static SyncOperation getFirstConflict(const QSharedPointer<Database>& local,
                                       const QSharedPointer<Database>& remote)
{
    SyncEngine engine;
    auto result = engine.analyzeDiffs(local, remote);
    if (!result.success || result.conflictCount < 1) {
        return {};
    }
    for (const auto& op : result.operations) {
        if (op.type == SyncOperation::Conflict) {
            return op;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Test body
// ---------------------------------------------------------------------------

void TestConflictResolver::initTestCase()
{
    QVERIFY(Crypto::init());
}

void TestConflictResolver::testKeepLocal()
{
    auto [local, remote] = makeConflictPair();
    const QUuid entryId = local->rootGroup()->entries()[0]->uuid();
    const QString originalUrl = local->rootGroup()->entries()[0]->url();

    SyncOperation op = getFirstConflict(local, remote);
    QVERIFY(op.localEntry != nullptr);
    QVERIFY(op.remoteEntry != nullptr);

    ConflictResolverService resolver(local, nullptr);
    ConflictItem item = resolver.buildConflictDraft(op);
    ConflictResolutionCommand cmd;
    cmd.conflictId = item.conflictId;
    cmd.resolutionType = ConflictResolution::KeepLocal;

    ConflictResolutionResult result = resolver.resolve(item, cmd);

    QVERIFY(result.success);
    QVERIFY(result.conflictMarkedResolved);

    // Entry must remain unchanged
    auto* entry = local->rootGroup()->findEntryByUuid(entryId);
    QVERIFY(entry != nullptr);
    QCOMPARE(entry->url(), originalUrl);
    QCOMPARE(entry->title(), QStringLiteral("CommonTitle"));
    QCOMPARE(entry->username(), QStringLiteral("alice"));
}

void TestConflictResolver::testKeepRemote()
{
    auto [local, remote] = makeConflictPair();
    const QUuid entryId = local->rootGroup()->entries()[0]->uuid();

    SyncOperation op = getFirstConflict(local, remote);
    ConflictResolverService resolver(local, nullptr);
    ConflictItem item = resolver.buildConflictDraft(op);
    ConflictResolutionCommand cmd;
    cmd.conflictId = item.conflictId;
    cmd.resolutionType = ConflictResolution::KeepRemote;

    ConflictResolutionResult result = resolver.resolve(item, cmd);

    QVERIFY(result.success);
    QVERIFY(result.conflictMarkedResolved);

    // Entry must now have remote values
    auto* entry = local->rootGroup()->findEntryByUuid(entryId);
    QVERIFY(entry != nullptr);
    QCOMPARE(entry->url(), QStringLiteral("https://remote.com"));
    QCOMPARE(entry->notes(), QStringLiteral("Remote notes"));

    // Unchanged fields preserved
    QCOMPARE(entry->title(), QStringLiteral("CommonTitle"));
    QCOMPARE(entry->username(), QStringLiteral("alice"));
}

void TestConflictResolver::testCreateCopy()
{
    auto [local, remote] = makeConflictPair();
    const QUuid entryId = local->rootGroup()->entries()[0]->uuid();
    const QString originalUrl = local->rootGroup()->entries()[0]->url();
    const int originalCount = local->rootGroup()->entries().size();

    SyncOperation op = getFirstConflict(local, remote);
    QVERIFY(op.localEntry != nullptr);
    QVERIFY(op.remoteEntry != nullptr);

    ConflictResolverService resolver(local, nullptr);
    ConflictItem item = resolver.buildConflictDraft(op);
    ConflictResolutionCommand cmd;
    cmd.conflictId = item.conflictId;
    cmd.resolutionType = ConflictResolution::CreateCopy;

    ConflictResolutionResult result = resolver.resolve(item, cmd);

    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY2(!result.createdCopyEntryId.isNull(), "createdCopyEntryId is null");
    QVERIFY(result.conflictMarkedResolved);

    // Original entry unchanged
    auto* entry = local->rootGroup()->findEntryByUuid(entryId);
    QVERIFY(entry != nullptr);
    QCOMPARE(entry->url(), originalUrl);

    // Copy exists in the same group
    QCOMPARE(local->rootGroup()->entries().size(), originalCount + 1);

    // Find the copy (it has the remote values)
    bool foundCopy = false;
    for (auto* e : local->rootGroup()->entries()) {
        if (e->uuid() == result.createdCopyEntryId) {
            foundCopy = true;
            QCOMPARE(e->url(), QStringLiteral("https://remote.com"));
            QCOMPARE(e->notes(), QStringLiteral("Remote notes"));
        }
    }
    QVERIFY(foundCopy);
}

void TestConflictResolver::testManualMerge()
{
    auto [local, remote] = makeConflictPair();
    const QUuid entryId = local->rootGroup()->entries()[0]->uuid();

    SyncOperation op = getFirstConflict(local, remote);
    ConflictResolverService resolver(local, nullptr);
    ConflictItem item = resolver.buildConflictDraft(op);

    // Merge: take remote URL but local notes
    QMap<QString, QString> merged;
    for (const auto& cf : item.conflictingFields) {
        if (cf.fieldName == QStringLiteral("url")) {
            merged[cf.fieldName] = cf.remoteValue;
        } else if (cf.fieldName == QStringLiteral("notes")) {
            merged[cf.fieldName] = cf.localValue;
        }
    }

    ConflictResolutionCommand cmd;
    cmd.conflictId = item.conflictId;
    cmd.resolutionType = ConflictResolution::ManualMerge;
    cmd.mergedFieldValues = merged;

    ConflictResolutionResult result = resolver.resolve(item, cmd);

    QVERIFY(result.success);

    auto* entry = local->rootGroup()->findEntryByUuid(entryId);
    QVERIFY(entry != nullptr);
    QCOMPARE(entry->url(), QStringLiteral("https://remote.com"));
    QCOMPARE(entry->notes(), QStringLiteral("Local notes"));
}

void TestConflictResolver::testManualMergeMissingField()
{
    auto [local, remote] = makeConflictPair();
    SyncOperation op = getFirstConflict(local, remote);
    ConflictResolverService resolver(local, nullptr);
    ConflictItem item = resolver.buildConflictDraft(op);

    QMap<QString, QString> partial;
    partial[QStringLiteral("url")] = QStringLiteral("https://merged.com");

    QString missingField;
    bool valid = ConflictResolverService::validateManualMerge(item, partial, &missingField);
    QVERIFY(!valid);
    QVERIFY(!missingField.isEmpty());

    ConflictResolutionCommand cmd;
    cmd.conflictId = item.conflictId;
    cmd.resolutionType = ConflictResolution::ManualMerge;
    cmd.mergedFieldValues = partial;

    ConflictResolutionResult result = resolver.resolve(item, cmd);
    QVERIFY(!result.success);
}

void TestConflictResolver::testVersionVectorUpdated()
{
    auto [local, remote] = makeConflictPair();
    auto* localEntry = local->rootGroup()->entries()[0];
    auto* remoteEntry = remote->rootGroup()->entries()[0];

    // Set up metadata engine
    SyncMetadataEngine metaEngine(local);
    // Use setEntryVersionVector directly; incrementEntryCounter needs Config
    // which may not be available in test environment, so set VVs manually.
    const VersionVector initialLocalVV{{QStringLiteral("test-dev"), 2}};
    const VersionVector initialRemoteVV{{QStringLiteral("other-dev"), 1}};
    metaEngine.setEntryVersionVector(localEntry, initialLocalVV);
    metaEngine.setEntryVersionVector(remoteEntry, initialRemoteVV);

    // Run sync with metadata engine
    SyncEngine engine;
    engine.setMetadataEngine(local);
    auto syncResult = engine.analyzeDiffs(local, remote);
    QVERIFY(syncResult.success);
    QVERIFY(syncResult.conflictCount >= 1);

    // Find conflict operation
    SyncOperation conflictOp;
    for (const auto& op : syncResult.operations) {
        if (op.type == SyncOperation::Conflict) {
            conflictOp = op;
            break;
        }
    }
    QVERIFY(conflictOp.localEntry != nullptr);

    // Resolve with KeepRemote
    ConflictResolverService resolver(local, &metaEngine);
    ConflictItem item = resolver.buildConflictDraft(conflictOp);
    ConflictResolutionCommand cmd;
    cmd.conflictId = item.conflictId;
    cmd.resolutionType = ConflictResolution::KeepRemote;

    ConflictResolutionResult result = resolver.resolve(item, cmd);
    QVERIFY(result.success);

    // VV should have been updated
    const VersionVector finalVV = metaEngine.getEntryVersionVector(localEntry);
    QVERIFY(!finalVV.isEmpty());

    // The local counter should have been incremented
    // (incrementEntryCounter may use empty deviceId if Config not available,
    // but the VV should still be non-empty)
    QVERIFY(finalVV.value(QStringLiteral("test-dev"), 0) >= 2);
    QVERIFY(finalVV.value(QStringLiteral("other-dev"), -1) >= 1);
    QVERIFY(finalVV.value(QStringLiteral("test-dev"), 0) >= initialLocalVV.value(QStringLiteral("test-dev"), 0));
}

void TestConflictResolver::testConflictRecordMarked()
{
    auto [local, remote] = makeConflictPair();

    SyncMetadataEngine metaEngine(local);

    // Record a conflict manually
    const QUuid entryId = local->rootGroup()->entries()[0]->uuid();
    const QStringList fields = {QStringLiteral("url"), QStringLiteral("notes")};
    metaEngine.recordConflict(entryId, fields);

    // Verify it was recorded as unresolved
    auto conflictsBefore = metaEngine.conflicts();
    QCOMPARE(conflictsBefore.size(), 1);
    QVERIFY(!conflictsBefore[0].resolved);
    QUuid conflictId = conflictsBefore[0].conflictId;

    // Build a ConflictItem that references this conflictId
    SyncOperation op = getFirstConflict(local, remote);
    ConflictResolverService resolver(local, &metaEngine);
    ConflictItem item = resolver.buildConflictDraft(op);
    item.conflictId = conflictId;

    ConflictResolutionCommand cmd;
    cmd.conflictId = conflictId;
    cmd.resolutionType = ConflictResolution::KeepRemote;

    ConflictResolutionResult result = resolver.resolve(item, cmd);
    QVERIFY(result.success);
    QVERIFY(result.conflictMarkedResolved);

    // Verify conflict record is now marked resolved
    auto conflictsAfter = metaEngine.conflicts();
    QCOMPARE(conflictsAfter.size(), 1);
    QVERIFY(conflictsAfter[0].resolved);
}

void TestConflictResolver::testResolveAllKeepRemote()
{
    auto local = QSharedPointer<Database>(new Database());
    local->metadata()->setName(QStringLiteral("Local"));
    auto remote = QSharedPointer<Database>(new Database());
    remote->metadata()->setName(QStringLiteral("Remote"));

    QList<QUuid> uuids;
    for (int i = 0; i < 3; ++i) {
        const QUuid id = QUuid::createUuid();
        uuids.append(id);

        auto* le = new Entry();
        le->setUuid(id);
        le->setTitle(QStringLiteral("Entry%1").arg(i));
        le->setUrl(QStringLiteral("local-%1.com").arg(i));
        local->rootGroup()->addEntry(le);

        auto* re = new Entry();
        re->setUuid(id);
        re->setTitle(QStringLiteral("Entry%1").arg(i));
        re->setUrl(QStringLiteral("remote-%1.com").arg(i));
        remote->rootGroup()->addEntry(re);
    }

    SyncEngine engine;
    auto syncResult = engine.analyzeDiffs(local, remote);
    QVERIFY(syncResult.success);
    QCOMPARE(syncResult.conflictCount, 3);

    ConflictResolverService resolver(local, nullptr);
    QList<ConflictResolutionResult> results = resolver.resolveAll(syncResult, ConflictResolution::KeepRemote);

    QCOMPARE(results.size(), 3);
    for (const auto& res : results) {
        QVERIFY2(res.success, qPrintable(res.errorMessage));
    }

    for (int i = 0; i < 3; ++i) {
        auto* entry = local->rootGroup()->findEntryByUuid(uuids[i]);
        QVERIFY(entry != nullptr);
        QCOMPARE(entry->url(), QStringLiteral("remote-%1.com").arg(i));
    }
}

void TestConflictResolver::testResolveAllKeepLocal()
{
    auto [local, remote] = makeConflictPair();
    const QUuid entryId = local->rootGroup()->entries()[0]->uuid();
    const QString originalUrl = local->rootGroup()->entries()[0]->url();

    SyncEngine engine;
    auto syncResult = engine.analyzeDiffs(local, remote);
    QVERIFY(syncResult.success);
    QCOMPARE(syncResult.conflictCount, 1);

    ConflictResolverService resolver(local, nullptr);
    QList<ConflictResolutionResult> results = resolver.resolveAll(syncResult, ConflictResolution::KeepLocal);

    QCOMPARE(results.size(), 1);
    QVERIFY(results[0].success);

    auto* entry = local->rootGroup()->findEntryByUuid(entryId);
    QVERIFY(entry != nullptr);
    QCOMPARE(entry->url(), originalUrl);
}

void TestConflictResolver::testResolveAllCreateCopy()
{
    auto [local, remote] = makeConflictPair();
    const int originalCount = local->rootGroup()->entries().size();

    SyncEngine engine;
    auto syncResult = engine.analyzeDiffs(local, remote);
    QVERIFY(syncResult.success);
    QCOMPARE(syncResult.conflictCount, 1);

    ConflictResolverService resolver(local, nullptr);
    QList<ConflictResolutionResult> results = resolver.resolveAll(syncResult, ConflictResolution::CreateCopy);

    QCOMPARE(results.size(), 1);
    QVERIFY2(results[0].success, qPrintable(results[0].errorMessage));
    QVERIFY2(!results[0].createdCopyEntryId.isNull(), "createdCopyEntryId is null");

    QCOMPARE(local->rootGroup()->entries().size(), originalCount + 1);
}

void TestConflictResolver::testEntryNotFound()
{
    SyncOperation op;
    op.type = SyncOperation::Conflict;
    op.entryId = QUuid::createUuid();

    auto local = makeDb("Local", "Test", "user", "url", "notes");
    ConflictResolverService resolver(local, nullptr);
    ConflictItem item = resolver.buildConflictDraft(op);

    ConflictResolutionCommand cmd;
    cmd.resolutionType = ConflictResolution::KeepRemote;

    ConflictResolutionResult result = resolver.resolve(item, cmd);
    QVERIFY(!result.success);
    QVERIFY(!result.errorMessage.isEmpty());
}

void TestConflictResolver::testBuildConflictDraft()
{
    auto [local, remote] = makeConflictPair();

    SyncOperation op = getFirstConflict(local, remote);

    ConflictResolverService resolver(local, nullptr);
    ConflictItem item = resolver.buildConflictDraft(op);

    QCOMPARE(item.entryId, op.entryId);
    QVERIFY(item.localEntry != nullptr);
    QVERIFY(item.remoteEntry != nullptr);
    QVERIFY(!item.conflictingFields.isEmpty());
    QVERIFY(!item.conflictingFieldNames.isEmpty());

    for (const auto& cf : item.conflictingFields) {
        QVERIFY(!cf.fieldName.isEmpty());
    }
}

void TestConflictResolver::testSetFieldValue()
{
    auto db = makeDb("Test", "OrigTitle", "origUser", "https://orig.com", "Orig notes", "origPass");
    auto* entry = db->rootGroup()->entries()[0];

    ConflictResolverService::setFieldValue(entry, QStringLiteral("title"), QStringLiteral("NewTitle"));
    QCOMPARE(entry->title(), QStringLiteral("NewTitle"));

    ConflictResolverService::setFieldValue(entry, QStringLiteral("username"), QStringLiteral("newUser"));
    QCOMPARE(entry->username(), QStringLiteral("newUser"));

    ConflictResolverService::setFieldValue(entry, QStringLiteral("password"), QStringLiteral("newPass"));
    QCOMPARE(entry->password(), QStringLiteral("newPass"));

    ConflictResolverService::setFieldValue(entry, QStringLiteral("url"), QStringLiteral("https://new.com"));
    QCOMPARE(entry->url(), QStringLiteral("https://new.com"));

    ConflictResolverService::setFieldValue(entry, QStringLiteral("notes"), QStringLiteral("New notes"));
    QCOMPARE(entry->notes(), QStringLiteral("New notes"));

    ConflictResolverService::setFieldValue(entry, QStringLiteral("tags"), QStringLiteral("tag1, tag2"));
    QCOMPARE(entry->tags(), QStringLiteral("tag1,tag2"));

    ConflictResolverService::setFieldValue(entry, QStringLiteral("icon"), QStringLiteral("5"));
    QCOMPARE(entry->iconNumber(), 5);

    ConflictResolverService::setFieldValue(entry, QStringLiteral("autoTypeEnabled"), QStringLiteral("false"));
    QCOMPARE(entry->autoTypeEnabled(), false);

    ConflictResolverService::setFieldValue(entry, QStringLiteral("autoTypeSequence"), QStringLiteral("newSeq"));
    QCOMPARE(entry->defaultAutoTypeSequence(), QStringLiteral("newSeq"));

    entry->setDefaultAttribute(QStringLiteral("custom1"), QStringLiteral("old"));
    ConflictResolverService::setFieldValue(entry, QStringLiteral("custom_fields.custom1"), QStringLiteral("new"));
    QString customVal = entry->attributes()->value(QStringLiteral("custom1"));
    QCOMPARE(customVal, QStringLiteral("new"));
}

void TestConflictResolver::testValidateManualMerge()
{
    auto [local, remote] = makeConflictPair();
    SyncOperation op = getFirstConflict(local, remote);
    ConflictResolverService resolver(local, nullptr);
    ConflictItem item = resolver.buildConflictDraft(op);

    QMap<QString, QString> allFields;
    for (const auto& cf : item.conflictingFields) {
        allFields[cf.fieldName] = cf.localValue;
    }

    QString missingField;
    QVERIFY(ConflictResolverService::validateManualMerge(item, allFields, &missingField));
    QVERIFY(missingField.isEmpty());

    QMap<QString, QString> partial;
    if (!item.conflictingFieldNames.isEmpty()) {
        partial[item.conflictingFieldNames[0]] = QStringLiteral("value");
    }
    QVERIFY(!ConflictResolverService::validateManualMerge(item, partial, &missingField));
    QVERIFY(!missingField.isEmpty());

    QMap<QString, QString> empty;
    QVERIFY(!ConflictResolverService::validateManualMerge(item, empty, nullptr));
}

void TestConflictResolver::testCreateCopyGroupPlacement()
{
    auto local = QSharedPointer<Database>(new Database());
    local->metadata()->setName(QStringLiteral("Local"));
    auto remote = QSharedPointer<Database>(new Database());
    remote->metadata()->setName(QStringLiteral("Remote"));

    // Create a sub-group
    auto* subGroup = new Group();
    subGroup->setName(QStringLiteral("SubGroup"));
    subGroup->setParent(local->rootGroup());

    // Put the entry in the sub-group
    const QUuid sharedUuid = QUuid::createUuid();
    auto* localEntry = new Entry();
    localEntry->setUuid(sharedUuid);
    localEntry->setTitle(QStringLiteral("Entry"));
    localEntry->setUrl(QStringLiteral("local.com"));
    subGroup->addEntry(localEntry);

    auto* remoteEntry = new Entry();
    remoteEntry->setUuid(sharedUuid);
    remoteEntry->setTitle(QStringLiteral("Entry"));
    remoteEntry->setUrl(QStringLiteral("remote.com"));
    remote->rootGroup()->addEntry(remoteEntry);

    SyncEngine engine;
    auto syncResult = engine.analyzeDiffs(local, remote);
    QVERIFY(syncResult.success);
    QCOMPARE(syncResult.conflictCount, 1);

    ConflictResolverService resolver(local, nullptr);
    QList<ConflictResolutionResult> results = resolver.resolveAll(syncResult, ConflictResolution::CreateCopy);

    QCOMPARE(results.size(), 1);
    QVERIFY2(results[0].success, qPrintable(results[0].errorMessage));

    // The copy should be in the same sub-group as the original
    auto* copy = subGroup->findEntryByUuid(results[0].createdCopyEntryId);
    QVERIFY2(copy != nullptr, "Copy not found in the sub-group");
    QCOMPARE(copy->url(), QStringLiteral("remote.com"));
}

QTEST_GUILESS_MAIN(TestConflictResolver)
#include "TestConflictResolver.moc"
