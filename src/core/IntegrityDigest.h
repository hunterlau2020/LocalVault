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

#ifndef KEEPASSXC_INTEGRITYDIGEST_H
#define KEEPASSXC_INTEGRITYDIGEST_H

#include <QString>

class Database;
class SyncMetadataEngine;

/**
 * Canonical-content digest helpers for the External Change Detector (Phase 8).
 *
 * Both functions return a lowercase hex SHA-256 string computed over canonical
 * JSON of logical content. The integrity_summary baseline is EXCLUDED from the
 * input, so persisting the resulting digest is self-consistent (no self-hash
 * oscillation — see the Phase 8 design document, decision A).
 *
 *   - metadataRootDigest: device_registry + sync_baselines + tombstones + conflicts
 *   - contentDigest:        entries (fields + custom attrs/data + version vectors)
 *
 * Thread-safety: not guaranteed. Use from a single thread only.
 */
namespace IntegrityDigest
{
    QString metadataRootDigest(const SyncMetadataEngine& engine);
    QString contentDigest(const Database& db, const SyncMetadataEngine& engine);
}

#endif // KEEPASSXC_INTEGRITYDIGEST_H
