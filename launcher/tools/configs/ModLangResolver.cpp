// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#include "ModLangResolver.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>

#include "archive/ArchiveReader.h"
#include "tools/diagnostics/DiagI18n.h"

namespace ConfigEditor {

namespace {

static QMutex s_langMutex;
static QHash<QString, QHash<QString, QString>> s_langCache;

void parseJsonLang(const QByteArray& data, QHash<QString, QString>& outMap)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (!doc.isObject()) {
        return;
    }
    QJsonObject obj = doc.object();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it.value().isString()) {
            outMap.insert(it.key(), it.value().toString());
        }
    }
}

void parseLegacyLang(const QByteArray& data, QHash<QString, QString>& outMap)
{
    QString text = QString::fromUtf8(data);
    QStringList lines = text.split('\n');

    for (const QString& rawLine : lines) {
        QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith("//") || line.startsWith('!')) {
            continue;
        }

        int eqPos = line.indexOf('=');
        if (eqPos != -1) {
            QString key = line.left(eqPos).trimmed();
            QString val = line.mid(eqPos + 1).trimmed();
            if (!key.isEmpty() && !val.isEmpty()) {
                outMap.insert(key, val);
            }
        }
    }
}

} // namespace

QStringList ModLangResolver::candidateModIdsFromConfig(const QString& configFilePath)
{
    QStringList candidates;
    QFileInfo fi(configFilePath);

    QString fileName = fi.fileName().toLower();
    if (fileName == "options.txt" || fileName == "optionsshaders.txt") {
        return candidates;
    }

    QString base = fi.completeBaseName().toLower();

    // Strip client/common/server suffixes if present:
    // e.g. jei-client -> jei, flywheel-client -> flywheel, ironchest-common -> ironchest
    static const QRegularExpression suffixRe(R"([-_](?:client|common|server|config|settings|options|mod)$)", QRegularExpression::CaseInsensitiveOption);
    QString cleanBase = base;
    cleanBase.remove(suffixRe);

    if (!cleanBase.isEmpty()) {
        candidates.append(cleanBase);
    }
    if (cleanBase != base && !base.isEmpty()) {
        candidates.append(base);
    }

    // Check parent directory if inside config subfolder:
    // e.g. config/forestry/common.cfg -> candidate forestry
    QDir parentDir = fi.dir();
    QString parentName = parentDir.dirName().toLower();
    if (!parentName.isEmpty() && parentName != "config" && !candidates.contains(parentName)) {
        candidates.prepend(parentName);
    }

    return candidates;
}

QHash<QString, QString> ModLangResolver::loadTranslationsForConfig(const QString& configFilePath, const QString& modsRoot)
{
    if (configFilePath.isEmpty() || modsRoot.isEmpty() || !QDir(modsRoot).exists()) {
        return {};
    }

    QStringList candidates = candidateModIdsFromConfig(configFilePath);
    if (candidates.isEmpty()) {
        return {};
    }

    // Find candidate jar
    QDir modsDir(modsRoot);
    QFileInfoList jarFiles = modsDir.entryInfoList({ "*.jar", "*.jar.disabled" }, QDir::Files, QDir::Name);

    QString matchedJarPath;
    for (const QString& candidate : candidates) {
        if (candidate.length() < 2) continue;

        // Try exact prefix first
        for (const auto& jarInfo : jarFiles) {
            QString jarName = jarInfo.fileName().toLower();
            if (jarName.startsWith(candidate + "-") ||
                jarName.startsWith(candidate + "_") ||
                jarName.startsWith(candidate + ".")) {
                matchedJarPath = jarInfo.absoluteFilePath();
                break;
            }
        }
        if (!matchedJarPath.isEmpty()) {
            break;
        }

        // Try contains
        for (const auto& jarInfo : jarFiles) {
            QString jarName = jarInfo.fileName().toLower();
            if (jarName.contains(candidate)) {
                matchedJarPath = jarInfo.absoluteFilePath();
                break;
            }
        }
        if (!matchedJarPath.isEmpty()) {
            break;
        }
    }

    if (matchedJarPath.isEmpty() || !QFile::exists(matchedJarPath)) {
        return {};
    }

    // Check cache
    QFileInfo jarFi(matchedJarPath);
    QString cacheKey = matchedJarPath + ":" + QString::number(jarFi.lastModified().toMSecsSinceEpoch());

    {
        QMutexLocker locker(&s_langMutex);
        if (s_langCache.contains(cacheKey)) {
            return s_langCache.value(cacheKey);
        }
    }

    QHash<QString, QString> enMap;
    QHash<QString, QString> ruMap;

    MMCZip::ArchiveReader reader(matchedJarPath);
    (void)reader.parse([&](MMCZip::ArchiveReader::File* f) -> Result<> {
        QString name = f->filename();
        QString lowerName = name.toLower();

        if (lowerName.contains("/lang/")) {
            if (lowerName.endsWith("ru_ru.json")) {
                auto res = f->readAll();
                if (res) parseJsonLang(res.value(), ruMap);
            } else if (lowerName.endsWith("ru_ru.lang")) {
                auto res = f->readAll();
                if (res) parseLegacyLang(res.value(), ruMap);
            } else if (lowerName.endsWith("en_us.json")) {
                auto res = f->readAll();
                if (res) parseJsonLang(res.value(), enMap);
            } else if (lowerName.endsWith("en_us.lang")) {
                auto res = f->readAll();
                if (res) parseLegacyLang(res.value(), enMap);
            }
        }
        return Result<>();
    });

    bool isRussian = DiagI18n::isRussian();
    QHash<QString, QString> finalMap;

    if (isRussian) {
        // Fallback to English, primary Russian
        finalMap = enMap;
        for (auto it = ruMap.begin(); it != ruMap.end(); ++it) {
            finalMap.insert(it.key(), it.value());
        }
    } else {
        finalMap = enMap;
        if (finalMap.isEmpty()) {
            finalMap = ruMap;
        }
    }

    {
        QMutexLocker locker(&s_langMutex);
        s_langCache.insert(cacheKey, finalMap);
    }

    return finalMap;
}

