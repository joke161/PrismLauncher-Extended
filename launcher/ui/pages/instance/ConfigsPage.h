// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#pragma once

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#include "minecraft/MinecraftInstance.h"
#include "tools/configs/ConfigDocument.h"
#include "ui/pages/BasePage.h"

class ConfigsPage : public QWidget, public BasePage {
    Q_OBJECT

public:
    explicit ConfigsPage(MinecraftInstance* instance, QWidget* parent = nullptr);
    ~ConfigsPage() override = default;

    QString displayName() const override;
    QIcon icon() const override;
    QString id() const override { return "configs"; }
    QString helpPage() const override { return "configs"; }
    bool shouldDisplay() const override { return true; }

    bool apply() override;
    void retranslate() override;

protected:
    void openedImpl() override;

private slots:
    void onFileSearchChanged(const QString& query);
    void onFileSelected(QTreeWidgetItem* current, QTreeWidgetItem* previous);
    void onSettingSearchChanged(const QString& query);
    void onModeChanged(int id);
    void onSaveClicked();
    void onResetClicked();
    void onRefreshFilesClicked();
    void onOpenFolderClicked();
    void onCodeEditorTextChanged();

private:
    struct UiFieldMapping {
        QString section;
        QString key;
        QString title;
        QWidget* rowWidget = nullptr;
        QWidget* editorWidget = nullptr;
        QWidget* dividerWidget = nullptr;
        QString description;
        QString currentValue;
    };

    struct UiSectionMapping {
        QString name;
        QWidget* cardWidget = nullptr;
        QList<UiFieldMapping> fields;
    };

    void setupUi();
    void scanConfigFiles();
    bool maybeSaveBeforeSwitch();
    void loadFile(const QString& filePath);
    void renderGuiCards();
    void updateSaveButton();
    void onFieldValueChanged(const QString& section, const QString& key, const QString& newValue);

    MinecraftInstance* m_instance = nullptr;
    ConfigEditor::ConfigDocument m_doc;
    QHash<QString, QString> m_langMap;
    QString m_currentFilePath;
    bool m_hasUnsavedChanges = false;
    bool m_ignoreCodeChanges = false;
    int m_totalConfigFilesCount = 0;

    // UI elements - Left Panel
    QLineEdit* m_fileSearchEdit = nullptr;
    QTreeWidget* m_fileTree = nullptr;
    QLabel* m_fileCountLabel = nullptr;
    QPushButton* m_btnRefreshFiles = nullptr;
    QPushButton* m_btnOpenFolder = nullptr;

    // UI elements - Right Panel
    QWidget* m_rightPanel = nullptr;
    QLabel* m_lblCurrentFile = nullptr;
    QLabel* m_lblCurrentPath = nullptr;
    QLineEdit* m_settingSearchEdit = nullptr;
    QButtonGroup* m_modeGroup = nullptr;
    QPushButton* m_btnModeGui = nullptr;
    QPushButton* m_btnModeCode = nullptr;
    QPushButton* m_btnSave = nullptr;
    QPushButton* m_btnReset = nullptr;

    QStackedWidget* m_stackedWidget = nullptr;
    QWidget* m_emptyStateWidget = nullptr;
    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_scrollContentWidget = nullptr;
    QVBoxLayout* m_scrollContentLayout = nullptr;
    QPlainTextEdit* m_codeEditor = nullptr;

    QList<UiSectionMapping> m_uiSections;
};

