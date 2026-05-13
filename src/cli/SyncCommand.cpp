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
#include "core/Database.h"
#include "core/SyncEngine.h"

#include <QCommandLineParser>
#include <QFileInfo>

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
    options.append(Command::KeyFileOption);
    options.append(Command::NoPasswordOption);
    options.append(Command::YubiKeyOption);

    // Remote auth options
    options.append(QCommandLineOption(
        QStringList() << QStringLiteral("remote-key-file"),
        QObject::tr("Key file for the remote database."),
        QStringLiteral("path")));
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

    if (remotePath.isEmpty()) {
        err << QObject::tr("Error: --remote/-r path is required.") << Qt::endl;
        return EXIT_FAILURE;
    }

    if (!QFileInfo::exists(localPath)) {
        err << QObject::tr("Error: local database not found: %1").arg(localPath) << Qt::endl;
        return EXIT_FAILURE;
    }
    if (!QFileInfo::exists(remotePath)) {
        err << QObject::tr("Error: remote database not found: %1").arg(remotePath) << Qt::endl;
        return EXIT_FAILURE;
    }

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

    out << QObject::tr("Opening remote database...") << Qt::endl;
    auto remoteDb = Utils::unlockDatabase(remotePath,
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
    SyncResult result = engine.analyzeDiffs(localDb, remoteDb);

    if (!result.success) {
        err << QObject::tr("Sync analysis failed: %1").arg(result.errorMessage) << Qt::endl;
        return EXIT_FAILURE;
    }

    out << result.summaryText() << Qt::endl;

    // --- Apply merges (if no conflicts) --------------------------------------
    if (result.hasConflicts()) {
        out << QObject::tr("Conflicts detected — skipping auto-merge.") << Qt::endl;
        out << QObject::tr("Resolve conflicts manually and re-run sync.") << Qt::endl;
        return EXIT_SUCCESS;
    }

    if (result.updatedCount == 0 && result.addedCount == 0) {
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

    out << QObject::tr("Sync complete. Local database saved.") << Qt::endl;
    return EXIT_SUCCESS;
}
