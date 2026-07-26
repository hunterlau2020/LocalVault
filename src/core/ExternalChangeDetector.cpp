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

#include "ExternalChangeDetector.h"

#include "core/SyncMetadata.h"
#include "core/IntegrityDigest.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/SnapshotService.h"

#include <QDateTime>
#include <QFileInfo>
#include <QSet>
#include <QUuid>

namespace
{
    // Heuristic: is the database stored in a cloud-synced / otherwise risky location?
    bool isRiskyDirectory(const QString& path)
    {
        if (path.isEmpty()) {
            return false;
        }
        const QString p = path.toLower();
        static const QStringList riskMarkers = {
            QStringLiteral("onedrive"),
            QStringLiteral("dropbox"),
            QStringLiteral("google drive"),
            QStringLiteral("googledrive"),
            QStringLiteral("icloud"),
        };
        for (const auto& marker : riskMarkers) {
            if (p.contains(marker)) {
                return true;
            }
        }
        return false;
    }
} // namespace

ExternalChangeDetector::ExternalChangeDetector(QSharedPointer<Database> db, SyncMetadataEngine* metadataEngine)
    : m_db(std::move(db))
    , m_metadataEngine(metadataEngine)
{
}

// ---------------------------------------------------------------------------
// Detection (read-only)
// ---------------------------------------------------------------------------

ExternalChangeReport ExternalChangeDetector::detectExternalChange(bool deep) const
{
    ExternalChangeReport report;

    if (!m_db || !m_metadataEngine) {
        report.findings << QStringLiteral("detector is not fully initialized (missing database or metadata engine)");
        return report;
    }

    const IntegritySummary baseline = m_metadataEngine->integritySummary();
    const QString actualContent = IntegrityDigest::contentDigest(*m_db, *m_metadataEngine);
    const QString actualMeta = IntegrityDigest::metadataRootDigest(*m_metadataEngine);
    report.actualContentDigest = actualContent;
    report.actualMetadataDigest = actualMeta;
    report.recordedContentDigest = baseline.fileSha256;
    report.recordedMetadataDigest = baseline.metadataRootDigest;

    // No baseline recorded yet → cannot detect; treat as clean (first run).
    if (baseline.isEmpty()) {
        report.findings << QStringLiteral("no integrity baseline recorded; "
                                          "save the database or run repair to establish one");
        report.suggestedActions << QStringLiteral("establish baseline");
        return report;
    }

    // Authoritative digest comparison.
    if (!baseline.fileSha256.isEmpty() && actualContent != baseline.fileSha256) {
        report.fileHashChanged = true;
        report.findings << QStringLiteral("content digest mismatch: database content was modified outside LocalVault");
    }
    if (!baseline.metadataRootDigest.isEmpty() && actualMeta != baseline.metadataRootDigest) {
        report.metadataMismatch = true;
        report.findings << QStringLiteral("metadata root digest mismatch: sync metadata was modified outside LocalVault");
    }

    // Advisory: file size/mtime (only meaningful when a file path exists).
    const QString path = m_db->filePath();
    if (!path.isEmpty()) {
        const QFileInfo fi(path);
        if (fi.exists()) {
            if (baseline.fileSize != 0 && fi.size() != baseline.fileSize) {
                report.fileSizeChanged = true;
                report.findings << QStringLiteral("file size changed (%1 -> %2)").arg(baseline.fileSize).arg(fi.size());
            }
            if (baseline.fileMtimeUtc.isValid() && fi.lastModified().toUTC() != baseline.fileMtimeUtc) {
                report.fileMtimeChanged = true;
                report.findings << QStringLiteral("file modification time changed");
            }
        }
    }

    if (isRiskyDirectory(path)) {
        report.riskyDirectoryDetected = true;
        report.findings << QStringLiteral("database resides in a synced/risky directory");
    }

    // Deep tier: structural consistency checks (on demand or when quick already flagged something).
    if (deep || report.fileHashChanged || report.metadataMismatch) {
        runDeepChecks(report);
    }

    // Resolve severity (highest applicable signal wins).
    if (report.fileHashChanged) {
        report.severity = ChangeSeverity::High;
    } else if (report.metadataMismatch) {
        report.severity = ChangeSeverity::Medium;
    } else if (report.riskyDirectoryDetected || report.fileSizeChanged || report.fileMtimeChanged
               || report.indexInconsistency) {
        report.severity = ChangeSeverity::Low;
    }

    if (report.severity == ChangeSeverity::High || report.severity == ChangeSeverity::Medium) {
        report.suggestedActions << QStringLiteral("run repair before syncing");
    }

    return report;
}

