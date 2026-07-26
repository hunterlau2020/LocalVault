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

#ifndef KEEPASSXC_EXTERNALCHANGEDETECTOR_H
#define KEEPASSXC_EXTERNALCHANGEDETECTOR_H

#include <QSharedPointer>
#include <QString>
#include <QStringList>

class Database;
class SyncMetadataEngine;

/**
 * Severity of a detected external change (Phase 8).
 */
enum class ChangeSeverity
{
    None,     // no external change detected
    Low,      // advisory only (file size/mtime drift or risky directory); digests intact
    Medium,   // metadata root digest mismatch — sync metadata was tampered
    High      // content digest mismatch — database content was tampered
};

/**
 * Result of an external-change detection run.
 */
struct ExternalChangeReport
{
    // Authoritative signals (cryptographic digests)
    bool fileHashChanged = false;     // content digest mismatch
    bool metadataMismatch = false;    // metadata root digest mismatch

    // Advisory / heuristic signals
    bool riskyDirectoryDetected = false;
    bool fileSizeChanged = false;
    bool fileMtimeChanged = false;
    bool indexInconsistency = false;  // deep check: tombstone/conflict referential issue

    ChangeSeverity severity = ChangeSeverity::None;

    QStringList findings;         // human-readable list of what was detected
    QStringList suggestedActions; // recommended next steps

    // Diagnostic: recorded baseline vs currently-computed digests
    QString recordedContentDigest;
    QString actualContentDigest;
    QString recordedMetadataDigest;
    QString actualMetadataDigest;
};

/**
 * Type of repair to apply (Phase 8 repair mode).
 */
enum class RepairPlanType
{
    None,                // no repair needed
    RebuildIndex,        // rebuild tombstone/conflict/snapshot index consistency
    ResetSyncBaseline,   // discard sync baselines, mark current DB as the new baseline
    MarkNewBranch        // treat current DB as a new branch (bump all version vectors)
};

/**
 * A repair plan derived from a detection report.
 */
struct RepairPlan
{
    RepairPlanType planType = RepairPlanType::None;
    QStringList steps;                 // human-readable description of the steps
    bool requireUserConfirmation = true;
};

/**
 * ExternalChangeDetector — detects modifications to the database made outside
 * LocalVault and provides a controlled repair entry point (Phase 8).
 *
 * Detection compares canonical digests of the current (in-memory) database
 * against the integrity_summary baseline persisted in KPXC_SYNC_METADATA. It is
 * meaningful when the database was freshly loaded from disk (open time, CLI
 * sync/repair), so the in-memory state reflects the on-disk file.
 *
 * Thread-safety: not guaranteed. Use from a single thread only.
 */
class ExternalChangeDetector
{
public:
    explicit ExternalChangeDetector(QSharedPointer<Database> db, SyncMetadataEngine* metadataEngine = nullptr);

    // --- Detection (read-only) ------------------------------------------------
    ExternalChangeReport detectExternalChange(bool deep = false) const;

    // Returns true if sync may proceed (no blocking change); false to abort.
    // If non-null, fills *report with the detection result.
    bool runPreSyncCheck(ExternalChangeReport* report = nullptr) const;

    // Records the current database state as the integrity baseline (digests +
    // advisory file size/mtime). Call after a successful save.
    void recordBaseline();

    // --- Repair (write path) --------------------------------------------------
    RepairPlan buildRepairPlan(const ExternalChangeReport& report) const;
    bool executeRepairPlan(const RepairPlan& plan, QString* error = nullptr);

private:
    void runDeepChecks(ExternalChangeReport& report) const;
    void rebuildIndexes();
    void resetSyncBaseline();
    void markNewBranch();

    QSharedPointer<Database> m_db;
    SyncMetadataEngine* m_metadataEngine;
};

#endif // KEEPASSXC_EXTERNALCHANGEDETECTOR_H
