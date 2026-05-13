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

#include "EntryDiff.h"

#include <QSet>

// -----------------------------------------------------------------------
// Helper: compare two optional strings and append a FieldDiff if different
// -----------------------------------------------------------------------
static void checkField(QList<FieldDiff>& fields,
                       const QString& name,
                       const QString& oldVal,
                       const QString& newVal)
{
    if (oldVal != newVal) {
        fields.append({name, oldVal, newVal});
    }
}

// -----------------------------------------------------------------------
// EntryDiff
// -----------------------------------------------------------------------
bool EntryDiff::isEmpty() const
{
    return changedFields.isEmpty();
}

QStringList EntryDiff::changedFieldNames() const
{
    QStringList names;
    names.reserve(changedFields.size());
    for (const auto& f : changedFields) {
        names.append(f.fieldName);
    }
    return names;
}

EntryDiff EntryDiff::compute(const EntrySnapshot& before, const EntrySnapshot& after)
{
    EntryDiff diff;
    diff.entryId = after.entryId; // use "after" identity

    // Standard fields
    checkField(diff.changedFields, QStringLiteral("title"), before.title, after.title);
    checkField(diff.changedFields, QStringLiteral("username"), before.username, after.username);
    checkField(diff.changedFields, QStringLiteral("password"), before.password, after.password);
    checkField(diff.changedFields, QStringLiteral("url"), before.url, after.url);
    checkField(diff.changedFields, QStringLiteral("notes"), before.notes, after.notes);

    // Custom attributes — each is a separate field "custom_fields.<key>"
    {
        QSet<QString> allKeys;
        for (const auto& k : before.customAttributes.keys()) {
            allKeys.insert(k);
        }
        for (const auto& k : after.customAttributes.keys()) {
            allKeys.insert(k);
        }

        for (const QString& key : allKeys) {
            const QString oldVal = before.customAttributes.value(key);
            const QString newVal = after.customAttributes.value(key);
            if (oldVal != newVal) {
                diff.changedFields.append(
                    {QStringLiteral("custom_fields.") + key, oldVal, newVal});
            }
        }
    }

    // Attachments — track added/removed keys (binary content not compared)
    {
        const QSet<QString> beforeKeys(before.attachmentKeys.begin(), before.attachmentKeys.end());
        const QSet<QString> afterKeys(after.attachmentKeys.begin(), after.attachmentKeys.end());

        for (const QString& key : beforeKeys) {
            if (!afterKeys.contains(key)) {
                diff.changedFields.append(
                    {QStringLiteral("attachment.") + key, QStringLiteral("[present]"), QStringLiteral("[removed]")});
            }
        }
        for (const QString& key : afterKeys) {
            if (!beforeKeys.contains(key)) {
                diff.changedFields.append(
                    {QStringLiteral("attachment.") + key, QStringLiteral("[absent]"), QStringLiteral("[added]")});
            }
        }
    }

    // Entry-level CustomData
    {
        QSet<QString> allKeys;
        for (const auto& k : before.customData.keys()) {
            allKeys.insert(k);
        }
        for (const auto& k : after.customData.keys()) {
            allKeys.insert(k);
        }

        for (const QString& key : allKeys) {
            const QString oldVal = before.customData.value(key);
            const QString newVal = after.customData.value(key);
            if (oldVal != newVal) {
                diff.changedFields.append({QStringLiteral("custom_data.") + key, oldVal, newVal});
            }
        }
    }

    // Other properties
    if (before.iconNumber != after.iconNumber) {
        diff.changedFields.append({QStringLiteral("icon"),
                                   QString::number(before.iconNumber),
                                   QString::number(after.iconNumber)});
    }

    if (before.expires != after.expires || before.expiryTime != after.expiryTime) {
        diff.changedFields.append({QStringLiteral("expiry"),
                                   before.expires ? before.expiryTime.toString(Qt::ISODate) : QStringLiteral("none"),
                                   after.expires ? after.expiryTime.toString(Qt::ISODate) : QStringLiteral("none")});
    }

    if (before.tags != after.tags) {
        diff.changedFields.append(
            {QStringLiteral("tags"), before.tags.join(QStringLiteral(", ")), after.tags.join(QStringLiteral(", "))});
    }

    if (before.autoTypeEnabled != after.autoTypeEnabled) {
        diff.changedFields.append({QStringLiteral("autoTypeEnabled"),
                                   before.autoTypeEnabled ? QStringLiteral("true") : QStringLiteral("false"),
                                   after.autoTypeEnabled ? QStringLiteral("true") : QStringLiteral("false")});
    }

    if (before.defaultAutoTypeSequence != after.defaultAutoTypeSequence) {
        diff.changedFields.append(
            {QStringLiteral("autoTypeSequence"), before.defaultAutoTypeSequence, after.defaultAutoTypeSequence});
    }

    return diff;
}
