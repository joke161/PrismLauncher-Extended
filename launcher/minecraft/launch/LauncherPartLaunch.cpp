// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2022 Sefa Eyeoglu <contact@scrumplex.net>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *
 *      Copyright 2013-2021 MultiMC Contributors
 *
 *      Licensed under the Apache License, Version 2.0 (the "License");
 *      you may not use this file except in compliance with the License.
 *      You may obtain a copy of the License at
 *
 *          http://www.apache.org/licenses/LICENSE-2.0
 *
 *      Unless required by applicable law or agreed to in writing, software
 *      distributed under the License is distributed on an "AS IS" BASIS,
 *      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *      See the License for the specific language governing permissions and
 *      limitations under the License.
 */

#include "LauncherPartLaunch.h"

#include <QRegularExpression>
#include <QStandardPaths>

#include "Application.h"
#include "FileSystem.h"
#include "launch/LaunchTask.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/auth/AccountList.h"
#include "minecraft/skins/LocalSkinServer.h"
#include "p2p/P2PManager.h"

#if defined(Q_OS_LINUX) && defined(ENABLE_GAMEMODE)
#include "gamemode_client.h"
#endif

LauncherPartLaunch::LauncherPartLaunch(LaunchTask* parent)
    : LaunchStep(parent)
    , m_process(parent->instance()->getJavaVersion().defaultsToUtf8() ? QStringConverter::Utf8 : QStringConverter::System)
{
    if (parent->instance()->settings()->get("CloseAfterLaunch").toBool()) {
        static const QRegularExpression s_settingUser(".*Setting user.+", QRegularExpression::CaseInsensitiveOption);
        std::shared_ptr<QMetaObject::Connection> connection{ new QMetaObject::Connection };
        *connection =
            connect(&m_process, &LoggedProcess::log, this, [connection](const QStringList& lines, [[maybe_unused]] MessageLevel level) {
                qDebug() << lines;
                if (lines.filter(s_settingUser).length() != 0) {
                    APPLICATION->closeAllWindows();
                    disconnect(*connection);
                }
            });
    }

    connect(&m_process, &LoggedProcess::log, this, &LauncherPartLaunch::logLines);
    connect(&m_process, &LoggedProcess::stateChanged, this, &LauncherPartLaunch::on_state);

    connect(&m_process, &LoggedProcess::log, this, [](const QStringList& lines, [[maybe_unused]] MessageLevel level) {
        static const QRegularExpression s_lanPortRegex(
            QStringLiteral(R"((?:Local game hosted on port|Started serving on|hosted on port|serving on port|Started LAN server on port|порт.*)\s*[:#]?\s*([0-9]{3,5}))"),
            QRegularExpression::CaseInsensitiveOption);
        for (const QString& line : lines) {
            auto match = s_lanPortRegex.match(line);
            if (match.hasMatch()) {
                bool ok = false;
                quint16 port = match.captured(1).toUShort(&ok);
                if (ok && port > 0) {
                    if (P2PManager::instance()) {
                        P2PManager::instance()->setLocalMinecraftPort(port);
                    }
                }
            }
        }
    });
}

