// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#include "EditAccountDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

#include "tools/diagnostics/DiagI18n.h"

EditAccountDialog::EditAccountDialog(MinecraftAccountPtr account, QWidget* parent)
    : QDialog(parent)
    , m_account(account)
{
    setupUi();

    if (m_account) {
        setWindowTitle(DiagI18n::tr("Edit Offline Account", "Редактировать оффлайн-аккаунт"));
        m_usernameEdit->setText(m_account->profileName());
        m_effectiveSkinPath = m_account->skinPath();
        m_selectedSkinSource = m_account->skinPath();

        if (!m_effectiveSkinPath.isEmpty() && QFile::exists(m_effectiveSkinPath)) {
            m_skinPathLabel->setText(QFileInfo(m_effectiveSkinPath).fileName());
            m_skinPathLabel->setToolTip(m_effectiveSkinPath);
            updateFacePreview(m_effectiveSkinPath);
            m_resetSkinBtn->setEnabled(true);
        } else {
            m_skinPathLabel->setText(DiagI18n::tr("Default Skin", "Скин по умолчанию"));
            updateFacePreview(QString());
            m_resetSkinBtn->setEnabled(false);
        }

        int modelIdx = (m_account->modelType() == "alex") ? 1 : 0;
        m_modelCombo->setCurrentIndex(modelIdx);
    } else {
        setWindowTitle(DiagI18n::tr("Add Offline Account", "Добавить оффлайн-аккаунт"));
        m_skinPathLabel->setText(DiagI18n::tr("Default Skin", "Скин по умолчанию"));
        updateFacePreview(QString());
        m_resetSkinBtn->setEnabled(false);
    }

    updateAcceptAllowed();
}

