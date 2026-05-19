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

#ifndef KEEPASSXC_HISTORYRETENTIONSERVICE_H
#define KEEPASSXC_HISTORYRETENTIONSERVICE_H

#include <QList>

class Entry;

/**
 * Policy for automatic history and snapshot retention.
 */
struct HistoryRetentionPolicy
{
    int maxSnapshotCount = 20;   // max snapshots to keep (0 = unlimited)
    int maxSnapshotDays = 90;    // max age of snapshots in days (0 = unlimited)
    int entryHistoryLimit = 10;  // max history items per entry (0 = unlimited)
};

/**
 * HistoryRetentionService — enforces retention policies on entry history.
 *
 * Uses KeePassXC's native entry history mechanism (removeHistoryItems, etc.)
 * for entry-level cleanup.  Snapshot retention is handled by SnapshotService.
 */
class HistoryRetentionService
{
public:
    /**
     * Enforce the entry history limit on a single entry.
     * Removes the oldest history items beyond the given limit.
     */
    static void enforceEntryHistoryLimit(Entry* entry, int limit);

    /**
     * Convenience overload that reads the limit from the policy.
     */
    static void enforceEntryHistoryLimit(Entry* entry, const HistoryRetentionPolicy& policy);

    /**
     * Batch cleanup across multiple entries.
     */
    static void cleanupEntryHistory(QList<Entry*> entries, const HistoryRetentionPolicy& policy);

private:
    HistoryRetentionService() = default;
};

#endif // KEEPASSXC_HISTORYRETENTIONSERVICE_H
