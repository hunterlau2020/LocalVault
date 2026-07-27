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

#ifndef KEEPASSXC_REMOTESTORAGEADAPTER_H
#define KEEPASSXC_REMOTESTORAGEADAPTER_H

#include <QJsonObject>
#include <QString>

#include <memory>

class QProcess;

/**
 * Configuration for one command-delegating remote (Phase 10, approach B).
 *
 * The JSON schema is INTENTIONALLY aligned with gui/remote/RemoteSettings
 * (same field names, msec units, top-level JSON array) so the GUI and the CLI
 * share the same KPXC_REMOTE_SYNC_SETTINGS CustomData blob without overwriting
 * each other's settings (design review 🔴#1).
 *
 * The command templates use the {TEMP_DATABASE} placeholder (alias {FILE}) which
 * the adapter substitutes with the local file path.
 */
struct RemoteStorageConfig
{
    QString name;
    QString downloadCommand;       // {TEMP_DATABASE}/{FILE} = destination local file
    QString downloadCommandInput;  // written to the process stdin (review 🔴#2: sftp batch / password piping)
    int downloadTimeoutMsec = 30000;
    QString uploadCommand;         // {TEMP_DATABASE}/{FILE} = source local file
    QString uploadCommandInput;
    int uploadTimeoutMsec = 30000;

    QJsonObject toJson() const;
    static RemoteStorageConfig fromJson(const QJsonObject& obj);
};

/**
 * Unified remote-storage interface (FSD §12.3, V1 subset).
 */
class IRemoteStorageAdapter
{
public:
    virtual ~IRemoteStorageAdapter() = default;
    virtual bool testConnection(QString* error = nullptr) = 0;
    virtual bool fetch(const QString& localTempPath, QString* error = nullptr) = 0; // remote → local temp
    virtual bool upload(const QString& localPath, QString* error = nullptr) = 0;   // local → remote
    virtual void cancel() = 0;
};

/**
 * Command-delegating adapter — runs user-configured download/upload commands.
 *
 * Execution model (review 🔴#3): commands are split with QProcess::splitCommand
 * and run via QProcess::start(program, args) — NO shell is involved, so there is
 * no shell injection. Shell features (redirection, pipes) require the user to
 * wrap the command explicitly in `sh -c "..."` / `cmd /c "..."`. The adapter
 * trusts the configured command itself (stored in Protected CustomData).
 *
 * Cloud-agnostic: rclone (Dropbox/GDrive/OneDrive/S3), BaiduPCS-Go (Baidu Pan),
 * scp (SFTP), curl (WebDAV) are all driven purely by the configured commands.
 *
 * Thread-safety: not guaranteed. Single-threaded use (CLI). cancel() may be
 * called from another thread to kill an in-flight transfer (used by timeout).
 */
class CommandDelegatingAdapter : public IRemoteStorageAdapter
{
public:
    explicit CommandDelegatingAdapter(RemoteStorageConfig config);
    ~CommandDelegatingAdapter() override; // out-of-line (QProcess is incomplete in header)

    bool testConnection(QString* error = nullptr) override;
    bool fetch(const QString& localTempPath, QString* error = nullptr) override;
    bool upload(const QString& localPath, QString* error = nullptr) override;
    void cancel() override;

private:
    /**
     * Run a command template (with {TEMP_DATABASE}/{FILE} → destPath), write
     * commandInput to stdin, wait up to timeoutMsec, kill on timeout.
     * @param requireNonEmptyDest  if true, success also requires destPath to be
     *                             a non-empty file (download/test; review 🟡#4).
     */
    bool runCommand(const QString& commandTemplate,
                    const QString& commandInput,
                    int timeoutMsec,
                    const QString& destPath,
                    bool requireNonEmptyDest,
                    QString* error);

    RemoteStorageConfig m_config;
    std::unique_ptr<QProcess> m_process; // member so cancel() can kill it
};

#endif // KEEPASSXC_REMOTESTORAGEADAPTER_H
