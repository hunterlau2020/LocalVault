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

#include "ShowMetadata.h"

#include "Utils.h"
#include "core/Metadata.h"

#include <QCommandLineParser>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <QDateTime>

constexpr const char* SYNC_METADATA_KEY = "KPXC_SYNC_METADATA";

ShowMetadata::ShowMetadata()
{
    name = QString("show-metadata");
    description = QObject::tr("Show or write database sync metadata in CustomData.");

    options.append(QCommandLineOption(
        QStringList() << QStringLiteral("w") << QStringLiteral("write"),
        QObject::tr("Write test sync metadata to the database (schema_version, device_registry, snapshot_index).")));
}

int ShowMetadata::executeWithDatabase(QSharedPointer<Database> database, QSharedPointer<QCommandLineParser> parser)
{
    auto& out = Utils::STDOUT;
    auto& err = Utils::STDERR;
    auto customData = database->metadata()->customData();

    if (parser->isSet("write")) {
        // Build test InternalMetadataRoot JSON
        QJsonObject deviceObj;
        deviceObj["device_id"] = QString("dev-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
        deviceObj["device_name"] = QString("test-device");
        deviceObj["registered_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

        QJsonArray deviceRegistry;
        deviceRegistry.append(deviceObj);

        QJsonObject metadata;
        metadata["schema_version"] = 1;
        metadata["device_registry"] = deviceRegistry;
        metadata["snapshot_index"] = QJsonArray();

        QByteArray jsonData = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
        customData->set(SYNC_METADATA_KEY, QString::fromUtf8(jsonData));

        QString errorMessage;
        if (!database->save(Database::Atomic, {}, &errorMessage)) {
            err << QObject::tr("Failed to save database: %1").arg(errorMessage) << Qt::endl;
            return EXIT_FAILURE;
        }

        out << QObject::tr("Sync metadata written to CustomData.") << Qt::endl;
    }

    // Display all CustomData keys
    auto keys = customData->keys();
    if (keys.isEmpty()) {
        out << QObject::tr("CustomData is empty.") << Qt::endl;
        return EXIT_SUCCESS;
    }

    out << QObject::tr("CustomData entries (%1):").arg(keys.size()) << Qt::endl;
    for (const auto& key : keys) {
        QString value = customData->value(key);
        bool isProtected = customData->isProtected(key);

        if (key == SYNC_METADATA_KEY) {
            // Pretty-print JSON
            QJsonDocument doc = QJsonDocument::fromJson(value.toUtf8());
            if (!doc.isNull()) {
                out << "  " << key << " = " << QString::fromUtf8(doc.toJson(QJsonDocument::Indented)) << Qt::endl;
                continue;
            }
        }

        if (isProtected) {
            out << "  " << key << " = " << QObject::tr("[PROTECTED]") << Qt::endl;
        } else {
            out << "  " << key << " = " << value << Qt::endl;
        }
    }

    return EXIT_SUCCESS;
}
