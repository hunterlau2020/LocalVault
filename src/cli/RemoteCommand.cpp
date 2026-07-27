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

#include "RemoteCommand.h"

#include "Utils.h"
#include "core/Database.h"
#include "core/RemoteConfigService.h"
#include "core/RemoteStorageAdapter.h"

#include <QCommandLineParser>
#include <QFileInfo>

const QCommandLineOption RemoteCommand::AddOption(QStringList() << QStringLiteral("add"),
    QObject::tr("Add or update a remote storage configuration."));

const QCommandLineOption RemoteCommand::ListOption(QStringList() << QStringLiteral("list"),
    QObject::tr("List configured remote storage backends."));

const QCommandLineOption RemoteCommand::TestOption(QStringList() << QStringLiteral("test"),
    QObject::tr("Test the connection to a configured remote."));

const QCommandLineOption RemoteCommand::RemoveOption(QStringList() << QStringLiteral("remove"),
    QObject::tr("Remove a configured remote."));

const QCommandLineOption RemoteCommand::NameOption(
    QStringList() << QStringLiteral("name"), QObject::tr("Name of the remote configuration."), QStringLiteral("name"));

const QCommandLineOption RemoteCommand::DownloadCmdOption(
    QStringList() << QStringLiteral("download-cmd"),
    QObject::tr("Download command template. Use {TEMP_DATABASE} or {FILE} as the local file placeholder."),
    QStringLiteral("command"));

const QCommandLineOption RemoteCommand::UploadCmdOption(
    QStringList() << QStringLiteral("upload-cmd"),
    QObject::tr("Upload command template. Use {TEMP_DATABASE} or {FILE} as the local file placeholder."),
    QStringLiteral("command"));

const QCommandLineOption RemoteCommand::TimeoutOption(
    QStringList() << QStringLiteral("timeout"),
    QObject::tr("Command timeout in seconds (default: 30)."),
    QStringLiteral("seconds"));

RemoteCommand::RemoteCommand()
{
    name = QStringLiteral("remote");
    description = QObject::tr("Configure and test remote storage backends for sync (Phase 10).");

    positionalArguments.append(
        {QStringLiteral("database"), QObject::tr("Path of the database."), QStringLiteral("")});

    options.append(AddOption);
    options.append(ListOption);
    options.append(TestOption);
    options.append(RemoveOption);
    options.append(NameOption);
    options.append(DownloadCmdOption);
    options.append(UploadCmdOption);
    options.append(TimeoutOption);
    options.append(Command::KeyFileOption);
    options.append(Command::NoPasswordOption);
    options.append(Command::YubiKeyOption);
}

int RemoteCommand::execute(const QStringList& arguments)
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

    const int actionCount = parser->isSet(AddOption) + parser->isSet(ListOption) + parser->isSet(TestOption)
                            + parser->isSet(RemoveOption);
    if (actionCount != 1) {
        err << QObject::tr("Error: specify exactly one of --add, --list, --test, --remove.") << Qt::endl;
        return EXIT_FAILURE;
    }

    auto db = Utils::unlockDatabase(dbPath,
                                    !parser->isSet(Command::NoPasswordOption),
                                    parser->value(Command::KeyFileOption),
                                    parser->value(Command::YubiKeyOption),
                                    parser->isSet(Command::QuietOption));
    if (!db) {
        return EXIT_FAILURE;
    }

    RemoteConfigService svc(db);

    // --- list -----------------------------------------------------------------
    if (parser->isSet(ListOption)) {
        const auto remotes = svc.load();
        out << QObject::tr("Configured remotes (%1):").arg(remotes.size()) << Qt::endl;
        for (const auto& r : remotes) {
            out << QObject::tr("  %1").arg(r.name) << Qt::endl;
            out << QObject::tr("    download: %1").arg(r.downloadCommand) << Qt::endl;
            out << QObject::tr("    upload:   %1").arg(r.uploadCommand) << Qt::endl;
        }
        return EXIT_SUCCESS;
    }

    // --- add / update ---------------------------------------------------------
    if (parser->isSet(AddOption)) {
        if (!parser->isSet(NameOption) || !parser->isSet(DownloadCmdOption) || !parser->isSet(UploadCmdOption)) {
            err << QObject::tr("Error: --add requires --name, --download-cmd, --upload-cmd.") << Qt::endl;
            return EXIT_FAILURE;
        }
        RemoteStorageConfig c;
        c.name = parser->value(NameOption);
        c.downloadCommand = parser->value(DownloadCmdOption);
        c.uploadCommand = parser->value(UploadCmdOption);
        if (parser->isSet(TimeoutOption)) {
            const int sec = parser->value(TimeoutOption).toInt();
            if (sec > 0) {
                c.downloadTimeoutMsec = c.uploadTimeoutMsec = sec * 1000;
            }
        }
        svc.upsert(c);

        // Security warning (review 🔴#3): configured commands execute via QProcess.
        out << QObject::tr("Warning: the configured commands are executed via QProcess (non-shell). "
                           "Only configure commands you trust — they run with your permissions and "
                           "credentials are managed by the external tool itself (e.g. rclone).")
            << Qt::endl;

        QString error;
        if (!db->save(Database::Atomic, {}, &error)) {
            err << QObject::tr("Error: failed to save database: %1").arg(error) << Qt::endl;
            return EXIT_FAILURE;
        }
        out << QObject::tr("Remote '%1' saved.").arg(c.name) << Qt::endl;
        return EXIT_SUCCESS;
    }

    // --- test -----------------------------------------------------------------
    if (parser->isSet(TestOption)) {
        if (!parser->isSet(NameOption)) {
            err << QObject::tr("Error: --test requires --name.") << Qt::endl;
            return EXIT_FAILURE;
        }
        const auto c = svc.get(parser->value(NameOption));
        if (c.name.isEmpty()) {
            err << QObject::tr("Error: no remote named '%1'.").arg(parser->value(NameOption)) << Qt::endl;
            return EXIT_FAILURE;
        }
        out << QObject::tr("Testing connection to '%1'...").arg(c.name) << Qt::endl;
        CommandDelegatingAdapter adapter(c);
        QString e;
        if (adapter.testConnection(&e)) {
            out << QObject::tr("OK") << Qt::endl;
            return EXIT_SUCCESS;
        }
        err << QObject::tr("FAILED: %1").arg(e) << Qt::endl;
        return EXIT_FAILURE;
    }

    // --- remove ---------------------------------------------------------------
    if (parser->isSet(RemoveOption)) {
        if (!parser->isSet(NameOption)) {
            err << QObject::tr("Error: --remove requires --name.") << Qt::endl;
            return EXIT_FAILURE;
        }
        if (!svc.remove(parser->value(NameOption))) {
            err << QObject::tr("Error: no remote named '%1'.").arg(parser->value(NameOption)) << Qt::endl;
            return EXIT_FAILURE;
        }
        QString error;
        if (!db->save(Database::Atomic, {}, &error)) {
            err << QObject::tr("Error: failed to save database: %1").arg(error) << Qt::endl;
            return EXIT_FAILURE;
        }
        out << QObject::tr("Removed '%1'.").arg(parser->value(NameOption)) << Qt::endl;
        return EXIT_SUCCESS;
    }

    return EXIT_SUCCESS;
}
