// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#pragma once

#include <QString>
#include <QStringList>
#include <QList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

namespace ConfigEditor {

enum class FieldType {
    Boolean,
    Integer,
    Double,
    Enum,
    String
};

enum class FileFormat {
    Toml,
    Cfg,
    Properties,
    Txt,
    Json
};

struct Field {
    int lineIndex = -1; // 0-based line index in m_rawLines
    QString section;
    QString key;
    QString rawKey;
    QString originalValue;
    QString currentValue;
    QString defaultValue;
    QString description;
    QStringList allowedValues;
    bool hasRange = false;
    double rangeMin = 0.0;
    double rangeMax = 100.0;
    bool isQuoted = false;
    FieldType type = FieldType::String;
    QString jsonPath; // Dot-separated key path for JSON
};

struct Section {
    QString name;
    QList<Field> fields;
};

class ConfigDocument {
public:
    ConfigDocument() = default;

    bool loadFromFile(const QString& filePath, QString* error = nullptr);
    bool loadFromText(const QString& text, FileFormat format, const QString& filePath = QString(), QString* error = nullptr);

    bool saveToFile(const QString& filePath, QString* error = nullptr);
    QString generateText() const;

    FileFormat format() const { return m_format; }
    const QString& filePath() const { return m_filePath; }
    const QList<Section>& sections() const { return m_sections; }
    QList<Section>& sections() { return m_sections; }

    bool updateFieldValue(const QString& section, const QString& key, const QString& newValue);
    bool isModified() const { return m_isModified; }
    void setModified(bool mod) { m_isModified = mod; }

    static FileFormat detectFormat(const QString& filePath);
    static bool createBackup(const QString& filePath, QString* error = nullptr);
    static bool restoreBackup(const QString& filePath, QString* error = nullptr);
    static bool hasBackup(const QString& filePath);

private:
    void parseLineBased(const QStringList& lines);
    void parseJson(const QString& text);
    static QString stripJsonComments(const QString& json);
    static QString replaceValueInLine(const QString& line, const Field& field, const QString& newVal);

    FileFormat m_format = FileFormat::Toml;
    QString m_filePath;
    QStringList m_rawLines;
    QString m_rawText;
    QList<Section> m_sections;
    bool m_isModified = false;

    // For JSON format
    QJsonDocument m_jsonDoc;
};

} // namespace ConfigEditor

