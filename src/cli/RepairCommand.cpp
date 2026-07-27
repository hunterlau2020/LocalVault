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

#include "RepairCommand.h"

#include "Utils.h"
#include "core/Database.h"
#include "core/ExternalChangeDetector.h"
#include "core/SyncMetadata.h"

#include <QCommandLineParser>
#include <QFileInfo>

const QCommandLineOption RepairCommand::TypeOption(
    QStringList() << QStringLiteral("type"),
    QObject::tr("Repair type: rebuild-index | reset-sync-baseline | mark-new-branch."),
    QStringLiteral("type"));

const QCommandLineOption RepairCommand::AutoOption(
    QStringList() << QStringLiteral("auto"),
    QObject::tr("Automatically execute the repair plan suggested by detection."));

const QCommandLineOption RepairCommand::DryRunOption(
    QStringList() << QStringLiteral("dry-run"),
    QObject::tr("Show the repair plan without applying it."));

RepairCommand::RepairCommand()
{
    name = QStringLiteral("repair");
    description = QObject::tr("Repair the database after external modification (Phase 8).");

    positionalArguments.append(
        {QStringLiteral("database"), QObject::tr("Path of the database."), QStringLiteral("")});

    options.append(TypeOption);
    options.append(AutoOption);
    options.append(DryRunOption);
    options.append(Command::KeyFileOption);
    options.append(Command::NoPasswordOption);
    options.append(Command::YubiKeyOption);
}

namespace
{
QString planTypeString(RepairPlanType t)
{
    switch (t) {
    case RepairPlanType::None:
        return QStringLiteral("none");
    case RepairPlanType::RebuildIndex:
        return QStringLiteral("rebuild-index");
    case RepairPlanType::ResetSyncBaseline:
        return QStringLiteral("reset-sync-baseline");
    case RepairPlanType::MarkNewBranch:
        return QStringLiteral("mark-new-branch");
    }
    return QStringLiteral("unknown");
}

RepairPlanType parsePlanType(const QString& s)
{
    if (s == QStringLiteral("rebuild-index")) {
        return RepairPlanType::RebuildIndex;
    }
    if (s == QStringLiteral("reset-sync-baseline")) {
        return RepairPlanType::ResetSyncBaseline;
    }
    if (s == QStringLiteral("mark-new-branch")) {
        return RepairPlanType::MarkNewBranch;
    }
    return RepairPlanType::None;
}
} // namespace

int RepairCommand::execute(const QStringList& arguments)
{
    auto& out = Utils::STDOUT;
    auto& err = Utils::STDERR;

    QSharedPointer<QCommandLineParser> parser = getCommandLineParser(arguments);
    if (parser.isNull()) {
        return EXIT_FAILURE;
    }

    // Validate --type early (review3 🔴#3): fail fast on bad input before
    // prompting for a password / touching the database.
    if (parser->isSet(TypeOption) && parsePlanType(parser->value(TypeOption)) == RepairPlanType::None) {
        err << QObject::tr("Error: unknown repair type '%1'. Valid values: "
                           "rebuild-index, reset-sync-baseline, mark-new-branch.")
                   .arg(parser->value(TypeOption))
            << Qt::endl;
        return EXIT_FAILURE;
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

    // Always detect first, so the suggested plan can be shown even with --type.
    const auto report = detector.detectExternalChange();
    const RepairPlan suggested = detector.buildRepairPlan(report);

    // Determine the plan to apply: explicit --type wins; otherwise the suggestion.
    RepairPlan plan;
    bool haveExplicitConsent = parser->isSet(AutoOption);
    if (parser->isSet(TypeOption)) {
        // --type was validated above; safe to use directly.
        plan.planType = parsePlanType(parser->value(TypeOption));
        plan.steps << QObject::tr("explicit repair type: %1").arg(planTypeString(plan.planType));
        haveExplicitConsent = true;
    } else {
        plan = suggested;
    }

    out << QObject::tr("Repair plan: %1").arg(planTypeString(plan.planType)) << Qt::endl;
    for (const auto& step : plan.steps) {
        out << "  - " << step << Qt::endl;
    }

    if (parser->isSet(DryRunOption)) {
        out << QObject::tr("(dry-run: no changes applied)") << Qt::endl;
        return EXIT_SUCCESS;
    }

    if (plan.planType == RepairPlanType::None) {
        out << QObject::tr("No repair needed.") << Qt::endl;
        return EXIT_SUCCESS;
    }

    // Require explicit consent (--type or --auto) before mutating the database.
    // Without either, only the plan is shown (review #2: --auto was ignored).
    if (!haveExplicitConsent) {
        out << QObject::tr("Plan shown only — pass --auto to apply the suggested plan, "
                           "or --type <type> to apply a specific repair.")
            << Qt::endl;
        return EXIT_SUCCESS;
    }

    QString error;
    if (!detector.executeRepairPlan(plan, &error)) {
        err << QObject::tr("Error: repair failed: %1").arg(error) << Qt::endl;
        return EXIT_FAILURE;
    }

    // Persist the repaired state (the writeDatabase hook refreshes the baseline).
    if (!db->save(Database::Atomic, {}, &error)) {
        err << QObject::tr("Error: failed to save database: %1").arg(error) << Qt::endl;
        return EXIT_FAILURE;
    }

    out << QObject::tr("Repair complete and database saved.") << Qt::endl;
    return EXIT_SUCCESS;
}
