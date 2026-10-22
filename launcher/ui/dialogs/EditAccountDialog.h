// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#pragma once

#include <QDialog>
#include <QRegularExpressionValidator>
#include "minecraft/auth/MinecraftAccount.h"

class QLineEdit;
class QCheckBox;
class QPushButton;
class QComboBox;
class QLabel;
class QDialogButtonBox;

class EditAccountDialog : public QDialog {
    Q_OBJECT
public:
    explicit EditAccountDialog(MinecraftAccountPtr account = nullptr, QWidget* parent = nullptr);
    ~EditAccountDialog() override = default;

    QString username() const;
    QString skinPath() const;
    QString modelType() const; // "steve" or "alex"

private slots:
    void onSelectSkinClicked();
    void onResetSkinClicked();
    void onUsernameEdited(const QString& text);
    void onAllowInvalidChanged(Qt::CheckState state);
    void updateAcceptAllowed();
    void updateFacePreview(const QString& path);

public slots:
    void accept() override;

private:
    void setupUi();

    MinecraftAccountPtr m_account;

    QLineEdit* m_usernameEdit = nullptr;
    QCheckBox* m_allowInvalidCheck = nullptr;
    QRegularExpressionValidator* m_usernameValidator = nullptr;

    QLabel* m_facePreviewLabel = nullptr;
    QLabel* m_skinPathLabel = nullptr;
    QPushButton* m_selectSkinBtn = nullptr;
    QPushButton* m_resetSkinBtn = nullptr;
    QComboBox* m_modelCombo = nullptr;
    QDialogButtonBox* m_buttonBox = nullptr;

    QString m_selectedSkinSource;
    QString m_effectiveSkinPath;
};

