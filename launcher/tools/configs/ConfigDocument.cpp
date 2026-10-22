// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#include "ConfigDocument.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>
#include <cmath>

#include "tools/diagnostics/DiagI18n.h"

namespace ConfigEditor {

FileFormat ConfigDocument::detectFormat(const QString& filePath)
{
    QString ext = QFileInfo(filePath).suffix().toLower();
    if (ext == "json" || ext == "json5") {
        return FileFormat::Json;
    }
    if (ext == "cfg") {
        return FileFormat::Cfg;
    }
    if (ext == "properties") {
        return FileFormat::Properties;
    }
    if (ext == "txt") {
        return FileFormat::Txt;
    }
    return FileFormat::Toml;
}

bool ConfigDocument::createBackup(const QString& filePath, QString* error)
{
    if (filePath.isEmpty() || !QFile::exists(filePath)) {
        if (error) {
            *error = "File does not exist: " + filePath;
        }
        return false;
    }

    QString bakPath = filePath + ".bak";
    if (QFile::exists(bakPath)) {
        QFile::remove(bakPath);
    }

    if (!QFile::copy(filePath, bakPath)) {
        if (error) {
            *error = "Failed to copy file to: " + bakPath;
        }
        return false;
    }
    return true;
}

bool ConfigDocument::restoreBackup(const QString& filePath, QString* error)
{
    QString bakPath = filePath + ".bak";
    if (!QFile::exists(bakPath)) {
        if (error) {
            *error = "Backup file not found: " + bakPath;
        }
        return false;
    }

    if (QFile::exists(filePath)) {
        QFile::remove(filePath);
    }

    if (!QFile::copy(bakPath, filePath)) {
        if (error) {
            *error = "Failed to restore backup from: " + bakPath;
        }
        return false;
    }
    return true;
}

bool ConfigDocument::hasBackup(const QString& filePath)
{
    return QFile::exists(filePath + ".bak");
}

bool ConfigDocument::loadFromFile(const QString& filePath, QString* error)
{
    m_filePath = filePath;
    m_format = detectFormat(filePath);
    m_sections.clear();
    m_isModified = false;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }

    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    m_rawText = in.readAll();
    file.close();

    return loadFromText(m_rawText, m_format, filePath, error);
}

bool ConfigDocument::loadFromText(const QString& text, FileFormat format, const QString& filePath, QString* error)
{
    m_rawText = text;
    m_format = format;
    if (!filePath.isEmpty()) {
        m_filePath = filePath;
    }
    m_sections.clear();
    m_isModified = false;

    if (m_format == FileFormat::Json) {
        parseJson(text);
        return true;
    }

    m_rawLines = text.split('\n');
    parseLineBased(m_rawLines);
    return true;
}

QString ConfigDocument::stripJsonComments(const QString& json)
{
    QString out;
    out.reserve(json.size());

    bool inString = false;
    bool inSingleComment = false;
    bool inMultiComment = false;
    int i = 0;

    while (i < json.size()) {
        QChar c = json[i];
        if (inSingleComment) {
            if (c == '\n') {
                inSingleComment = false;
                out.append(c);
            }
            i++;
        } else if (inMultiComment) {
            if (c == '*' && i + 1 < json.size() && json[i + 1] == '/') {
                inMultiComment = false;
                i += 2;
            } else {
                i++;
            }
        } else if (inString) {
            out.append(c);
            if (c == '\\' && i + 1 < json.size()) {
                out.append(json[i + 1]);
                i += 2;
                continue;
            }
            if (c == '"') {
                inString = false;
            }
            i++;
        } else {
            if (c == '"') {
                inString = true;
                out.append(c);
                i++;
            } else if (c == '/' && i + 1 < json.size() && json[i + 1] == '/') {
                inSingleComment = true;
                i += 2;
            } else if (c == '/' && i + 1 < json.size() && json[i + 1] == '*') {
                inMultiComment = true;
                i += 2;
            } else {
                out.append(c);
                i++;
            }
        }
    }

    // Strip trailing commas before } or ]
    static const QRegularExpression trailingCommaRe(R"(,\s*([\}\]]))");
    out.replace(trailingCommaRe, "\\1");
    return out;
}

