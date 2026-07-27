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

#include "RemoteConfigService.h"

#include "core/CustomData.h"
#include "core/Database.h"
#include "core/Metadata.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>

RemoteConfigService::RemoteConfigService(QSharedPointer<Database> db)
    : m_db(std::move(db))
{
}

void RemoteConfigService::setDatabase(QSharedPointer<Database> db)
{
    m_db = std::move(db);
}

bool RemoteConfigService::hasDatabase() const
{
    return !m_db.isNull();
}

QList<RemoteStorageConfig> RemoteConfigService::load() const
{
    QList<RemoteStorageConfig> list;
    if (!m_db) {
        return list;
    }
    const QString raw = m_db->metadata()->customData()->value(CustomData::RemoteProgramSettings);
    if (raw.isEmpty()) {
        return list;
    }
    QJsonParseError err;
    const auto doc = QJsonDocument::fromJson(raw.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        return list;
    }
    for (const auto& v : doc.array()) {
        if (v.isObject()) {
            list.append(RemoteStorageConfig::fromJson(v.toObject()));
        }
    }
    return list;
}

void RemoteConfigService::save(const QList<RemoteStorageConfig>& remotes)
{
    if (!m_db) {
        return;
    }
    QJsonArray arr;
    for (const auto& r : remotes) {
        arr.append(r.toJson());
    }
    const QByteArray json = QJsonDocument(arr).toJson(QJsonDocument::Compact);
    m_db->metadata()->customData()->set(CustomData::RemoteProgramSettings, QString::fromUtf8(json));
}

RemoteStorageConfig RemoteConfigService::get(const QString& name) const
{
    for (const auto& r : load()) {
        if (r.name == name) {
            return r;
        }
    }
    return RemoteStorageConfig{};
}

bool RemoteConfigService::upsert(const RemoteStorageConfig& config)
{
    auto list = load();
    bool found = false;
    for (auto& r : list) {
        if (r.name == config.name) {
            r = config;
            found = true;
            break;
        }
    }
    if (!found) {
        list.append(config);
    }
    save(list);
    return true;
}

bool RemoteConfigService::remove(const QString& name)
{
    auto list = load();
    const int before = list.size();
    list.erase(std::remove_if(list.begin(),
                              list.end(),
                              [&name](const RemoteStorageConfig& r) { return r.name == name; }),
               list.end());
    if (list.size() == before) {
        return false;
    }
    save(list);
    return true;
}
