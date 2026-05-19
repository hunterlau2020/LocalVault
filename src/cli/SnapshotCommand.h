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

#ifndef KEEPASSXC_SNAPSHOTCOMMAND_H
#define KEEPASSXC_SNAPSHOTCOMMAND_H

#include "Command.h"

class SnapshotCommand : public Command
{
public:
    SnapshotCommand();

    int execute(const QStringList& arguments) override;

    static const QCommandLineOption CreateOption;
    static const QCommandLineOption ListOption;
    static const QCommandLineOption DeleteOption;
    static const QCommandLineOption RestoreOption;
    static const QCommandLineOption ProtectOption;
    static const QCommandLineOption UnprotectOption;
};

#endif // KEEPASSXC_SNAPSHOTCOMMAND_H
