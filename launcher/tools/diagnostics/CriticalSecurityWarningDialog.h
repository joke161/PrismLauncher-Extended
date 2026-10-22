// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#pragma once

#include <QDialog>
#include <QList>
#include <QString>

#include "ModAuditor.h"

class ModFolderModel;

class CriticalSecurityWarningDialog : public QDialog {
    Q_OBJECT

public:
    explicit CriticalSecurityWarningDialog(const QList<AuditReport>& dangerousReports,
                                          ModFolderModel* model = nullptr,
                                          QWidget* parent = nullptr);
    ~CriticalSecurityWarningDialog() override = default;

private slots:
    void onDisableAllDangerous();

private:
    void setupUi();

    QList<AuditReport> m_reports;
    ModFolderModel* m_model = nullptr;
};

