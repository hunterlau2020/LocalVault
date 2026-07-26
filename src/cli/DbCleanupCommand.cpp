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

#include "DbCleanupCommand.h"

#include "Utils.h"
#include "core/Database.h"
#include "core/ExternalChangeDetector.h"
#include "core/LifecycleManager.h"
#include "core/SyncMetadata.h"

#include <QCommandLineParser>
#include <QFileInfo>

const QCommandLineOption DbCleanupCommand::MaxSnapshotsOption(
    QStringList() << QStringLiteral("max-snapshots"),
    QObject::tr("Maximum number of snapshots to keep (default: 20, 0 = unlimited)."),
    QStringLiteral("count"));

const QCommandLineOption DbCleanupCommand::MaxDaysOption(
    QStringList() << QStringLiteral("max-days"),
    QObject::tr("Maximum age of snapshots in days (default: 90, 0 = unlimited)."),
    QStringLiteral("days"));

const QCommandLineOption DbCleanupCommand::EntryHistoryLimitOption(
    QStringList() << QStringLiteral("entry-history-limit"),
    QObject::tr("Maximum history items per entry (default: 10, 0 = unlimited)."),
    QStringLiteral("count"));

const QCommandLineOption DbCleanupCommand::TombstoneCleanupOption(
    QStringList() << QStringLiteral("tombstone-cleanup"),
    QObject::tr("Enable cleanup of tombstones (disabled by default)."));

DbCleanupCommand::DbCleanupCommand()
{
    name = QStringLiteral("db-cleanup");
    description = QObject::tr("Clean up database snapshots, entry history, and tombstones.");

    positionalArguments.append(
        {QStringLiteral("database"), QObject::tr("Path of the database."), QStringLiteral("")});

    options.append(MaxSnapshotsOption);
    options.append(MaxDaysOption);
    options.append(EntryHistoryLimitOption);
    options.append(TombstoneCleanupOption);
    options.append(Command::KeyFileOption);
    options.append(Command::NoPasswordOption);
    options.append(Command::YubiKeyOption);
}

int DbCleanupCommand::execute(const QStringList& arguments)
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

    // Open database
    auto db = Utils::unlockDatabase(dbPath,
                                    !parser->isSet(Command::NoPasswordOption),
                                    parser->value(Command::KeyFileOption),
                                    parser->value(Command::YubiKeyOption),
                                    parser->isSet(Command::QuietOption));
    if (!db) {
        return EXIT_FAILURE;
    }

    // Build policy from command-line options (or use defaults)
    LifecyclePolicy policy;
    if (parser->isSet(MaxSnapshotsOption)) {
        bool ok = false;
        int val = parser->value(MaxSnapshotsOption).toInt(&ok);
        if (ok) {
            policy.maxSnapshotCount = val;
        }
    }
    if (parser->isSet(MaxDaysOption)) {
        bool ok = false;
        int val = parser->value(MaxDaysOption).toInt(&ok);
        if (ok) {
            policy.maxSnapshotDays = val;
        }
    }
    if (parser->isSet(EntryHistoryLimitOption)) {
        bool ok = false;
        int val = parser->value(EntryHistoryLimitOption).toInt(&ok);
        if (ok) {
            policy.entryHistoryLimit = val;
        }
    }
    policy.tombstoneCleanupEnabled = parser->isSet(TombstoneCleanupOption);

    // Set up metadata engine for tombstone cleanup
    SyncMetadataEngine metadataEngine(db);

    // Phase 8 (review #3): block compaction if the database was modified
    // externally — cleaning up a tampered database could discard evidence or
    // operate on inconsistent indexes.
    {
        ExternalChangeDetector detector(db, &metadataEngine);
        ExternalChangeReport report;
        if (!detector.runPreSyncCheck(&report)) {
            err << QObject::tr("Error: cannot clean up — the database shows signs of "
                               "external modification. Run `keepassxc-cli db-check` / `repair` first. "
                               "Findings: %1")
                       .arg(report.findings.join(QStringLiteral("; ")))
                << Qt::endl;
            return EXIT_FAILURE;
        }
    }

    // Run cleanup
    out << QObject::tr("Running database cleanup...") << Qt::endl;
    LifecycleManager manager(db, &metadataEngine);
    CleanupExecutionReport report = manager.runCleanup(policy);

    out << QObject::tr("Cleanup complete:") << Qt::endl;
    out << QObject::tr("  Snapshots removed:  %1").arg(report.cleanedSnapshots) << Qt::endl;
    out << QObject::tr("  History items removed: %1").arg(report.cleanedHistoryRecords) << Qt::endl;
    if (policy.tombstoneCleanupEnabled) {
        out << QObject::tr("  Tombstones removed: %1").arg(report.cleanedTombstones) << Qt::endl;
    }
    out << QObject::tr("  Estimated bytes reclaimed: %1").arg(report.reclaimedBytes) << Qt::endl;

    // Save database after cleanup
    QString error;
    if (!db->save(Database::Atomic, {}, &error)) {
        err << QObject::tr("Error: failed to save database: %1").arg(error) << Qt::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
