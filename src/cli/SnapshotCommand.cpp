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

#include "SnapshotCommand.h"

#include "Utils.h"
#include "core/Database.h"
#include "core/SnapshotService.h"

#include <QCommandLineParser>
#include <QFileInfo>

#include <QtEndian>

const QCommandLineOption SnapshotCommand::CreateOption(QStringList() << QStringLiteral("create"),
                                                        QObject::tr("Create a new database snapshot."));

const QCommandLineOption SnapshotCommand::ListOption(QStringList() << QStringLiteral("list"),
                                                      QObject::tr("List all database snapshots."));

const QCommandLineOption SnapshotCommand::DeleteOption(
    QStringList() << QStringLiteral("delete"),
    QObject::tr("Delete a snapshot by its ID."),
    QStringLiteral("snapshot-id"));

const QCommandLineOption SnapshotCommand::RestoreOption(
    QStringList() << QStringLiteral("restore"),
    QObject::tr("Restore a snapshot by its ID."),
    QStringLiteral("snapshot-id"));

const QCommandLineOption SnapshotCommand::ProtectOption(
    QStringList() << QStringLiteral("protect"),
    QObject::tr("Mark a snapshot as protected (cannot be deleted)."),
    QStringLiteral("snapshot-id"));

const QCommandLineOption SnapshotCommand::UnprotectOption(
    QStringList() << QStringLiteral("unprotect"),
    QObject::tr("Remove protected status from a snapshot."),
    QStringLiteral("snapshot-id"));

SnapshotCommand::SnapshotCommand()
{
    name = QStringLiteral("snapshot");
    description = QObject::tr("Manage database snapshots (create, list, delete, restore, protect).");

    positionalArguments.append(
        {QStringLiteral("database"), QObject::tr("Path of the database."), QStringLiteral("")});

    options.append(CreateOption);
    options.append(ListOption);
    options.append(DeleteOption);
    options.append(RestoreOption);
    options.append(ProtectOption);
    options.append(UnprotectOption);
    options.append(Command::KeyFileOption);
    options.append(Command::NoPasswordOption);
    options.append(Command::YubiKeyOption);
}

