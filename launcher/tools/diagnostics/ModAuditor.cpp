// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#include "ModAuditor.h"
#include "DiagI18n.h"
#include "Application.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include "archive/ArchiveReader.h"

namespace {
struct CachedAuditEntry {
    AuditReport report;
    QDateTime lastModified;
    qint64 fileSize = 0;
    qint64 lastCheckedMs = 0;
};

static QMutex s_cacheMutex;
static bool s_auditCacheLoaded = false;
static QHash<QString, CachedAuditEntry> s_auditCache;

static QString getModAuditCacheFilePath()
{
    QString baseDir;
    auto* app = dynamic_cast<Application*>(QCoreApplication::instance());
    if (app) {
        baseDir = app->dataRoot();
    }
    if (baseDir.isEmpty()) {
        baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    if (baseDir.isEmpty()) {
        baseDir = QDir::currentPath();
    }
    return QDir(baseDir).filePath("mod_audit_cache.json");
}

static QJsonObject reportToJson(const AuditReport& r)
{
    QJsonObject obj;
    obj["jarPath"] = r.jarPath;
    obj["modName"] = r.modName;
    obj["modId"] = r.modId;
    obj["version"] = r.version;
    obj["classCount"] = r.classCount;
    obj["totalClasses"] = r.totalClasses;
    obj["totalFiles"] = r.totalFiles;
    obj["scanTimeMs"] = double(r.scanTimeMs);
    obj["riskScore"] = r.riskScore;
    obj["verdict"] = static_cast<int>(r.verdict);

    QJsonArray natArr;
    for (const auto& lib : r.nativeLibraries) {
        natArr.append(lib);
    }
    obj["nativeLibraries"] = natArr;

    QJsonArray findingsArr;
    for (const auto& f : r.findings) {
        QJsonObject fObj;
        fObj["ruleId"] = f.ruleId;
        fObj["category"] = f.category;
        fObj["severity"] = static_cast<int>(f.severity);
        fObj["title"] = f.title;
        fObj["description"] = f.description;
        fObj["evidence"] = f.evidence;
        fObj["className"] = f.className;
        fObj["score"] = f.score;
        findingsArr.append(fObj);
    }
    obj["findings"] = findingsArr;

    return obj;
}

static AuditReport reportFromJson(const QJsonObject& obj)
{
    AuditReport r;
    r.jarPath = obj["jarPath"].toString();
    r.modName = obj["modName"].toString();
    r.modId = obj["modId"].toString();
    r.version = obj["version"].toString();
    r.classCount = obj["classCount"].toInt();
    r.totalClasses = obj["totalClasses"].toInt();
    r.totalFiles = obj["totalFiles"].toInt();
    r.scanTimeMs = qint64(obj["scanTimeMs"].toDouble());
    r.riskScore = obj["riskScore"].toInt();
    r.verdict = static_cast<AuditVerdict>(obj["verdict"].toInt());

    QJsonArray natArr = obj["nativeLibraries"].toArray();
    for (const auto& val : natArr) {
        r.nativeLibraries.append(val.toString());
    }

    QJsonArray findingsArr = obj["findings"].toArray();
    for (const auto& val : findingsArr) {
        if (!val.isObject()) continue;
        QJsonObject fObj = val.toObject();
        AuditFinding f;
        f.ruleId = fObj["ruleId"].toString();
        f.category = fObj["category"].toString();
        f.severity = static_cast<AuditSeverity>(fObj["severity"].toInt());
        f.title = fObj["title"].toString();
        f.description = fObj["description"].toString();
        f.evidence = fObj["evidence"].toString();
        f.className = fObj["className"].toString();
        f.score = fObj["score"].toInt();
        r.findings.append(f);
    }

    return r;
}

static void loadAuditCacheLocked()
{
    if (s_auditCacheLoaded) return;
    s_auditCacheLoaded = true;

    QString filePath = getModAuditCacheFilePath();
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return;

    QJsonObject entriesObj = doc.object()["entries"].toObject();
    for (auto it = entriesObj.constBegin(); it != entriesObj.constEnd(); ++it) {
        if (!it.value().isObject()) continue;
        QJsonObject entryObj = it.value().toObject();

        CachedAuditEntry entry;
        entry.lastModified = QDateTime::fromMSecsSinceEpoch(qint64(entryObj["lastModified"].toDouble()));
        entry.fileSize = qint64(entryObj["fileSize"].toDouble());
        entry.report = reportFromJson(entryObj["report"].toObject());

        s_auditCache[it.key()] = entry;
    }
}

static void saveAuditCacheLocked()
{
    QString filePath = getModAuditCacheFilePath();
    QDir().mkpath(QFileInfo(filePath).dir().path());
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }

    QJsonObject root;
    root["version"] = 1;

    QJsonObject entriesObj;
    for (auto it = s_auditCache.constBegin(); it != s_auditCache.constEnd(); ++it) {
        QJsonObject entryObj;
        entryObj["lastModified"] = double(it.value().lastModified.toMSecsSinceEpoch());
        entryObj["fileSize"] = double(it.value().fileSize);
        entryObj["report"] = reportToJson(it.value().report);
        entriesObj[it.key()] = entryObj;
    }
    root["entries"] = entriesObj;

    QJsonDocument doc(root);
    file.write(doc.toJson(QJsonDocument::Compact));
    file.commit();
}

static QString getTrustedModsFilePath()
{
    QString baseDir;
    auto* app = dynamic_cast<Application*>(QCoreApplication::instance());
    if (app) {
        baseDir = app->dataRoot();
    }
    if (baseDir.isEmpty()) {
        baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    if (baseDir.isEmpty()) {
        baseDir = QDir::currentPath();
    }
    return QDir(baseDir).filePath("trusted_mods.json");
}

static QMutex s_trustedMutex;
static bool s_trustedLoaded = false;
static QSet<QString> s_trustedIdentifiers;
static QSet<QString> s_trustedFileNames;
static QSet<QString> s_trustedSha256s;
static QHash<QString, QString> s_sha256Cache;

static void loadTrustedModsLocked()
{
    s_trustedIdentifiers.clear();
    s_trustedFileNames.clear();
    s_trustedSha256s.clear();
    QString filePath = getTrustedModsFilePath();
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        s_trustedLoaded = true;
        return;
    }
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isObject()) {
        QJsonArray arr = doc.object()["trusted"].toArray();
        for (const auto& val : arr) {
            if (val.isString()) {
                QString str = val.toString().trimmed().toLower();
                if (str.isEmpty()) continue;
                s_trustedIdentifiers.insert(str);
                if (str.length() == 64 && !str.contains('.')) {
                    s_trustedSha256s.insert(str);
                } else {
                    if (str.endsWith(".disabled")) str.chop(9);
                    s_trustedFileNames.insert(str);
                }
            } else if (val.isObject()) {
                auto obj = val.toObject();
                if (obj.contains("fileName")) {
                    QString fn = obj["fileName"].toString().trimmed().toLower();
                    if (!fn.isEmpty()) {
                        s_trustedIdentifiers.insert(fn);
                        if (fn.endsWith(".disabled")) fn.chop(9);
                        s_trustedFileNames.insert(fn);
                    }
                }
                if (obj.contains("sha256")) {
                    QString hash = obj["sha256"].toString().trimmed().toLower();
                    if (!hash.isEmpty()) {
                        s_trustedIdentifiers.insert(hash);
                        s_trustedSha256s.insert(hash);
                    }
                }
            }
        }
    }
    s_trustedLoaded = true;
}

static QString calculateFileSha256(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (hash.addData(&f)) {
        return QString::fromUtf8(hash.result().toHex()).toLower();
    }
    return {};
}

static inline QString normalizeCacheKey(const QString& path)
{
    if (path.isEmpty()) return {};
    QString p = path;
    p.replace('\\', '/');
    if (p.endsWith(".disabled", Qt::CaseInsensitive)) {
        p.chop(9);
    }
    return p;
}