void LauncherPartLaunch::executeTask()
{
    QString jarPath = APPLICATION->getJarPath("NewLaunch.jar");
    if (jarPath.isEmpty()) {
        const char* reason = QT_TR_NOOP("Launcher library could not be found. Please check your installation.");
        emit logLine(tr(reason), MessageLevel::Fatal);
        emitFailed(tr(reason));
        return;
    }

    auto* instance = m_parent->instance();

    QString legacyJarPath;
    if (instance->getLauncher() == "legacy" || instance->shouldApplyOnlineFixes()) {
        legacyJarPath = APPLICATION->getJarPath("NewLaunchLegacy.jar");
        if (legacyJarPath.isEmpty()) {
            const char* reason = QT_TR_NOOP("Legacy launcher library could not be found. Please check your installation.");
            emit logLine(tr(reason), MessageLevel::Fatal);
            emitFailed(tr(reason));
            return;
        }
    }

    m_launchScript = instance->createLaunchScript(m_session, m_targetToJoin);
    QStringList args = instance->javaArguments();

    bool isP2PActive = (P2PManager::instance() &&
        (P2PManager::instance()->isConnected() ||
         P2PManager::instance()->isHosting() ||
         P2PManager::instance()->isGuest()));

    if (m_session && (m_session->user_type == "offline" || m_session->launchMode == LaunchMode::Offline || isP2PActive)) {
        QString activeSkinPath = m_session->skinPath;
        QString activeModelType = m_session->modelType.isEmpty() ? "steve" : m_session->modelType;

        if (activeSkinPath.isEmpty() && APPLICATION->accounts()) {
            auto* accounts = APPLICATION->accounts();
            for (int i = 0; i < accounts->count(); ++i) {
                auto acc = accounts->at(i);
                if (!acc) continue;
                if (acc->profileName().compare(m_session->player_name, Qt::CaseInsensitive) == 0 ||
                    acc->profileId().remove('-').compare(m_session->uuid.remove('-'), Qt::CaseInsensitive) == 0) {
                    activeSkinPath = acc->skinPath();
                    activeModelType = acc->modelType();
                    break;
                }
            }
        }

        auto* skinServer = LocalSkinServer::instance();
        if (skinServer && skinServer->ensureStarted()) {
            if (!activeSkinPath.isEmpty() && QFile::exists(activeSkinPath)) {
                skinServer->registerSkin(m_session->uuid, activeSkinPath, activeModelType);
                skinServer->registerSkin(m_session->player_name, activeSkinPath, activeModelType);
            }
            args.append(skinServer->jvmArguments());
            emit logLine(QString("Local skin server active at %1 for profile '%2' (model: %3)")
                             .arg(skinServer->url(), m_session->player_name, activeModelType),
                         MessageLevel::Launcher);
        }

        if (m_session->user_type == "offline" || m_session->launchMode == LaunchMode::Offline) {
            // Ensure mandatory Minecraft command line parameters for offline account:
            // --username <имя_оффлайн_аккаунта>
            // --uuid <стабильный_оффлайн_UUID>
            // --accessToken offline_session_valid
            if (!m_launchScript.contains("param --username")) {
                m_launchScript += QString("param --username\nparam %1\n").arg(m_session->player_name);
            }
            if (!m_launchScript.contains("param --uuid")) {
                m_launchScript += QString("param --uuid\nparam %1\n").arg(m_session->uuid);
            }
            if (!m_launchScript.contains("param --accessToken")) {
                m_launchScript += "param --accessToken\nparam offline_session_valid\n";
            } else {
                m_launchScript.replace("param --accessToken\nparam \n", "param --accessToken\nparam offline_session_valid\n");
                m_launchScript.replace("param --accessToken\nparam null\n", "param --accessToken\nparam offline_session_valid\n");
                m_launchScript.replace("param --accessToken\nparam 0\n", "param --accessToken\nparam offline_session_valid\n");
            }
        }

        // Ensure proper mainClass in launch script (e.g. Fabric KnotClient)
        QString properMainClass = instance->getMainClass();
        if (!properMainClass.isEmpty()) {
            if (!m_launchScript.contains("mainClass ")) {
                m_launchScript.prepend("mainClass " + properMainClass + "\n");
            } else if (properMainClass.contains("knot") && m_launchScript.contains("mainClass net.minecraft.client.Minecraft")) {
                m_launchScript.replace("mainClass net.minecraft.client.Minecraft", "mainClass " + properMainClass);
            }
        }
    }

    QString allArgs = args.join(" ");
    emit logLine("Java arguments:\n  " + m_parent->censorPrivateInfo(allArgs) + "\n", MessageLevel::Launcher);

    auto javaPath = FS::ResolveExecutable(instance->settings()->get("JavaPath").toString());

    m_process.setProcessEnvironment(instance->createLaunchEnvironment());

    // make detachable - this will keep the process running even if the object is destroyed
    m_process.setDetachable(true);

    auto classPath = instance->getClassPath();
    classPath.prepend(jarPath);

    if (!legacyJarPath.isEmpty()) {
        classPath.prepend(legacyJarPath);
    }

    auto natPath = instance->getNativePath();
#ifdef Q_OS_WIN
    natPath = FS::getPathNameInLocal8bit(natPath);
#endif
    args << "-Djava.library.path=" + natPath;

    args << "-cp";
#ifdef Q_OS_WIN
    QStringList processed;
    for (auto& item : classPath) {
        processed << FS::getPathNameInLocal8bit(item);
    }
    args << processed.join(';');
#else
    args << classPath.join(':');
#endif
    args << "org.prismlauncher.EntryPoint";

    qDebug() << args.join(' ');

    QString wrapperCommandStr = instance->getWrapperCommand().trimmed();
    if (!wrapperCommandStr.isEmpty()) {
        auto wrapperArgs = m_parent->substituteVariables(wrapperCommandStr);
        auto wrapperCommand = wrapperArgs.takeFirst();
        auto realWrapperCommand = QStandardPaths::findExecutable(wrapperCommand);
        if (realWrapperCommand.isEmpty()) {
            const char* reason = QT_TR_NOOP("The wrapper command \"%1\" couldn't be found.");
            emit logLine(QString(reason).arg(wrapperCommand), MessageLevel::Fatal);
            emitFailed(tr(reason).arg(wrapperCommand));
            return;
        }
        emit logLine("Wrapper command is:\n" + wrapperCommandStr + "\n\n", MessageLevel::Launcher);
        args.prepend(javaPath);
        m_process.start(wrapperCommand, wrapperArgs + args);
    } else {
        m_process.start(javaPath, args);
    }

#if defined(Q_OS_LINUX) && defined(ENABLE_GAMEMODE)
    if (instance->settings()->get("EnableFeralGamemode").toBool() && APPLICATION->capabilities() & Application::SupportsGameMode) {
        auto pid = m_process.processId();
        if (pid != 0) {
            gamemode_request_start_for(static_cast<pid_t>(pid));
        }
    }
#endif
}

