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

#include "TestEntrySnapshot.h"

#include "core/Entry.h"
#include "core/EntryDiff.h"
#include "core/EntrySnapshot.h"
#include "crypto/Crypto.h"

QTEST_GUILESS_MAIN(TestEntrySnapshot)

void TestEntrySnapshot::initTestCase()
{
    QVERIFY(Crypto::init());
}

void TestEntrySnapshot::testCapture()
{
    Entry entry;
    entry.setUuid(QUuid::createUuid());
    entry.setTitle("Test Title");
    entry.setUsername("testuser");
    entry.setPassword("secret123");
    entry.setUrl("https://example.com");
    entry.setNotes("Some notes");

    auto snap = EntrySnapshot::capture(&entry);

    QCOMPARE(snap.entryId, entry.uuid());
    QCOMPARE(snap.title, QStringLiteral("Test Title"));
    QCOMPARE(snap.username, QStringLiteral("testuser"));
    QCOMPARE(snap.password, QStringLiteral("secret123"));
    QCOMPARE(snap.url, QStringLiteral("https://example.com"));
    QCOMPARE(snap.notes, QStringLiteral("Some notes"));
    QVERIFY(snap.customAttributes.isEmpty());
    QVERIFY(snap.attachmentKeys.isEmpty());
}

void TestEntrySnapshot::testDiff()
{
    Entry entry;
    entry.setUuid(QUuid::createUuid());
    entry.setTitle("Original");
    entry.setUsername("user1");
    entry.setPassword("pass1");

    auto before = EntrySnapshot::capture(&entry);

    // Modify
    entry.setTitle("Modified");
    entry.setUsername("user2");
    entry.setUrl("https://example.com");

    auto after = EntrySnapshot::capture(&entry);

    auto diff = EntryDiff::compute(before, after);

    QVERIFY(!diff.isEmpty());
    QCOMPARE(diff.entryId, entry.uuid());

    auto names = diff.changedFieldNames();
    QVERIFY(names.contains("title"));
    QVERIFY(names.contains("username"));
    QVERIFY(names.contains("url"));
    QVERIFY(!names.contains("password")); // unchanged
    QVERIFY(!names.contains("notes")); // unchanged

    // Check old/new values
    for (const auto& f : diff.changedFields) {
        if (f.fieldName == "title") {
            QCOMPARE(f.oldValue, QStringLiteral("Original"));
            QCOMPARE(f.newValue, QStringLiteral("Modified"));
        }
    }
}

void TestEntrySnapshot::testNoDiff()
{
    Entry entry;
    entry.setUuid(QUuid::createUuid());
    entry.setTitle("Same");
    entry.setUsername("same");

    auto before = EntrySnapshot::capture(&entry);
    auto after = EntrySnapshot::capture(&entry);

    auto diff = EntryDiff::compute(before, after);
    QVERIFY(diff.isEmpty());
}

void TestEntrySnapshot::testCustomAttributes()
{
    Entry entry;
    entry.setUuid(QUuid::createUuid());
    entry.setTitle("Test");

    // Set custom attributes
    entry.attributes()->set("my-field", "value1");
    entry.attributes()->set("other-field", "other1");

    auto before = EntrySnapshot::capture(&entry);

    // Modify custom attribute
    entry.attributes()->set("my-field", "value2");

    auto after = EntrySnapshot::capture(&entry);

    auto diff = EntryDiff::compute(before, after);

    QVERIFY(!diff.isEmpty());
    QVERIFY(diff.changedFieldNames().contains("custom_fields.my-field"));
    QVERIFY(!diff.changedFieldNames().contains("custom_fields.other-field"));

    for (const auto& f : diff.changedFields) {
        if (f.fieldName == "custom_fields.my-field") {
            QCOMPARE(f.oldValue, QStringLiteral("value1"));
            QCOMPARE(f.newValue, QStringLiteral("value2"));
        }
    }
}

void TestEntrySnapshot::testAttachments()
{
    Entry entry;
    entry.setUuid(QUuid::createUuid());
    entry.setTitle("Test");

    auto before = EntrySnapshot::capture(&entry);
    QVERIFY(before.attachmentKeys.isEmpty());

    // Add attachment
    entry.attachments()->set("file.txt", QByteArray("hello"));
    auto after = EntrySnapshot::capture(&entry);

    QCOMPARE(after.attachmentKeys.size(), 1);
    QCOMPARE(after.attachmentKeys.first(), QStringLiteral("file.txt"));

    auto diff = EntryDiff::compute(before, after);
    QVERIFY(!diff.isEmpty());
    QVERIFY(diff.changedFieldNames().contains("attachment.file.txt"));
}
