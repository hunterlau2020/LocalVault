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

#include "SyncCommand.h"

#include "Command.h"
#include "Utils.h"
#include "core/ConflictResolver.h"
#include "core/Database.h"
#include "core/RemoteConfigService.h"
#include "core/RemoteStorageAdapter.h"
#include "core/SyncEngine.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <memory>

SyncCommand::SyncCommand()
{
    name = QStringLiteral("sync");
    description = QObject::tr("Synchronize entries between two databases. Performs manual sync main chain: "
                              "diff analysis, auto-merge, and conflict reporting.");

    positionalArguments.append({QStringLiteral("database"), QObject::tr("Path of the local database."), QStringLiteral("")});

    options.append(QCommandLineOption(
        QStringList() << QStringLiteral("r") << QStringLiteral("remote"),
        QObject::tr("Path of the remote database."),
        QStringLiteral("path")));
    options.append(QCommandLineOption(
        QStringList() << QStringLiteral("remote-storage"),
        QObject::tr("Name of a configured remote storage backend (Phase 10). "
                    "Mutually exclusive with --remote."),
        QStringLiteral("name")));
    options.append(Command::KeyFileOption);
    options.append(Command::NoPasswordOption);
    options.append(Command::YubiKeyOption);

    // Remote auth options
    options.append(QCommandLineOption(
        QStringList() << QStringLiteral("remote-key-file"),
        QObject::tr("Key file for the remote database."),
        QStringLiteral("path")));

    // Conflict resolution options
    options.append(QCommandLineOption(
        QStringList() << QStringLiteral("resolve"),
        QObject::tr("Automatically resolve conflicts using the given strategy: "
                     "keep-local, keep-remote, or create-copy."),
        QStringLiteral("strategy")));
}

