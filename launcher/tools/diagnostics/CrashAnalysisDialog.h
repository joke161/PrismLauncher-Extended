// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#pragma once

#include <QDialog>
#include <QList>
#include <QPushButton>
#include <QString>

#include "CrashAnalyzer.h"

class BaseInstance;

class CrashAnalysisDialog : public QDialog {
    Q_OBJECT

public:
    explicit CrashAnalysisDialog(const QList<CrashIssue>& issues,
                                 const QString& instanceRoot,
                                 BaseInstance* instance = nullptr,
                                 QWidget* parent = nullptr);
    ~CrashAnalysisDialog() override = default;

private slots:
    void onCopyReport();
    void onOpenCrashReportsFolder();
    void onOpenModsFolder();
    void onAutoFixDownloadMod(const QString& modName, QPushButton* btn);
    void onAutoFixDisableMod(const QString& modFileName, QPushButton* btn);
    void onAutoFixDisableBothMods(const QString& targets, QPushButton* btn);
    void onAutoFixResetConfig(const QString& configFileName, QPushButton* btn);
    void onAutoFixDisableShaders(QPushButton* btn);
    void onAutoFixResetResourcePacks(QPushButton* btn);
    void onAutoFixEnableForgeErroringRemoval(QPushButton* btn);
    void onAutoFixIncreaseMemory(QPushButton* btn);

private:
    void setupUi();
    QString buildReportText() const;
    QString getGameRoot() const;
    QString getModsRoot() const;
    void markButtonCompleted(QPushButton* btn, const QString& text);

    QList<CrashIssue> m_issues;
    QString m_instanceRoot;
    BaseInstance* m_instance = nullptr;
};