QString ModLangResolver::findTitle(const QHash<QString, QString>& langMap, const QString& section, const QString& key)
{
    if (langMap.isEmpty() || key.isEmpty()) {
        return QString();
    }

    QString dotKey = "." + key.toLower();
    QString keyLower = key.toLower();

    // 1. Direct suffix match
    for (auto it = langMap.begin(); it != langMap.end(); ++it) {
        QString lk = it.key().toLower();
        if (lk.endsWith(".tooltip") || lk.endsWith(".desc") || lk.endsWith(".description") ||
            lk.endsWith(".comment") || lk.endsWith(".help")) {
            continue;
        }

        if (lk.endsWith(dotKey) || lk == keyLower) {
            return it.value();
        }
        if (lk.endsWith(dotKey + ".name") || lk.endsWith(dotKey + "_name")) {
            return it.value();
        }
    }

    // 2. Normalized without underscores/dashes match
    QString normKey = keyLower;
    normKey.remove('_').remove('-');

    if (normKey != keyLower) {
        QString normDotKey = "." + normKey;
        for (auto it = langMap.begin(); it != langMap.end(); ++it) {
            QString lk = it.key().toLower();
            if (lk.endsWith(".tooltip") || lk.endsWith(".desc") || lk.endsWith(".description") ||
                lk.endsWith(".comment") || lk.endsWith(".help")) {
                continue;
            }

            QString normLk = lk;
            normLk.remove('_').remove('-');

            if (normLk.endsWith(normDotKey) || normLk == normKey) {
                return it.value();
            }
        }
    }

    return QString();
}

QString ModLangResolver::findDescription(const QHash<QString, QString>& langMap, const QString& section, const QString& key)
{
    if (langMap.isEmpty() || key.isEmpty()) {
        return QString();
    }

    QString dotKey = "." + key.toLower();
    QString keyLower = key.toLower();

    static const QStringList tooltipSuffixes = {
        ".tooltip",
        ".desc",
        ".description",
        ".comment",
        ".help"
    };

    // 1. Exact tooltip suffix match
    for (auto it = langMap.begin(); it != langMap.end(); ++it) {
        QString lk = it.key().toLower();

        for (const auto& suf : tooltipSuffixes) {
            if (lk.endsWith(dotKey + suf)) {
                return it.value();
            }
        }
    }

    // 2. Normalized without underscores/dashes
    QString normKey = keyLower;
    normKey.remove('_').remove('-');

    if (normKey != keyLower) {
        QString normDotKey = "." + normKey;
        for (auto it = langMap.begin(); it != langMap.end(); ++it) {
            QString lk = it.key().toLower();
            QString normLk = lk;
            normLk.remove('_').remove('-');

            for (const auto& suf : tooltipSuffixes) {
                if (normLk.endsWith(normDotKey + suf)) {
                    return it.value();
                }
            }
        }
    }

    return QString();
}