void ConfigDocument::parseJson(const QString& text)
{
    QString cleanJson = stripJsonComments(text);
    QJsonParseError parseErr;
    m_jsonDoc = QJsonDocument::fromJson(cleanJson.toUtf8(), &parseErr);

    if (m_jsonDoc.isNull() || (!m_jsonDoc.isObject() && !m_jsonDoc.isArray())) {
        // Fallback: create raw text editor representation
        return;
    }

    if (m_jsonDoc.isObject()) {
        QJsonObject root = m_jsonDoc.object();

        std::function<void(const QString&, const QJsonObject&)> parseObj =
            [&](const QString& currentSec, const QJsonObject& obj) {
                Section sec;
                sec.name = currentSec;

                for (auto it = obj.begin(); it != obj.end(); ++it) {
                    QString key = it.key();
                    QJsonValue val = it.value();

                    if (val.isObject()) {
                        QString subSec = currentSec.isEmpty() ? key : currentSec + "." + key;
                        parseObj(subSec, val.toObject());
                    } else {
                        Field f;
                        f.section = currentSec;
                        f.key = key;
                        f.rawKey = key;
                        f.jsonPath = currentSec.isEmpty() ? key : currentSec + "." + key;

                        if (val.isBool()) {
                            f.type = FieldType::Boolean;
                            f.currentValue = val.toBool() ? "true" : "false";
                        } else if (val.isDouble()) {
                            double d = val.toDouble();
                            if (std::floor(d) == d && !QString::number(d).contains('.')) {
                                f.type = FieldType::Integer;
                                f.currentValue = QString::number(static_cast<qint64>(d));
                            } else {
                                f.type = FieldType::Double;
                                f.currentValue = QString::number(d, 'f', 4);
                                while (f.currentValue.endsWith('0') && f.currentValue.contains('.')) {
                                    f.currentValue.chop(1);
                                }
                                if (f.currentValue.endsWith('.')) {
                                    f.currentValue.chop(1);
                                }
                            }
                        } else if (val.isString()) {
                            f.type = FieldType::String;
                            f.currentValue = val.toString();
                            f.isQuoted = true;
                        } else if (val.isArray()) {
                            f.type = FieldType::String;
                            QJsonDocument arrDoc(val.toArray());
                            f.currentValue = QString::fromUtf8(arrDoc.toJson(QJsonDocument::Compact));
                        } else {
                            f.type = FieldType::String;
                            f.currentValue = QString();
                        }
                        f.originalValue = f.currentValue;
                        sec.fields.append(f);
                    }
                }

                if (!sec.fields.isEmpty()) {
                    m_sections.append(sec);
                }
            };

        Section generalSec;
        generalSec.name = "General";

        for (auto it = root.begin(); it != root.end(); ++it) {
            if (it.value().isObject()) {
                parseObj(it.key(), it.value().toObject());
            } else {
                Field f;
                f.section = "General";
                f.key = it.key();
                f.rawKey = it.key();
                f.jsonPath = it.key();

                QJsonValue val = it.value();
                if (val.isBool()) {
                    f.type = FieldType::Boolean;
                    f.currentValue = val.toBool() ? "true" : "false";
                } else if (val.isDouble()) {
                    double d = val.toDouble();
                    if (std::floor(d) == d && !QString::number(d).contains('.')) {
                        f.type = FieldType::Integer;
                        f.currentValue = QString::number(static_cast<qint64>(d));
                    } else {
                        f.type = FieldType::Double;
                        f.currentValue = QString::number(d, 'f', 6);
                        while (f.currentValue.endsWith('0') && f.currentValue.contains('.')) {
                            f.currentValue.chop(1);
                        }
                        if (f.currentValue.endsWith('.')) {
                            f.currentValue.chop(1);
                        }
                    }
                } else if (val.isString()) {
                    f.type = FieldType::String;
                    f.currentValue = val.toString();
                    f.isQuoted = true;
                } else {
                    f.type = FieldType::String;
                    f.currentValue = QString();
                }
                f.originalValue = f.currentValue;
                generalSec.fields.append(f);
            }
        }

        if (!generalSec.fields.isEmpty()) {
            m_sections.prepend(generalSec);
        }
    }
}

