// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

enum class AuditSeverity {
    Info,
    Notice,
    Warning,
    Critical
};

enum class AuditVerdict {
    Clean,
    Notice,
    Suspicious,
    Dangerous
};

struct AuditFinding {
    QString ruleId;
    QString category;
    AuditSeverity severity;
    QString title;
    QString description;
    QString evidence;
    QString className;
    int score = 0;
};

struct AuditReport {
    QString jarPath;
    QString modName;
    QString modId;
    QString version;
    int classCount = 0;
    int totalClasses = 0;
    int totalFiles = 0;
    qint64 scanTimeMs = 0;
    int riskScore = 0;
    AuditVerdict verdict = AuditVerdict::Clean;
    QList<AuditFinding> findings;
    QStringList nativeLibraries;
};

class ModAuditor {
public:
    static AuditReport auditJar(const QString& jarPath, bool forceScan = false);
    static bool hasCachedReport(const QString& jarPath);
    static AuditReport getCachedReport(const QString& jarPath);
    static void setCachedReport(const QString& jarPath, const AuditReport& report);
    static void clearCache();
    static QString getAuditCacheFilePath();

    static QString verdictToString(AuditVerdict verdict);
    static QString verdictToColor(AuditVerdict verdict);
    static QString verdictToIconName(AuditVerdict verdict);
    static QString severityToString(AuditSeverity severity);

    static bool isModTrusted(const QString& jarPath);
    static void setModTrusted(const QString& jarPath, bool trusted);
    static QStringList getTrustedModsList();
    static void clearTrustedModsCache();

private:
    struct ClassFacts {
        QString className;
        QStringList methodRefs;
        QStringList stringConstants;
        QStringList classRefs;
    };

    static bool parseClassConstantPool(const QByteArray& data, ClassFacts& facts);
    static void scanFacts(const ClassFacts& facts, QList<AuditFinding>& findings);
};