void ModLangResolver::clearCache()
{
    QMutexLocker locker(&s_langMutex);
    s_langCache.clear();
}

struct CommonKeyTranslation {
    const char* key;
    const char* en;
    const char* ru;
};

static const CommonKeyTranslation s_commonKeyTranslations[] = {
    { "shouldPlay", "Should Play Sound", "Воспроизводить звук" },
    { "soundEvent", "Sound Event", "Звуковое событие" },
    { "id", "Identifier", "Идентификатор" },
    { "enabled", "Enabled", "Включено" },
    { "enable", "Enable", "Включить" },
    { "disabled", "Disabled", "Выключено" },
    { "volume", "Volume", "Громкость" },
    { "pitch", "Pitch", "Высота звука" },
    { "scale", "Scale", "Масштаб" },
    { "guiScale", "GUI Scale", "Масштаб интерфейса" },
    { "range", "Range", "Диапазон" },
    { "maxRange", "Max Range", "Максимальный диапазон" },
    { "minRange", "Min Range", "Минимальный диапазон" },
    { "mode", "Mode", "Режим" },
    { "opacity", "Opacity", "Непрозрачность" },
    { "transparency", "Transparency", "Прозрачность" },
    { "renderDistance", "Render Distance", "Дистанция прорисовки" },
    { "maxParticles", "Max Particles", "Максимум частиц" },
    { "particles", "Particles", "Частицы" },
    { "color", "Color", "Цвет" },
    { "frequency", "Frequency", "Частота" },
    { "speed", "Speed", "Скорость" },
    { "debug", "Debug Mode", "Режим отладки" },
    { "debugMode", "Debug Mode", "Режим отладки" },
    { "animation", "Animations", "Анимации" },
    { "animations", "Animations", "Анимации" },
    { "smoothLighting", "Smooth Lighting", "Мягкое освещение" },
    { "fov", "Field of View (FOV)", "Поле зрения (FOV)" },
    { "gamma", "Brightness", "Яркость" },
    { "difficulty", "Difficulty", "Сложность" },
    { "graphicsMode", "Graphics", "Графика" },
    { "autoJump", "Auto-Jump", "Авто-прыжок" },
    { "fullscreen", "Fullscreen", "Полноэкранный режим" },
    { "vsync", "VSync", "Вертикальная синхронизация" },
    { "fpsLimit", "Max Framerate", "Макс. частота кадров (FPS)" },
    { "maxFps", "Max Framerate", "Макс. частота кадров (FPS)" },
    { "showSubtitles", "Show Subtitles", "Субтитры" },
    { "ocelotSitChests", "Ocelots Sit on Chests", "Оцелоты сидят на сундуках" },
    { "hud", "HUD Overlay", "Интерфейс (HUD)" },
    { "hudScale", "HUD Scale", "Масштаб интерфейса (HUD)" },
    { "duration", "Duration", "Длительность" },
    { "delay", "Delay", "Задержка" },
    { "interval", "Interval", "Интервал" },
    { "offset", "Offset", "Смещение" },
    { "offsetX", "Offset X", "Смещение по X" },
    { "offsetY", "Offset Y", "Смещение по Y" },
    { "offsetZ", "Offset Z", "Смещение по Z" },
    { "width", "Width", "Ширина" },
    { "height", "Height", "Высота" },
    { "size", "Size", "Размер" },
    { "fontSize", "Font Size", "Размер шрифта" },
    { "fontColor", "Font Color", "Цвет шрифта" },
    { "backgroundColor", "Background Color", "Цвет фона" },
    { "borderColor", "Border Color", "Цвет границы" },
    { "shadow", "Shadows", "Тени" },
    { "shadows", "Shadows", "Тени" },
    { "enableShaders", "Enable Shaders", "Включить шейдеры" }
};

QString ModLangResolver::lookupCommonKeyTranslation(const QString& key)
{
    bool isRu = DiagI18n::isRussian();
    for (const auto& item : s_commonKeyTranslations) {
        if (key.compare(item.key, Qt::CaseInsensitive) == 0) {
            return isRu ? QString::fromUtf8(item.ru) : QString::fromUtf8(item.en);
        }
    }
    return QString();
}