void EditAccountDialog::setupUi()
{
    setMinimumWidth(440);
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(12);

    // Profile Details Group
    auto* profileGroup = new QGroupBox(DiagI18n::tr("Profile Information", "Данные профиля"), this);
    auto* profileLayout = new QVBoxLayout(profileGroup);
    profileLayout->setSpacing(8);

    auto* nameForm = new QFormLayout();
    m_usernameEdit = new QLineEdit(profileGroup);
    m_usernameEdit->setPlaceholderText(DiagI18n::tr("Username (e.g. Player)", "Имя игрока (например, Player)"));
    const QRegularExpression usernameRegExp("^[A-Za-z0-9_]{3,16}$");
    m_usernameValidator = new QRegularExpressionValidator(usernameRegExp, this);
    m_usernameEdit->setValidator(m_usernameValidator);
    connect(m_usernameEdit, &QLineEdit::textEdited, this, &EditAccountDialog::onUsernameEdited);
    nameForm->addRow(DiagI18n::tr("Player Name:", "Имя игрока:"), m_usernameEdit);
    profileLayout->addLayout(nameForm);

    m_allowInvalidCheck = new QCheckBox(DiagI18n::tr("Allow invalid usernames", "Разрешить нестандартные имена"), profileGroup);
    m_allowInvalidCheck->setToolTip(DiagI18n::tr(
        "A username is valid only if it is from 3 to 16 characters in length and contains only letters, numbers, and underscores.",
        "Имя считается стандартным, если состоит из 3-16 символов английского алфавита, цифр и знаков подчеркивания."));
    connect(m_allowInvalidCheck, &QCheckBox::checkStateChanged, this, &EditAccountDialog::onAllowInvalidChanged);
    profileLayout->addWidget(m_allowInvalidCheck);

    mainLayout->addWidget(profileGroup);

    // Skin Customization Group (Block «Кастомизация скина»)
    auto* skinGroup = new QGroupBox(DiagI18n::tr("Skin Customization", "Кастомизация скина"), this);
    auto* skinLayout = new QVBoxLayout(skinGroup);
    skinLayout->setSpacing(10);

    auto* skinRow = new QHBoxLayout();
    skinRow->setSpacing(12);

    // Face preview thumbnail
    m_facePreviewLabel = new QLabel(skinGroup);
    m_facePreviewLabel->setFixedSize(64, 64);
    m_facePreviewLabel->setStyleSheet(
        "QLabel {"
        "  background-color: palette(base);"
        "  border: 1px solid rgba(128, 128, 128, 0.4);"
        "  border-radius: 6px;"
        "}"
    );
    m_facePreviewLabel->setAlignment(Qt::AlignCenter);
    skinRow->addWidget(m_facePreviewLabel);

    // Buttons and info
    auto* skinDetailsLayout = new QVBoxLayout();
    skinDetailsLayout->setSpacing(6);

    auto* skinBtnRow = new QHBoxLayout();
    skinBtnRow->setSpacing(8);

    m_selectSkinBtn = new QPushButton(DiagI18n::tr("Choose skin file (.png)...", "Выбрать файл скина (.png)..."), skinGroup);
    connect(m_selectSkinBtn, &QPushButton::clicked, this, &EditAccountDialog::onSelectSkinClicked);
    skinBtnRow->addWidget(m_selectSkinBtn);

    m_resetSkinBtn = new QPushButton(DiagI18n::tr("Reset", "Сбросить"), skinGroup);
    connect(m_resetSkinBtn, &QPushButton::clicked, this, &EditAccountDialog::onResetSkinClicked);
    skinBtnRow->addWidget(m_resetSkinBtn);

    skinDetailsLayout->addLayout(skinBtnRow);

    m_skinPathLabel = new QLabel(skinGroup);
    m_skinPathLabel->setStyleSheet("color: gray; font-size: 11px;");
    m_skinPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    skinDetailsLayout->addWidget(m_skinPathLabel);

    skinRow->addLayout(skinDetailsLayout, 1);
    skinLayout->addLayout(skinRow);

    // Model selection
    auto* modelForm = new QFormLayout();
    m_modelCombo = new QComboBox(skinGroup);
    m_modelCombo->addItem(DiagI18n::tr("Classic (Steve) — 4px arms", "Классическая (Стив) — 4px"), "steve");
    m_modelCombo->addItem(DiagI18n::tr("Slim arms (Alex) — 3px arms", "Тонкие руки (Алекс) — 3px"), "alex");
    modelForm->addRow(DiagI18n::tr("Model geometry:", "Геометрия модели:"), m_modelCombo);
    skinLayout->addLayout(modelForm);

    mainLayout->addWidget(skinGroup);

    // Dialog buttons
    m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttonBox->button(QDialogButtonBox::Ok)->setText(DiagI18n::tr("OK", "ОК"));
    m_buttonBox->button(QDialogButtonBox::Cancel)->setText(DiagI18n::tr("Cancel", "Отмена"));
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &EditAccountDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(m_buttonBox);
}

QString EditAccountDialog::username() const
{
    return m_usernameEdit->text().trimmed();
}

QString EditAccountDialog::skinPath() const
{
    return m_effectiveSkinPath;
}

QString EditAccountDialog::modelType() const
{
    return m_modelCombo->currentData().toString();
}

void EditAccountDialog::onUsernameEdited(const QString&)
{
    updateAcceptAllowed();
}

void EditAccountDialog::onAllowInvalidChanged(const Qt::CheckState checkState)
{
    m_usernameEdit->setValidator(checkState == Qt::Checked ? nullptr : m_usernameValidator);
    updateAcceptAllowed();
}

void EditAccountDialog::updateAcceptAllowed()
{
    const QString name = username();
    const bool valid = m_allowInvalidCheck->isChecked() ? !name.isEmpty() : m_usernameEdit->hasAcceptableInput();
    m_buttonBox->button(QDialogButtonBox::Ok)->setEnabled(valid);
}

