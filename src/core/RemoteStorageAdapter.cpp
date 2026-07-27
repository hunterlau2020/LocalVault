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

#include "RemoteStorageAdapter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QUuid>

// ---------------------------------------------------------------------------
// RemoteStorageConfig
// ---------------------------------------------------------------------------

QJsonObject RemoteStorageConfig::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("name")] = name;
    o[QStringLiteral("downloadCommand")] = downloadCommand;
    o[QStringLiteral("downloadCommandInput")] = downloadCommandInput;
    o[QStringLiteral("downloadTimeoutMsec")] = downloadTimeoutMsec;
    o[QStringLiteral("uploadCommand")] = uploadCommand;
    o[QStringLiteral("uploadCommandInput")] = uploadCommandInput;
    o[QStringLiteral("uploadTimeoutMsec")] = uploadTimeoutMsec;
    return o;
}

RemoteStorageConfig RemoteStorageConfig::fromJson(const QJsonObject& o)
{
    RemoteStorageConfig c;
    c.name = o.value(QStringLiteral("name")).toString();
    c.downloadCommand = o.value(QStringLiteral("downloadCommand")).toString();
    c.downloadCommandInput = o.value(QStringLiteral("downloadCommandInput")).toString();
    c.downloadTimeoutMsec = o.value(QStringLiteral("downloadTimeoutMsec")).toInt(30000);
    c.uploadCommand = o.value(QStringLiteral("uploadCommand")).toString();
    c.uploadCommandInput = o.value(QStringLiteral("uploadCommandInput")).toString();
    c.uploadTimeoutMsec = o.value(QStringLiteral("uploadTimeoutMsec")).toInt(30000);
    return c;
}

// ---------------------------------------------------------------------------
// CommandDelegatingAdapter
// ---------------------------------------------------------------------------

CommandDelegatingAdapter::CommandDelegatingAdapter(RemoteStorageConfig config)
    : m_config(std::move(config))
{
}

CommandDelegatingAdapter::~CommandDelegatingAdapter() = default;

bool CommandDelegatingAdapter::runCommand(const QString& commandTemplate,
                                          const QString& commandInput,
                                          int timeoutMsec,
                                          const QString& destPath,
                                          bool requireNonEmptyDest,
                                          QString* error)
{
    // Substitute placeholders. {TEMP_DATABASE} aligns with gui/remote; {FILE} is
    // a friendlier alias. Use NATIVE separators: Windows cmd treats '/' as a
    // command switch, so a forward-slashed path breaks cmd-based commands.
    const QString nativeDest = QDir::toNativeSeparators(destPath);
    QString cmd = commandTemplate;
    cmd.replace(QStringLiteral("{TEMP_DATABASE}"), nativeDest);
    cmd.replace(QStringLiteral("{FILE}"), nativeDest);

    m_process.reset(new QProcess());

    // Non-shell execution (review 🔴#3): splitCommand splits program+args WITHOUT
    // invoking a shell, so there is no shell injection. Shell features require an
    // explicit `sh -c` / `cmd /c` wrapper supplied by the user.
    QStringList parts = QProcess::splitCommand(cmd);
    if (parts.isEmpty()) {
        if (error) {
            *error = QStringLiteral("empty command");
        }
        return false;
    }
    const QString program = parts.takeFirst();
    m_process->start(program, parts);
    if (!m_process->waitForStarted(5000)) {
        if (error) {
            *error = QStringLiteral("failed to start '%1'").arg(program);
        }
        return false;
    }

    if (!commandInput.isEmpty()) {
        m_process->write(commandInput.toUtf8());
        m_process->write("\n");
        m_process->waitForBytesWritten(5000);
        m_process->closeWriteChannel();
    }

    if (!m_process->waitForFinished(timeoutMsec)) {
        m_process->kill();
        m_process->waitForFinished(5000);
        if (error) {
            *error = QStringLiteral("command timed out after %1 ms: %2").arg(timeoutMsec).arg(program);
        }
        return false;
    }

    if (m_process->exitStatus() != QProcess::NormalExit || m_process->exitCode() != 0) {
        if (error) {
            // Some tools (e.g. Windows `copy` builtin) write errors to stdout, not stderr.
            QString detail = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
            if (detail.isEmpty()) {
                detail = QString::fromUtf8(m_process->readAllStandardOutput()).trimmed();
            }
            *error = QStringLiteral("command failed (exit %1): %2").arg(m_process->exitCode()).arg(detail);
        }
        return false;
    }

    if (requireNonEmptyDest) {
        const QFileInfo fi(destPath);
        if (!fi.exists() || fi.size() == 0) {
            if (error) {
                *error = QStringLiteral("command succeeded but produced an empty file");
            }
            return false;
        }
    }
    return true;
}

bool CommandDelegatingAdapter::testConnection(QString* error)
{
    // Download to a throwaway temp file; success = exit 0 AND non-empty file
    // (review 🟡#4: some tools exit 0 on network failure with an empty file).
    const QString tmp = QDir::temp().absoluteFilePath(
        QStringLiteral("remote-test-%1.kdbx").arg(QUuid::createUuid().toString(QUuid::Id128)));
    const bool ok = runCommand(m_config.downloadCommand,
                               m_config.downloadCommandInput,
                               m_config.downloadTimeoutMsec,
                               tmp,
                               true,
                               error);
    QFile::remove(tmp);
    return ok;
}

bool CommandDelegatingAdapter::fetch(const QString& localTempPath, QString* error)
{
    return runCommand(m_config.downloadCommand,
                      m_config.downloadCommandInput,
                      m_config.downloadTimeoutMsec,
                      localTempPath,
                      true,
                      error);
}

bool CommandDelegatingAdapter::upload(const QString& localPath, QString* error)
{
    return runCommand(m_config.uploadCommand,
                      m_config.uploadCommandInput,
                      m_config.uploadTimeoutMsec,
                      localPath,
                      false,
                      error);
}

void CommandDelegatingAdapter::cancel()
{
    if (m_process) {
        m_process->kill();
    }
}