QString ModLangResolver::formatCamelOrSnakeCase(const QString& raw)
{
    if (raw.isEmpty()) {
        return QString();
    }

    if (raw.contains('.')) {
        QStringList parts = raw.split('.', Qt::SkipEmptyParts);
        QStringList formattedParts;
        for (const auto& p : parts) {
            formattedParts.append(formatCamelOrSnakeCase(p));
        }
        return formattedParts.join(" • ");
    }

    QString s = raw;
    s.replace('_', ' ').replace('-', ' ');

    QString result;
    result.reserve(s.size() * 2);

    for (int i = 0; i < s.size(); ++i) {
        QChar c = s[i];
        if (i > 0 && c.isUpper() && (s[i - 1].isLower() || (i + 1 < s.size() && s[i + 1].isLower()))) {
            if (!result.endsWith(' ')) {
                result.append(' ');
            }
        }
        result.append(c);
    }

    QStringList words = result.split(' ', Qt::SkipEmptyParts);
    for (int i = 0; i < words.size(); ++i) {
        QString w = words[i];
        if (!w.isEmpty()) {
            words[i] = w.left(1).toUpper() + w.mid(1);
        }
    }
    return words.join(' ');
}

QString ModLangResolver::resolveSectionTitle(const QString& secName, const QHash<QString, QString>& langMap)
{
    if (!langMap.isEmpty()) {
        QString trans = ModLangResolver::findTitle(langMap, "", secName);
        if (!trans.isEmpty()) {
            return trans;
        }

        QString lowerSec = secName.toLower();
        for (auto it = langMap.begin(); it != langMap.end(); ++it) {
            QString lk = it.key().toLower();
            if (lk.contains(".category." + lowerSec) || lk.contains(".section." + lowerSec) || lk.endsWith("." + lowerSec)) {
                return it.value();
            }
        }
    }

    return formatCamelOrSnakeCase(secName);
}

QString ModLangResolver::formatKeyDisplayName(const QString& mcKey)
{
    bool isRu = DiagI18n::isRussian();
    QString k = mcKey.trimmed();

    if (k == "key.mouse.left" || k == "key.mouse.0") return isRu ? "ЛКМ (Mouse Left)" : "Mouse Left (LMB)";
    if (k == "key.mouse.right" || k == "key.mouse.1") return isRu ? "ПКМ (Mouse Right)" : "Mouse Right (RMB)";
    if (k == "key.mouse.middle" || k == "key.mouse.2") return isRu ? "СКМ (Mouse Middle)" : "Mouse Middle (MMB)";
    if (k == "key.mouse.4") return isRu ? "Боковая мышь 4" : "Mouse Button 4";
    if (k == "key.mouse.5") return isRu ? "Боковая мышь 5" : "Mouse Button 5";

    if (k == "key.keyboard.space") return isRu ? "Пробел (Space)" : "Space";
    if (k == "key.keyboard.left.shift") return "Left Shift";
    if (k == "key.keyboard.right.shift") return "Right Shift";
    if (k == "key.keyboard.left.control") return "Left Ctrl";
    if (k == "key.keyboard.right.control") return "Right Ctrl";
    if (k == "key.keyboard.left.alt") return "Left Alt";
    if (k == "key.keyboard.right.alt") return "Right Alt";
    if (k == "key.keyboard.enter") return "Enter";
    if (k == "key.keyboard.tab") return "Tab";
    if (k == "key.keyboard.backspace") return "Backspace";
    if (k == "key.keyboard.caps.lock") return "Caps Lock";
    if (k == "key.keyboard.up") return isRu ? "Стрелка Вверх" : "Up Arrow";
    if (k == "key.keyboard.down") return isRu ? "Стрелка Вниз" : "Down Arrow";
    if (k == "key.keyboard.left") return isRu ? "Стрелка Влево" : "Left Arrow";
    if (k == "key.keyboard.right") return isRu ? "Стрелка Вправо" : "Right Arrow";

    if (k.startsWith("key.keyboard.")) {
        QString name = k.mid(13);
        if (name.length() == 1) {
            return name.toUpper();
        }
        if (name.startsWith("f") && name.length() <= 3) {
            return name.toUpper();
        }
        return name.left(1).toUpper() + name.mid(1);
    }

    if (k.startsWith("key.mouse.")) {
        return isRu ? QString("Мышь %1").arg(k.mid(10)) : QString("Mouse %1").arg(k.mid(10));
    }

    return k.isEmpty() ? (isRu ? "Не назначена" : "None") : k;
}

} // namespace ConfigEditor