static bool isWhitelistedPackage(const QString& className)
{
    QString norm = className;
    norm.replace('\\', '/');
    while (norm.startsWith('/')) {
        norm.remove(0, 1);
    }
    if (norm.startsWith("META-INF/versions/", Qt::CaseInsensitive)) {
        int nextSlash = norm.indexOf('/', 18);
        if (nextSlash != -1) {
            norm = norm.mid(nextSlash + 1);
        }
    }
    static const QStringList s_whitelistPrefixes = {
        "com/caoccao/javet/",
        "org/lwjgl/",
        "oshi/",
        "io/netty/",
        "com/sun/jna/",
        "org/apache/",
        "com/google/",
        "org/sqlite/",
        "net/java/dev/jna/",
        "com/moulberry/",
        "dev/isxander/",
        "org/spongepowered/asm/",
        "org/spongepowered/",
        "com/llamalad7/mixinextras/",
        "net/fabricmc/",
        "org/objectweb/asm/"
    };
    for (const auto& prefix : s_whitelistPrefixes) {
        if (norm.startsWith(prefix, Qt::CaseInsensitive) ||
            norm.contains("/" + prefix, Qt::CaseInsensitive)) {
            return true;
        }
    }

    static const QStringList s_whitelistTokens = {
        "euphoriapatches",
        "e4mc",
        "e4steam",
        "spongepowered",
        "mixinextras",
        "fabricmc"
    };
    for (const auto& token : s_whitelistTokens) {
        if (norm.contains(token, Qt::CaseInsensitive)) {
            return true;
        }
    }

    return false;
}

static void addOrMergeFinding(QList<AuditFinding>& findings, const AuditFinding& newFinding)
{
    for (auto& existing : findings) {
        if (existing.ruleId == newFinding.ruleId) {
            if (newFinding.score > existing.score) {
                existing.score = newFinding.score;
            }
            if (newFinding.severity > existing.severity) {
                existing.severity = newFinding.severity;
                existing.title = newFinding.title;
                existing.description = newFinding.description;
            }
            if (!newFinding.className.isEmpty()) {
                QStringList cur = existing.className.split(", ", Qt::SkipEmptyParts);
                if (!cur.contains(newFinding.className) && !cur.contains("...")) {
                    if (cur.size() < 5) {
                        existing.className += ", " + newFinding.className;
                    } else if (cur.size() == 5) {
                        existing.className += ", ...";
                    }
                }
            }
            if (!newFinding.evidence.isEmpty() && !existing.evidence.contains(newFinding.evidence)) {
                if (existing.evidence.length() < 256) {
                    existing.evidence += "; " + newFinding.evidence;
                }
            }
            return;
        }
    }
    findings.append(newFinding);
}

static bool isObfuscatedClassName(const QString& fullPath)
{
    QString p = fullPath;
    p.replace('\\', '/');
    if (p.endsWith(".class", Qt::CaseInsensitive)) {
        p.chop(6);
    }
    while (p.startsWith('/')) {
        p.remove(0, 1);
    }
    if (p.startsWith("META-INF/versions/", Qt::CaseInsensitive)) {
        int nextSlash = p.indexOf('/', 18);
        if (nextSlash != -1) {
            p = p.mid(nextSlash + 1);
        }
    }
    QStringList parts = p.split('/', Qt::SkipEmptyParts);
    if (parts.isEmpty()) return false;

    QString simpleName = parts.last();
    if (simpleName == "package-info" || simpleName == "module-info") {
        return false;
    }

    QString baseName = simpleName;
    int dollarIdx = baseName.indexOf('$');
    if (dollarIdx != -1) {
        baseName = baseName.left(dollarIdx);
    }

    // Class name shorter than 3 characters (e.g. a.class, ns.class)
    if (!baseName.isEmpty() && baseName.length() < 3) {
        return true;
    }

    // Package consists of 1-2 letter directories (e.g. a/b/...)
    if (parts.size() >= 2) {
        bool allPackagePartsShort = true;
        for (int i = 0; i < parts.size() - 1; ++i) {
            if (parts[i].length() > 2) {
                allPackagePartsShort = false;
                break;
            }
        }
        if (allPackagePartsShort) {
            return true;
        }
    }

    return false;
}
} // namespace

bool ModAuditor::hasCachedReport(const QString& jarPath)
{
    if (jarPath.isEmpty()) return false;
    QMutexLocker locker(&s_cacheMutex);
    loadAuditCacheLocked();

    QString key = normalizeCacheKey(jarPath);
    auto it = s_auditCache.find(key);
    if (it == s_auditCache.end()) {
        return false;
    }

    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (it.value().lastCheckedMs > 0 && (now - it.value().lastCheckedMs) < 1000) {
        return true;
    }

    QFileInfo fi(jarPath);
    if (!fi.exists()) {
        s_auditCache.erase(it);
        saveAuditCacheLocked();
        return false;
    }

    if (fi.lastModified() > it.value().lastModified || (it.value().fileSize > 0 && fi.size() != it.value().fileSize)) {
        s_auditCache.erase(it);
        saveAuditCacheLocked();
        return false;
    }

    it.value().lastCheckedMs = now;
    return true;
}

AuditReport ModAuditor::getCachedReport(const QString& jarPath)
{
    if (jarPath.isEmpty()) return {};
    QMutexLocker locker(&s_cacheMutex);
    loadAuditCacheLocked();

    QString key = normalizeCacheKey(jarPath);
    auto it = s_auditCache.constFind(key);
    if (it != s_auditCache.constEnd()) {
        return it.value().report;
    }
    return {};
}

void ModAuditor::setCachedReport(const QString& jarPath, const AuditReport& report)
{
    if (jarPath.isEmpty()) return;
    QMutexLocker locker(&s_cacheMutex);
    loadAuditCacheLocked();

    QString key = normalizeCacheKey(jarPath);
    QFileInfo fi(jarPath);
    CachedAuditEntry entry;
    entry.report = report;
    entry.lastModified = fi.exists() ? fi.lastModified() : QDateTime::currentDateTime();
    entry.fileSize = fi.exists() ? fi.size() : 0;
    entry.lastCheckedMs = QDateTime::currentMSecsSinceEpoch();

    s_auditCache[key] = entry;
    saveAuditCacheLocked();
}

void ModAuditor::clearCache()
{
    QMutexLocker locker(&s_cacheMutex);
    s_auditCache.clear();
    s_auditCacheLoaded = true;
    QString filePath = getModAuditCacheFilePath();
    if (QFile::exists(filePath)) {
        QFile::remove(filePath);
    }
}

QString ModAuditor::getAuditCacheFilePath()
{
    return getModAuditCacheFilePath();
}

QString ModAuditor::verdictToString(AuditVerdict verdict)
{
    switch (verdict) {
        case AuditVerdict::Dangerous:
            return DiagI18n::tr("DANGEROUS", "ОПАСНО");
        case AuditVerdict::Suspicious:
            return DiagI18n::tr("WARNING", "ВНИМАНИЕ");
        case AuditVerdict::Notice:
            return DiagI18n::tr("NOTICE", "ЗАМЕЧАНИЕ");
        case AuditVerdict::Clean:
        default:
            return DiagI18n::tr("Safe (No threats detected)", "Безопасен (Угроз не обнаружено)");
    }
}

QString ModAuditor::verdictToColor(AuditVerdict verdict)
{
    switch (verdict) {
        case AuditVerdict::Dangerous:
            return "#dc3545"; // Red (80-100)
        case AuditVerdict::Suspicious:
            return "#e67e22"; // Orange / Yellow (40-79)
        case AuditVerdict::Notice:
            return "#3498db"; // Grey / Blue (1-39)
        case AuditVerdict::Clean:
        default:
            return "#28a745"; // Green (0)
    }
}

QString ModAuditor::verdictToIconName(AuditVerdict verdict)
{
    switch (verdict) {
        case AuditVerdict::Dangerous:
            return "dialog-error";
        case AuditVerdict::Suspicious:
            return "dialog-warning";
        case AuditVerdict::Notice:
            return "dialog-information";
        case AuditVerdict::Clean:
        default:
            return "security-high";
    }
}

