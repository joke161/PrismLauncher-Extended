// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Octol1ttle <l1ttleofficial@outlook.com>
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
 */

#include "EnsureOfflineLibraries.h"

#include <QDir>
#include "FileSystem.h"
#include "minecraft/PackProfile.h"
#include "minecraft/ProfileUtils.h"
#include "minecraft/VersionFile.h"

EnsureOfflineLibraries::EnsureOfflineLibraries(LaunchTask* parent, MinecraftInstance* instance) : LaunchStep(parent), m_instance(instance)
{}

void EnsureOfflineLibraries::executeTask()
{
    const auto profile = m_instance->getPackProfile()->getProfile();
    QStringList allJars;
    if (profile) {
        profile->getLibraryFiles(m_instance->runtimeContext(), allJars, allJars, m_instance->getLocalLibraryPath(), m_instance->binRoot(),
                                 false);
    }

    // Also include libraries from local patches in patches/
    const QString patchDir = FS::PathCombine(m_instance->instanceRoot(), "patches");
    QDir pDir(patchDir);
    if (pDir.exists()) {
        const auto patchFiles = pDir.entryInfoList({ "*.json" }, QDir::Files);
        for (const auto& fi : patchFiles) {
            auto vfile = ProfileUtils::parseJsonFile(fi, false);
            if (!vfile) continue;
            QStringList dummyNative, dummy32, dummy64;
            for (auto lib : vfile->libraries) {
                if (!lib) continue;
                lib->getApplicableFiles(m_instance->runtimeContext(), allJars, dummyNative, dummy32, dummy64, m_instance->getLocalLibraryPath());
            }
            if (vfile->mainJar) {
                vfile->mainJar->getApplicableFiles(m_instance->runtimeContext(), allJars, dummyNative, dummy32, dummy64, m_instance->getLocalLibraryPath());
            }
        }
    }
    allJars.removeDuplicates();

    QStringList missing;
    bool hasMinecraftJar = false;
    bool hasFabricJar = false;
    bool fabricRequired = (m_instance->getPackProfile()->getComponent("net.fabricmc.fabric-loader") != nullptr ||
                           m_instance->getPackProfile()->getComponent("net.fabricmc") != nullptr ||
                           QFile::exists(FS::PathCombine(patchDir, "net.fabricmc.json")) ||
                           QFile::exists(FS::PathCombine(patchDir, "net.fabricmc.fabric-loader.json")));

    for (const auto& jar : allJars) {
        bool exists = QFile::exists(jar);
        if (!exists) {
            QString fileName = QFileInfo(jar).fileName();
            if (QFile::exists(FS::PathCombine(m_instance->getLocalLibraryPath(), fileName)) ||
                QFile::exists(FS::PathCombine("libraries", fileName))) {
                exists = true;
            }
        }

        if (jar.contains("minecraft", Qt::CaseInsensitive) && jar.endsWith(".jar")) {
            if (exists) hasMinecraftJar = true;
        }
        if (jar.contains("fabric-loader", Qt::CaseInsensitive) && jar.endsWith(".jar")) {
            if (exists) hasFabricJar = true;
        }

        if (!exists) {
            missing.append(jar);
        }
    }

    if (!hasMinecraftJar) {
        QString mcVersion = m_instance->getPackProfile()->getComponentVersion("net.minecraft");
        if (QFile::exists(FS::PathCombine(m_instance->binRoot(), "minecraft.jar")) ||
            QFile::exists(FS::PathCombine("versions", mcVersion, mcVersion + ".jar"))) {
            hasMinecraftJar = true;
        }
    }

    if (missing.isEmpty()) {
        emitSucceeded();
        return;
    }

    // In offline mode: if the core Minecraft jar (and Fabric loader jar, if required)
    // physically exist on disk, bypass missing non-essential libraries and proceed with launch!
    if (hasMinecraftJar && (!fabricRequired || hasFabricJar)) {
        emit logLine(tr("Offline launch: Core Minecraft / Fabric libraries exist on disk. Proceeding."), MessageLevel::Launcher);
        emitSucceeded();
        return;
    }

    emit logLine("Missing libraries:", MessageLevel::Error);
    for (const auto& jar : missing) {
        emit logLine("  " + jar, MessageLevel::Error);
    }
    emit logLine(tr("\nThis instance cannot be launched because some libraries are missing or have not been downloaded yet. Please "
                    "try again in online mode with a working Internet connection"),
                 MessageLevel::Fatal);
    emitFailed("Required libraries are missing");
}
