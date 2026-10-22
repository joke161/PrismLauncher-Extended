// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

namespace ConfigEditor {

class ModLangResolver {
public:
    // Resolves and loads translation key-value pairs from mod jar corresponding to config file
    static QHash<QString, QString> loadTranslationsForConfig(const QString& configFilePath, const QString& modsRoot);

    // Looks up human-readable title for a setting key
    static QString findTitle(const QHash<QString, QString>& langMap, const QString& section, const QString& key);

    // Looks up tooltip / description for a setting key
    static QString findDescription(const QHash<QString, QString>& langMap, const QString& section, const QString& key);

    // Built-in common translation dictionary for universal mod keys
    static QString lookupCommonKeyTranslation(const QString& key);

    // Formats camelCase and snake_case strings into human-readable spaced words
    static QString formatCamelOrSnakeCase(const QString& raw);

    // Resolves and formats section titles
    static QString resolveSectionTitle(const QString& secName, const QHash<QString, QString>& langMap);

    // Converts Minecraft internal key code to friendly display name
    static QString formatKeyDisplayName(const QString& mcKey);

    // Extracts candidate mod IDs from a config file path
    static QStringList candidateModIdsFromConfig(const QString& configFilePath);

    // Clears cached translations
    static void clearCache();
};

} // namespace ConfigEditor