QString ModAuditor::severityToString(AuditSeverity severity)
{
    switch (severity) {
        case AuditSeverity::Critical:
            return DiagI18n::tr("Critical", "Критическая");
        case AuditSeverity::Warning:
            return DiagI18n::tr("Warning", "Предупреждение");
        case AuditSeverity::Notice:
            return DiagI18n::tr("Notice", "Замечание");
        case AuditSeverity::Info:
        default:
            return DiagI18n::tr("Information", "Информация");
    }
}

bool ModAuditor::isModTrusted(const QString& jarPath)
{
    if (jarPath.isEmpty()) return false;
    QMutexLocker locker(&s_trustedMutex);
    if (!s_trustedLoaded) {
        loadTrustedModsLocked();
    }

    QString fileName = jarPath;
    int lastSlash = std::max(fileName.lastIndexOf('/'), fileName.lastIndexOf('\\'));
    if (lastSlash != -1) {
        fileName = fileName.mid(lastSlash + 1);
    }
    if (fileName.endsWith(".disabled", Qt::CaseInsensitive)) {
        fileName.chop(9);
    }
    fileName = fileName.toLower();

    if (s_trustedFileNames.contains(fileName)) {
        return true;
    }

    // Fast exit: if no SHA-256 hashes are trusted, never touch disk or hash files!
    if (s_trustedSha256s.isEmpty()) {
        return false;
    }

    QString sha256 = s_sha256Cache.value(jarPath);
    if (sha256.isEmpty()) {
        sha256 = calculateFileSha256(jarPath);
        if (!sha256.isEmpty()) {
            s_sha256Cache.insert(jarPath, sha256);
        }
    }
    return !sha256.isEmpty() && s_trustedSha256s.contains(sha256);
}

void ModAuditor::setModTrusted(const QString& jarPath, bool trusted)
{
    if (jarPath.isEmpty()) return;
    QMutexLocker locker(&s_trustedMutex);
    if (!s_trustedLoaded) {
        loadTrustedModsLocked();
    }
    QString fileName = jarPath;
    int lastSlash = std::max(fileName.lastIndexOf('/'), fileName.lastIndexOf('\\'));
    if (lastSlash != -1) {
        fileName = fileName.mid(lastSlash + 1);
    }
    if (fileName.endsWith(".disabled", Qt::CaseInsensitive)) {
        fileName.chop(9);
    }

    QString sha256 = s_sha256Cache.value(jarPath);
    if (sha256.isEmpty()) {
        sha256 = calculateFileSha256(jarPath);
        if (!sha256.isEmpty()) {
            s_sha256Cache.insert(jarPath, sha256);
        }
    }

    QString filePath = getTrustedModsFilePath();
    QJsonObject rootObj;
    QFile readFile(filePath);
    if (readFile.open(QIODevice::ReadOnly)) {
        rootObj = QJsonDocument::fromJson(readFile.readAll()).object();
        readFile.close();
    }

    QJsonArray arr = rootObj["trusted"].toArray();
    QJsonArray newArr;

    for (const auto& item : arr) {
        QString itemFile;
        QString itemHash;
        if (item.isString()) {
            itemFile = item.toString();
        } else if (item.isObject()) {
            itemFile = item.toObject()["fileName"].toString();
            itemHash = item.toObject()["sha256"].toString();
        }
        bool matches = (!itemFile.isEmpty() && itemFile.compare(fileName, Qt::CaseInsensitive) == 0) ||
                       (!itemHash.isEmpty() && !sha256.isEmpty() && itemHash.compare(sha256, Qt::CaseInsensitive) == 0);
        if (!matches) {
            newArr.append(item);
        }
    }

    if (trusted) {
        QJsonObject newEntry;
        newEntry["fileName"] = fileName;
        if (!sha256.isEmpty()) {
            newEntry["sha256"] = sha256;
        }
        newEntry["dateAdded"] = QDateTime::currentDateTime().toString(Qt::ISODate);
        newArr.append(newEntry);
    }

    rootObj["version"] = 1;
    rootObj["trusted"] = newArr;

    QDir().mkpath(QFileInfo(filePath).dir().path());
    QSaveFile writeFile(filePath);
    if (writeFile.open(QIODevice::WriteOnly)) {
        writeFile.write(QJsonDocument(rootObj).toJson(QJsonDocument::Indented));
        writeFile.commit();
    }

    loadTrustedModsLocked();
}

QStringList ModAuditor::getTrustedModsList()
{
    QMutexLocker locker(&s_trustedMutex);
    if (!s_trustedLoaded) {
        loadTrustedModsLocked();
    }
    return s_trustedIdentifiers.values();
}

void ModAuditor::clearTrustedModsCache()
{
    QMutexLocker locker(&s_trustedMutex);
    s_trustedLoaded = false;
    s_trustedIdentifiers.clear();
    s_trustedFileNames.clear();
    s_trustedSha256s.clear();
    s_sha256Cache.clear();
}

