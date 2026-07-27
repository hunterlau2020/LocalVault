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

#ifndef KEEPASSXC_REMOTECONFIGSERVICE_H
#define KEEPASSXC_REMOTECONFIGSERVICE_H

#include "core/RemoteStorageAdapter.h"

#include <QList>
#include <QSharedPointer>

class Database;

/**
 * Stores the list of named remote-storage configs (Phase 10) in the
 * KPXC_REMOTE_SYNC_SETTINGS CustomData key, which is Protected (encrypted at rest).
 *
 * The JSON schema (top-level array of RemoteStorageConfig) is aligned with
 * gui/remote/RemoteSettings so the GUI and CLI share the same blob (design
 * review 🔴#1).
 *
 * Thread-safety: not guaranteed. Use from a single thread only.
 */
class RemoteConfigService
{
public:
    explicit RemoteConfigService() = default;
    explicit RemoteConfigService(QSharedPointer<Database> db);

    void setDatabase(QSharedPointer<Database> db);
    bool hasDatabase() const;

    // Load all named remotes from CustomData.
    QList<RemoteStorageConfig> load() const;
    // Persist the full list (overwrites).
    void save(const QList<RemoteStorageConfig>& remotes);

    // Look up by name; returns a config with empty `name` if not found.
    RemoteStorageConfig get(const QString& name) const;
    // Add or replace by name. Returns true if the list changed.
    bool upsert(const RemoteStorageConfig& config);
    // Remove by name. Returns true if a config was removed.
    bool remove(const QString& name);

private:
    QSharedPointer<Database> m_db;
};

#endif // KEEPASSXC_REMOTECONFIGSERVICE_H