int SyncCommand::execute(const QStringList& arguments)
{
    auto& out = Utils::STDOUT;
    auto& err = Utils::STDERR;

    QSharedPointer<QCommandLineParser> parser = getCommandLineParser(arguments);
    if (parser.isNull()) {
        return EXIT_FAILURE;
    }

    const QStringList args = parser->positionalArguments();
    const QString localPath = args.at(0);
    const QString remotePath = parser->value(QStringLiteral("remote"));
    const QString remoteStorageName = parser->value(QStringLiteral("remote-storage"));

    // --remote and --remote-storage are mutually exclusive (review 🟡#5).
    if (!remotePath.isEmpty() && !remoteStorageName.isEmpty()) {
        err << QObject::tr("Error: --remote and --remote-storage are mutually exclusive.") << Qt::endl;
        return EXIT_FAILURE;
    }
    if (remotePath.isEmpty() && remoteStorageName.isEmpty()) {
        err << QObject::tr("Error: provide either --remote/-r <path> or --remote-storage <name>.")
            << Qt::endl;
        return EXIT_FAILURE;
    }

    if (!QFileInfo::exists(localPath)) {
        err << QObject::tr("Error: local database not found: %1").arg(localPath) << Qt::endl;
        return EXIT_FAILURE;
    }

    // Phase 10 remote-storage state. The RAII guard removes the fetched temp file
    // on every return path (no manual cleanup at each early return needed).
    QString fetchedTempPath;
    struct TempFileGuard
    {
        const QString& path;
        ~TempFileGuard()
        {
            if (!path.isEmpty()) {
                QFile::remove(path);
            }
        }
    } fetchedTempGuard{fetchedTempPath};
    std::unique_ptr<CommandDelegatingAdapter> remoteAdapter;
    const bool isRemoteStorage = !remoteStorageName.isEmpty();
    QString effectiveRemotePath = remotePath;

    // --- Open both databases ------------------------------------------------
    out << QObject::tr("Opening local database...") << Qt::endl;
    auto localDb = Utils::unlockDatabase(localPath,
                                         !parser->isSet(Command::NoPasswordOption),
                                         parser->value(Command::KeyFileOption),
                                         parser->value(Command::YubiKeyOption),
                                         parser->isSet(Command::QuietOption));
    if (!localDb) {
        return EXIT_FAILURE;
    }

    // Resolve the effective remote path: fetch via adapter (--remote-storage) or
    // use the local path (--remote).
    if (isRemoteStorage) {
        RemoteConfigService svc(localDb);
        const RemoteStorageConfig cfg = svc.get(remoteStorageName);
        if (cfg.name.isEmpty()) {
            err << QObject::tr("Error: no remote storage backend named '%1'.").arg(remoteStorageName)
                << Qt::endl;
            return EXIT_FAILURE;
        }
        fetchedTempPath = QDir::temp().absoluteFilePath(
            QStringLiteral("remote-sync-%1.kdbx").arg(QUuid::createUuid().toString(QUuid::Id128)));
        QFile::remove(fetchedTempPath);
        remoteAdapter = std::make_unique<CommandDelegatingAdapter>(cfg);
        out << QObject::tr("Fetching remote database via '%1'...").arg(cfg.name) << Qt::endl;
        QString fetchErr;
        if (!remoteAdapter->fetch(fetchedTempPath, &fetchErr)) {
            err << QObject::tr("Failed to fetch remote database: %1").arg(fetchErr) << Qt::endl;
            return EXIT_FAILURE;
        }
        effectiveRemotePath = fetchedTempPath;
    } else if (!QFileInfo::exists(remotePath)) {
        err << QObject::tr("Error: remote database not found: %1").arg(remotePath) << Qt::endl;
        return EXIT_FAILURE;
    }

    out << QObject::tr("Opening remote database...") << Qt::endl;
    auto remoteDb = Utils::unlockDatabase(effectiveRemotePath,
                                          !parser->isSet(Command::NoPasswordOption),
                                          parser->value(QStringLiteral("remote-key-file")),
                                          {},
                                          parser->isSet(Command::QuietOption));
    if (!remoteDb) {
        err << QObject::tr("Failed to open remote database.") << Qt::endl;
        return EXIT_FAILURE;
    }

    // --- Run sync analysis --------------------------------------------------
    out << QObject::tr("Analyzing differences...") << Qt::endl;

    SyncEngine engine;
    engine.setMetadataEngine(localDb);
    SyncResult result = engine.analyzeDiffs(localDb, remoteDb);

    if (!result.success) {
        err << QObject::tr("Sync analysis failed: %1").arg(result.errorMessage) << Qt::endl;
        return EXIT_FAILURE;
    }

    out << result.summaryText() << Qt::endl;

    // --- Handle conflicts ---------------------------------------------------
    if (result.hasConflicts()) {
        const QString resolveStrategy = parser->value(QStringLiteral("resolve"));
        if (resolveStrategy.isEmpty()) {
            out << QObject::tr("Conflicts detected — skipping auto-merge.") << Qt::endl;
            out << QObject::tr("Use --resolve to auto-resolve (keep-local, keep-remote, or create-copy).") << Qt::endl;
            return EXIT_SUCCESS;
        }

        // Parse the resolution strategy
        ConflictResolution strategy;
        if (resolveStrategy == QStringLiteral("keep-local")) {
            strategy = ConflictResolution::KeepLocal;
        } else if (resolveStrategy == QStringLiteral("keep-remote")) {
            strategy = ConflictResolution::KeepRemote;
        } else if (resolveStrategy == QStringLiteral("create-copy")) {
            strategy = ConflictResolution::CreateCopy;
        } else {
            err << QObject::tr("Unknown resolution strategy '%1'. Use keep-local, keep-remote, or create-copy.").arg(resolveStrategy) << Qt::endl;
            return EXIT_FAILURE;
        }

        out << QObject::tr("Resolving conflicts (%1 strategy)...").arg(resolveStrategy) << Qt::endl;

        ConflictResolverService resolver(localDb, engine.metadataEngine());
        QList<ConflictResolutionResult> results = resolver.resolveAll(result, strategy);

        int failures = 0;
        int copies = 0;
        for (const auto& res : results) {
            if (!res.success) {
                ++failures;
                err << QObject::tr("  Failed to resolve conflict: %1").arg(res.errorMessage) << Qt::endl;
            }
            if (!res.createdCopyEntryId.isNull()) {
                ++copies;
            }
        }

        if (failures > 0) {
            err << QObject::tr("Conflict resolution completed with %1 failure(s).").arg(failures) << Qt::endl;
            return EXIT_FAILURE;
        }

        out << QObject::tr("All %1 conflict(s) resolved successfully.").arg(results.size()) << Qt::endl;
        if (copies > 0) {
            out << QObject::tr("  %1 conflict copy/copies created.").arg(copies) << Qt::endl;
        }
    }

    if (result.updatedCount == 0 && result.addedCount == 0 && !result.hasConflicts()) {
        out << QObject::tr("Nothing to merge.") << Qt::endl;
        return EXIT_SUCCESS;
    }

    out << QObject::tr("Applying changes to local database...") << Qt::endl;
    engine.applyMerges(result, localDb, remoteDb);

    // --- Save local database ------------------------------------------------
    QString errorMsg;
    if (!localDb->save(Database::Atomic, {}, &errorMsg)) {
        err << QObject::tr("Failed to save local database: %1").arg(errorMsg) << Qt::endl;
        return EXIT_FAILURE;
    }

    // --- Upload back (remote-storage only). MUST run after save() (review 🟡#6).
    if (isRemoteStorage) {
        out << QObject::tr("Uploading local database back to '%1'...").arg(remoteStorageName) << Qt::endl;
        QString uploadErr;
        if (!remoteAdapter->upload(localPath, &uploadErr)) {
            err << QObject::tr("Local database saved, but failed to upload back: %1").arg(uploadErr)
                << Qt::endl;
            return EXIT_FAILURE;
        }
    }

    out << QObject::tr("Sync complete. Local database saved.") << Qt::endl;
    return EXIT_SUCCESS;
}