bool ModAuditor::parseClassConstantPool(const QByteArray& data, ClassFacts& facts)
{
    if (data.size() < 10) return false;

    // Java class magic: 0xCAFEBABE
    const auto* raw = reinterpret_cast<const unsigned char*>(data.constData());
    if (raw[0] != 0xCA || raw[1] != 0xFE || raw[2] != 0xBA || raw[3] != 0xBE) {
        return false;
    }

    int pos = 8; // Skip magic (4), minor (2), major (2)
    if (pos + 2 > data.size()) return false;

    int poolCount = (raw[pos] << 8) | raw[pos + 1];
    pos += 2;

    QHash<int, QString> utf8Map;
    QHash<int, int> stringMap;
    QHash<int, int> classMap;
    QList<QPair<int, int>> methodRefs; // class_idx, name_and_type_idx
    QHash<int, int> nameAndTypeMap;    // idx -> name_idx

    for (int idx = 1; idx < poolCount && pos < data.size(); ++idx) {
        unsigned char tag = raw[pos++];
        switch (tag) {
            case 1: { // CONSTANT_Utf8
                if (pos + 2 > data.size()) return false;
                int len = (raw[pos] << 8) | raw[pos + 1];
                pos += 2;
                if (pos + len > data.size()) return false;
                QString str = QString::fromUtf8(reinterpret_cast<const char*>(raw + pos), len);
                utf8Map.insert(idx, str);
                pos += len;
                break;
            }
            case 3: // CONSTANT_Integer
            case 4: // CONSTANT_Float
                pos += 4;
                break;
            case 5: // CONSTANT_Long
            case 6: // CONSTANT_Double
                pos += 8;
                idx++; // Takes 2 slots in the constant pool!
                break;
            case 7: { // CONSTANT_Class
                if (pos + 2 > data.size()) return false;
                int nameIdx = (raw[pos] << 8) | raw[pos + 1];
                pos += 2;
                classMap.insert(idx, nameIdx);
                break;
            }
            case 8: { // CONSTANT_String
                if (pos + 2 > data.size()) return false;
                int strIdx = (raw[pos] << 8) | raw[pos + 1];
                pos += 2;
                stringMap.insert(idx, strIdx);
                break;
            }
            case 9:  // CONSTANT_Fieldref
            case 10: // CONSTANT_Methodref
            case 11: { // CONSTANT_InterfaceMethodref
                if (pos + 4 > data.size()) return false;
                int classIdx = (raw[pos] << 8) | raw[pos + 1];
                int natIdx = (raw[pos + 2] << 8) | raw[pos + 3];
                pos += 4;
                if (tag == 10 || tag == 11) {
                    methodRefs.append(qMakePair(classIdx, natIdx));
                }
                break;
            }
            case 12: { // CONSTANT_NameAndType
                if (pos + 4 > data.size()) return false;
                int nameIdx = (raw[pos] << 8) | raw[pos + 1];
                pos += 4; // nameIdx (2) + descIdx (2)
                nameAndTypeMap.insert(idx, nameIdx);
                break;
            }
            case 15: // CONSTANT_MethodHandle
                pos += 3;
                break;
            case 16: // CONSTANT_MethodType
                pos += 2;
                break;
            case 17: // CONSTANT_Dynamic
            case 18: // CONSTANT_InvokeDynamic
                pos += 4;
                break;
            case 19: // CONSTANT_Module
            case 20: // CONSTANT_Package
                pos += 2;
                break;
            default:
                // Unknown tag, abort parsing this class
                return false;
        }
    }

    // Collect string constants
    QSet<QString> uniqueStrings;
    for (int strIdx : stringMap.values()) {
        QString val = utf8Map.value(strIdx);
        if (!val.isEmpty()) {
            uniqueStrings.insert(val);
        }
    }
    // Also include any Utf8 constants that look like URLs, webhooks, stealers, persistence or file paths
    for (const QString& val : utf8Map.values()) {
        if (val.length() >= 3 && (
            val.contains("://") ||
            val.contains("discord", Qt::CaseInsensitive) ||
            val.contains("telegram", Qt::CaseInsensitive) ||
            val.contains("webhook", Qt::CaseInsensitive) ||
            val.contains("transfer.sh", Qt::CaseInsensitive) ||
            val.contains("gofile.io", Qt::CaseInsensitive) ||
            val.contains("accounts.json", Qt::CaseInsensitive) ||
            val.contains("profiles.json", Qt::CaseInsensitive) ||
            val.contains(".lunarclient", Qt::CaseInsensitive) ||
            val.contains("leveldb", Qt::CaseInsensitive) ||
            val.contains(".ldb", Qt::CaseInsensitive) ||
            val.contains("local state", Qt::CaseInsensitive) ||
            val.contains("login data", Qt::CaseInsensitive) ||
            val.contains("cookies", Qt::CaseInsensitive) ||
            val.contains("wallet", Qt::CaseInsensitive) ||
            val.contains("metamask", Qt::CaseInsensitive) ||
            val.contains("powershell", Qt::CaseInsensitive) ||
            val.contains("cmd", Qt::CaseInsensitive) ||
            val.contains("certutil", Qt::CaseInsensitive) ||
            val.contains("currentversion", Qt::CaseInsensitive) ||
            val.contains("startup", Qt::CaseInsensitive) ||
            val.contains("autostart", Qt::CaseInsensitive) ||
            val.contains("crontab", Qt::CaseInsensitive) ||
            val.contains("bashrc", Qt::CaseInsensitive) ||
            val.contains(".exe") || val.contains(".dll") || val.contains(".so")
        )) {
            uniqueStrings.insert(val);
        }
    }
    facts.stringConstants = uniqueStrings.values();

    // Collect class references
    QSet<QString> uniqueClassRefs;
    for (int nameIdx : classMap.values()) {
        QString cName = utf8Map.value(nameIdx);
        if (!cName.isEmpty()) {
            uniqueClassRefs.insert(cName);
        }
    }
    facts.classRefs = uniqueClassRefs.values();

    // Collect method refs: Owner.methodName
    for (const auto& pair : methodRefs) {
        int classIdx = pair.first;
        int natIdx = pair.second;

        int classNameIdx = classMap.value(classIdx, 0);
        int nameIdx = nameAndTypeMap.value(natIdx, 0);

        QString className = utf8Map.value(classNameIdx);
        QString methodName = utf8Map.value(nameIdx);

        if (!className.isEmpty() && !methodName.isEmpty()) {
            facts.methodRefs.append(className + "." + methodName);
        }
    }

    if (facts.className.isEmpty() && pos + 4 <= data.size()) {
        int thisClassIdx = (raw[pos + 2] << 8) | raw[pos + 3];
        int thisClassNameIdx = classMap.value(thisClassIdx, 0);
        QString internalName = utf8Map.value(thisClassNameIdx);
        if (!internalName.isEmpty()) {
            facts.className = internalName + ".class";
        }
    }

    return true;
}

