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

#ifndef KEEPASSXC_ENTRYSNAPSHOT_H
#define KEEPASSXC_ENTRYSNAPSHOT_H

#include <QDateTime>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QUuid>

class Entry;

/**
 * Lightweight value-object snapshot of an Entry's fields at a point in time.
 *
 * Unlike Entry's beginUpdate()/endUpdate() mechanism (which clones full Entry
 * objects), EntrySnapshot captures only the field values relevant for diff
 * computation and sync metadata.  It is designed to be cheap to copy and store.
 */
struct EntrySnapshot
{
    // Identity
    QUuid entryId;

    // Standard string fields (from EntryAttributes)
    QString title;
    QString username;
    QString password;
    QString url;
    QString notes;

    // Custom (non-standard) attributes — key → value
    QMap<QString, QString> customAttributes;

    // Attachment key names (binary data is NOT captured in the snapshot)
    QStringList attachmentKeys;

    // Entry-level CustomData (filtered to exclude auto-generated keys)
    QMap<QString, QString> customData;

    // Other properties
    int iconNumber = 0;
    bool expires = false;
    QDateTime expiryTime;
    QStringList tags;
    bool autoTypeEnabled = true;
    QString defaultAutoTypeSequence;

    // Timestamps (display-only, not used for conflict resolution)
    QDateTime creationTime;
    QDateTime lastModificationTime;

    /** Build a snapshot from the current state of \p entry. */
    static EntrySnapshot capture(Entry* entry);
};

#endif // KEEPASSXC_ENTRYSNAPSHOT_H
