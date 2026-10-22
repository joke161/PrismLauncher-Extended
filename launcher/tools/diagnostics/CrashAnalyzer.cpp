// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#include "CrashAnalyzer.h"
#include "DiagI18n.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringConverter>

#include "BaseInstance.h"
#include "archive/ArchiveReader.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/mod/Mod.h"
#include "minecraft/mod/ModFolderModel.h"

namespace {
bool isStandardIgnoredClass(const QString& className)
{
    static const QStringList ignoredPrefixes = {
        "java.", "javax.", "jdk.", "sun.", "com.sun.",
        "net.minecraft.", "com.mojang.",
        "org.spongepowered.asm.", "org.spongepowered.tools.",
        "org.objectweb.asm.", "net.fabricmc.loader.", "cpw.mods.modlauncher.",
        "net.minecraftforge.fml.", "net.neoforged.fml.",
        "org.lwjgl.", "io.netty.", "org.apache.", "com.google.gson.",
        "com.google.common.", "it.unimi.dsi.fastutil.", "kotlin.", "scala."
    };
    for (const auto& prefix : ignoredPrefixes) {
        if (className.startsWith(prefix, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}
}  // namespace

QString CrashAnalyzer::severityToString(CrashSeverity severity)
{
    switch (severity) {
        case CrashSeverity::Fatal:
            return DiagI18n::tr("Fatal Error", "Критическая ошибка");
        case CrashSeverity::Error:
            return DiagI18n::tr("Error", "Ошибка");
        case CrashSeverity::Warning:
            return DiagI18n::tr("Warning", "Предупреждение");
        case CrashSeverity::Info:
        default:
            return DiagI18n::tr("Information", "Информация");
    }
}

QString CrashAnalyzer::severityToIconName(CrashSeverity severity)
{
    switch (severity) {
        case CrashSeverity::Fatal:
        case CrashSeverity::Error:
            return "status-bad";
        case CrashSeverity::Warning:
            return "status-yellow";
        case CrashSeverity::Info:
        default:
            return "status-good";
    }
}

QString CrashAnalyzer::readLatestCrashReport(const QString& instanceRoot, BaseInstance* instance)
{
    QString reportsPath;
    if (instance) {
        reportsPath = instance->gameRoot() + "/crash-reports";
    }
    if (reportsPath.isEmpty() || !QDir(reportsPath).exists()) {
        if (!instanceRoot.isEmpty()) {
            if (QDir(instanceRoot + "/minecraft/crash-reports").exists()) {
                reportsPath = instanceRoot + "/minecraft/crash-reports";
            } else if (QDir(instanceRoot + "/.minecraft/crash-reports").exists()) {
                reportsPath = instanceRoot + "/.minecraft/crash-reports";
            } else if (QDir(instanceRoot + "/crash-reports").exists()) {
                reportsPath = instanceRoot + "/crash-reports";
            }
        }
    }

    if (reportsPath.isEmpty() || !QDir(reportsPath).exists()) {
        return QString();
    }

    QDir crashDir(reportsPath);
    QFileInfoList list = crashDir.entryInfoList(QStringList() << "crash-*.txt", QDir::Files, QDir::Time);
    if (list.isEmpty()) {
        return QString();
    }

    const QFileInfo& newest = list.first();
    // Allow crash reports up to 7 days old
    if (newest.lastModified().secsTo(QDateTime::currentDateTime()) > 604800) {
        return QString();
    }

    QFile file(newest.absoluteFilePath());
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString::fromUtf8(file.read(1024 * 1024));
    }

    return QString();
}

QString CrashAnalyzer::resolveModJar(const QString& instanceRoot, const QString& hint, BaseInstance* instance)
{
    QString query = hint.trimmed();
    if (query.isEmpty()) {
        return QString();
    }

    // Strip suffixes like .mixins.json or .json to get clean base query
    QString baseQuery = query;
    if (baseQuery.endsWith(".json", Qt::CaseInsensitive)) {
        static const QRegularExpression mixinSuffixRe(R"([\.\-_]mixins?(?:\.[a-zA-Z0-9_\-]+)?\.json$|\.json$)", QRegularExpression::CaseInsensitiveOption);
        baseQuery.remove(mixinSuffixRe);
    }

    // 1. If BaseInstance is provided, check its ModFolderModel
    if (instance) {
        auto* mc = dynamic_cast<MinecraftInstance*>(instance);
        if (mc && mc->loaderModList()) {
            auto allMods = mc->loaderModList()->allMods();
            // A. Exact modId match
            for (auto* mod : allMods) {
                if (mod->modId().compare(query, Qt::CaseInsensitive) == 0 ||
                    mod->modId().compare(baseQuery, Qt::CaseInsensitive) == 0) {
                    return mod->fileinfo().fileName();
                }
            }
            // B. Exact filename match
            for (auto* mod : allMods) {
                QString fn = mod->fileinfo().fileName();
                if (fn.compare(query, Qt::CaseInsensitive) == 0 ||
                    fn.compare(baseQuery, Qt::CaseInsensitive) == 0 ||
                    fn.compare(query + ".jar", Qt::CaseInsensitive) == 0 ||
                    fn.compare(baseQuery + ".jar", Qt::CaseInsensitive) == 0) {
                    return fn;
                }
            }
            // C. Mod display name match
            for (auto* mod : allMods) {
                if (!mod->name().isEmpty() &&
                    (mod->name().compare(query, Qt::CaseInsensitive) == 0 ||
                     mod->name().compare(baseQuery, Qt::CaseInsensitive) == 0)) {
                    return mod->fileinfo().fileName();
                }
            }
            // D. Filename starts with query prefix
            for (auto* mod : allMods) {
                QString fn = mod->fileinfo().fileName();
                if (fn.startsWith(baseQuery + "-", Qt::CaseInsensitive) ||
                    fn.startsWith(baseQuery + "_", Qt::CaseInsensitive) ||
                    fn.startsWith(baseQuery + ".", Qt::CaseInsensitive) ||
                    fn.startsWith(query + "-", Qt::CaseInsensitive) ||
                    fn.startsWith(query + "_", Qt::CaseInsensitive)) {
                    return fn;
                }
            }
        }
    }

    // 2. Scan mods folder on disk
    QString modsPath;
    if (instance) {
        modsPath = instance->modsRoot();
    }
    if (modsPath.isEmpty() || !QDir(modsPath).exists()) {
        if (!instanceRoot.isEmpty()) {
            if (QDir(instanceRoot + "/minecraft/mods").exists()) {
                modsPath = instanceRoot + "/minecraft/mods";
            } else if (QDir(instanceRoot + "/.minecraft/mods").exists()) {
                modsPath = instanceRoot + "/.minecraft/mods";
            } else if (QDir(instanceRoot + "/mods").exists()) {
                modsPath = instanceRoot + "/mods";
            }
        }
    }

    if (modsPath.isEmpty() || !QDir(modsPath).exists()) {
        return QString();
    }

    QDir modsDir(modsPath);
    QFileInfoList jarFiles = modsDir.entryInfoList(QStringList() << "*.jar" << "*.jar.disabled", QDir::Files);
    if (jarFiles.isEmpty()) {
        return QString();
    }

    // A. Direct filename match or prefix match
    for (const auto& fi : jarFiles) {
        QString fn = fi.fileName();
        if (fn.compare(query, Qt::CaseInsensitive) == 0 ||
            fn.compare(baseQuery, Qt::CaseInsensitive) == 0 ||
            fn.compare(query + ".jar", Qt::CaseInsensitive) == 0 ||
            fn.compare(baseQuery + ".jar", Qt::CaseInsensitive) == 0 ||
            fn.compare(query + ".jar.disabled", Qt::CaseInsensitive) == 0 ||
            fn.compare(baseQuery + ".jar.disabled", Qt::CaseInsensitive) == 0 ||
            fn.startsWith(baseQuery + "-", Qt::CaseInsensitive) ||
            fn.startsWith(baseQuery + "_", Qt::CaseInsensitive) ||
            fn.startsWith(baseQuery + ".", Qt::CaseInsensitive) ||
            fn.startsWith(query + "-", Qt::CaseInsensitive) ||
            fn.startsWith(query + "_", Qt::CaseInsensitive)) {
            return fn;
        }
    }

    // B. Search inside archives for mixin config, metadata, or class packages
    QString classPathQuery = query;
    if (classPathQuery.endsWith(".class")) {
        classPathQuery.chop(6);
    }
    classPathQuery.replace('.', '/');

    for (const auto& fi : jarFiles) {
        try {
            MMCZip::ArchiveReader reader(fi.absoluteFilePath());
            if (reader.collectFiles().has_value()) {
                QStringList files = reader.getFiles();

                // Exact match (e.g. mixin json config)
                if (files.contains(query, Qt::CaseInsensitive) ||
                    files.contains(baseQuery + ".mixins.json", Qt::CaseInsensitive)) {
                    return fi.fileName();
                }

                // Check class / package path
                if (!classPathQuery.isEmpty() && classPathQuery.contains('/')) {
                    for (const auto& innerFile : files) {
                        if (innerFile.startsWith(classPathQuery, Qt::CaseInsensitive)) {
                            return fi.fileName();
                        }
                    }
                }

                // Check fabric.mod.json
                if (files.contains("fabric.mod.json")) {
                    auto res = reader.readFile("fabric.mod.json");
                    if (res.has_value()) {
                        QJsonDocument doc = QJsonDocument::fromJson(res.value());
                        if (doc.isObject()) {
                            QJsonObject obj = doc.object();
                            if (obj["id"].toString().compare(query, Qt::CaseInsensitive) == 0 ||
                                obj["id"].toString().compare(baseQuery, Qt::CaseInsensitive) == 0 ||
                                obj["name"].toString().compare(query, Qt::CaseInsensitive) == 0 ||
                                obj["name"].toString().compare(baseQuery, Qt::CaseInsensitive) == 0) {
                                return fi.fileName();
                            }
                            QJsonArray mixinsArr = obj["mixins"].toArray();
                            for (const auto& mVal : mixinsArr) {
                                QString cfg = mVal.isString() ? mVal.toString() : mVal.toObject()["config"].toString();
                                if (cfg.compare(query, Qt::CaseInsensitive) == 0 ||
                                    cfg.compare(baseQuery + ".mixins.json", Qt::CaseInsensitive) == 0) {
                                    return fi.fileName();
                                }
                            }
                        }
                    }
                }

                // Check META-INF/mods.toml or neoforge.mods.toml
                static const QStringList forgeModFiles = { "META-INF/mods.toml", "META-INF/neoforge.mods.toml" };
                for (const auto& tomlPath : forgeModFiles) {
                    if (files.contains(tomlPath)) {
                        auto res = reader.readFile(tomlPath);
                        if (res.has_value()) {
                            QString tomlText = QString::fromUtf8(res.value());
                            static const QRegularExpression modIdRe(R"(modId\s*=\s*["']([^"']+)["'])", QRegularExpression::CaseInsensitiveOption);
                            auto match = modIdRe.match(tomlText);
                            if (match.hasMatch() &&
                                (match.captured(1).compare(query, Qt::CaseInsensitive) == 0 ||
                                 match.captured(1).compare(baseQuery, Qt::CaseInsensitive) == 0)) {
                                return fi.fileName();
                            }
                        }
                    }
                }

                // Check quilt.mod.json
                if (files.contains("quilt.mod.json")) {
                    auto res = reader.readFile("quilt.mod.json");
                    if (res.has_value()) {
                        QJsonDocument doc = QJsonDocument::fromJson(res.value());
                        if (doc.isObject()) {
                            QJsonObject obj = doc.object()["quilt_loader"].toObject();
                            if (obj["id"].toString().compare(query, Qt::CaseInsensitive) == 0 ||
                                obj["id"].toString().compare(baseQuery, Qt::CaseInsensitive) == 0) {
                                return fi.fileName();
                            }
                        }
                    }
                }
            }
        } catch (...) {
            // Skip unreadable archives
        }
    }

    // C. Substring match fallback (e.g. "combatant" in "combatant-fabric-mc1.20-1.0.jar")
    if (baseQuery.length() >= 4) {
        for (const auto& fi : jarFiles) {
            QString fn = fi.fileName();
            if (fn.contains(baseQuery, Qt::CaseInsensitive)) {
                return fn;
            }
        }
    }

    // C. Sub-identifier fallback
    if (query.contains(':')) {
        QString prefix = query.section(':', 0, 0).trimmed();
        if (!prefix.isEmpty() && prefix != query) {
            QString resolved = resolveModJar(instanceRoot, prefix, instance);
            if (!resolved.isEmpty()) return resolved;
        }
    } else if (query.contains('.')) {
        QString prefix = query.section('.', 0, 0).trimmed();
        if (prefix.length() >= 3 && prefix != query) {
            QString resolved = resolveModJar(instanceRoot, prefix, instance);
            if (!resolved.isEmpty()) return resolved;
        }
    }

    return QString();
}

QString CrashAnalyzer::findModJarForMixin(const QString& instanceRoot, const QString& mixinConfig, const QString& modId, BaseInstance* instance)
{
    if (!modId.isEmpty()) {
        QString r = resolveModJar(instanceRoot, modId, instance);
        if (!r.isEmpty()) return r;
    }
    if (!mixinConfig.isEmpty()) {
        QString r = resolveModJar(instanceRoot, mixinConfig, instance);
        if (!r.isEmpty()) return r;
    }
    return !modId.isEmpty() ? modId : QString();
}

QList<CrashIssue> CrashAnalyzer::analyze(const QString& logContent, const QString& instanceRoot, BaseInstance* instance)
{
    QList<CrashIssue> issues;

    QString fullText = logContent;
    QString crashReport = readLatestCrashReport(instanceRoot, instance);
    if (!crashReport.isEmpty()) {
        fullText += "\n--- CRASH REPORT ---\n" + crashReport;
    }

    if (fullText.trimmed().isEmpty()) {
        return issues;
    }

    // Run checkers in priority order
    checkMissingDependencies(fullText, issues);
    checkMixinErrors(fullText, instanceRoot, issues, instance);
    checkTickingEntity(fullText, instanceRoot, issues, instance);
    checkCorruptedFiles(fullText, instanceRoot, issues);
    checkShaderAndResourcePack(fullText, instanceRoot, issues);
    checkMemoryIssues(fullText, issues);
    checkWrongLoader(fullText, issues);
    checkJavaVersion(fullText, issues);
    checkJvmArguments(fullText, issues);
    checkDuplicateMods(fullText, issues);
    checkGraphicsIssues(fullText, issues);
    checkGenericExceptions(fullText, instanceRoot, issues, instance);

    return issues;
}

void CrashAnalyzer::checkMixinErrors(const QString& fullText, const QString& instanceRoot, QList<CrashIssue>& issues, BaseInstance* instance)
{
    static const QRegularExpression mixinRe(
        R"((?:Mixin \[([a-zA-Z0-9_\.\-]+\.json):?([a-zA-Z0-9_\.\-\$]*)(?:\s+from\s+mod\s+([a-zA-Z0-9_\.\-]+))?\][^\n]*FAILED during APPLY|Critical injection failure:\s+([^\n]+)|Mixin apply failed\s+([^\n]+)|in config \[([a-zA-Z0-9_\.\-]+\.json)\] FAILED|org\.spongepowered\.asm\.mixin\.transformer\.throwables\.MixinTransformerError|org\.spongepowered\.asm\.mixin\.throwables\.MixinApplyError|InjectionError:[^\n]*|([a-zA-Z0-9_\.\-]+\.mixins?\.json)[^\n]*(?:FAILED|Error|failure)))",
        QRegularExpression::CaseInsensitiveOption
    );

    auto match = mixinRe.match(fullText);
    if (!match.hasMatch()) {
        return;
    }

    // Extract error block around the match (NOT searching full log from beginning!)
    int matchStart = match.capturedStart();
    int blockStart = qMax(0, matchStart - 5000);
    int blockEnd = qMin((int)fullText.length(), (int)match.capturedEnd() + 8000);
    QString errorBlock = fullText.mid(blockStart, blockEnd - blockStart);

    QStringList rawConfigs;
    QStringList candidateModIds;

    // A. Mixin JSON configs: ([a-zA-Z0-9_\.\-]+?)\.mixins?\.json or in config [xxx.json]
    static const QRegularExpression cfgRe(
        R"(([a-zA-Z0-9_\.\-]+?)\.mixins?(?:\.[a-zA-Z0-9_\-]+)?\.json)",
        QRegularExpression::CaseInsensitiveOption
    );
    auto itCfg = cfgRe.globalMatch(errorBlock);
    while (itCfg.hasNext()) {
        auto m = itCfg.next();
        QString fullCfg = m.captured(0);
        QString baseId = m.captured(1);
        static const QRegularExpression suffixStripRe(R"([\.\-_](?:common|core|fabric|forge|neoforge|client|server)$)", QRegularExpression::CaseInsensitiveOption);
        baseId.remove(suffixStripRe);

        if (!rawConfigs.contains(fullCfg, Qt::CaseInsensitive)) rawConfigs.append(fullCfg);
        if (!candidateModIds.contains(baseId, Qt::CaseInsensitive) && !isStandardIgnoredClass(baseId)) candidateModIds.append(baseId);
    }

    // Also check "in config [xxx.json]"
    static const QRegularExpression inCfgRe(R"(in config \[([a-zA-Z0-9_\.\-]+\.json)\])", QRegularExpression::CaseInsensitiveOption);
    auto itInCfg = inCfgRe.globalMatch(errorBlock);
    while (itInCfg.hasNext()) {
        auto m = itInCfg.next();
        QString fullCfg = m.captured(1);
        if (!rawConfigs.contains(fullCfg, Qt::CaseInsensitive)) rawConfigs.append(fullCfg);
    }

    // B. "from mod [xxx]" or "from mod xxx" STRICTLY within errorBlock
    static const QRegularExpression fromModRe(R"(from mod\s+\[?([a-zA-Z0-9_\.\-]+)\]?)", QRegularExpression::CaseInsensitiveOption);
    auto itMod = fromModRe.globalMatch(errorBlock);
    while (itMod.hasNext()) {
        auto m = itMod.next();
        QString mid = m.captured(1).trimmed();
        if (!mid.isEmpty() && !candidateModIds.contains(mid, Qt::CaseInsensitive) && !isStandardIgnoredClass(mid)) {
            candidateModIds.append(mid);
        }
    }

    // C. Redirector / Inject / Callback method targets: e.g. "Redirector combatant$..." or "Redirector punchy$..."
    static const QRegularExpression redirRe(
        R"((?:Redirector|Callback|Inject|ModifyArg|ModifyVariable|Overwrite|WrapOperation)\s+([a-zA-Z0-9_\-]+)\$)",
        QRegularExpression::CaseInsensitiveOption
    );
    auto itRedir = redirRe.globalMatch(errorBlock);
    while (itRedir.hasNext()) {
        auto m = itRedir.next();
        QString pfx = m.captured(1).trimmed();
        if (!pfx.isEmpty() && !candidateModIds.contains(pfx, Qt::CaseInsensitive) && !isStandardIgnoredClass(pfx)) {
            candidateModIds.append(pfx);
        }
    }

    // D. "cancelled by [xxx]" / "conflicts with [xxx]" / "modified by [xxx]"
    static const QRegularExpression confRe(
        R"((?:cancelled by|conflicts with|incompatible with|modified by|target method was modified by|failed injection check in)\s+\[?([a-zA-Z0-9_\.\-]+)\]?)",
        QRegularExpression::CaseInsensitiveOption
    );
    auto itConf = confRe.globalMatch(errorBlock);
    while (itConf.hasNext()) {
        auto m = itConf.next();
        QString target = m.captured(1).trimmed();
        if (target.endsWith(".json", Qt::CaseInsensitive)) {
            if (!rawConfigs.contains(target, Qt::CaseInsensitive)) rawConfigs.append(target);
            QString tId = target;
            tId.remove(QRegularExpression(R"([\.\-_]mixins?(?:\.[a-zA-Z0-9_\-]+)?\.json$|\.json$)", QRegularExpression::CaseInsensitiveOption));
            if (!candidateModIds.contains(tId, Qt::CaseInsensitive) && !isStandardIgnoredClass(tId)) {
                candidateModIds.append(tId);
            }
        } else if (!target.isEmpty() && !candidateModIds.contains(target, Qt::CaseInsensitive) && !isStandardIgnoredClass(target)) {
            candidateModIds.append(target);
        }
    }

    // E. Extract target class name from the failing mixin (e.g. ScreenEffectRendererMixin -> ScreenEffectRenderer)
    QString targetClass;
    static const QRegularExpression targetClassRe(
        R"((?:in config\s+\[?[a-zA-Z0-9_\.\-]+\]?:\s*([a-zA-Z0-9_]+Mixin)|(?:\.mixins?\.json):([a-zA-Z0-9_]+Mixin)|into\s+(?:[a-zA-Z0-9_\.\$]+\.)?([a-zA-Z0-9_]+)|target\s+(?:[a-zA-Z0-9_\.\$]+\.)?([a-zA-Z0-9_]+)))",
        QRegularExpression::CaseInsensitiveOption
    );
    auto itTC = targetClassRe.match(errorBlock);
    if (itTC.hasMatch()) {
        for (int i = 1; i <= 4; ++i) {
            QString cap = itTC.captured(i).trimmed();
            if (!cap.isEmpty()) {
                targetClass = cap;
                break;
            }
        }
    }
    if (targetClass.endsWith("Mixin", Qt::CaseInsensitive) && targetClass.length() > 5) {
        targetClass.chop(5);
    } else if (targetClass.startsWith("Mixin", Qt::CaseInsensitive) && targetClass.length() > 5) {
        targetClass.remove(0, 5);
    }

    // F. Check "Mixins in Stacktrace:" section in crash reports for other mods targeting the class
    static const QRegularExpression stackMixinsHeaderRe(
        R"((?:--\s*Mixins in Stacktrace\s*--|Mixins in Stacktrace:)\s*\n((?:[ \t]+[^\n]+\n)+))",
        QRegularExpression::CaseInsensitiveOption
    );
    auto itStack = stackMixinsHeaderRe.match(fullText);
    if (itStack.hasMatch()) {
        QString stackSection = itStack.captured(1);
        static const QRegularExpression stackModRe(
            R"(\((?:[a-zA-Z0-9_\.\-]+:)?([a-zA-Z0-9_\.\-]+?)(?:\.mixins?\.json)?\)|from mod\s+([a-zA-Z0-9_\.\-]+))",
            QRegularExpression::CaseInsensitiveOption
        );
        auto itSM = stackModRe.globalMatch(stackSection);
        while (itSM.hasNext()) {
            auto m = itSM.next();
            QString mid = !m.captured(1).trimmed().isEmpty() ? m.captured(1).trimmed() : m.captured(2).trimmed();
            if (!mid.isEmpty() && !candidateModIds.contains(mid, Qt::CaseInsensitive) && !isStandardIgnoredClass(mid)) {
                candidateModIds.append(mid);
            }
        }
    }

    // G. Check "Suspected Mods:" line in crash reports
    static const QRegularExpression suspRe(
        R"(Suspected Mods?:\s*([^\n]+))",
        QRegularExpression::CaseInsensitiveOption
    );
    auto itSusp = suspRe.match(fullText);
    if (itSusp.hasMatch()) {
        QString suspLine = itSusp.captured(1);
        static const QRegularExpression suspTokenRe(R"(([a-zA-Z0-9_\-]+))");
        auto itTok = suspTokenRe.globalMatch(suspLine);
        while (itTok.hasNext()) {
            auto m = itTok.next();
            QString tok = m.captured(1).trimmed();
            if (tok.length() >= 3 && !candidateModIds.contains(tok, Qt::CaseInsensitive) && !isStandardIgnoredClass(tok)) {
                candidateModIds.append(tok);
            }
        }
    }

    // H. Search fullText for other mixin configs referencing the exact targetClass
    if (!targetClass.isEmpty()) {
        QRegularExpression searchTCRe(
            QString(R"(([a-zA-Z0-9_\.\-]+?)\.mixins?(?:\.[a-zA-Z0-9_\-]+)?\.json[^\n]{0,140}%1|%1[^\n]{0,140}([a-zA-Z0-9_\.\-]+?)\.mixins?(?:\.[a-zA-Z0-9_\-]+)?\.json)").arg(QRegularExpression::escape(targetClass)),
            QRegularExpression::CaseInsensitiveOption
        );
        auto itGlobalTC = searchTCRe.globalMatch(fullText);
        while (itGlobalTC.hasNext()) {
            auto m = itGlobalTC.next();
            QString mid1 = m.captured(1).trimmed();
            QString mid2 = m.captured(2).trimmed();
            QString mid = !mid1.isEmpty() ? mid1 : mid2;
            if (!mid.isEmpty() && !candidateModIds.contains(mid, Qt::CaseInsensitive) && !isStandardIgnoredClass(mid)) {
                candidateModIds.append(mid);
            }
        }
    }

    // Resolve candidate mod IDs and configs to physical .jar files
    QStringList resolvedJars;
    for (const auto& mid : candidateModIds) {
        QString jar = resolveModJar(instanceRoot, mid, instance);
        if (!jar.isEmpty() && !resolvedJars.contains(jar, Qt::CaseInsensitive)) {
            resolvedJars.append(jar);
        }
    }
    for (const auto& cfg : rawConfigs) {
        QString jar = resolveModJar(instanceRoot, cfg, instance);
        if (!jar.isEmpty() && !resolvedJars.contains(jar, Qt::CaseInsensitive)) {
            resolvedJars.append(jar);
        }
    }

    // I. If only 1 mod was found so far, scan installed jars to find any other mod targeting targetClass
    if (resolvedJars.size() < 2 && !targetClass.isEmpty()) {
        QString modsPath;
        if (instance) {
            modsPath = instance->modsRoot();
        }
        if (modsPath.isEmpty() || !QDir(modsPath).exists()) {
            if (!instanceRoot.isEmpty()) {
                if (QDir(instanceRoot + "/minecraft/mods").exists()) {
                    modsPath = instanceRoot + "/minecraft/mods";
                } else if (QDir(instanceRoot + "/.minecraft/mods").exists()) {
                    modsPath = instanceRoot + "/.minecraft/mods";
                } else if (QDir(instanceRoot + "/mods").exists()) {
                    modsPath = instanceRoot + "/mods";
                }
            }
        }
        if (!modsPath.isEmpty() && QDir(modsPath).exists()) {
            QDir dir(modsPath);
            QFileInfoList jarFiles = dir.entryInfoList(QStringList() << "*.jar", QDir::Files, QDir::Name);
            for (const auto& fi : jarFiles) {
                QString fn = fi.fileName();
                if (!resolvedJars.isEmpty() && resolvedJars.contains(fn, Qt::CaseInsensitive)) {
                    continue;
                }
                try {
                    MMCZip::ArchiveReader reader(fi.absoluteFilePath());
                    if (reader.collectFiles().has_value()) {
                        const auto& files = reader.getFiles();
                        bool matches = false;
                        for (const auto& innerFile : files) {
                            if (innerFile.contains(targetClass, Qt::CaseInsensitive)) {
                                matches = true;
                                break;
                            }
                        }
                        if (matches) {
                            resolvedJars.append(fn);
                            if (resolvedJars.size() >= 2) {
                                break;
                            }
                        }
                    }
                } catch (...) {}
            }
        }
    }

    CrashIssue issue;
    issue.id = "mixin_failure";
    issue.severity = CrashSeverity::Fatal;

    QString primaryName;
    QString secondaryName;

    if (resolvedJars.size() >= 2) {
        primaryName = resolvedJars[0];
        secondaryName = resolvedJars[1];
        issue.affectedFiles.append(primaryName);
        issue.affectedFiles.append(secondaryName);
        issue.title = DiagI18n::tr("Mixin Conflict (%1 <-> %2)", "Конфликт Mixin (%1 <-> %2)").arg(primaryName, secondaryName);
        QString targetInfo = !targetClass.isEmpty() ? QString(" (%1)").arg(targetClass) : QString();
        issue.details = DiagI18n::tr(
            "Conflict detected between mods '%1' and '%2'. Both mods attempted to modify game bytecode for class%3.",
            "Обнаружен конфликт между модами '%1' и '%2'. Оба мода пытаются внедрить свой код в один и тот же класс игры%3."
        ).arg(primaryName, secondaryName, targetInfo);
    } else if (resolvedJars.size() == 1) {
        primaryName = resolvedJars[0];
        issue.affectedFiles.append(primaryName);
        issue.title = DiagI18n::tr("Mixin Code Injection Conflict (%1)", "Конфликт внедрения Mixin (%1)").arg(primaryName);
        QString mixinName = !rawConfigs.isEmpty() ? rawConfigs.first() : (!targetClass.isEmpty() ? targetClass : "mixin");
        issue.details = DiagI18n::tr(
            "Mod '%1' failed to apply bytecode mixin (%2). This indicates an incompatibility with the current game version or another installed mod.",
            "Мод '%1' вызвал сбой при внедрении Mixin-кода (%2). Это указывает на несовместимость с текущей версией игры или другим модом."
        ).arg(primaryName, mixinName);
    } else {
        // Fallback when no JAR file found on disk (e.g. mock test)
        primaryName = !candidateModIds.isEmpty() ? candidateModIds.first() : (!rawConfigs.isEmpty() ? rawConfigs.first() : "mod");
        if (candidateModIds.size() > 1) {
            secondaryName = candidateModIds[1];
        } else if (rawConfigs.size() > 1) {
            secondaryName = rawConfigs[1];
        }

        if (!secondaryName.isEmpty() && secondaryName != primaryName) {
            issue.title = DiagI18n::tr("Mixin Conflict (%1 <-> %2)", "Конфликт Mixin (%1 <-> %2)").arg(primaryName, secondaryName);
            issue.details = DiagI18n::tr(
                "Conflict detected between mods '%1' and '%2'. One mod attempted to modify bytecode altered or cancelled by another.",
                "Обнаружен конфликт между модами '%1' и '%2'."
            ).arg(primaryName, secondaryName);
        } else {
            issue.title = DiagI18n::tr("Mixin Code Injection Conflict (%1)", "Конфликт внедрения Mixin (%1)").arg(primaryName);
            issue.details = DiagI18n::tr(
                "A mod failed to apply bytecode mixin (%1).",
                "Мод вызвал сбой при внедрении Mixin-кода (%1)."
            ).arg(!rawConfigs.isEmpty() ? rawConfigs.first() : primaryName);
        }
    }

    issue.recommendation = DiagI18n::tr(
        "Try updating the affected mod to the latest version. If the crash persists, disable one of the conflicting mods.",
        "Попробуйте обновить конфликтующие моды до последней версии. Если сбой продолжается, временно отключите один из модов."
    );

    QString evidenceLine = match.captured(0);
    static const QRegularExpression detailedEvidenceRe(
        R"((?:[^\n]*(?:FAILED during APPLY|Critical injection failure|Mixin apply failed|InjectionError)[^\n]*))",
        QRegularExpression::CaseInsensitiveOption
    );
    auto mEv = detailedEvidenceRe.match(errorBlock);
    if (mEv.hasMatch() && !mEv.captured(0).trimmed().isEmpty()) {
        evidenceLine = mEv.captured(0).trimmed();
    }
    issue.rawEvidence.append(evidenceLine);

    // Provide Action Buttons for all identified culprit mods
    bool isConflictBetweenTwo = (!secondaryName.isEmpty() && secondaryName != primaryName);

    if (!primaryName.isEmpty()) {
        CrashAction actA;
        actA.type = CrashActionType::DisableCulpritMod;
        actA.target = primaryName;
        if (isConflictBetweenTwo) {
            actA.label = DiagI18n::tr("Disable only [%1]", "Отключить только [%1]").arg(primaryName);
            actA.explanation = DiagI18n::tr("Disables '%1' while keeping '%2' active.",
                                           "Временно отключает только '%1', оставляя '%2' активным.").arg(primaryName, secondaryName);
        } else {
            actA.label = DiagI18n::tr("Disable Mod [%1]", "Отключить проблемный мод [%1]").arg(primaryName);
            actA.explanation = DiagI18n::tr("The file will be safely renamed to %1.disabled (the mod can be restored anytime).",
                                           "Файл мода будет переименован в %1.disabled (мод не удаляется и его можно вернуть).").arg(primaryName);
        }
        issue.actions.append(actA);

        issue.actionType = CrashActionType::DisableCulpritMod;
        issue.actionTarget = primaryName;
        issue.actionLabel = actA.label;
    }

    if (isConflictBetweenTwo) {
        CrashAction actB;
        actB.type = CrashActionType::DisableCulpritMod;
        actB.target = secondaryName;
        actB.label = DiagI18n::tr("Disable only [%1]", "Отключить только [%1]").arg(secondaryName);
        actB.explanation = DiagI18n::tr("Disables '%1' while keeping '%2' active.",
                                       "Временно отключает только '%1', оставляя '%2' активным.").arg(secondaryName, primaryName);
        issue.actions.append(actB);

        // Action 3: Option to disable BOTH conflicting mods at once
        CrashAction actBoth;
        actBoth.type = CrashActionType::DisableBothMods;
        actBoth.target = primaryName + "|" + secondaryName;
        actBoth.label = DiagI18n::tr("Disable Both Mods [%1 & %2]", "Отключить оба мода [%1 и %2]").arg(primaryName, secondaryName);
        actBoth.explanation = DiagI18n::tr("Safely disables both conflicting mods (%1 and %2) at once.",
                                           "Безопасно переименовывает оба конфликтующих мода (%1 и %2) в .disabled.").arg(primaryName, secondaryName);
        issue.actions.append(actBoth);
    }

    issues.append(issue);
}

void CrashAnalyzer::checkTickingEntity(const QString& fullText, const QString& instanceRoot, QList<CrashIssue>& issues, BaseInstance* instance)
{
    static const QRegularExpression tickingRe(
        R"((?:Description:\s+Ticking (entity|block entity)|java\.lang\.[a-zA-Z0-9_\$]*Exception:\s+Ticking (entity|block entity)))",
        QRegularExpression::CaseInsensitiveOption
    );

    auto match = tickingRe.match(fullText);
    if (!match.hasMatch()) {
        return;
    }

    bool isBlockEntity = match.captured(1).contains("block", Qt::CaseInsensitive) ||
                         match.captured(2).contains("block", Qt::CaseInsensitive);

    CrashIssue issue;
    issue.id = isBlockEntity ? "ticking_block_entity" : "ticking_entity";
    issue.severity = CrashSeverity::Fatal;

    static const QRegularExpression typeRe(R"((?:Entity Type|Name|Block):\s*([a-zA-Z0-9_\.\-]+:[a-zA-Z0-9_\.\-]+))", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression locRe(R"((?:Exact location|Block location):\s*([^\n\r]+))", QRegularExpression::CaseInsensitiveOption);

    QString objectId;
    auto mType = typeRe.match(fullText);
    if (mType.hasMatch()) {
        objectId = mType.captured(1);
    }

    QString location;
    auto mLoc = locRe.match(fullText);
    if (mLoc.hasMatch()) {
        location = mLoc.captured(1).trimmed();
    }

    QString namespaceId = objectId.contains(':') ? objectId.section(':', 0, 0) : QString();
    QString modJar = !namespaceId.isEmpty() && namespaceId != "minecraft"
                         ? resolveModJar(instanceRoot, namespaceId, instance)
                         : QString();

    if (isBlockEntity) {
        issue.title = DiagI18n::tr("Crash Caused by Broken Block Entity (%1)", "Сбойный блок в мире (%1)")
                          .arg(!objectId.isEmpty() ? objectId : DiagI18n::tr("Block", "Блок"));
        issue.details = DiagI18n::tr(
            "The world crashed while ticking block entity '%1'%2. A block entity is corrupted or executing invalid code.",
            "Мир игры аварийно завершился при обработке блока '%1'%2. Блок повреждён или вызвал ошибку в мире."
        ).arg(!objectId.isEmpty() ? objectId : DiagI18n::tr("unknown", "неизвестно"),
              !location.isEmpty() ? (" (" + location + ")") : QString());
    } else {
        issue.title = DiagI18n::tr("Crash Caused by Ticking Entity (%1)", "Сбойная сущность в мире (%1)")
                          .arg(!objectId.isEmpty() ? objectId : DiagI18n::tr("Entity", "Сущность"));
        issue.details = DiagI18n::tr(
            "The game crashed while ticking entity '%1'%2. The entity data may be corrupted or triggered a fatal logic bug.",
            "Игра вылетела при обновлении сущности '%1'%2. Данные моба/сущности повреждены или вызвали критический сбой."
        ).arg(!objectId.isEmpty() ? objectId : DiagI18n::tr("unknown", "неизвестно"),
              !location.isEmpty() ? (" (" + location + ")") : QString());
    }

    issue.recommendation = DiagI18n::tr(
        "1. Enable auto-removal of erroring entities in Forge/NeoForge server config.\n"
        "2. If the entity belongs to a specific mod, temporarily disable that mod to load the world.",
        "1. Включите авто-удаление сбойных сущностей в конфиге Forge/NeoForge.\n"
        "2. Если сущность принадлежит конкретному моду, временно отключите этот мод для очистки мира."
    );

    issue.rawEvidence.append(match.captured(0));
    if (mType.hasMatch()) issue.rawEvidence.append(mType.captured(0));
    if (mLoc.hasMatch()) issue.rawEvidence.append(mLoc.captured(0));

    // Action 1: Disable mod if resolved
    if (!modJar.isEmpty()) {
        issue.affectedFiles.append(modJar);
        CrashAction actMod;
        actMod.type = CrashActionType::DisableCulpritMod;
        actMod.target = modJar;
        actMod.label = DiagI18n::tr("Disable Mod [%1]", "Отключить мод [%1]").arg(modJar);
        actMod.explanation = DiagI18n::tr("The file will be safely renamed to %1.disabled to prevent the entity from crashing the game.",
                                         "Файл мода будет переименован в %1.disabled для предотвращения вылета игры.").arg(modJar);
        issue.actions.append(actMod);

        issue.actionType = CrashActionType::DisableCulpritMod;
        issue.actionTarget = modJar;
        issue.actionLabel = actMod.label;
    }

    // Action 2: Enable Forge removeErroringEntities
    CrashAction actForge;
    actForge.type = CrashActionType::EnableForgeRemoveErroringEntities;
    actForge.target = "forge-server.toml";
    actForge.label = DiagI18n::tr("Enable Auto-Removal of Broken Entities (Forge/NeoForge)", "Включить авто-удаление сбойных сущностей (Forge/NeoForge)");
    actForge.explanation = DiagI18n::tr(
        "Enables removeErroringEntities=true and removeErroringBlockEntities=true in server config to safely delete corrupted entities on world load.",
        "Включает removeErroringEntities=true и removeErroringBlockEntities=true в конфиге сервера для безопасного удаления сбойных объектов при загрузке мира."
    );
    issue.actions.append(actForge);

    if (issue.actionType == CrashActionType::None) {
        issue.actionType = actForge.type;
        issue.actionTarget = actForge.target;
        issue.actionLabel = actForge.label;
    }

    issues.append(issue);
}

void CrashAnalyzer::checkCorruptedFiles(const QString& fullText, const QString& instanceRoot, QList<CrashIssue>& issues)
{
    Q_UNUSED(instanceRoot);

    static const QRegularExpression configDetectRe(
        R"((?:Failed loading config file|Failed to load config|Error loading config|ParsingException:\s*Not enough data available|NightConfig|JsonSyntaxException|MalformedJsonException))",
        QRegularExpression::CaseInsensitiveOption
    );

    auto mConfig = configDetectRe.match(fullText);
    if (mConfig.hasMatch()) {
        QString configFile;

        // Try extracting from "Failed loading config file create-client.toml" etc.
        static const QRegularExpression specificFileRe(
            R"((?:Failed loading config file|Failed to load config|Error loading config|config file|reading config|at config|in file)\s*[:=]?\s*([a-zA-Z0-9_\.\-\/\\]+\.(?:toml|json5?|cfg|properties)))",
            QRegularExpression::CaseInsensitiveOption
        );
        auto mSpec = specificFileRe.match(fullText);
        if (mSpec.hasMatch()) {
            configFile = QFileInfo(mSpec.captured(1).trimmed()).fileName();
        }

        if (configFile.isEmpty()) {
            static const QRegularExpression findFileRe(
                R"(config[/\\]([a-zA-Z0-9_\.\-]+\.(?:toml|json5?|cfg|properties)))",
                QRegularExpression::CaseInsensitiveOption
            );
            auto mFind = findFileRe.match(fullText);
            if (mFind.hasMatch()) {
                configFile = QFileInfo(mFind.captured(1).trimmed()).fileName();
            }
        }

        if (configFile.isEmpty()) {
            static const QRegularExpression anyConfigRe(
                R"(([a-zA-Z0-9_\.\-]+\.(?:toml|json5?|cfg)))",
                QRegularExpression::CaseInsensitiveOption
            );
            auto mAny = anyConfigRe.match(fullText);
            if (mAny.hasMatch()) {
                configFile = QFileInfo(mAny.captured(1).trimmed()).fileName();
            }
        }

        CrashIssue issue;
        issue.id = "corrupted_config";
        issue.severity = CrashSeverity::Fatal;
        issue.title = DiagI18n::tr("Corrupted Mod Configuration File (%1)", "Повреждённый файл конфигурации (%1)")
                          .arg(!configFile.isEmpty() ? configFile : DiagI18n::tr("config", "конфиг"));

        if (!configFile.isEmpty()) {
            issue.affectedFiles.append(configFile);
        }

        issue.details = DiagI18n::tr(
            "A mod failed to parse configuration file '%1' due to syntax error or unexpected end of file (Not enough data / invalid JSON or TOML).",
            "Мод не смог прочитать файл конфигурации '%1' из-за синтаксической ошибки или неполного файла (Not enough data / повреждённый JSON или TOML)."
        ).arg(!configFile.isEmpty() ? configFile : DiagI18n::tr("unknown", "неизвестно"));

        issue.recommendation = DiagI18n::tr(
            "Reset the configuration file. Prism Launcher can rename it to .bak, and the game will recreate a clean default config on next launch.",
            "Сбросьте повреждённый конфиг. Лаунчер переименует его в .bak, и игра автоматически создаст чистый файл настроек при следующем запуске."
        );

        if (mSpec.hasMatch()) {
            issue.rawEvidence.append(mSpec.captured(0));
        } else {
            issue.rawEvidence.append(mConfig.captured(0));
        }

        if (!configFile.isEmpty()) {
            CrashAction act;
            act.type = CrashActionType::ResetCorruptedConfig;
            act.target = configFile;
            act.label = DiagI18n::tr("Reset Config [%1]", "Сбросить повреждённый конфиг [%1]").arg(configFile);
            act.explanation = DiagI18n::tr("Renames config/%1 to %1.bak so the game generates default settings safely.",
                                           "Переименовывает config/%1 в %1.bak для безопасного создания настроек по умолчанию.").arg(configFile);
            issue.actions.append(act);

            issue.actionType = CrashActionType::ResetCorruptedConfig;
            issue.actionTarget = configFile;
            issue.actionLabel = act.label;
        }

        issues.append(issue);
    }

    static const QRegularExpression zipRe(
        R"((ZipException|CorruptZipFileException|invalid CEN header|invalid LOC header)[^\n]*?(?:in file\s+([^\n]+)|([a-zA-Z0-9_\.\-]+\.jar)))",
        QRegularExpression::CaseInsensitiveOption
    );

    auto mZip = zipRe.match(fullText);
    if (mZip.hasMatch()) {
        CrashIssue issue;
        issue.id = "corrupted_jar";
        issue.severity = CrashSeverity::Fatal;
        issue.title = DiagI18n::tr("Corrupted JAR File (Archive)", "Повреждённый JAR-файл (архив)");
        QString jarName = !mZip.captured(2).isEmpty() ? mZip.captured(2) : mZip.captured(3);
        jarName = QFileInfo(jarName).fileName();

        if (!jarName.isEmpty()) {
            issue.affectedFiles.append(jarName);
            CrashAction act;
            act.type = CrashActionType::DisableCulpritMod;
            act.target = jarName;
            act.label = DiagI18n::tr("Disable Corrupted JAR [%1]", "Отключить повреждённый JAR [%1]").arg(jarName);
            act.explanation = DiagI18n::tr("Renames the corrupted archive to .disabled to allow Minecraft to launch.",
                                           "Переименовывает повреждённый архив в .disabled для нормального запуска игры.");
            issue.actions.append(act);

            issue.actionType = CrashActionType::DisableCulpritMod;
            issue.actionTarget = jarName;
            issue.actionLabel = act.label;
        }
        issue.details = DiagI18n::tr(
            "A mod or library JAR file is corrupted or incompletely downloaded.",
            "Один из JAR-файлов мода или библиотеки повреждён или не до конца скачан."
        );
        issue.recommendation = DiagI18n::tr(
            "Re-download the affected file or disable it from the mods folder.",
            "Перескачайте повреждённый файл или отключите его в папке mods."
        );
        issue.rawEvidence.append(mZip.captured(0));
        issues.append(issue);
    }
}

void CrashAnalyzer::checkShaderAndResourcePack(const QString& fullText, const QString& instanceRoot, QList<CrashIssue>& issues)
{
    Q_UNUSED(instanceRoot);

    // 1. Shaders Check - Must be strictly in an ERROR context:
    // A) Explicit shader compilation / linker / GL failure:
    //    "Error creating shader program", "Failed to compile shader", "Program linked with compile errors", "OpenGL Error: 1282 ... shader"
    // B) Lines with [ERROR]/[FATAL]/[STDERR] mentioning shaderpack / iris / oculus / optionsshaders
    // C) Exception lines / stack traces mentioning shaderpack / iris / oculus (e.g. ShaderException, at net.coderbot.iris... inside exception)
    // D) Crash report description or exception section mentioning shaderpack / iris / oculus
    static const QRegularExpression shaderErrorRe(
        R"((?:(?:\[[^\]]*(?:ERROR|FATAL|STDERR)[^\]]*\][^\n]*(?:Error creating shader|Failed to (?:compile|link) shader|Program linked with compile errors|ShaderException|Iris.*Exception|ShaderPack[^\n]*(?:failed|error|crash)|net\.coderbot\.iris|net\.irisshaders|com\.customhuds\.oculus|optionsshaders\.txt))|(?:(?:ShaderException|net\.coderbot\.iris[^\n]*(?:Exception|Error)|net\.irisshaders[^\n]*(?:Exception|Error))[^\n]*)|(?:Failed to (?:compile|link) shader|Error creating shader program|Program linked with compile errors|OpenGL Error:\s*1282[^\n]*shader)|(?:---- Minecraft Crash Report ----[\s\S]*?(?:Description:[^\n]*(?:shader|iris|oculus)|(?:net\.coderbot\.iris|net\.irisshaders|ShaderPack|optionsshaders)[^\n]*(?:Exception|Error)))))",
        QRegularExpression::CaseInsensitiveOption
    );

    auto mShader = shaderErrorRe.match(fullText);
    if (mShader.hasMatch()) {
        CrashIssue issue;
        issue.id = "shader_crash";
        issue.severity = CrashSeverity::Fatal;
        issue.title = DiagI18n::tr("Crash Caused by Shaders (Iris / Oculus / ShaderPack)", "Вылет из-за шейдеров (Iris / Oculus / ShaderPack)");
        issue.details = DiagI18n::tr(
            "A shader pack failed to compile or caused an OpenGL rendering pipeline crash on startup.",
            "Шейдер-пак вызвал ошибку компиляции шейдеров или сбой конвейера OpenGL при запуске игры."
        );
        issue.recommendation = DiagI18n::tr(
            "Disable active shaders. Prism Launcher can rename optionsshaders.txt to .bak without deleting your downloaded shader packs.",
            "Отключите активные шейдеры. Лаунчер переименует optionsshaders.txt в .bak, при этом файлы шейдеров в папке shaderpacks/ останутся на месте."
        );
        issue.rawEvidence.append(mShader.captured(0));

        CrashAction act;
        act.type = CrashActionType::DisableShaders;
        act.target = "optionsshaders.txt";
        act.label = DiagI18n::tr("Disable Active Shaders", "Отключить активные шейдеры");
        act.explanation = DiagI18n::tr("Renames optionsshaders.txt to optionsshaders.txt.bak. Shader files in shaderpacks/ are preserved.",
                                       "Переименовывает optionsshaders.txt в .bak. Сами файлы шейдеров сохраняются.");
        issue.actions.append(act);

        issue.actionType = CrashActionType::DisableShaders;
        issue.actionTarget = "optionsshaders.txt";
        issue.actionLabel = act.label;

        issues.append(issue);
    }

    // 2. Resource Packs / Atlas Check - Must be in an ERROR / Exception context
    static const QRegularExpression rpRe(
        R"((?:(?:\[[^\]]*(?:ERROR|FATAL|STDERR)[^\]]*\][^\n]*(?:AtlasStitcher|StitcherException|Unable to fit:[^\n]+atlas|Texture atlas stitched with errors|Failed to reload resource packs|Unable to reload resources))|(?:StitcherException[^\n]*)|(?:Unable to fit: [^,]+, size: [0-9]+x[0-9]+)|(?:---- Minecraft Crash Report ----[\s\S]*?(?:StitcherException|AtlasStitcher|Unable to fit:[^\n]+atlas))))",
        QRegularExpression::CaseInsensitiveOption
    );

    auto mRp = rpRe.match(fullText);
    if (mRp.hasMatch()) {
        CrashIssue issue;
        issue.id = "resource_pack_crash";
        issue.severity = CrashSeverity::Fatal;
        issue.title = DiagI18n::tr("Texture Stitching / Resource Pack Failure", "Сбой при загрузке текстур / ресурспаков");
        issue.details = DiagI18n::tr(
            "Texture atlas stitching failed. A high-resolution or incompatible resource pack exceeded GPU texture limits or contains invalid textures.",
            "Сбой склейки текстурного атласа. Тяжёлый ресурспак высокого разрешения превысил лимиты видеокарты или содержит некорректные текстуры."
        );
        issue.recommendation = DiagI18n::tr(
            "Disable active resource packs to return to default textures. Your resourcepack archives will not be deleted.",
            "Отключите активные ресурспаки для возврата к стандартным текстурам. Архивы ресурспаков останутся в папке resourcepacks/."
        );
        issue.rawEvidence.append(mRp.captured(0));

        CrashAction act;
        act.type = CrashActionType::ResetResourcePacks;
        act.target = "options.txt";
        act.label = DiagI18n::tr("Disable Active Resource Packs", "Отключить активные ресурспаки");
        act.explanation = DiagI18n::tr("Resets resourcePacks in options.txt to default. Files in resourcepacks/ remain untouched.",
                                       "Сбрасывает список ресурспаков в options.txt к стандартным. Архивы в папке сохраняются.");
        issue.actions.append(act);

        issue.actionType = CrashActionType::ResetResourcePacks;
        issue.actionTarget = "options.txt";
        issue.actionLabel = act.label;

        issues.append(issue);
    }
}

void CrashAnalyzer::checkMemoryIssues(const QString& fullText, QList<CrashIssue>& issues)
{
    static const QRegularExpression oomRe(
        R"(java\.lang\.OutOfMemoryError|GC overhead limit exceeded|Java heap space|OutOfMemoryError:\s*Metaspace)",
        QRegularExpression::CaseInsensitiveOption
    );
    static const QRegularExpression memoryTooHighRe(
        R"(Could not reserve enough space for|Invalid maximum heap size|Native memory allocation \(mmap\) failed|Cannot allocate memory)",
        QRegularExpression::CaseInsensitiveOption
    );

    if (oomRe.match(fullText).hasMatch()) {
        CrashIssue issue;
        issue.id = "out_of_memory";
        issue.severity = CrashSeverity::Fatal;
        issue.title = DiagI18n::tr("Out of Memory (Java Heap Space)", "Нехватка оперативной памяти (OOM)");
        issue.details = DiagI18n::tr(
            "Minecraft ran out of allocated Java heap memory.",
            "Игре не хватило выделенной оперативной памяти для загрузки или работы."
        );
        issue.recommendation = DiagI18n::tr(
            "Increase allocated memory in Instance Settings -> Java -> Memory (+2048 MB).",
            "Увеличьте объём выделяемой памяти в Настройках экземпляра -> Java -> Память (+2048 МБ)."
        );
        issue.rawEvidence.append("java.lang.OutOfMemoryError: Java heap space");

        CrashAction act;
        act.type = CrashActionType::IncreaseAllocatedMemory;
        act.target = "MaxMemAlloc";
        act.label = DiagI18n::tr("Increase Allocated RAM (+2048 MB)", "Увеличить выделенную ОЗУ (+2048 МБ)");
        act.explanation = DiagI18n::tr("Increases maximum allocated memory for this instance by 2048 MB in settings.",
                                       "Увеличивает максимальный объём выделенной оперативной памяти на 2048 МБ в настройках инстанции.");
        issue.actions.append(act);

        issue.actionType = CrashActionType::IncreaseAllocatedMemory;
        issue.actionTarget = "MaxMemAlloc";
        issue.actionLabel = act.label;

        issues.append(issue);
    } else if (memoryTooHighRe.match(fullText).hasMatch()) {
        CrashIssue issue;
        issue.id = "out_of_memory";
        issue.severity = CrashSeverity::Fatal;
        issue.title = DiagI18n::tr("Cannot Allocate Memory for Heap", "Невозможно выделить память под Heap");
        issue.details = DiagI18n::tr(
            "Java could not reserve the requested memory amount from the operating system.",
            "Java не смогла зарезервировать запрошенный объём памяти у операционной системы."
        );
        issue.recommendation = DiagI18n::tr(
            "1. Reduce allocated memory in Instance Settings.\n"
            "2. Ensure you are running a 64-bit Java runtime rather than 32-bit.",
            "1. Уменьшите объём выделяемой памяти в настройках экземпляра.\n"
            "2. Убедитесь, что установлена и выбрана 64-битная версия Java, а не 32-битная."
        );
        issue.rawEvidence.append("Could not reserve enough space for object heap");
        issues.append(issue);
    }
}

void CrashAnalyzer::checkMissingDependencies(const QString& fullText, QList<CrashIssue>& issues)
{
    static const QRegularExpression fabricSolutionRe(
        R"(A potential solution has been determined:\s*\n\s*-\s*Install ([^,\n]+)(?:,\s*version\s*([^\n]+))?)",
        QRegularExpression::CaseInsensitiveOption
    );

    auto mSolution = fabricSolutionRe.match(fullText);
    if (mSolution.hasMatch()) {
        CrashIssue issue;
        issue.id = "missing_dependencies";
        issue.severity = CrashSeverity::Fatal;
        issue.title = DiagI18n::tr("Missing Mod Dependency", "Отсутствует зависимость мода");
        QString missingMod = mSolution.captured(1).trimmed();
        QString reqVersion = mSolution.captured(2).trimmed();

        issue.summary = DiagI18n::tr("Required to install: %1", "Требуется установить: %1").arg(missingMod);
        issue.details = DiagI18n::tr("The game requires an additional mod or library to start: '%1'%2.",
                                     "Для запуска требуется установить дополнительный мод или библиотеку: '%1'%2.")
                                     .arg(missingMod, reqVersion.isEmpty() ? QString() : " (" + reqVersion + ")");
        issue.recommendation = DiagI18n::tr(
            "Proposed Solution: Install mod '%1'%2.\n"
            "• Click the 'Auto-Fix' button below to open Prism's mod downloader.\n"
            "• Select the matching version and confirm download into your mods folder.",
            "Возможный вариант исправления: Установить мод '%1'%2.\n"
            "• Нажмите кнопку 'Авто-исправление' ниже, чтобы открыть встроенный загрузчик Prism.\n"
            "• Выберите подходящую версию и подтвердите загрузку в папку mods."
        ).arg(missingMod, reqVersion.isEmpty() ? QString() : " (" + reqVersion + ")");
        issue.rawEvidence.append(mSolution.captured(0));

        issue.actionType = CrashActionType::DownloadMissingMod;
        issue.actionTarget = missingMod;
        issue.actionLabel = DiagI18n::tr("Auto-Fix: Install '%1'", "Авто-исправление: Установить '%1'").arg(missingMod);

        CrashAction act;
        act.type = issue.actionType;
        act.target = issue.actionTarget;
        act.label = issue.actionLabel;
        act.explanation = DiagI18n::tr("Opens Mod Downloader to search and install compatible version",
                                       "Открывает каталог модов для скачивания совместимой версии");
        issue.actions.append(act);

        issues.append(issue);
        return;
    }

    static const QRegularExpression fabricDepRe(
        R"(Mod '([^']+)' \(([^)]+)\) requires (?:version )?([^,]+) of (?:mod )?'([^']+)', which is missing!)",
        QRegularExpression::CaseInsensitiveOption
    );
    static const QRegularExpression genDepRe(
        R"(requires (?:mod )?'([^']+)', which is missing!)",
        QRegularExpression::CaseInsensitiveOption
    );
    static const QRegularExpression forgeDepRe(
        R"(Mod ([a-zA-Z0-9_\.\-]+) requires ([a-zA-Z0-9_\.\-]+) ([a-zA-Z0-9_\.\-+]+) or above\s*Currently, \2 is (not installed|[\w.\-+]+))",
        QRegularExpression::CaseInsensitiveOption
    );
    static const QRegularExpression forgeMandatoryRe(
        R"(Missing or unsupported mandatory dependencies:\s*\n\s*Mod ID: '([^']+)', Requested by: '([^']+)')",
        QRegularExpression::CaseInsensitiveOption
    );

    auto mFabric = fabricDepRe.match(fullText);
    if (mFabric.hasMatch()) {
        CrashIssue issue;
        issue.id = "missing_dependencies";
        issue.severity = CrashSeverity::Fatal;
        issue.title = DiagI18n::tr("Missing Mod Dependency", "Отсутствует зависимость мода");
        QString mod = mFabric.captured(1);
        QString missingMod = mFabric.captured(4);
        QString reqVersion = mFabric.captured(3);

        issue.summary = DiagI18n::tr("Mod '%1' requires '%2' (%3)", "Моду '%1' требуется '%2' (%3)").arg(mod, missingMod, reqVersion);
        issue.details = DiagI18n::tr(
            "Mod '%1' cannot start because the required mod '%2' (version %3) was not found.",
            "Мод '%1' не может запуститься, так как не найден обязательный мод '%2' (версии %3)."
        ).arg(mod, missingMod, reqVersion);
        issue.recommendation = DiagI18n::tr(
            "Proposed Solution: Install mod '%1' (version %2).\n"
            "• Click the 'Auto-Fix' button below to open Prism's mod downloader.\n"
            "• Select the matching version and confirm download into your mods folder.",
            "Возможный вариант исправления: Установить мод '%1' (версии %2).\n"
            "• Нажмите кнопку 'Авто-исправление' ниже, чтобы открыть встроенный загрузчик Prism.\n"
            "• Выберите подходящую версию и подтвердите загрузку в папку mods."
        ).arg(missingMod, reqVersion);
        issue.rawEvidence.append(mFabric.captured(0));

        issue.actionType = CrashActionType::DownloadMissingMod;
        issue.actionTarget = missingMod;
        issue.actionLabel = DiagI18n::tr("Auto-Fix: Install '%1'", "Авто-исправление: Установить '%1'").arg(missingMod);

        CrashAction act;
        act.type = issue.actionType;
        act.target = issue.actionTarget;
        act.label = issue.actionLabel;
        act.explanation = DiagI18n::tr("Opens Mod Downloader to search and install compatible version",
                                       "Открывает каталог модов для скачивания совместимой версии");
        issue.actions.append(act);

        issues.append(issue);
        return;
    }

    auto mForge = forgeDepRe.match(fullText);
    if (mForge.hasMatch()) {
        CrashIssue issue;
        issue.id = "missing_dependencies";
        issue.severity = CrashSeverity::Fatal;
        issue.title = DiagI18n::tr("Missing Mod Dependency (Forge/NeoForge)", "Отсутствует зависимость (Forge/NeoForge)");
        QString mod = mForge.captured(1);
        QString missingMod = mForge.captured(2);
        QString reqVersion = mForge.captured(3);
        QString currentStatus = mForge.captured(4);

        issue.summary = DiagI18n::tr("Mod '%1' requires '%2' >= %3 (currently: %4)", "Моду '%1' требуется '%2' >= %3 (сейчас: %4)").arg(mod, missingMod, reqVersion, currentStatus);
        issue.details = DiagI18n::tr(
            "Mod '%1' requires library '%2' version %3 or above. Current status: %4.",
            "Мод '%1' требует наличия библиотеки '%2' версии %3 или выше. Текущий статус: %4."
        ).arg(mod, missingMod, reqVersion, currentStatus);
        issue.recommendation = DiagI18n::tr(
            "Proposed Solution: Update or install '%1' (version %2 or above).\n"
            "• Click the 'Auto-Fix' button below to open Prism's mod downloader.\n"
            "• Select the matching version and confirm download.",
            "Возможный вариант исправления: Обновить или установить '%1' (версии %2 или выше).\n"
            "• Нажмите кнопку 'Авто-исправление' ниже, чтобы открыть встроенный загрузчик Prism.\n"
            "• Выберите подходящую версию и подтвердите загрузку."
        ).arg(missingMod, reqVersion);
        issue.rawEvidence.append(mForge.captured(0));

        if (missingMod != "minecraft" && missingMod != "forge" && missingMod != "neoforge") {
            issue.actionType = CrashActionType::DownloadMissingMod;
            issue.actionTarget = missingMod;
            issue.actionLabel = DiagI18n::tr("Auto-Fix: Install '%1'", "Авто-исправление: Установить '%1'").arg(missingMod);

            CrashAction act;
            act.type = issue.actionType;
            act.target = issue.actionTarget;
            act.label = issue.actionLabel;
            act.explanation = DiagI18n::tr("Opens Mod Downloader to search and install compatible version",
                                           "Открывает каталог модов для скачивания совместимой версии");
            issue.actions.append(act);
        }

        issues.append(issue);
        return;
    }

    auto mMandatory = forgeMandatoryRe.match(fullText);
    if (mMandatory.hasMatch()) {
        CrashIssue issue;
        issue.id = "missing_dependencies";
        issue.severity = CrashSeverity::Fatal;
        issue.title = DiagI18n::tr("Missing Mandatory Mods", "Отсутствуют обязательные моды");
        QString missingMod = mMandatory.captured(1);
        QString requestedBy = mMandatory.captured(2);

        issue.details = DiagI18n::tr("Mod '%1' requires mandatory mod '%2'.", "Мод '%1' требует обязательный мод '%2'.").arg(requestedBy, missingMod);
        issue.recommendation = DiagI18n::tr(
            "Proposed Solution: Install mod '%1'.\n"
            "• Click the 'Auto-Fix' button below to open Prism's mod downloader.",
            "Возможный вариант исправления: Установить мод '%1'.\n"
            "• Нажмите кнопку 'Авто-исправление' ниже, чтобы открыть встроенный загрузчик Prism."
        ).arg(missingMod);
        issue.rawEvidence.append(mMandatory.captured(0));

        if (missingMod != "minecraft" && missingMod != "forge" && missingMod != "neoforge") {
            issue.actionType = CrashActionType::DownloadMissingMod;
            issue.actionTarget = missingMod;
            issue.actionLabel = DiagI18n::tr("Auto-Fix: Install '%1'", "Авто-исправление: Установить '%1'").arg(missingMod);

            CrashAction act;
            act.type = issue.actionType;
            act.target = issue.actionTarget;
            act.label = issue.actionLabel;
            act.explanation = DiagI18n::tr("Opens Mod Downloader to search and install compatible version",
                                           "Открывает каталог модов для скачивания совместимой версии");
            issue.actions.append(act);
        }

        issues.append(issue);
        return;
    }

    auto mGen = genDepRe.match(fullText);
    if (mGen.hasMatch()) {
        CrashIssue issue;
        issue.id = "missing_dependencies";
        issue.severity = CrashSeverity::Fatal;
        issue.title = DiagI18n::tr("Missing Required Mod", "Не найден обязательный мод");
        QString missingMod = mGen.captured(1);
        issue.details = DiagI18n::tr("Missing dependency: '%1'.", "Не найдена зависимость: '%1'.").arg(missingMod);
        issue.recommendation = DiagI18n::tr(
            "Proposed Solution: Install mod '%1'.\n"
            "• Click the 'Auto-Fix' button below to open Prism's mod downloader.",
            "Возможный вариант исправления: Установить мод '%1'.\n"
            "• Нажмите кнопку 'Авто-исправление' ниже, чтобы открыть встроенный загрузчик Prism."
        ).arg(missingMod);
        issue.rawEvidence.append(mGen.captured(0));

        if (missingMod != "minecraft" && missingMod != "forge" && missingMod != "neoforge") {
            issue.actionType = CrashActionType::DownloadMissingMod;
            issue.actionTarget = missingMod;
            issue.actionLabel = DiagI18n::tr("Auto-Fix: Install '%1'", "Авто-исправление: Установить '%1'").arg(missingMod);

            CrashAction act;
            act.type = issue.actionType;
            act.target = issue.actionTarget;
            act.label = issue.actionLabel;
            act.explanation = DiagI18n::tr("Opens Mod Downloader to search and install compatible version",
                                           "Открывает каталог модов для скачивания совместимой версии");
            issue.actions.append(act);
        }

        issues.append(issue);
    }
}

void CrashAnalyzer::checkWrongLoader(const QString& fullText, QList<CrashIssue>& issues)
{
    static const QRegularExpression wrongLoaderRe(
        R"((found Fabric mod|is a fabric mod|Fabric mods cannot be loaded on Forge|is a Forge mod|Forge mods cannot be loaded on Fabric))",
        QRegularExpression::CaseInsensitiveOption
    );

    auto match = wrongLoaderRe.match(fullText);
    if (match.hasMatch()) {
        CrashIssue issue;
        issue.id = "wrong_loader";
        issue.severity = CrashSeverity::Fatal;
        issue.title = DiagI18n::tr("Mod for Wrong Mod Loader", "Мод для другого загрузчика");
        issue.details = DiagI18n::tr(
            "A mod intended for a different loader was installed (e.g., Fabric mod on Forge, or Forge mod on Fabric).",
            "В сборку попал мод, предназначенный для другого модлоадера (например, Fabric-мод в Forge или наоборот)."
        );
        issue.recommendation = DiagI18n::tr(
            "Review your mods and replace the incompatible mod with a build for your active mod loader.",
            "Проверьте установленные моды и замените несовместимый мод на версию под используемый загрузчик."
        );
        issue.rawEvidence.append(match.captured(0));
        issues.append(issue);
    }
}

void CrashAnalyzer::checkJavaVersion(const QString& fullText, QList<CrashIssue>& issues)
{
    static const QRegularExpression javaVersionRe(
        R"(has been compiled by a more recent version of the Java Runtime \(class file version (\d+\.\d+)\), this version of the Java Runtime only recognizes class file versions up to (\d+\.\d+))"
    );

    auto match = javaVersionRe.match(fullText);
    if (match.hasMatch()) {
        CrashIssue issue;
        issue.id = "java_version_incompatible";
        issue.severity = CrashSeverity::Fatal;
        issue.title = DiagI18n::tr("Java Version Too Old", "Устаревшая версия Java");

        QString compiledVer = match.captured(1);
        QString runtimeVer = match.captured(2);

        int compMajor = compiledVer.section('.', 0, 0).toInt();
        int reqJava = compMajor >= 45 ? compMajor - 44 : 8;

        issue.details = DiagI18n::tr(
            "A mod requires Java %1 or newer (class file %2), but the current Java runtime only supports up to class file %3.",
            "Одному из модов требуется Java %1 или новее (class file %2), но текущая Java поддерживает только версии до %3."
        ).arg(QString::number(reqJava), compiledVer, runtimeVer);

        issue.recommendation = DiagI18n::tr(
            "Go to Instance Settings -> Java, and select or install Java %1 (64-bit).",
            "Перейдите в Настройки экземпляра -> Java и выберите или установите Java %1 (64-бит)."
        ).arg(QString::number(reqJava));

        issue.rawEvidence.append(match.captured(0));
        issues.append(issue);
        return;
    }

    static const QRegularExpression javaClassVerOldRe(
        R"(UnsupportedClassVersionError: ([^\n]+))"
    );
    auto mOld = javaClassVerOldRe.match(fullText);
    if (mOld.hasMatch()) {
        CrashIssue issue;
        issue.id = "java_version_incompatible";
        issue.severity = CrashSeverity::Fatal;
        issue.title = DiagI18n::tr("Incompatible Java Version", "Несовместимая версия Java");
        issue.details = DiagI18n::tr("Java class version mismatch detected.", "Обнаружено несоответствие версии Java требованиям модов.");
        issue.recommendation = DiagI18n::tr("Check your Java settings and ensure the appropriate Java version is configured for this Minecraft version.",
                                            "Проверьте настройки Java и убедитесь, что выбрана подходящая версия Java для этой версии Minecraft.");
        issue.rawEvidence.append(mOld.captured(0));
        issues.append(issue);
    }
}

void CrashAnalyzer::checkJvmArguments(const QString& fullText, QList<CrashIssue>& issues)
{
    static const QRegularExpression jvmArgsRe(
        R"((Unrecognized VM option '([^']+)'|Could not create the Java Virtual Machine|Error: Could not find or load main class))",
        QRegularExpression::CaseInsensitiveOption
    );

    auto match = jvmArgsRe.match(fullText);
    if (match.hasMatch()) {
        CrashIssue issue;
        issue.id = "invalid_jvm_args";
        issue.severity = CrashSeverity::Fatal;
        issue.title = DiagI18n::tr("Invalid Java Virtual Machine Arguments", "Некорректные параметры запуска Java (JVM)");
        QString option = match.captured(2);
        if (!option.isEmpty()) {
            issue.details = DiagI18n::tr("Unrecognized JVM parameter: '%1'.", "Неизвестный параметр запуска JVM: '%1'.").arg(option);
        } else {
            issue.details = DiagI18n::tr("The Java Virtual Machine failed to initialize with the configured startup flags.",
                                         "Java Virtual Machine не смогла запуститься с текущими флагами командной строки.");
        }
        issue.recommendation = DiagI18n::tr(
            "Remove invalid Java arguments in Settings -> Java -> JVM Arguments.",
            "Удалите неподдерживаемые флаги запуска в Настройки -> Java -> Параметры JVM."
        );
        issue.rawEvidence.append(match.captured(0));
        issues.append(issue);
    }
}

void CrashAnalyzer::checkDuplicateMods(const QString& fullText, QList<CrashIssue>& issues)
{
    static const QRegularExpression dupRe(
        R"((DuplicateModsFoundException|Duplicate mod ID found: '([^']+)'|Found duplicate mod: ([^\n]+)))",
        QRegularExpression::CaseInsensitiveOption
    );

    auto match = dupRe.match(fullText);
    if (match.hasMatch()) {
        CrashIssue issue;
        issue.id = "duplicate_mods";
        issue.severity = CrashSeverity::Fatal;
        issue.title = DiagI18n::tr("Duplicate Mods Detected", "Обнаружены дубликаты модов");
        QString modId = match.captured(2).isEmpty() ? match.captured(3) : match.captured(2);
        issue.details = DiagI18n::tr(
            "Two different versions of the same mod (%1) were found in the mods folder.",
            "В папке mods обнаружены две разные версии одного и того же мода (%1)."
        ).arg(modId.trimmed().isEmpty() ? DiagI18n::tr("check log", "проверьте лог") : modId);
        issue.recommendation = DiagI18n::tr(
            "Open your mods folder and delete the older duplicate JAR file.",
            "Откройте папку mods и удалите старую версию дублирующегося JAR-файла."
        );
        issue.rawEvidence.append(match.captured(0));
        issues.append(issue);
    }
}

void CrashAnalyzer::checkGraphicsIssues(const QString& fullText, QList<CrashIssue>& issues)
{
    static const QRegularExpression gfxRe(
        R"((# C\s+\[(nvoglv64\.dll|atio6axx\.dll|ig9icd64\.dll|amdvlk64\.dll)[^\]]*\]|org\.lwjgl\.glfw\.GLFWErrorCallback|GLFW error 65542|WGL: The driver does not appear to support OpenGL))",
        QRegularExpression::CaseInsensitiveOption
    );

    auto match = gfxRe.match(fullText);
    if (match.hasMatch()) {
        CrashIssue issue;
        issue.id = "graphics_failure";
        issue.severity = CrashSeverity::Fatal;
        issue.title = DiagI18n::tr("Graphics Driver / OpenGL Crash", "Сбой графического драйвера / OpenGL");
        issue.details = DiagI18n::tr(
            "Minecraft crashed inside the graphics driver or OpenGL initialization routines.",
            "Произошёл сбой внутри графического видеодрайвера или при инициализации OpenGL."
        );
        issue.recommendation = DiagI18n::tr(
            "1. Update your graphics drivers to the latest version.\n"
            "2. If you are using a laptop with dual GPUs, configure it to run Minecraft on the dedicated GPU.",
            "1. Обновите драйверы видеокарты до последней версии.\n"
            "2. Если у вас ноутбук с двумя видеокартами, настройте запуск Minecraft на дискретной видеокарте."
        );
        issue.rawEvidence.append(match.captured(0));
        issues.append(issue);
    }
}

void CrashAnalyzer::checkGenericExceptions(const QString& fullText, const QString& instanceRoot, QList<CrashIssue>& issues, BaseInstance* instance)
{
    // Avoid duplicate issue if a specific fatal error has already been found
    for (const auto& issue : issues) {
        if (issue.id == "mixin_failure" || issue.id == "ticking_entity" || issue.id == "ticking_block_entity" ||
            issue.id == "corrupted_config" || issue.id == "shader_crash" || issue.id == "resource_pack_crash") {
            return;
        }
    }

    static const QRegularExpression exRe(
        R"((?:Caused by:\s+([a-zA-Z0-9_\.\$]+(?:Exception|Error)):\s*([^\n\r]*)|Exception in thread [^\n]+ ([a-zA-Z0-9_\.\$]+(?:Exception|Error)):\s*([^\n\r]*)))",
        QRegularExpression::CaseInsensitiveOption
    );

    auto match = exRe.match(fullText);
    if (!match.hasMatch()) {
        return;
    }

    QString exType = !match.captured(1).isEmpty() ? match.captured(1) : match.captured(3);
    QString exMsg = !match.captured(2).isEmpty() ? match.captured(2) : match.captured(4);

    int matchPos = match.capturedEnd();
    QString afterEx = fullText.mid(matchPos, 2048);
    static const QRegularExpression stackFrameRe(R"(at\s+([a-zA-Z0-9_\.\$]+)\.([a-zA-Z0-9_\$]+)\([^\)]+\))");
    auto frameIter = stackFrameRe.globalMatch(afterEx);

    QString culpritClass;
    while (frameIter.hasNext()) {
        auto mFrame = frameIter.next();
        QString className = mFrame.captured(1);
        if (!isStandardIgnoredClass(className)) {
            culpritClass = className;
            break;
        }
    }

    if (culpritClass.isEmpty()) {
        return;
    }

    QString culpritJar = resolveModJar(instanceRoot, culpritClass, instance);

    CrashIssue issue;
    issue.id = "mod_crash";
    issue.severity = CrashSeverity::Fatal;

    QString modDisplayName = !culpritJar.isEmpty() ? culpritJar : culpritClass;
    issue.title = DiagI18n::tr("Crash in Mod Code (%1)", "Сбой в коде мода (%1)").arg(modDisplayName);

    if (!culpritJar.isEmpty()) {
        issue.affectedFiles.append(culpritJar);
    }

    issue.details = DiagI18n::tr(
        "An unhandled exception '%1' occurred in mod class '%2': %3",
        "Необработанное исключение '%1' произошло в классе мода '%2': %3"
    ).arg(exType, culpritClass, exMsg.trimmed().isEmpty() ? DiagI18n::tr("no message", "без описания") : exMsg.trimmed());

    issue.recommendation = DiagI18n::tr(
        "Try updating or temporarily disabling the mod causing the exception.",
        "Попробуйте обновить или временно отключить мод, вызвавший исключение."
    );

    issue.rawEvidence.append(match.captured(0));
    issues.append(issue);

    if (!culpritJar.isEmpty()) {
        CrashAction act;
        act.type = CrashActionType::DisableCulpritMod;
        act.target = culpritJar;
        act.label = DiagI18n::tr("Disable Mod [%1]", "Отключить проблемный мод [%1]").arg(culpritJar);
        act.explanation = DiagI18n::tr("The file will be safely renamed to %1.disabled (the mod can be restored anytime).",
                                       "Файл мода будет переименован в %1.disabled (мод не удаляется и его можно вернуть).").arg(culpritJar);
        issue.actions.append(act);

        issue.actionType = CrashActionType::DisableCulpritMod;
        issue.actionTarget = culpritJar;
        issue.actionLabel = act.label;
    }

    issues.append(issue);
}