void ModAuditor::scanFacts(const ClassFacts& facts, QList<AuditFinding>& findings)
{
    bool isWhitelisted = isWhitelistedPackage(facts.className);

    // =========================================================================
    // 1. String Constant Heuristics
    // =========================================================================

    // Rule 2: Real Stealers (Minecraft/Launcher accounts, Discord/Browsers, Crypto Wallets)
    // Category: "Кража данных (Стилер)" (+95 pts)
    static const QRegularExpression s_stealerRe(
        R"((?:launcher_accounts\.json|launcher_profiles\.json|feather[/\\]accounts\.json|\.lunarclient|accounts\.json|leveldb|\.ldb|local\s+state|login\s+data|network[/\\]cookies|discordcanary|discordptb|exodus\.wallet|electrum[/\\]wallets|metamask|appdata[/\\]roaming))",
        QRegularExpression::CaseInsensitiveOption
    );

    // Rule 3: Data Exfiltration Endpoints (strictly /api/webhooks/ for Discord and api.telegram.org/bot for Telegram)
    // Completely ignore normal invite links like discord.gg/ or discord.com/invite
    static const QRegularExpression s_exfilRe(
        R"((?:(?:\/api\/webhooks\/|\/api\/v\d+\/webhooks\/)|api\.telegram\.org\/bot|transfer\.sh|gofile\.io))",
        QRegularExpression::CaseInsensitiveOption
    );

    // Rule 4: System Persistence / Autostart (Registry, Startup, Task Scheduler, Systemd, Crontab, Hidden Powershell)
    // Category: "Модификация системы / Автозагрузка" (+95 pts)
    static const QRegularExpression s_persistenceRe(
        R"((?:currentversion[/\\]run|start\s*menu[/\\]programs[/\\]startup|\.config\/autostart|\/?systemd\/user\/|\/?etc\/crontab|\.bashrc|certutil(?:\.exe)?))",
        QRegularExpression::CaseInsensitiveOption
    );

    static const QRegularExpression s_powershellHiddenRe(
        R"(powershell(?:\.exe)?.*?(?:-windowstyle\s+hidden|-w\s+hidden|-enc|-encodedcommand))",
        QRegularExpression::CaseInsensitiveOption
    );

    static const QRegularExpression s_cmdExecRe(
        R"(cmd(?:\.exe)?\s*\/c)",
        QRegularExpression::CaseInsensitiveOption
    );

    // Keylogger strings (Win32 API)
    static const QRegularExpression s_keyloggerRe(
        R"((?:GetAsyncKeyState|SetWindowsHookEx[AW]?|GetKeyboardState|GetKeyState|user32\.dll))",
        QRegularExpression::CaseInsensitiveOption
    );

    // JNDI Injection (Log4Shell)
    static const QRegularExpression s_jndiRe(
        R"(\$\{jndi:(?:ldap|ldaps|rmi|dns)://)",
        QRegularExpression::CaseInsensitiveOption
    );

    // Raw IP Address
    static const QRegularExpression s_rawIpRe(
        R"(https?://(?:\d{1,3}\.){3}\d{1,3}(?::\d{2,5})?(?:/\S*)?)",
        QRegularExpression::CaseInsensitiveOption
    );

    QSet<QString> reportedStealers;
    QSet<QString> reportedExfil;
    QSet<QString> reportedPersistence;

    for (const QString& str : facts.stringConstants) {
        if (str.length() < 3 || str.length() > 2048) continue;

        // --- Rule 2: Real Stealers ---
        auto mSteal = s_stealerRe.match(str);
        if (mSteal.hasMatch()) {
            QString matchedKey = mSteal.captured(0).toLower();
            if (!reportedStealers.contains(matchedKey)) {
                reportedStealers.insert(matchedKey);
                AuditFinding f;
                f.ruleId = "data_theft_stealer";
                f.category = DiagI18n::tr("Data Theft (Stealer)", "Кража данных (Стилер)");
                f.severity = AuditSeverity::Critical;
                f.score = 95;
                f.title = DiagI18n::tr("Credential / Session Theft Target", "Попытка кражи учетных данных или сессий");
                f.description = DiagI18n::tr("Detected access to sensitive files or tokens: %1 (used by Minecraft launchers, browsers, Discord, or crypto wallets).",
                                             "Обнаружено обращение к файлам учетных данных или токенов: %1 (используется лаунчерами Minecraft, браузерами, Discord или криптокошельками).")
                                    .arg(mSteal.captured(0));
                f.evidence = str;
                f.className = facts.className;
                addOrMergeFinding(findings, f);
            }
        }

        // --- Rule 3: Data Exfiltration ---
        auto mExfil = s_exfilRe.match(str);
        if (mExfil.hasMatch()) {
            QString matchedKey = mExfil.captured(0).toLower();
            if (!reportedExfil.contains(matchedKey)) {
                reportedExfil.insert(matchedKey);
                AuditFinding f;
                f.ruleId = "data_exfil";
                f.category = DiagI18n::tr("Data Exfiltration", "Скрытая отправка данных");
                f.severity = AuditSeverity::Critical;
                f.score = 80;
                f.title = DiagI18n::tr("Exfiltration Service Endpoint", "Канал скрытой отправки данных");
                f.description = DiagI18n::tr("Bytecode contains a data exfiltration channel: %1 (Discord webhook/API, Telegram bot, transfer.sh, gofile.io).",
                                             "В коде обнаружен адрес скрытой передачи данных: %1 (Discord webhook/API, Telegram bot, transfer.sh, gofile.io).")
                                    .arg(mExfil.captured(0));
                f.evidence = str;
                f.className = facts.className;
                addOrMergeFinding(findings, f);
            }
        }

        // --- Rule 4: System Persistence / Autostart ---
        auto mPersist = s_persistenceRe.match(str);
        auto mPsHidden = s_powershellHiddenRe.match(str);
        auto mCmd = s_cmdExecRe.match(str);

        bool isBenignBrowserCmd = str.contains("cmd", Qt::CaseInsensitive) &&
                                  str.contains("start", Qt::CaseInsensitive) &&
                                  (str.contains("http://") || str.contains("https://"));

        if (mPersist.hasMatch() || mPsHidden.hasMatch() || (mCmd.hasMatch() && !isBenignBrowserCmd)) {
            QString matched = mPersist.hasMatch() ? mPersist.captured(0) : (mPsHidden.hasMatch() ? mPsHidden.captured(0) : mCmd.captured(0));
            QString matchedKey = matched.toLower();
            if (!reportedPersistence.contains(matchedKey)) {
                reportedPersistence.insert(matchedKey);
                AuditFinding f;
                f.ruleId = "system_persistence";
                f.category = DiagI18n::tr("System Persistence / Autostart", "Модификация системы / Автозагрузка");
                f.severity = AuditSeverity::Critical;
                f.score = 95;
                f.title = DiagI18n::tr("System Autostart / Persistence Modification", "Модификация автозагрузки / Закрепление в системе");
                f.description = DiagI18n::tr("Detected attempt to gain persistence across reboots via autostart directories, registry keys, cron, or hidden scripts (%1).",
                                             "Обнаружена попытка закрепиться в автозагрузке ОС через реестр Windows, скрипты автозапуска, systemd или cron (%1).")
                                    .arg(matched);
                f.evidence = str;
                f.className = facts.className;
                addOrMergeFinding(findings, f);
            }
        }

        // --- Keylogger native API strings ---
        auto mKey = s_keyloggerRe.match(str);
        if (mKey.hasMatch() && !isWhitelisted) {
            AuditFinding f;
            f.ruleId = "keylogger_native";
            f.category = DiagI18n::tr("Keylogger", "Кейлоггер");
            f.severity = AuditSeverity::Critical;
            f.score = 90;
            f.title = DiagI18n::tr("Global Keyboard Hook", "Глобальный перехват клавиатуры");
            f.description = DiagI18n::tr("Native OS functions detected for intercepting keystrokes outside the game.",
                                         "Найдены вызовы нативных функций ОС для перехвата нажатий клавиш вне игры.");
            f.evidence = mKey.captured(0);
            f.className = facts.className;
            addOrMergeFinding(findings, f);
        }

        // --- JNDI Injection (Log4Shell) ---
        auto mJndi = s_jndiRe.match(str);
        if (mJndi.hasMatch()) {
            AuditFinding f;
            f.ruleId = "jndi_injection";
            f.category = DiagI18n::tr("Remote Execution (RCE)", "Удалённое исполнение (RCE)");
            f.severity = AuditSeverity::Critical;
            f.score = 95;
            f.title = DiagI18n::tr("JNDI Injection String (Log4Shell)", "JNDI-инъекция (Log4Shell)");
            f.description = DiagI18n::tr("Remote code execution string detected via JNDI/LDAP lookup.",
                                         "Обнаружена строка вызова удалённой загрузки кода через JNDI/LDAP.");
            f.evidence = mJndi.captured(0);
            f.className = facts.className;
            addOrMergeFinding(findings, f);
        }

        // --- Raw IP address ---
        auto mIp = s_rawIpRe.match(str);
        if (mIp.hasMatch() && !str.contains("127.0.0.1") && !str.contains("0.0.0.0") && !isWhitelisted) {
            AuditFinding f;
            f.ruleId = "raw_ip";
            f.category = DiagI18n::tr("Network", "Сеть");
            f.severity = AuditSeverity::Notice;
            f.score = 5;
            f.title = DiagI18n::tr("Direct IP Address Connection", "Прямой IP-адрес");
            f.description = DiagI18n::tr("Connection to a server directly via raw IP address instead of a domain name.",
                                         "Обращение к серверу напрямую по IP-адресу вместо доменного имени.");
            f.evidence = mIp.captured(0);
            f.className = facts.className;
            addOrMergeFinding(findings, f);
        }
    }

    // =========================================================================
    // 2. Method References & Whitelist Checks
    // =========================================================================

    bool reportedProcessExec = false;
    bool reportedJNativeHook = false;
    bool reportedJndiLookup = false;
    bool isBrowserOpener = false;
    bool isHardwareProbe = false;
    bool isFileManager = false;
    QString benignEvidence;

    static const QRegularExpression browserStringsRe(
        R"((?:xdg-open|gio(?:\s+open)?|sensible-browser|x-www-browser|rundll32(?:\.exe)?|url\.dll|FileProtocolHandler|open\s+-a|openurl|openuri|browseTo))",
        QRegularExpression::CaseInsensitiveOption
    );
    static const QRegularExpression browserClassRe(
        R"((?:browse|desktop|browser|url|uri|link|screen|gui|xdg))",
        QRegularExpression::CaseInsensitiveOption
    );
    static const QRegularExpression probeStringsRe(
        R"((?:lspci|glxinfo|vulkaninfo|wmic(?:\.exe)?|dxdiag|nvidia-smi|sysctl|uname|/proc/cpuinfo|/proc/|/sys/|system_profiler|sw_vers))",
        QRegularExpression::CaseInsensitiveOption
    );
    static const QRegularExpression probeClassRe(
        R"((?:probe|adapter|graphics|gpu|environment|hardware|driver|device|systeminfo|compat|vendor))",
        QRegularExpression::CaseInsensitiveOption
    );
    static const QRegularExpression fileManagerStringsRe(
        R"((?:(?:^|[^a-zA-Z0-9_])explorer(?:\.exe)?(?:$|[^a-zA-Z0-9_])|nautilus|dolphin|thunar|showInFolder|openFolder|openDirectory))",
        QRegularExpression::CaseInsensitiveOption
    );
    static const QRegularExpression fileManagerClassRe(
        R"((?:folder|directory|explorer|filemanager))",
        QRegularExpression::CaseInsensitiveOption
    );

    for (const QString& method : facts.methodRefs) {
        // Standard Java Desktop link/folder opener API (Desktop.browse, Desktop.open, Desktop.getDesktop)
        if (method.startsWith("java/awt/Desktop.browse") ||
            method.startsWith("java/awt/Desktop.open") ||
            method.startsWith("java/awt/Desktop.getDesktop") ||
            method.startsWith("java/awt/Desktop.edit")) {
            isBrowserOpener = true;
            AuditFinding f;
            f.ruleId = "benign_desktop_api";
            f.category = DiagI18n::tr("Safe Utility Call", "Штатный вызов");
            f.severity = AuditSeverity::Info;
            f.score = 0;
            f.title = DiagI18n::tr("Desktop Helper (java.awt.Desktop)", "Штатный вызов java.awt.Desktop");
            f.description = DiagI18n::tr("The mod uses standard Java Desktop API to open URLs or files.",
                                         "Мод использует стандартный Java Desktop API для открытия ссылок или файлов.");
            f.evidence = method;
            f.className = facts.className;
            addOrMergeFinding(findings, f);
            continue;
        }

        if (!reportedJNativeHook && method.contains("org/jnativehook")) {
            reportedJNativeHook = true;
            AuditFinding f;
            f.ruleId = "jnativehook_lib";
            f.category = DiagI18n::tr("Keylogger", "Кейлоггер");
            f.severity = isWhitelisted ? AuditSeverity::Info : AuditSeverity::Critical;
            f.score = isWhitelisted ? 0 : 80;
            f.title = DiagI18n::tr("JNativeHook Library Usage", "Использование библиотеки JNativeHook");
            f.description = DiagI18n::tr("Global keyboard and mouse hook library detected.",
                                         "Подключение глобальных хуков клавиатуры и мыши.");
            f.evidence = method;
            f.className = facts.className;
            addOrMergeFinding(findings, f);
        } else if (!reportedProcessExec && (method.startsWith("java/lang/Runtime.exec") || method.startsWith("java/lang/ProcessBuilder.start") || method.contains("ProcessBuilder.<init>"))) {
            reportedProcessExec = true;

            // Whitelisted library? Award 0 score, Info status
            if (isWhitelisted) {
                AuditFinding f;
                f.ruleId = "whitelisted_lib_call";
                f.category = DiagI18n::tr("Safe Utility Call", "Штатный вызов");
                f.severity = AuditSeverity::Info;
                f.score = 0;
                f.title = DiagI18n::tr("Whitelisted Library System Call", "Штатный системный вызов библиотеки");
                f.description = DiagI18n::tr("System command execution inside a known legitimate library (%1).",
                                             "Системный вызов находится внутри доверенной библиотеки (%1).").arg(facts.className);
                f.evidence = method;
                f.className = facts.className;
                addOrMergeFinding(findings, f);
                continue;
            }

            for (const QString& str : facts.stringConstants) {
                auto mBrowser = browserStringsRe.match(str);
                if (mBrowser.hasMatch()) {
                    isBrowserOpener = true;
                    benignEvidence = mBrowser.captured(0);
                    break;
                }
                auto mProbe = probeStringsRe.match(str);
                if (mProbe.hasMatch()) {
                    isHardwareProbe = true;
                    benignEvidence = mProbe.captured(0);
                    break;
                }
                auto mFile = fileManagerStringsRe.match(str);
                if (mFile.hasMatch()) {
                    isFileManager = true;
                    benignEvidence = mFile.captured(0);
                    break;
                }
            }

            if (!isBrowserOpener && browserClassRe.match(facts.className).hasMatch()) {
                isBrowserOpener = true;
                if (benignEvidence.isEmpty()) benignEvidence = facts.className;
            }
            if (!isHardwareProbe && probeClassRe.match(facts.className).hasMatch()) {
                isHardwareProbe = true;
                if (benignEvidence.isEmpty()) benignEvidence = facts.className;
            }
            if (!isFileManager && fileManagerClassRe.match(facts.className).hasMatch()) {
                isFileManager = true;
                if (benignEvidence.isEmpty()) benignEvidence = facts.className;
            }

            if (isBrowserOpener) {
                AuditFinding f;
                f.ruleId = "benign_browser_open";
                f.category = DiagI18n::tr("Safe Utility Call", "Штатный вызов");
                f.severity = AuditSeverity::Info;
                f.score = 0;
                f.title = DiagI18n::tr("Browser Link Opener (xdg-open / desktop)", "Открытие ссылок в браузере (xdg-open / desktop)");
                f.description = DiagI18n::tr("The mod uses standard OS utilities to open web links or documentation in the default browser.",
                                             "Мод использует стандартную утилиту ОС для открытия веб-ссылок или документации в браузере по умолчанию.");
                f.evidence = benignEvidence.isEmpty() ? method : QString("%1 (%2)").arg(method, benignEvidence);
                f.className = facts.className;
                addOrMergeFinding(findings, f);
            } else if (isHardwareProbe) {
                AuditFinding f;
                f.ruleId = "benign_hardware_probe";
                f.category = DiagI18n::tr("Safe Utility Call", "Штатный вызов");
                f.severity = AuditSeverity::Info;
                f.score = 0;
                f.title = DiagI18n::tr("Hardware / GPU Probe (lspci / wmic)", "Диагностика оборудования / GPU (lspci / wmic)");
                f.description = DiagI18n::tr("The mod queries GPU or system specifications for graphics compatibility fixes.",
                                             "Мод опрашивает модель видеокарты или конфигурацию системы для применения графических фиксов.");
                f.evidence = benignEvidence.isEmpty() ? method : QString("%1 (%2)").arg(method, benignEvidence);
                f.className = facts.className;
                addOrMergeFinding(findings, f);
            } else if (isFileManager) {
                AuditFinding f;
                f.ruleId = "benign_file_manager";
                f.category = DiagI18n::tr("Safe Utility Call", "Штатный вызов");
                f.severity = AuditSeverity::Info;
                f.score = 0;
                f.title = DiagI18n::tr("File Manager Helper", "Открытие папки в файловом менеджере");
                f.description = DiagI18n::tr("The mod opens a folder or highlights a file in the system file manager.",
                                             "Мод открывает каталог или выделяет файл в системном проводнике.");
                f.evidence = benignEvidence.isEmpty() ? method : QString("%1 (%2)").arg(method, benignEvidence);
                f.className = facts.className;
                addOrMergeFinding(findings, f);
            } else {
                AuditFinding f;
                f.ruleId = "process_exec_unknown";
                f.category = DiagI18n::tr("OS Commands", "Команды ОС");
                f.severity = AuditSeverity::Notice;
                f.score = 15;
                f.title = DiagI18n::tr("External Process Execution", "Запуск внешнего процесса");
                f.description = DiagI18n::tr("The mod invokes an external system command via ProcessBuilder or Runtime.exec. Check if this mod requires external tools.",
                                             "Мод запускает внешнюю команду через ProcessBuilder или Runtime.exec. Проверьте, требуются ли моду внешние инструменты.");
                f.evidence = method;
                f.className = facts.className;
                addOrMergeFinding(findings, f);
            }
        } else if (!reportedJndiLookup && method.contains("javax/naming/InitialContext.lookup")) {
            reportedJndiLookup = true;
            AuditFinding f;
            f.ruleId = "jndi_lookup";
            f.category = DiagI18n::tr("JNDI", "JNDI");
            f.severity = isWhitelisted ? AuditSeverity::Info : AuditSeverity::Warning;
            f.score = isWhitelisted ? 0 : 30;
            f.title = DiagI18n::tr("JNDI / Naming Lookup", "JNDI / Naming Lookup");
            f.description = DiagI18n::tr("External directory naming lookup via JNDI.",
                                         "Вызов внешнего каталога имен через JNDI.");
            f.evidence = method;
            f.className = facts.className;
            addOrMergeFinding(findings, f);
        }
    }

    // =========================================================================
    // 3. Rule 5: Combo Rules (Contextual intra-class analysis)
    // =========================================================================

    if (!isWhitelisted) {
        // Detect ClassLoader / dynamic code loading
        bool hasClassLoader = false;
        for (const auto& cRef : facts.classRefs) {
            if (cRef == "java/net/URLClassLoader") {
                hasClassLoader = true;
                break;
            }
        }
        if (!hasClassLoader) {
            for (const auto& mRef : facts.methodRefs) {
                if (mRef.contains("URLClassLoader") || mRef.contains("defineClass")) {
                    hasClassLoader = true;
                    break;
                }
            }
        }
        if (!hasClassLoader) {
            for (const auto& s : facts.stringConstants) {
                if (s == "java.net.URLClassLoader" || s == "URLClassLoader" || s == "defineClass") {
                    hasClassLoader = true;
                    break;
                }
            }
        }

        // Detect Explicit Remote Network capabilities for Stage 0 Downloader:
        // http://, https://, raw IP address, or java/net/Socket
        bool hasExplicitRemoteNetwork = false;
        for (const auto& cRef : facts.classRefs) {
            if (cRef == "java/net/Socket" || cRef == "java/net/ssl/SSLSocket") {
                hasExplicitRemoteNetwork = true;
                break;
            }
        }
        if (!hasExplicitRemoteNetwork) {
            for (const auto& mRef : facts.methodRefs) {
                if (mRef.startsWith("java/net/Socket.") || mRef.startsWith("java/net/ssl/SSLSocket.")) {
                    hasExplicitRemoteNetwork = true;
                    break;
                }
            }
        }
        if (!hasExplicitRemoteNetwork) {
            for (const auto& str : facts.stringConstants) {
                if (str.startsWith("http://", Qt::CaseInsensitive) ||
                    str.startsWith("https://", Qt::CaseInsensitive) ||
                    str.contains("http://", Qt::CaseInsensitive) ||
                    str.contains("https://", Qt::CaseInsensitive)) {
                    hasExplicitRemoteNetwork = true;
                    break;
                }
                if (s_rawIpRe.match(str).hasMatch()) {
                    hasExplicitRemoteNetwork = true;
                    break;
                }
            }
        }

        // Detect General Network capabilities (including HttpURLConnection / URL)
        bool hasNetwork = hasExplicitRemoteNetwork;
        if (!hasNetwork) {
            for (const auto& cRef : facts.classRefs) {
                if (cRef == "java/net/URL" ||
                    cRef == "java/net/HttpURLConnection" ||
                    cRef == "java/net/HttpsURLConnection" ||
                    cRef.contains("HttpClient") ||
                    cRef.contains("HttpRequest")) {
                    hasNetwork = true;
                    break;
                }
            }
        }
        if (!hasNetwork) {
            for (const auto& mRef : facts.methodRefs) {
                if (mRef.startsWith("java/net/URL.") ||
                    mRef.startsWith("java/net/HttpURLConnection.") ||
                    mRef.startsWith("java/net/HttpsURLConnection.") ||
                    mRef.contains("HttpClient")) {
                    hasNetwork = true;
                    break;
                }
            }
        }

        // Detect real process execution (ignoring benign link openers, probes and file managers)
        bool hasRealProcess = reportedProcessExec && !isBrowserOpener && !isHardwareProbe && !isFileManager;
        if (!reportedProcessExec) {
            for (const auto& cRef : facts.classRefs) {
                if (cRef == "java/lang/ProcessBuilder") {
                    for (const QString& str : facts.stringConstants) {
                        if (browserStringsRe.match(str).hasMatch()) isBrowserOpener = true;
                        if (probeStringsRe.match(str).hasMatch()) isHardwareProbe = true;
                        if (fileManagerStringsRe.match(str).hasMatch()) isFileManager = true;
                    }
                    if (!isBrowserOpener && !isHardwareProbe && !isFileManager) {
                        hasRealProcess = true;
                    }
                    break;
                }
            }
        }

        // Aggravating factors check:
        // 1. Obfuscated class name (< 3 chars, e.g. a.class, ns.class)
        bool isObf = isObfuscatedClassName(facts.className);

        // 2. Hidden shell commands (powershell, cmd.exe, -w hidden, -enc, /bin/sh, curl , wget )
        static const QRegularExpression s_aggravatingShellRe(
            R"((?:powershell(?:\.exe)?|cmd(?:\.exe)?\s*\/c|-w\s+hidden|-windowstyle\s+hidden|-enc\b|-encodedcommand\b|\/bin\/sh|\/bin\/bash|curl\s+|wget\s+))",
            QRegularExpression::CaseInsensitiveOption
        );
        bool hasStealthShell = false;
        for (const QString& str : facts.stringConstants) {
            if (s_aggravatingShellRe.match(str).hasMatch()) {
                bool isBenignBrowser = str.contains("cmd", Qt::CaseInsensitive) &&
                                       str.contains("start", Qt::CaseInsensitive) &&
                                       (str.contains("http://") || str.contains("https://"));
                if (!isBenignBrowser) {
                    hasStealthShell = true;
                    break;
                }
            }
        }

        // 3. Base64 + Method.invoke
        bool hasBase64 = false;
        for (const auto& cRef : facts.classRefs) {
            if (cRef.contains("Base64")) {
                hasBase64 = true;
                break;
            }
        }
        if (!hasBase64) {
            for (const auto& mRef : facts.methodRefs) {
                if (mRef.contains("Base64")) {
                    hasBase64 = true;
                    break;
                }
            }
        }
        bool hasMethodInvoke = false;
        for (const auto& mRef : facts.methodRefs) {
            if (mRef.contains("java/lang/reflect/Method.invoke")) {
                hasMethodInvoke = true;
                break;
            }
        }
        bool hasBase64Invoke = hasBase64 && hasMethodInvoke;

        // 4. Stealer paths or webhooks
        bool hasStealerOrWebhook = !reportedStealers.isEmpty() || !reportedExfil.isEmpty();
        if (!hasStealerOrWebhook) {
            for (const QString& str : facts.stringConstants) {
                if (s_stealerRe.match(str).hasMatch() || s_exfilRe.match(str).hasMatch()) {
                    hasStealerOrWebhook = true;
                    break;
                }
            }
        }

        bool hasClassAggravators = isObf || hasStealthShell || hasBase64Invoke || hasStealerOrWebhook;

        // Combo 5.1: Stage 0 Downloader (URLClassLoader/defineClass + Explicit Remote Network)
        // Clean code = 40 pts (Warning), Aggravated code = 95 pts (Critical)
        if (hasClassLoader && hasExplicitRemoteNetwork) {
            AuditFinding f;
            f.ruleId = "combo_stage0_downloader";
            f.category = DiagI18n::tr("Hidden Code Loading", "Скрытая загрузка кода");
            f.severity = hasClassAggravators ? AuditSeverity::Critical : AuditSeverity::Warning;
            f.score = hasClassAggravators ? 95 : 40;
            f.title = DiagI18n::tr("Dynamic Code Downloader (Stage 0)", "Скрытая загрузка кода (Stage 0 Downloader)");
            f.description = DiagI18n::tr("The class combines remote network connections (http/https/socket) with dynamic ClassLoader instantiation (URLClassLoader / defineClass), commonly used to fetch and run remote payloads (Fractureiser pattern).",
                                         "Класс объединяет явные сетевые соединения (http/https/socket) с динамической загрузкой байткода (URLClassLoader / defineClass), что типично для поэтапной загрузки вирусов (паттерн Fractureiser).");
            f.evidence = QString("URLClassLoader/defineClass + Remote Network in %1").arg(facts.className);
            f.className = facts.className;
            addOrMergeFinding(findings, f);
        }

        // Combo 5.2: Network Dropper (Network + Process execution)
        // Clean code = 40 pts (Warning), Aggravated code = 95 pts (Critical)
        if (hasNetwork && hasRealProcess) {
            AuditFinding f;
            f.ruleId = "combo_network_dropper";
            f.category = DiagI18n::tr("Dropper (Download & Execute)", "Загрузка и запуск файла");
            f.severity = hasClassAggravators ? AuditSeverity::Critical : AuditSeverity::Warning;
            f.score = hasClassAggravators ? 95 : 40;
            f.title = DiagI18n::tr("Network Dropper (Download & Execute)", "Загрузка и запуск файла");
            f.description = DiagI18n::tr("The class combines network operations with process launching (ProcessBuilder / Runtime.exec), characteristic of malware droppers that download an executable and run it.",
                                         "Класс объединяет работу с сетью и запуск внешних процессов ОС (ProcessBuilder / Runtime.exec), что характерно для дропперов (скачивание исполняемого файла и его запуск).");
            f.evidence = QString("Network + Process execution in %1").arg(facts.className);
            f.className = facts.className;
            addOrMergeFinding(findings, f);
        }

        // Combo 5.3: Obfuscation + Dangerous Action
        bool hasDangerous = hasRealProcess || hasClassLoader || hasBase64Invoke;

        if (isObf && hasDangerous) {
            AuditFinding f;
            f.ruleId = "combo_obfuscated_danger";
            f.category = DiagI18n::tr("Suspicious Obfuscation", "Подозрительная обфускация");
            f.severity = AuditSeverity::Warning;
            f.score = 40;
            f.title = DiagI18n::tr("Obfuscated Class with Dangerous Calls", "Подозрительная обфускация");
            f.description = DiagI18n::tr("Class with a suspicious or shortened name (%1) performs high-risk operations (ProcessBuilder, Runtime.exec, URLClassLoader, or Base64 reflection invoke).",
                                         "Класс с подозрительно коротким или обфусцированным именем (%1) выполняет высокорисковые операции (запуск процессов, URLClassLoader или Base64 дешифрование с вызовом через reflection).")
                                .arg(facts.className);
            QString dangerType;
            if (hasRealProcess) dangerType += "Process Execution; ";
            if (hasClassLoader) dangerType += "URLClassLoader/defineClass; ";
            if (hasBase64Invoke) dangerType += "Base64+Method.invoke; ";
            f.evidence = QString("Obfuscated class '%1' -> %2").arg(facts.className, dangerType.trimmed());
            f.className = facts.className;
            addOrMergeFinding(findings, f);
        }
    }
}