int SnapshotCommand::execute(const QStringList& arguments)
{
    auto& out = Utils::STDOUT;
    auto& err = Utils::STDERR;

    QSharedPointer<QCommandLineParser> parser = getCommandLineParser(arguments);
    if (parser.isNull()) {
        return EXIT_FAILURE;
    }

    const QStringList args = parser->positionalArguments();
    const QString dbPath = args.at(0);

    if (!QFileInfo::exists(dbPath)) {
        err << QObject::tr("Error: database not found: %1").arg(dbPath) << Qt::endl;
        return EXIT_FAILURE;
    }

    // Count how many snapshot actions were requested
    int actionCount = 0;
    actionCount += parser->isSet(CreateOption) ? 1 : 0;
    actionCount += parser->isSet(ListOption) ? 1 : 0;
    actionCount += parser->isSet(DeleteOption) ? 1 : 0;
    actionCount += parser->isSet(RestoreOption) ? 1 : 0;
    actionCount += parser->isSet(ProtectOption) ? 1 : 0;
    actionCount += parser->isSet(UnprotectOption) ? 1 : 0;

    if (actionCount == 0) {
        err << QObject::tr("Error: no snapshot action specified (--create, --list, --delete, --restore, --protect, --unprotect).")
            << Qt::endl;
        return EXIT_FAILURE;
    }
    if (actionCount > 1) {
        err << QObject::tr("Error: only one snapshot action can be specified at a time.") << Qt::endl;
        return EXIT_FAILURE;
    }

    // Open database
    auto db = Utils::unlockDatabase(dbPath,
                                    !parser->isSet(Command::NoPasswordOption),
                                    parser->value(Command::KeyFileOption),
                                    parser->value(Command::YubiKeyOption),
                                    parser->isSet(Command::QuietOption));
    if (!db) {
        return EXIT_FAILURE;
    }

    SnapshotService service(db);

    // --- Create -----------------------------------------------------------
    if (parser->isSet(CreateOption)) {
        const QString reason = QStringLiteral("cli_manual");
        if (!service.createSnapshot(reason)) {
            err << QObject::tr("Error: failed to create snapshot.") << Qt::endl;
            return EXIT_FAILURE;
        }

        // Show the newly created snapshot
        auto snapshots = service.listSnapshots();
        if (!snapshots.isEmpty()) {
            const auto& s = snapshots.last();
            out << QObject::tr("Snapshot created:") << Qt::endl;
            out << QObject::tr("  ID:   %1").arg(s.snapshotId.toString(QUuid::WithoutBraces)) << Qt::endl;
            out << QObject::tr("  Time: %1").arg(s.createdAt.toLocalTime().toString(Qt::ISODate)) << Qt::endl;
            out << QObject::tr("  Size: %1 bytes").arg(s.fileSize) << Qt::endl;
            out << QObject::tr("  Path: %1").arg(s.localPath) << Qt::endl;
        }

        QString error;
        if (!db->save(Database::Atomic, {}, &error)) {
            err << QObject::tr("Error: failed to save database: %1").arg(error) << Qt::endl;
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    }

    // --- List -------------------------------------------------------------
    if (parser->isSet(ListOption)) {
        auto snapshots = service.listSnapshots();
        if (snapshots.isEmpty()) {
            out << QObject::tr("No snapshots found.") << Qt::endl;
            return EXIT_SUCCESS;
        }

        out << QObject::tr("Snapshots (%1):").arg(snapshots.size()) << Qt::endl;
        for (const auto& s : snapshots) {
            const QString flags = s.protected_ ? QStringLiteral(" [PROTECTED]") : QString();
            out << QObject::tr("  %1  %2  %3 bytes  %4%5")
                       .arg(s.createdAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                            -20)
                       .arg(s.snapshotId.toString(QUuid::WithoutBraces).left(12))
                       .arg(s.fileSize)
                       .arg(s.reason)
                       .arg(flags)
                << Qt::endl;
        }

        return EXIT_SUCCESS;
    }

    // --- Delete -----------------------------------------------------------
    if (parser->isSet(DeleteOption)) {
        const QString idStr = parser->value(DeleteOption);
        const QUuid snapId = QUuid::fromString(idStr);
        if (snapId.isNull()) {
            err << QObject::tr("Error: invalid snapshot ID: %1").arg(idStr) << Qt::endl;
            return EXIT_FAILURE;
        }

        if (!service.deleteSnapshot(snapId)) {
            err << QObject::tr("Error: failed to delete snapshot (not found or protected).") << Qt::endl;
            return EXIT_FAILURE;
        }

        out << QObject::tr("Snapshot deleted: %1").arg(idStr) << Qt::endl;

        QString error;
        if (!db->save(Database::Atomic, {}, &error)) {
            err << QObject::tr("Error: failed to save database: %1").arg(error) << Qt::endl;
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    }

    // --- Restore ----------------------------------------------------------
    if (parser->isSet(RestoreOption)) {
        const QString idStr = parser->value(RestoreOption);
        const QUuid snapId = QUuid::fromString(idStr);
        if (snapId.isNull()) {
            err << QObject::tr("Error: invalid snapshot ID: %1").arg(idStr) << Qt::endl;
            return EXIT_FAILURE;
        }

        if (!service.restoreSnapshot(snapId)) {
            err << QObject::tr("Error: failed to restore snapshot (not found).") << Qt::endl;
            return EXIT_FAILURE;
        }

        out << QObject::tr("Snapshot restored: %1 (a protective snapshot was created automatically).").arg(idStr)
            << Qt::endl;
        return EXIT_SUCCESS;
    }

    // --- Protect -----------------------------------------------------------
    if (parser->isSet(ProtectOption)) {
        const QString idStr = parser->value(ProtectOption);
        const QUuid snapId = QUuid::fromString(idStr);
        if (snapId.isNull()) {
            err << QObject::tr("Error: invalid snapshot ID: %1").arg(idStr) << Qt::endl;
            return EXIT_FAILURE;
        }

        if (!service.markProtected(snapId)) {
            err << QObject::tr("Error: failed to protect snapshot (not found).") << Qt::endl;
            return EXIT_FAILURE;
        }

        out << QObject::tr("Snapshot marked as protected: %1").arg(idStr) << Qt::endl;

        QString error;
        if (!db->save(Database::Atomic, {}, &error)) {
            err << QObject::tr("Error: failed to save database: %1").arg(error) << Qt::endl;
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    }

    // --- Unprotect ---------------------------------------------------------
    if (parser->isSet(UnprotectOption)) {
        const QString idStr = parser->value(UnprotectOption);
        const QUuid snapId = QUuid::fromString(idStr);
        if (snapId.isNull()) {
            err << QObject::tr("Error: invalid snapshot ID: %1").arg(idStr) << Qt::endl;
            return EXIT_FAILURE;
        }

        if (!service.unmarkProtected(snapId)) {
            err << QObject::tr("Error: failed to unprotect snapshot (not found).") << Qt::endl;
            return EXIT_FAILURE;
        }

        out << QObject::tr("Snapshot protection removed: %1").arg(idStr) << Qt::endl;

        QString error;
        if (!db->save(Database::Atomic, {}, &error)) {
            err << QObject::tr("Error: failed to save database: %1").arg(error) << Qt::endl;
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    }

    return EXIT_SUCCESS;
}