void EditAccountDialog::updateFacePreview(const QString& path)
{
    if (path.isEmpty() || !QFile::exists(path)) {
        // Draw default placeholder icon
        QPixmap emptyPixmap(64, 64);
        emptyPixmap.fill(Qt::transparent);
        QPainter p(&emptyPixmap);
        p.setPen(QColor(128, 128, 128, 120));
        p.drawRect(8, 8, 47, 47);
        p.setPen(QColor(128, 128, 128, 180));
        p.drawText(emptyPixmap.rect(), Qt::AlignCenter, DiagI18n::tr("No Skin", "Нет скина"));
        m_facePreviewLabel->setPixmap(emptyPixmap);
        return;
    }

    QImage skinImage(path);
    if (skinImage.isNull()) {
        m_facePreviewLabel->setText(DiagI18n::tr("Invalid", "Ошибка"));
        return;
    }

    // Extract Minecraft 8x8 face and overlay hat
    QImage faceImage(8, 8, QImage::Format_ARGB32);
    faceImage.fill(Qt::transparent);
    QPainter painter(&faceImage);

    // Base head layer: (8, 8, 8, 8)
    if (skinImage.width() >= 16 && skinImage.height() >= 16) {
        painter.drawImage(0, 0, skinImage.copy(8, 8, 8, 8));
    }
    // Outer hat layer: (40, 8, 8, 8)
    if (skinImage.width() >= 48 && skinImage.height() >= 16) {
        painter.drawImage(0, 0, skinImage.copy(40, 8, 8, 8));
    }
    painter.end();

    // Scale up nearest neighbor (pixelated) to 64x64
    QPixmap facePixmap = QPixmap::fromImage(faceImage).scaled(64, 64, Qt::KeepAspectRatio, Qt::FastTransformation);
    m_facePreviewLabel->setPixmap(facePixmap);
}

void EditAccountDialog::onSelectSkinClicked()
{
    QString selected = QFileDialog::getOpenFileName(
        this,
        DiagI18n::tr("Choose skin file (.png)", "Выбрать файл скина (.png)"),
        QString(),
        DiagI18n::tr("Skin Images (*.png)", "Файлы скинов (*.png)")
    );

    if (selected.isEmpty()) {
        return;
    }

    QImage test(selected);
    if (test.isNull()) {
        QMessageBox::warning(this,
                             DiagI18n::tr("Invalid Image", "Неверный файл"),
                             DiagI18n::tr("The selected file is not a valid PNG image.", "Выбранный файл не является корректным изображением PNG."));
        return;
    }

    m_selectedSkinSource = selected;
    m_skinPathLabel->setText(QFileInfo(selected).fileName());
    m_skinPathLabel->setToolTip(selected);
    updateFacePreview(selected);
    m_resetSkinBtn->setEnabled(true);
}

void EditAccountDialog::onResetSkinClicked()
{
    m_selectedSkinSource.clear();
    m_effectiveSkinPath.clear();
    m_skinPathLabel->setText(DiagI18n::tr("Default Skin", "Скин по умолчанию"));
    m_skinPathLabel->setToolTip(QString());
    updateFacePreview(QString());
    m_resetSkinBtn->setEnabled(false);
}

void EditAccountDialog::accept()
{
    const QString safeName = username();
    if (safeName.isEmpty()) {
        return;
    }

    // If skin was chosen, copy it to skins/<username>_skin.png
    if (!m_selectedSkinSource.isEmpty() && QFile::exists(m_selectedSkinSource)) {
        QDir skinsDir("skins");
        if (!skinsDir.exists()) {
            skinsDir.mkpath(".");
        }

        QString targetFileName = QString("%1_skin.png").arg(safeName);
        QString targetPath = skinsDir.filePath(targetFileName);

        if (QFileInfo(m_selectedSkinSource).absoluteFilePath() != QFileInfo(targetPath).absoluteFilePath()) {
            if (QFile::exists(targetPath)) {
                QFile::remove(targetPath);
            }
            if (QFile::copy(m_selectedSkinSource, targetPath)) {
                m_effectiveSkinPath = targetPath;
            } else {
                m_effectiveSkinPath = m_selectedSkinSource;
            }
        } else {
            m_effectiveSkinPath = targetPath;
        }
    }

    if (m_account) {
        m_account->setSkinPath(m_effectiveSkinPath);
        m_account->setModelType(modelType());
    }

    QDialog::accept();
}

