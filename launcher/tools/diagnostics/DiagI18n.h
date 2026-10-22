// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#pragma once

#include <QString>
#include <QLocale>
#include "Application.h"
#include "settings/SettingsObject.h"
#include "settings/Setting.h"

namespace DiagI18n {

inline bool isRussian()
{
    auto* app = APPLICATION_DYN;
    if (app) {
        auto settings = app->settings();
        if (settings) {
            QString lang = settings->get("Language").toString();
            if (!lang.isEmpty()) {
                return lang.startsWith("ru", Qt::CaseInsensitive);
            }
        }
    }
    return QLocale().name().startsWith("ru", Qt::CaseInsensitive);
}

inline QString tr(const char* en, const char* ru)
{
    return isRussian() ? QString::fromUtf8(ru) : QString::fromUtf8(en);
}

} // namespace DiagI18n