void ConfigDocument::parseLineBased(const QStringList& lines)
{
    QString fileName = QFileInfo(m_filePath).fileName().toLower();
    bool isOptionsTxt = (fileName == "options.txt");
    bool isShadersTxt = (fileName == "optionsshaders.txt");

    QString currentSection = isShadersTxt ? "Shader Options" : (isOptionsTxt ? "Game Options" : "General");
    QStringList cfgSectionStack;
    QStringList pendingComments;
    QStringList pendingAllowed;
    QString pendingDefaultVal;
    bool pendingHasRange = false;
    double pendingRangeMin = 0.0;
    double pendingRangeMax = 100.0;

    static const QRegularExpression tomlSectionRe(R"(^\s*\[+([A-Za-z0-9_.\- ]+)\]+\s*$)");
    static const QRegularExpression cfgSectionRe(R"(^\s*([A-Za-z0-9_.\- ]+)\s*\{\s*$)");
    static const QRegularExpression cfgCloseRe(R"(^\s*\}\s*$)");

    static const QRegularExpression oldRangeRe(R"(\[range:\s*([+-]?\d+(?:\.\d+)?)\s*(?:~|-|\.\.)\s*([+-]?\d+(?:\.\d+)?)(?:,\s*default:\s*([+-]?\d+(?:\.\d+)?))?\])", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression newRangeRe(R"((?:[Rr]ange|range)[:\s]+\[?([+-]?\d+(?:\.\d+)?)\s*(?:~|-|\.\.|,)\s*([+-]?\d+(?:\.\d+)?)(?:\])?)");
    static const QRegularExpression minMaxRe(R"(min:\s*([+-]?\d+(?:\.\d+)?).*?max:\s*([+-]?\d+(?:\.\d+)?))", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression defaultRe(R"((?:^|\s|\[)(?:[Dd]efault|default)[:\s]+([^\]\r\n,]+)(?:\])?)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression allowedRe(R"((?:Allowed\s*Values?|Allowed|Valid\s*values?|Options)[:\s]+\[?([^\]\r\n]+)\]?)", QRegularExpression::CaseInsensitiveOption);

    auto getSectionPtr = [this](const QString& name) -> Section* {
        for (auto& sec : m_sections) {
            if (sec.name.compare(name, Qt::CaseInsensitive) == 0) {
                return &sec;
            }
        }
        Section newSec;
        newSec.name = name;
        m_sections.append(newSec);
        return &m_sections.last();
    };

    for (int lineIdx = 0; lineIdx < lines.size(); ++lineIdx) {
        QString line = lines[lineIdx];
        QString trimmed = line.trimmed();

        if (trimmed.isEmpty()) {
            // Keep pending comments only if not separated by multiple blank lines
            continue;
        }

        // Comment lines
        if (trimmed.startsWith('#') || trimmed.startsWith("//") || trimmed.startsWith('!')) {
            QString commentBody = trimmed;
            while (commentBody.startsWith('#') || commentBody.startsWith('/') || commentBody.startsWith('!')) {
                commentBody = commentBody.mid(1);
            }
            commentBody = commentBody.trimmed();

            // 1. Check old Forge range format: [range: min ~ max, default: X] or [range: min ~ max]
            auto oldRMatch = oldRangeRe.match(commentBody);
            if (oldRMatch.hasMatch()) {
                pendingHasRange = true;
                pendingRangeMin = oldRMatch.captured(1).toDouble();
                pendingRangeMax = oldRMatch.captured(2).toDouble();
                if (!oldRMatch.captured(3).isEmpty()) {
                    pendingDefaultVal = oldRMatch.captured(3).trimmed();
                }
                commentBody.remove(oldRMatch.capturedStart(0), oldRMatch.capturedLength(0));
                commentBody = commentBody.trimmed();
            } else {
                // 2. Check standard range: Range: min ~ max
                auto rMatch = newRangeRe.match(commentBody);
                if (rMatch.hasMatch()) {
                    pendingHasRange = true;
                    pendingRangeMin = rMatch.captured(1).toDouble();
                    pendingRangeMax = rMatch.captured(2).toDouble();
                    commentBody.remove(rMatch.capturedStart(0), rMatch.capturedLength(0));
                    commentBody = commentBody.trimmed();
                } else {
                    auto mmMatch = minMaxRe.match(commentBody);
                    if (mmMatch.hasMatch()) {
                        pendingHasRange = true;
                        pendingRangeMin = mmMatch.captured(1).toDouble();
                        pendingRangeMax = mmMatch.captured(2).toDouble();
                        commentBody.remove(mmMatch.capturedStart(0), mmMatch.capturedLength(0));
                        commentBody = commentBody.trimmed();
                    }
                }
            }

            // Check for default: X
            auto defMatch = defaultRe.match(commentBody);
            if (defMatch.hasMatch()) {
                if (pendingDefaultVal.isEmpty()) {
                    pendingDefaultVal = defMatch.captured(1).trimmed();
                }
                commentBody.remove(defMatch.capturedStart(0), defMatch.capturedLength(0));
                commentBody = commentBody.trimmed();
            }

            // Check for allowed values pattern
            auto aMatch = allowedRe.match(commentBody);
            if (aMatch.hasMatch()) {
                QString rawAllowed = aMatch.captured(1).trimmed();
                QStringList parts;
                if (rawAllowed.contains(',')) {
                    parts = rawAllowed.split(',', Qt::SkipEmptyParts);
                } else if (rawAllowed.contains('|')) {
                    parts = rawAllowed.split('|', Qt::SkipEmptyParts);
                } else {
                    parts = rawAllowed.split(' ', Qt::SkipEmptyParts);
                }
                QStringList cleanParts;
                for (auto& p : parts) {
                    QString item = p.trimmed();
                    if (item.startsWith('"') && item.endsWith('"')) {
                        item = item.mid(1, item.length() - 2);
                    }
                    if (!item.isEmpty()) {
                        cleanParts.append(item);
                    }
                }
                if (cleanParts.size() >= 2) {
                    pendingAllowed = cleanParts;
                }
                commentBody.remove(aMatch.capturedStart(0), aMatch.capturedLength(0));
                commentBody = commentBody.trimmed();
            }

            // Clean up residual punctuation
            if (commentBody == "[]" || commentBody == ":" || commentBody == "-") {
                commentBody.clear();
            }

            if (!commentBody.isEmpty()) {
                pendingComments.append(commentBody);
            }
            continue;
        }

        // Section headers
        auto tomlSecMatch = tomlSectionRe.match(line);
        if (tomlSecMatch.hasMatch()) {
            cfgSectionStack.clear();
            currentSection = tomlSecMatch.captured(1).trimmed();
            pendingComments.clear();
            pendingAllowed.clear();
            pendingDefaultVal.clear();
            pendingHasRange = false;
            continue;
        }

        auto cfgSecMatch = cfgSectionRe.match(line);
        if (cfgSecMatch.hasMatch()) {
            cfgSectionStack.append(cfgSecMatch.captured(1).trimmed());
            currentSection = cfgSectionStack.join('.');
            pendingComments.clear();
            pendingAllowed.clear();
            pendingDefaultVal.clear();
            pendingHasRange = false;
            continue;
        }

        if (cfgCloseRe.match(line).hasMatch()) {
            if (!cfgSectionStack.isEmpty()) {
                cfgSectionStack.removeLast();
            }
            currentSection = cfgSectionStack.isEmpty() ? "General" : cfgSectionStack.join('.');
            continue;
        }

        // Key-Value lines
        int eqPos = line.indexOf('=');
        int colonPos = line.indexOf(':');

        int delimPos = -1;
        if (eqPos != -1) {
            delimPos = eqPos;
        } else if (colonPos != -1) {
            delimPos = colonPos;
        }

        if (delimPos != -1) {
            QString keyPart = line.left(delimPos).trimmed();
            QString valPart = line.mid(delimPos + 1).trimmed();

            if (keyPart.isEmpty()) {
                continue;
            }

            // Forge CFG type prefix: B:, I:, D:, S:
            FieldType explicitType = FieldType::String;
            bool hasExplicitType = false;
            QString cleanKey = keyPart;

            if (keyPart.size() >= 2 && keyPart[1] == ':') {
                QChar t = keyPart[0].toUpper();
                if (t == 'B') { explicitType = FieldType::Boolean; hasExplicitType = true; cleanKey = keyPart.mid(2); }
                else if (t == 'I') { explicitType = FieldType::Integer; hasExplicitType = true; cleanKey = keyPart.mid(2); }
                else if (t == 'D') { explicitType = FieldType::Double; hasExplicitType = true; cleanKey = keyPart.mid(2); }
                else if (t == 'S') { explicitType = FieldType::String; hasExplicitType = true; cleanKey = keyPart.mid(2); }
            }

            if (cleanKey.startsWith('"') && cleanKey.endsWith('"')) {
                cleanKey = cleanKey.mid(1, cleanKey.length() - 2);
            }

            // Separate value from inline comment
            int commentPos = -1;
            bool inQuotes = false;
            for (int cIdx = 0; cIdx < valPart.size(); ++cIdx) {
                QChar c = valPart[cIdx];
                if (c == '"' && (cIdx == 0 || valPart[cIdx - 1] != '\\')) {
                    inQuotes = !inQuotes;
                } else if (!inQuotes) {
                    if (c == '#' || (c == '/' && cIdx + 1 < valPart.size() && valPart[cIdx + 1] == '/')) {
                        commentPos = cIdx;
                        break;
                    }
                }
            }

            QString rawValStr = (commentPos != -1) ? valPart.left(commentPos).trimmed() : valPart;

            bool isQuoted = false;
            QString cleanVal = rawValStr;
            if (cleanVal.size() >= 2 && cleanVal.startsWith('"') && cleanVal.endsWith('"')) {
                isQuoted = true;
                cleanVal = cleanVal.mid(1, cleanVal.length() - 2);
            } else if (cleanVal.size() >= 2 && cleanVal.startsWith('\'') && cleanVal.endsWith('\'')) {
                isQuoted = true;
                cleanVal = cleanVal.mid(1, cleanVal.length() - 2);
            }

            // Logical grouping for options.txt
            QString activeSection = currentSection;
            if (isOptionsTxt) {
                if (cleanKey.startsWith("soundCategory_")) {
                    activeSection = "Audio / Sound";
                } else if (cleanKey.startsWith("key_")) {
                    activeSection = "Controls / Keybinds";
                } else if (cleanKey.startsWith("modelPart_")) {
                    activeSection = "Skin / Model Customization";
                } else {
                    activeSection = "Game Options";
                }
            }

            // Determine field type
            FieldType type = FieldType::String;
            if (hasExplicitType) {
                type = explicitType;
            } else if (pendingAllowed.size() >= 2) {
                type = FieldType::Enum;
            } else if (cleanVal.compare("true", Qt::CaseInsensitive) == 0 || cleanVal.compare("false", Qt::CaseInsensitive) == 0) {
                type = FieldType::Boolean;
            } else if (pendingHasRange) {
                if (cleanVal.contains('.') || QString::number(pendingRangeMin).contains('.') || QString::number(pendingRangeMax).contains('.')) {
                    type = FieldType::Double;
                } else {
                    type = FieldType::Integer;
                }
            } else {
                bool isInt = false;
                cleanVal.toInt(&isInt);
                if (isInt) {
                    type = FieldType::Integer;
                } else {
                    bool isDbl = false;
                    cleanVal.toDouble(&isDbl);
                    if (isDbl && cleanVal.contains('.')) {
                        type = FieldType::Double;
                    } else {
                        type = FieldType::String;
                    }
                }
            }

            Field f;
            f.lineIndex = lineIdx;
            f.section = activeSection;
            f.key = cleanKey;
            f.rawKey = keyPart;
            f.originalValue = cleanVal;
            f.currentValue = cleanVal;
            f.defaultValue = pendingDefaultVal;

            QString desc = pendingComments.join("\n").trimmed();
            if (!pendingDefaultVal.isEmpty()) {
                QString defText = DiagI18n::tr("Default: %1", "По умолчанию: %1").arg(pendingDefaultVal);
                if (!desc.contains(pendingDefaultVal)) {
                    if (!desc.isEmpty()) {
                        desc += "\n" + defText;
                    } else {
                        desc = defText;
                    }
                }
            }
            f.description = desc;
            f.allowedValues = pendingAllowed;
            f.hasRange = pendingHasRange;
            f.rangeMin = pendingRangeMin;
            f.rangeMax = pendingRangeMax;
            f.isQuoted = isQuoted;
            f.type = type;

            Section* secPtr = getSectionPtr(activeSection);
            secPtr->fields.append(f);

            pendingComments.clear();
            pendingAllowed.clear();
            pendingDefaultVal.clear();
            pendingHasRange = false;
        }
    }
}

QString ConfigDocument::replaceValueInLine(const QString& line, const Field& field, const QString& newVal)
{
    int eqPos = line.indexOf('=');
    int colonPos = line.indexOf(':');

    int delimPos = -1;
    if (eqPos != -1) {
        delimPos = eqPos;
    } else if (colonPos != -1) {
        delimPos = colonPos;
    }

    if (delimPos == -1) {
        return line;
    }

    QString prefix = line.left(delimPos + 1);
    QString rest = line.mid(delimPos + 1);

    int valStart = 0;
    while (valStart < rest.size() && rest[valStart].isSpace()) {
        valStart++;
    }
    QString leadingSpace = rest.left(valStart);
    QString afterSpace = rest.mid(valStart);

    int commentPos = -1;
    bool inQuotes = false;
    for (int i = 0; i < afterSpace.size(); ++i) {
        QChar c = afterSpace[i];
        if (c == '"' && (i == 0 || afterSpace[i - 1] != '\\')) {
            inQuotes = !inQuotes;
        } else if (!inQuotes) {
            if (c == '#' || (c == '/' && i + 1 < afterSpace.size() && afterSpace[i + 1] == '/')) {
                commentPos = i;
                break;
            }
        }
    }

    QString inlineComment;
    if (commentPos != -1) {
        inlineComment = afterSpace.mid(commentPos);
    }

    QString formattedVal = field.isQuoted ? QString("\"%1\"").arg(newVal) : newVal;

    QString result = prefix + leadingSpace + formattedVal;
    if (!inlineComment.isEmpty()) {
        result += " " + inlineComment.trimmed();
    }
    return result;
}

QString ConfigDocument::generateText() const
{
    if (m_format == FileFormat::Json) {
        if (!m_jsonDoc.isObject()) {
            return m_rawText;
        }

        QJsonObject root = m_jsonDoc.object();

        std::function<void(QJsonObject&, const QStringList&, const QString&, FieldType)> setNestedValue =
            [&](QJsonObject& obj, const QStringList& parts, const QString& val, FieldType type) {
                if (parts.isEmpty()) return;

                if (parts.size() == 1) {
                    QString k = parts[0];
                    if (type == FieldType::Boolean) {
                        obj[k] = (val.compare("true", Qt::CaseInsensitive) == 0);
                    } else if (type == FieldType::Integer) {
                        obj[k] = val.toLongLong();
                    } else if (type == FieldType::Double) {
                        obj[k] = val.toDouble();
                    } else {
                        obj[k] = val;
                    }
                } else {
                    QString k = parts[0];
                    QJsonObject subObj = obj[k].toObject();
                    setNestedValue(subObj, parts.mid(1), val, type);
                    obj[k] = subObj;
                }
            };

        for (const auto& sec : m_sections) {
            for (const auto& f : sec.fields) {
                if (f.currentValue != f.originalValue) {
                    QStringList parts = f.jsonPath.split('.', Qt::SkipEmptyParts);
                    if (parts.size() > 1 && parts[0] == "General") {
                        parts.removeFirst();
                    }
                    setNestedValue(root, parts, f.currentValue, f.type);
                }
            }
        }

        QJsonDocument updatedDoc(root);
        return QString::fromUtf8(updatedDoc.toJson(QJsonDocument::Indented));
    }

    // Line-based formats
    QStringList lines = m_rawLines;
    for (const auto& sec : m_sections) {
        for (const auto& f : sec.fields) {
            if (f.currentValue != f.originalValue && f.lineIndex >= 0 && f.lineIndex < lines.size()) {
                lines[f.lineIndex] = replaceValueInLine(lines[f.lineIndex], f, f.currentValue);
            }
        }
    }
    return lines.join('\n');
}

bool ConfigDocument::saveToFile(const QString& filePath, QString* error)
{
    QString content = generateText();

    QSaveFile saveFile(filePath);
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) {
            *error = saveFile.errorString();
        }
        return false;
    }

    QTextStream out(&saveFile);
    out.setEncoding(QStringConverter::Utf8);
    out << content;

    if (!saveFile.commit()) {
        if (error) {
            *error = saveFile.errorString();
        }
        return false;
    }

    m_rawText = content;
    if (m_format != FileFormat::Json) {
        m_rawLines = content.split('\n');
    }

    for (auto& sec : m_sections) {
        for (auto& f : sec.fields) {
            f.originalValue = f.currentValue;
        }
    }
    m_isModified = false;
    return true;
}

bool ConfigDocument::updateFieldValue(const QString& section, const QString& key, const QString& newValue)
{
    for (auto& sec : m_sections) {
        if (sec.name == section) {
            for (auto& f : sec.fields) {
                if (f.key == key) {
                    if (f.currentValue != newValue) {
                        f.currentValue = newValue;
                        m_isModified = true;
                        return true;
                    }
                    return false;
                }
            }
        }
    }
    return false;
}

} // namespace ConfigEditor
