// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

class BaseInstance;

enum class CrashSeverity {
    Info,
    Warning,
    Error,
    Fatal
};

enum class CrashActionType {
    None,
    DownloadMissingMod,
    DisableCulpritMod,
    DisableBothMods,
    ResetCorruptedConfig,
    DisableShaders,
    ResetResourcePacks,
    EnableForgeRemoveErroringEntities,
    IncreaseAllocatedMemory
};

struct CrashAction {
    CrashActionType type = CrashActionType::None;
    QString target;
    QString label;
    QString explanation;
};

struct CrashIssue {
    QString id;
    QString title;
    CrashSeverity severity = CrashSeverity::Error;
    QString summary;
    QString details;
    QString recommendation;
    QStringList affectedFiles;
    QStringList rawEvidence;

    QList<CrashAction> actions;

    // Backwards compatibility fields
    CrashActionType actionType = CrashActionType::None;
    QString actionTarget;
    QString actionLabel;
};

class CrashAnalyzer {
public:
    static QList<CrashIssue> analyze(const QString& logContent, const QString& instanceRoot = QString(), BaseInstance* instance = nullptr);
    static QString readLatestCrashReport(const QString& instanceRoot, BaseInstance* instance = nullptr);
    static QString severityToString(CrashSeverity severity);
    static QString severityToIconName(CrashSeverity severity);

    static QString resolveModJar(const QString& instanceRoot, const QString& hint, BaseInstance* instance = nullptr);
    static QString findModJarForMixin(const QString& instanceRoot, const QString& mixinConfig, const QString& modId = QString(), BaseInstance* instance = nullptr);

private:
    static void checkMixinErrors(const QString& fullText, const QString& instanceRoot, QList<CrashIssue>& issues, BaseInstance* instance = nullptr);
    static void checkMissingDependencies(const QString& fullText, QList<CrashIssue>& issues);
    static void checkWrongLoader(const QString& fullText, QList<CrashIssue>& issues);
    static void checkMemoryIssues(const QString& fullText, QList<CrashIssue>& issues);
    static void checkJavaVersion(const QString& fullText, QList<CrashIssue>& issues);
    static void checkJvmArguments(const QString& fullText, QList<CrashIssue>& issues);
    static void checkCorruptedFiles(const QString& fullText, const QString& instanceRoot, QList<CrashIssue>& issues);
    static void checkDuplicateMods(const QString& fullText, QList<CrashIssue>& issues);
    static void checkGraphicsIssues(const QString& fullText, QList<CrashIssue>& issues);
    static void checkShaderAndResourcePack(const QString& fullText, const QString& instanceRoot, QList<CrashIssue>& issues);
    static void checkTickingEntity(const QString& fullText, const QString& instanceRoot, QList<CrashIssue>& issues, BaseInstance* instance = nullptr);
    static void checkGenericExceptions(const QString& fullText, const QString& instanceRoot, QList<CrashIssue>& issues, BaseInstance* instance = nullptr);
};