AuditReport ModAuditor::auditJar(const QString& jarPath, bool forceScan)
{
    if (!forceScan && hasCachedReport(jarPath)) {
        return getCachedReport(jarPath);
    }

    QElapsedTimer timer;
    timer.start();

    AuditReport report;
    report.jarPath = jarPath;
    QFileInfo fi(jarPath);
    report.modName = fi.fileName();

    MMCZip::ArchiveReader reader(jarPath);

    // Read metadata and classes
    (void)reader.parse([&report](MMCZip::ArchiveReader::File* f) -> Result<> {
        report.totalFiles++;
        QString name = f->filename();

        // Check native binaries
        if (name.endsWith(".dll", Qt::CaseInsensitive) ||
            name.endsWith(".so", Qt::CaseInsensitive) ||
            name.endsWith(".dylib", Qt::CaseInsensitive)) {
            report.nativeLibraries.append(name);
        }

        // Read metadata: Fabric / Quilt
        if (name == "fabric.mod.json" || name == "quilt.mod.json") {
            auto dataRes = f->readAll();
            if (dataRes.has_value()) {
                auto doc = QJsonDocument::fromJson(dataRes.value());
                if (doc.isObject()) {
                    auto obj = doc.object();
                    if (obj.contains("id")) report.modId = obj["id"].toString();
                    if (obj.contains("name")) report.modName = obj["name"].toString(report.modName);
                    if (obj.contains("version")) report.version = obj["version"].toString();
                }
            }
            return Result<>();
        }

        // Read metadata: Forge / NeoForge mods.toml
        if (name == "META-INF/mods.toml") {
            auto dataRes = f->readAll();
            if (dataRes.has_value()) {
                QString toml = QString::fromUtf8(dataRes.value());
                static const QRegularExpression dispNameRe(R"(displayName\s*=\s*["']([^"']+)["'])");
                static const QRegularExpression modIdRe(R"(modId\s*=\s*["']([^"']+)["'])");
                static const QRegularExpression verRe(R"(version\s*=\s*["']([^"']+)["'])");
                auto mDisp = dispNameRe.match(toml);
                if (mDisp.hasMatch()) report.modName = mDisp.captured(1);
                auto mId = modIdRe.match(toml);
                if (mId.hasMatch()) report.modId = mId.captured(1);
                auto mVer = verRe.match(toml);
                if (mVer.hasMatch()) report.version = mVer.captured(1);
            }
            return Result<>();
        }

        // Read metadata: Legacy mcmod.info
        if (name == "mcmod.info") {
            auto dataRes = f->readAll();
            if (dataRes.has_value()) {
                auto doc = QJsonDocument::fromJson(dataRes.value());
                QJsonObject obj;
                if (doc.isArray() && !doc.array().isEmpty() && doc.array()[0].isObject()) {
                    obj = doc.array()[0].toObject();
                } else if (doc.isObject()) {
                    obj = doc.object();
                }
                if (!obj.isEmpty()) {
                    if (obj.contains("name")) report.modName = obj["name"].toString();
                    if (obj.contains("modid")) report.modId = obj["modid"].toString();
                    if (obj.contains("version")) report.version = obj["version"].toString();
                }
            }
            return Result<>();
        }

        // Parse classes (up to 10000 classes to scan large mods completely while protecting against zip bombs)
        if (name.endsWith(".class", Qt::CaseInsensitive)) {
            report.totalClasses++;
            if (report.classCount < 10000) {
                report.classCount++;
                auto classDataRes = f->readAll();
                if (classDataRes.has_value()) {
                    ClassFacts facts;
                    facts.className = name;
                    if (parseClassConstantPool(classDataRes.value(), facts)) {
                        scanFacts(facts, report.findings);
                    }
                }
            }
            return Result<>();
        }

        return f->skip();
    });

    if (!report.nativeLibraries.isEmpty()) {
        bool allWhitelisted = true;
        for (const auto& lib : report.nativeLibraries) {
            if (!isWhitelistedPackage(lib)) {
                allWhitelisted = false;
                break;
            }
        }
        AuditFinding f;
        f.ruleId = "native_libs";
        f.category = DiagI18n::tr("Native Code", "Нативный код");
        if (allWhitelisted) {
            f.severity = AuditSeverity::Info;
            f.score = 0;
            f.title = DiagI18n::tr("Whitelisted Native Libraries (.dll / .so)", "Встроенные библиотеки из белого списка (.dll / .so)");
            f.description = DiagI18n::tr("Archive contains compiled binary libraries belonging to trusted components (JNA, SQLite, LWJGL, OSHI, etc.).",
                                         "Архив содержит бинарные библиотеки доверенных компонентов (JNA, SQLite, LWJGL, OSHI и т.д.).");
        } else {
            f.severity = AuditSeverity::Notice;
            f.score = 5;
            f.title = DiagI18n::tr("Bundled Native Libraries (.dll / .so)", "Встроенные нативные библиотеки (.dll / .so)");
            f.description = DiagI18n::tr("Archive contains compiled binary libraries. Their raw machine code cannot be checked via static bytecode analysis.",
                                         "Архив содержит скомпилированные бинарные файлы. Их содержимое не может быть проверено статическим анализом.");
        }
        f.evidence = report.nativeLibraries.join(", ");
        addOrMergeFinding(report.findings, f);
    }

    // Check if the mod as a whole contains any aggravating factors (stealers, persistence, hidden shells, obfuscated dangerous calls)
    bool modHasAggravators = false;
    for (const auto& f : report.findings) {
        if (f.ruleId == "data_theft_stealer" ||
            f.ruleId == "data_exfil" ||
            f.ruleId == "system_persistence" ||
            f.ruleId == "keylogger_native" ||
            f.ruleId == "jndi_injection" ||
            f.ruleId == "combo_obfuscated_danger") {
            modHasAggravators = true;
            break;
        }
    }

    if (modHasAggravators) {
        for (auto& f : report.findings) {
            if (f.ruleId == "combo_stage0_downloader" || f.ruleId == "combo_network_dropper") {
                f.severity = AuditSeverity::Critical;
                f.score = 95;
            }
        }
    }

    // Calculate score deduplicated per category (award points per category once across the whole mod)
    QHash<QString, int> categoryMaxScore;

    for (const auto& f : report.findings) {
        if (!categoryMaxScore.contains(f.category) || f.score > categoryMaxScore[f.category]) {
            categoryMaxScore[f.category] = f.score;
        }
    }

    int score = 0;
    for (auto it = categoryMaxScore.constBegin(); it != categoryMaxScore.constEnd(); ++it) {
        score += it.value();
    }

    report.riskScore = qMin(100, score);

    if (report.riskScore >= 80) {
        report.verdict = AuditVerdict::Dangerous;
    } else if (report.riskScore >= 40) {
        report.verdict = AuditVerdict::Suspicious;
    } else if (report.riskScore >= 1) {
        report.verdict = AuditVerdict::Notice;
    } else {
        report.verdict = AuditVerdict::Clean;
    }

    report.scanTimeMs = timer.elapsed();

    setCachedReport(jarPath, report);

    return report;
}