void LauncherPartLaunch::on_state(LoggedProcess::State state)
{
    switch (state) {
        case LoggedProcess::FailedToStart: {
            //: Error message displayed if instace can't start
            const char* reason = QT_TR_NOOP("Could not launch Minecraft: %1");
            emit logLine(QString(reason).arg(m_process.errorString()), MessageLevel::Fatal);
            emitFailed(tr(reason).arg(m_process.errorString()));
            return;
        }
        case LoggedProcess::Aborted:
        case LoggedProcess::Crashed: {
            m_parent->setPid(-1);
            m_parent->instance()->setMinecraftRunning(false);
            emitFailed(tr("Game crashed."));
            return;
        }
        case LoggedProcess::Finished: {
            auto* instance = m_parent->instance();
            if (instance->settings()->get("CloseAfterLaunch").toBool()) {
                APPLICATION->showMainWindow();
            }

            m_parent->setPid(-1);
            m_parent->instance()->setMinecraftRunning(false);
            // if the exit code wasn't 0, report this as a crash
            auto exitCode = m_process.exitCode();
            if (exitCode != 0) {
                emitFailed(tr("Game crashed."));
                return;
            }
            // FIXME: make this work again
            //  m_postlaunchprocess.processEnvironment().insert("INST_EXITCODE", QString(exitCode));
            //  run post-exit
            emitSucceeded();
            break;
        }
        case LoggedProcess::Running:
            emit logLine(QString("Minecraft process ID: %1\n\n").arg(m_process.processId()), MessageLevel::Launcher);
            m_parent->setPid(m_process.processId());
            // send the launch script to the launcher part
            m_process.write(m_launchScript.toUtf8());

            mayProceed = true;
            emit readyForLaunch();
            break;
        default:
            break;
    }
}

void LauncherPartLaunch::setWorkingDirectory(const QString& wd)
{
    m_process.setWorkingDirectory(wd);
}

void LauncherPartLaunch::proceed()
{
    if (mayProceed) {
        m_parent->instance()->setMinecraftRunning(true);
        QString launchString("launch\n");
        m_process.write(launchString.toUtf8());
        mayProceed = false;
    }
}

bool LauncherPartLaunch::abort()
{
    if (mayProceed) {
        mayProceed = false;
        QString launchString("abort\n");
        m_process.write(launchString.toUtf8());
    } else {
        auto state = m_process.state();
        if (state == LoggedProcess::Running || state == LoggedProcess::Starting) {
            m_process.kill();
        }
    }
    return true;
}
