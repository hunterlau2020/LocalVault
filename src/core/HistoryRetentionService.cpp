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

#include "HistoryRetentionService.h"

#include "core/Entry.h"

// ---------------------------------------------------------------------------
// Entry history limit enforcement
// ---------------------------------------------------------------------------

void HistoryRetentionService::enforceEntryHistoryLimit(Entry* entry, int limit)
{
    if (!entry || limit <= 0) {
        return;
    }

    const auto history = entry->historyItems();
    if (history.size() <= limit) {
        return;
    }

    // historyItems() returns items sorted oldest-to-newest.
    // Remove the oldest items beyond the limit.
    const int removeCount = history.size() - limit;
    QList<Entry*> toRemove = history.mid(0, removeCount);
    entry->removeHistoryItems(toRemove);
}

void HistoryRetentionService::enforceEntryHistoryLimit(Entry* entry, const HistoryRetentionPolicy& policy)
{
    enforceEntryHistoryLimit(entry, policy.entryHistoryLimit);
}

void HistoryRetentionService::cleanupEntryHistory(QList<Entry*> entries, const HistoryRetentionPolicy& policy)
{
    for (auto* entry : entries) {
        enforceEntryHistoryLimit(entry, policy);
    }
}