bool ExternalChangeDetector::runPreSyncCheck(ExternalChangeReport* report) const
{
    ExternalChangeReport local;
    ExternalChangeReport& r = report ? *report : local;
    r = detectExternalChange(/*deep=*/false);
    // Proceed only when there is no authoritative (Medium/High) change.
    return r.severity == ChangeSeverity::None || r.severity == ChangeSeverity::Low;
}

void ExternalChangeDetector::recordBaseline()
{
    if (!m_db || !m_metadataEngine) {
        return;
    }
    IntegritySummary s;
    s.metadataRootDigest = IntegrityDigest::metadataRootDigest(*m_metadataEngine);
    s.fileSha256 = IntegrityDigest::contentDigest(*m_db, *m_metadataEngine);
    s.checkedAtUtc = QDateTime::currentDateTimeUtc();

    const QString path = m_db->filePath();
    if (!path.isEmpty()) {
        const QFileInfo fi(path);
        if (fi.exists()) {
            s.fileSize = fi.size();
            s.fileMtimeUtc = fi.lastModified().toUTC();
        }
    }
    m_metadataEngine->setIntegritySummary(s);
}

// ---------------------------------------------------------------------------
// Deep checks
// ---------------------------------------------------------------------------

void ExternalChangeDetector::runDeepChecks(ExternalChangeReport& report) const
{
    if (!m_db || !m_metadataEngine) {
        return;
    }

    QSet<QUuid> liveIds;
    if (auto* root = m_db->rootGroup()) {
        const auto entries = root->entriesRecursive(false);
        for (const Entry* e : entries) {
            liveIds.insert(e->uuid());
        }
    }

    // Tombstone referencing a still-live entry → logical inconsistency.
    for (const auto& tb : m_metadataEngine->tombstones()) {
        if (liveIds.contains(tb.entryId)) {
            report.indexInconsistency = true;
            report.findings << QStringLiteral("tombstone references a live entry: %1")
                                   .arg(tb.entryId.toString(QUuid::Id128));
        }
    }

    // Conflict referencing an entry that is neither live nor tombstoned.
    QSet<QUuid> tombIds;
    for (const auto& tb : m_metadataEngine->tombstones()) {
        tombIds.insert(tb.entryId);
    }
    for (const auto& cf : m_metadataEngine->conflicts()) {
        if (!liveIds.contains(cf.entryId) && !tombIds.contains(cf.entryId)) {
            report.indexInconsistency = true;
            report.findings << QStringLiteral("conflict references an unknown entry: %1")
                                   .arg(cf.entryId.toString(QUuid::Id128));
        }
    }
}

// ---------------------------------------------------------------------------
// Repair (write path)
// ---------------------------------------------------------------------------

