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

#include "IntegrityDigest.h"

#include "core/SyncMetadata.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/Metadata.h"
#include "core/TimeInfo.h"
#include "crypto/CryptoHash.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

namespace
{
    QString sha256HexOf(const QJsonObject& obj)
    {
        const QByteArray json = QJsonDocument(obj).toJson(QJsonDocument::Compact);
        return QString::fromUtf8(CryptoHash::hash(json, CryptoHash::Sha256).toHex());
    }

    // Recursively serialize the group tree (review #3): names, nesting, entry
    // placement, and group-level custom data — all previously invisible to the
    // digest, so renaming/restructuring groups or moving entries went undetected.
    // \p depth guards against maliciously deep nesting (review3 🔴#2): a crafted
    // KDBX could otherwise cause unbounded recursion / stack overflow.
    QJsonObject serializeGroup(const Group* g, int depth = 0)
    {
        QJsonObject obj;
        if (depth > 256) {
            obj[QStringLiteral("_truncated")] = true;
            return obj;
        }
        obj[QStringLiteral("uuid")] = g->uuid().toString(QUuid::Id128);
        obj[QStringLiteral("name")] = g->name();

        QJsonArray entryUuids;
        for (const Entry* e : g->entries()) {
            entryUuids.append(e->uuid().toString(QUuid::Id128));
        }
        obj[QStringLiteral("entries")] = entryUuids;

        QJsonObject cdata;
        const auto cdKeys = g->customData()->keys();
        for (const auto& k : cdKeys) {
            cdata.insert(k, g->customData()->value(k));
        }
        obj[QStringLiteral("custom_data")] = cdata;

        QJsonArray childrenArr;
        for (const Group* c : g->children()) {
            childrenArr.append(serializeGroup(c, depth + 1));
        }
        obj[QStringLiteral("children")] = childrenArr;
        return obj;
    }
} // namespace

QString IntegrityDigest::metadataRootDigest(const SyncMetadataEngine& engine)
{
    return sha256HexOf(engine.digestMetadataJson());
}

QString IntegrityDigest::contentDigest(const Database& db, const SyncMetadataEngine& engine)
{
    QJsonObject root;
    root[QStringLiteral("kind")] = QStringLiteral("LocalVault.content.v1");

    // Database-level metadata (review #1): catch external edits to the database
    // name / recycle-bin config that entry-level hashing alone would miss.
    QJsonObject dbMeta;
    dbMeta[QStringLiteral("name")] = db.metadata()->name();
    dbMeta[QStringLiteral("recycle_bin_enabled")] = db.metadata()->recycleBinEnabled();
    root[QStringLiteral("database")] = dbMeta;

    QJsonArray entriesArr;
    if (const Group* rootGroup = db.rootGroup()) {
        const auto entries = rootGroup->entriesRecursive(false);
        for (const Entry* entry : entries) {
            QJsonObject e;
            e[QStringLiteral("uuid")] = entry->uuid().toString(QUuid::Id128);
            e[QStringLiteral("title")] = entry->title();
            e[QStringLiteral("username")] = entry->username();
            e[QStringLiteral("url")] = entry->url();
            e[QStringLiteral("notes")] = entry->notes();
            e[QStringLiteral("password")] = entry->password();

            // Custom attributes (user-defined fields)
            QJsonObject attrs;
            const auto attrKeys = entry->attributes()->customKeys();
            for (const auto& k : attrKeys) {
                attrs.insert(k, entry->attributes()->value(k));
            }
            e[QStringLiteral("custom_attributes")] = attrs;

            // Attachment CONTENT hashes (review #1): swapping an attachment's bytes
            // while keeping its filename was previously invisible to the digest.
            QJsonObject attachHashes;
            const auto attachKeys = entry->attachments()->keys();
            for (const auto& k : attachKeys) {
                attachHashes.insert(
                    k,
                    QString::fromUtf8(
                        CryptoHash::hash(entry->attachments()->value(k), CryptoHash::Sha256).toHex()));
            }
            e[QStringLiteral("attachments")] = attachHashes;

            // Custom data (holds KPXC_SYNC_VV version vectors, etc.)
            QJsonObject cdata;
            const auto cdKeys = entry->customData()->keys();
            for (const auto& k : cdKeys) {
                cdata.insert(k, entry->customData()->value(k));
            }
            e[QStringLiteral("custom_data")] = cdata;

            const TimeInfo ti = entry->timeInfo();
            e[QStringLiteral("created")] = ti.creationTime().toUTC().toString(Qt::ISODate);
            e[QStringLiteral("modified")] = ti.lastModificationTime().toUTC().toString(Qt::ISODate);
            e[QStringLiteral("tags")] = entry->tags();
            e[QStringLiteral("expires")] = ti.expires();
            if (ti.expires()) {
                e[QStringLiteral("expiry_time")] = ti.expiryTime().toUTC().toString(Qt::ISODate);
            }

            entriesArr.append(e);
        }
        // Group tree (review #3): names, nesting, entry placement, group custom data.
        root[QStringLiteral("groups")] = serializeGroup(rootGroup);
    }
    root[QStringLiteral("entries")] = entriesArr;

    return sha256HexOf(root);
}
