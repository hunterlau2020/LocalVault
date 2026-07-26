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

#include "core/IntegrityDigest.h"
#include "core/SyncMetadata.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/Metadata.h"
#include "crypto/Crypto.h"

#include <QSharedPointer>
#include <QUuid>
#include <QTest>

class TestIntegrityDigest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void testDeterminism();
    void testContentChangeDetected();
    void testMetadataChangeDetected();
    void testDigestExcludesIntegritySummary();

private:
    QSharedPointer<Database> makeDb();
};

void TestIntegrityDigest::initTestCase()
{
    QVERIFY(Crypto::init());
}

QSharedPointer<Database> TestIntegrityDigest::makeDb()
{
    auto db = QSharedPointer<Database>(new Database());
    db->metadata()->setName(QStringLiteral("digest-test"));

    auto* entry = new Entry();
    entry->setUuid(QUuid::createUuid());
    entry->setTitle(QStringLiteral("Entry1"));
    entry->setUsername(QStringLiteral("alice"));
    entry->setUrl(QStringLiteral("https://example.com"));
    entry->setNotes(QStringLiteral("hello"));
    db->rootGroup()->addEntry(entry);

    return db;
}

// ---------------------------------------------------------------------------
// Determinism: same input → same digest, and digests are 64-char hex.
// ---------------------------------------------------------------------------

void TestIntegrityDigest::testDeterminism()
{
    auto db = makeDb();
    SyncMetadataEngine engine(db);

    const QString m1 = IntegrityDigest::metadataRootDigest(engine);
    const QString m2 = IntegrityDigest::metadataRootDigest(engine);
    QCOMPARE(m1, m2);

    const QString c1 = IntegrityDigest::contentDigest(*db, engine);
    const QString c2 = IntegrityDigest::contentDigest(*db, engine);
    QCOMPARE(c1, c2);

    QCOMPARE(m1.length(), 64);
    QCOMPARE(c1.length(), 64);

    // The two digest kinds are distinct.
    QVERIFY(m1 != c1);
}

// ---------------------------------------------------------------------------
// Content edit is detected by contentDigest.
// ---------------------------------------------------------------------------

void TestIntegrityDigest::testContentChangeDetected()
{
    auto db = makeDb();
    SyncMetadataEngine engine(db);

    const QString before = IntegrityDigest::contentDigest(*db, engine);
    db->rootGroup()->entries().at(0)->setNotes(QStringLiteral("modified"));
    const QString after = IntegrityDigest::contentDigest(*db, engine);

    QVERIFY(before != after);
}

// ---------------------------------------------------------------------------
// Metadata change (tombstone) is detected by metadataRootDigest.
// ---------------------------------------------------------------------------

void TestIntegrityDigest::testMetadataChangeDetected()
{
    auto db = makeDb();
    SyncMetadataEngine engine(db);

    const QString before = IntegrityDigest::metadataRootDigest(engine);
    engine.addTombstone(db->rootGroup()->entries().at(0)->uuid());
    const QString after = IntegrityDigest::metadataRootDigest(engine);

    QVERIFY(before != after);
}

// ---------------------------------------------------------------------------
// Decision A: integrity_summary is excluded from the digest input, so changing
// the baseline must NOT change metadataRootDigest (self-consistency).
// ---------------------------------------------------------------------------

void TestIntegrityDigest::testDigestExcludesIntegritySummary()
{
    auto db = makeDb();
    SyncMetadataEngine engine(db);

    const QString digestBefore = IntegrityDigest::metadataRootDigest(engine);

    IntegritySummary s;
    s.fileSha256 = QStringLiteral("deadbeef");
    s.metadataRootDigest = QStringLiteral("cafef00d");
    s.fileSize = 9999;
    engine.setIntegritySummary(s);

    QCOMPARE(IntegrityDigest::metadataRootDigest(engine), digestBefore);
}

QTEST_GUILESS_MAIN(TestIntegrityDigest)
#include "TestIntegrityDigest.moc"