RepairPlan ExternalChangeDetector::buildRepairPlan(const ExternalChangeReport& report) const
{
    RepairPlan plan;
    plan.requireUserConfirmation = true;

    switch (report.severity) {
    case ChangeSeverity::High:
        // Content was tampered with — isolate the current state as a new branch.
        plan.planType = RepairPlanType::MarkNewBranch;
        plan.steps << QStringLiteral("create a pre-repair snapshot")
                   << QStringLiteral("mark the current database as a new sync branch (bump version vectors)")
                   << QStringLiteral("reset the sync baseline")
                   << QStringLiteral("re-establish the integrity baseline");
        break;
    case ChangeSeverity::Medium:
        // Sync metadata was tampered with — rebuild index consistency and reset baseline.
        plan.planType = RepairPlanType::RebuildIndex;
        plan.steps << QStringLiteral("create a pre-repair snapshot")
                   << QStringLiteral("rebuild tombstone/conflict index consistency")
                   << QStringLiteral("reset the sync baseline")
                   << QStringLiteral("re-establish the integrity baseline");
        break;
    case ChangeSeverity::Low:
        plan.planType = RepairPlanType::ResetSyncBaseline;
        plan.steps << QStringLiteral("create a pre-repair snapshot")
                   << QStringLiteral("reset the sync baseline")
                   << QStringLiteral("re-establish the integrity baseline");
        break;
    case ChangeSeverity::None:
    default:
        plan.planType = RepairPlanType::None;
        plan.requireUserConfirmation = false;
        plan.steps << QStringLiteral("no repair needed");
        break;
    }
    return plan;
}

bool ExternalChangeDetector::executeRepairPlan(const RepairPlan& plan, QString* error)
{
    if (!m_db || !m_metadataEngine) {
        if (error) {
            *error = QStringLiteral("detector is not fully initialized (missing database or metadata engine)");
        }
        return false;
    }
    if (plan.planType == RepairPlanType::None) {
        return true;
    }

    // Pre-repair snapshot (best-effort), mirroring the SyncEngine/LifecycleManager reuse pattern.
    {
        SnapshotService ss(m_db);
        ss.createSnapshot(QStringLiteral("repair_pre"));
    }

    switch (plan.planType) {
    case RepairPlanType::RebuildIndex:
        rebuildIndexes();
        resetSyncBaseline();
        break;
    case RepairPlanType::ResetSyncBaseline:
        resetSyncBaseline();
        break;
    case RepairPlanType::MarkNewBranch:
        markNewBranch();
        resetSyncBaseline();
        break;
    case RepairPlanType::None:
        break;
    }

    // Re-establish the integrity baseline from the now-current state, then persist.
    recordBaseline();
    m_metadataEngine->saveToDatabase();
    return true;
}

void ExternalChangeDetector::rebuildIndexes()
{
    if (!m_db || !m_metadataEngine) {
        return;
    }
    QSet<QUuid> liveIds;
    if (auto* root = m_db->rootGroup()) {
        const auto entries = root->entriesRecursive(false);
        for (const Entry* e : entries) {
            liveIds.insert(e->uuid());
        }
    }
    // Purge tombstones that reference still-live entries.
    const auto tombstones = m_metadataEngine->tombstones();
    for (const auto& tb : tombstones) {
        if (liveIds.contains(tb.entryId)) {
            m_metadataEngine->removeTombstone(tb.entryId);
        }
    }
    // Resolve conflicts referencing entries that are neither live nor tombstoned.
    QSet<QUuid> tombIds;
    for (const auto& tb : m_metadataEngine->tombstones()) {
        tombIds.insert(tb.entryId);
    }
    const auto conflicts = m_metadataEngine->conflicts();
    for (const auto& cf : conflicts) {
        if (!cf.resolved && !liveIds.contains(cf.entryId) && !tombIds.contains(cf.entryId)) {
            m_metadataEngine->markConflictResolved(cf.conflictId);
        }
    }
}

void ExternalChangeDetector::resetSyncBaseline()
{
    if (m_metadataEngine) {
        m_metadataEngine->clearSyncBaselines();
    }
}

void ExternalChangeDetector::markNewBranch()
{
    if (!m_db || !m_metadataEngine) {
        return;
    }
    if (auto* root = m_db->rootGroup()) {
        const auto entries = root->entriesRecursive(false);
        for (Entry* e : entries) {
            m_metadataEngine->incrementEntryCounter(e);
        }
    }
}
