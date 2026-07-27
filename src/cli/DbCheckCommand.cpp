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

#include "DbCheckCommand.h"

#include "Utils.h"
#include "core/Database.h"
#include "core/ExternalChangeDetector.h"
#include "core/SyncMetadata.h"

#include <QCommandLineParser>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

const QCommandLineOption DbCheckCommand::DeepOption(
    QStringList() << QStringLiteral("deep"),
    QObject::tr("Run deep structural consistency checks in addition to digest comparison."));

const QCommandLineOption DbCheckCommand::JsonOption(
    QStringList() << QStringLiteral("json"),
    QObject::tr("Output the report as machine-readable JSON."));

const QCommandLineOption DbCheckCommand::PrettyOption(
    QStringList() << QStringLiteral("pretty"),
    QObject::tr("Pretty-print JSON output (indented). Use with --json."));

DbCheckCommand::DbCheckCommand()
{
    name = QStringLiteral("db-check");
    description = QObject::tr("Detect whether the database was modified outside LocalVault (Phase 8).");

    positionalArguments.append(
        {QStringLiteral("database"), QObject::tr("Path of the database."), QStringLiteral("")});

    options.append(DeepOption);
    options.append(JsonOption);
    options.append(PrettyOption);
    options.append(Command::KeyFileOption);
    options.append(Command::NoPasswordOption);
    options.append(Command::YubiKeyOption);
}

namespace
{
QString severityString(ChangeSeverity s)
{
    switch (s) {
    case ChangeSeverity::None:
        return QStringLiteral("none");
    case ChangeSeverity::Low:
        return QStringLiteral("low");
    case ChangeSeverity::Medium:
        return QStringLiteral("medium");
    case ChangeSeverity::High:
        return QStringLiteral("high");
    }
    return QStringLiteral("unknown");
}
} // namespace

int DbCheckCommand::execute(const QStringList& arguments)
{
    auto& out = Utils::STDOUT;
    auto& err = Utils::STDERR;

    QSharedPointer<QCommandLineParser> parser = getCommandLineParser(arguments);
    if (parser.isNull()) {
        return EXIT_FAILURE;
    }

    // --pretty only affects --json output; warn if given alone (review3-followup #3).
    if (parser->isSet(PrettyOption) && !parser->isSet(JsonOption)) {
        err << QObject::tr("Warning: --pretty has no effect without --json.") << Qt::endl;
    }

    const QStringList args = parser->positionalArguments();
    const QString dbPath = args.at(0);

    if (!QFileInfo::exists(dbPath)) {
        err << QObject::tr("Error: database not found: %1").arg(dbPath) << Qt::endl;
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

    SyncMetadataEngine metadataEngine(db);
    ExternalChangeDetector detector(db, &metadataEngine);

    const bool deep = parser->isSet(DeepOption);
    const auto report = detector.detectExternalChange(deep);

    if (parser->isSet(JsonOption)) {
        // Full machine-readable report (review #4): booleans, diagnostic digests,
        // findings, and suggested actions.
        QJsonObject json;
        json[QStringLiteral("severity")] = severityString(report.severity);
        json[QStringLiteral("file_hash_changed")] = report.fileHashChanged;
        json[QStringLiteral("metadata_mismatch")] = report.metadataMismatch;
        json[QStringLiteral("index_inconsistency")] = report.indexInconsistency;
        json[QStringLiteral("risky_directory")] = report.riskyDirectoryDetected;
        json[QStringLiteral("recorded_content_digest")] = report.recordedContentDigest;
        json[QStringLiteral("actual_content_digest")] = report.actualContentDigest;
        json[QStringLiteral("recorded_metadata_digest")] = report.recordedMetadataDigest;
        json[QStringLiteral("actual_metadata_digest")] = report.actualMetadataDigest;
        QJsonArray findingsArr;
        for (const auto& f : report.findings) {
            findingsArr.append(f);
        }
        json[QStringLiteral("findings")] = findingsArr;
        QJsonArray actionsArr;
        for (const auto& a : report.suggestedActions) {
            actionsArr.append(a);
        }
        json[QStringLiteral("suggested_actions")] = actionsArr;
        const QJsonDocument::JsonFormat fmt = parser->isSet(PrettyOption) ? QJsonDocument::Indented
                                                                           : QJsonDocument::Compact;
        out << QJsonDocument(json).toJson(fmt) << Qt::endl;
    } else {
        out << QObject::tr("External change check (deep=%1):")
                   .arg(deep ? QObject::tr("yes") : QObject::tr("no"))
            << Qt::endl;
        out << "  " << QObject::tr("Severity: %1").arg(severityString(report.severity)) << Qt::endl;
        out << "  " << QObject::tr("Content digest changed: %1")
                   .arg(report.fileHashChanged ? QObject::tr("yes") : QObject::tr("no"))
            << Qt::endl;
        out << "  " << QObject::tr("Metadata digest changed: %1")
                   .arg(report.metadataMismatch ? QObject::tr("yes") : QObject::tr("no"))
            << Qt::endl;
        out << "  " << QObject::tr("Index inconsistency: %1")
                   .arg(report.indexInconsistency ? QObject::tr("yes") : QObject::tr("no"))
            << Qt::endl;
        out << "  " << QObject::tr("Risky directory: %1")
                   .arg(report.riskyDirectoryDetected ? QObject::tr("yes") : QObject::tr("no"))
            << Qt::endl;
        if (!report.findings.isEmpty()) {
            out << QObject::tr("  Findings:") << Qt::endl;
            for (const auto& f : report.findings) {
                out << "    - " << f << Qt::endl;
            }
        }
        if (!report.suggestedActions.isEmpty()) {
            out << QObject::tr("  Suggested actions:") << Qt::endl;
            for (const auto& a : report.suggestedActions) {
                out << "    - " << a << Qt::endl;
            }
        }
    }

    // Exit non-zero only when a blocking (Medium/High) change was detected.
    return (report.severity == ChangeSeverity::High || report.severity == ChangeSeverity::Medium)
               ? EXIT_FAILURE
               : EXIT_SUCCESS;
}
