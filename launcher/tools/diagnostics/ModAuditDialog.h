// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#pragma once

#include <QDialog>
#include <QString>

#include "ModAuditor.h"

class ModAuditDialog : public QDialog {
    Q_OBJECT

public:
    explicit ModAuditDialog(const AuditReport& report, QWidget* parent = nullptr);
    ~ModAuditDialog() override = default;

private slots:
    void onCopyReport();

private:
    void setupUi();
    QString buildReportText() const;

    AuditReport m_report;
};

