// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#include "CriticalSecurityWarningDialog.h"
#include "ModAuditDialog.h"
#include "DiagI18n.h"
#include "minecraft/mod/ModFolderModel.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFile>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

CriticalSecurityWarningDialog::CriticalSecurityWarningDialog(const QList<AuditReport>& dangerousReports,
                                                           ModFolderModel* model,
                                                           QWidget* parent)
    : QDialog(parent), m_reports(dangerousReports), m_model(model)
{
    setupUi();
}

void CriticalSecurityWarningDialog::setupUi()
{
    setWindowTitle(DiagI18n::tr("CRITICAL SECURITY THREAT DETECTED!", "ОБНАРУЖЕНА КРИТИЧЕСКАЯ УГРОЗА БЕЗОПАСНОСТИ!"));
    setWindowIcon(QIcon::fromTheme("dialog-error", QIcon::fromTheme("status-bad")));
    resize(640, 500);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    // Banner Header
    auto* bannerFrame = new QFrame(this);
    bannerFrame->setObjectName("criticalBanner");
    bannerFrame->setStyleSheet(
        "QFrame#criticalBanner {"
        "  background-color: #3b1115;"
        "  border: 2px solid #dc3545;"
        "  border-radius: 8px;"
        "  padding: 12px;"
        "}");

    auto* bannerLayout = new QHBoxLayout(bannerFrame);
    bannerLayout->setContentsMargins(8, 8, 8, 8);
    bannerLayout->setSpacing(16);

    auto* iconLabel = new QLabel(bannerFrame);
    iconLabel->setPixmap(QIcon::fromTheme("dialog-error", QIcon::fromTheme("status-bad")).pixmap(56, 56));
    iconLabel->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    bannerLayout->addWidget(iconLabel);

    auto* textLayout = new QVBoxLayout();
    textLayout->setSpacing(6);

    auto* titleLabel = new QLabel(bannerFrame);
    titleLabel->setText(QString("<span style='font-size: 17px; font-weight: bold; color: #ff4d4f;'>%1</span>")
                            .arg(DiagI18n::tr("CRITICAL SECURITY THREAT DETECTED!", "ОБНАРУЖЕНА КРИТИЧЕСКАЯ УГРОЗА БЕЗОПАСНОСТИ!")));
    titleLabel->setWordWrap(true);
    textLayout->addWidget(titleLabel);

    auto* descLabel = new QLabel(bannerFrame);
    descLabel->setText(DiagI18n::tr(
        "Prism Launcher detected dangerous code or backdoor indicators in the mod(s) below. "
        "Running Minecraft with these mods can compromise your PC, steal credentials (Discord, Minecraft tokens), or execute arbitrary code.",
        "Prism Launcher обнаружил опасный вредоносный код или признаки бэкдора в указанных ниже модах. "
        "Запуск игры с ними может скомпрометировать систему, украсть учетные данные (Discord, Minecraft) или выполнить сторонний вредоносный код."));
    descLabel->setWordWrap(true);
    descLabel->setStyleSheet("color: #ffcccc; font-size: 13px;");
    textLayout->addWidget(descLabel);

    bannerLayout->addLayout(textLayout, 1);
    mainLayout->addWidget(bannerFrame);

    // Scroll Area with list of infected mods
    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* listWidget = new QWidget(scrollArea);
    auto* listLayout = new QVBoxLayout(listWidget);
    listLayout->setContentsMargins(0, 4, 0, 4);
    listLayout->setSpacing(10);

    for (int i = 0; i < m_reports.size(); ++i) {
        const auto& report = m_reports[i];
        auto* modCard = new QFrame(listWidget);
        modCard->setObjectName("modCard");
        modCard->setStyleSheet(
            "QFrame#modCard {"
            "  background-color: #2b1d20;"
            "  border: 1px solid #c82333;"
            "  border-radius: 6px;"
            "  padding: 10px;"
            "}");

        auto* cardLayout = new QVBoxLayout(modCard);
        cardLayout->setSpacing(6);

        // Header: Mod title + Risk Score badge
        auto* topRow = new QHBoxLayout();
        auto* nameLabel = new QLabel(QString("<b>%1</b>").arg(report.modName), modCard);
        nameLabel->setStyleSheet("font-size: 14px; font-weight: bold; color: #fff;");
        topRow->addWidget(nameLabel, 1);

        QString badgeText;
        QString badgeColor;
        if (report.riskScore >= 80) {
            badgeText = DiagI18n::tr("DANGEROUS • Risk %1/100", "ОПАСНО • Риск %1/100").arg(report.riskScore);
            badgeColor = "#dc3545"; // Red
        } else if (report.riskScore >= 40) {
            badgeText = DiagI18n::tr("WARNING • Risk %1/100", "ВНИМАНИЕ • Риск %1/100").arg(report.riskScore);
            badgeColor = "#e67e22"; // Orange / Yellow
        } else if (report.riskScore >= 1) {
            badgeText = DiagI18n::tr("NOTICE • Risk %1/100", "ЗАМЕЧАНИЕ • Риск %1/100").arg(report.riskScore);
            badgeColor = "#3498db"; // Grey / Blue
        } else {
            badgeText = DiagI18n::tr("SAFE • Risk 0/100", "БЕЗОПАСЕН • Риск 0/100");
            badgeColor = "#28a745"; // Green
        }

        auto* riskBadge = new QLabel(QString(" %1 ").arg(badgeText), modCard);
        riskBadge->setStyleSheet(
            QString("background-color: %1; color: white; font-weight: bold; font-size: 11px; "
                    "padding: 3px 6px; border-radius: 4px;").arg(badgeColor));
        topRow->addWidget(riskBadge);
        cardLayout->addLayout(topRow);

        // Filename
        auto* fileLabel = new QLabel(QString("📁 %1").arg(QFileInfo(report.jarPath).fileName()), modCard);
        fileLabel->setStyleSheet("color: #bbb; font-family: monospace; font-size: 11px;");
        cardLayout->addWidget(fileLabel);

        // Key threats list
        int shownFindings = 0;
        QSet<QString> seenThreats;
        for (const auto& finding : report.findings) {
            if (finding.severity == AuditSeverity::Critical || finding.severity == AuditSeverity::Warning) {
                QString key = finding.title + ":" + finding.description;
                if (seenThreats.contains(key)) continue;
                seenThreats.insert(key);

                auto* findingLabel = new QLabel(QString("• <b>%1</b>: %2").arg(finding.title, finding.description), modCard);
                findingLabel->setWordWrap(true);
                findingLabel->setStyleSheet("color: #ff9999; font-size: 12px; margin-left: 8px;");
                cardLayout->addWidget(findingLabel);
                shownFindings++;
                if (shownFindings >= 4) break;
            }
        }

        // View report and Trust buttons
        auto* btnLayout = new QHBoxLayout();
        btnLayout->addStretch(1);

        auto* btnTrust = new QPushButton(DiagI18n::tr("Trust This Mod (Ignore)", "Доверять этому моду (Игнорировать)"), modCard);
        btnTrust->setIcon(QIcon::fromTheme("security-high", QIcon::fromTheme("emblem-favorite")));
        btnTrust->setStyleSheet("QPushButton { background-color: #2e4053; color: white; padding: 4px 8px; border-radius: 4px; }");
        connect(btnTrust, &QPushButton::clicked, this, [this, btnTrust, path = report.jarPath]() {
            ModAuditor::setModTrusted(path, true);
            btnTrust->setEnabled(false);
            btnTrust->setText(DiagI18n::tr("✓ Trusted (Exception added)", "✓ В доверенных (Исключение добавлено)"));
            btnTrust->setStyleSheet("QPushButton { background-color: #27ae60; color: white; padding: 4px 8px; border-radius: 4px; }");
            if (m_model) {
                m_model->update();
            }
        });
        btnLayout->addWidget(btnTrust);

        auto* btnReport = new QPushButton(DiagI18n::tr("View Full Audit...", "Подробный отчет..."), modCard);
        btnReport->setIcon(QIcon::fromTheme("dialog-information"));
        connect(btnReport, &QPushButton::clicked, this, [this, report]() {
            ModAuditDialog dlg(report, this);
            dlg.exec();
        });
        btnLayout->addWidget(btnReport);
        cardLayout->addLayout(btnLayout);

        listLayout->addWidget(modCard);
    }

    listLayout->addStretch(1);
    scrollArea->setWidget(listWidget);
    mainLayout->addWidget(scrollArea, 1);

    // Reassurance notice
    auto* noticeLabel = new QLabel(this);
    noticeLabel->setText(QString("ℹ️ <b>%1:</b> %2")
                             .arg(DiagI18n::tr("Safety Guarantee", "Гарантия безопасности"))
                             .arg(DiagI18n::tr("Mods will be safely disabled by appending '.disabled' to their filenames. Files are NOT deleted from disk.",
                                              "Моды будут безопасно отключены добавлением суффикса '.disabled' к имени файла. Файлы НЕ удаляются с диска.")));
    noticeLabel->setWordWrap(true);
    noticeLabel->setStyleSheet("color: #88d0ff; font-size: 12px; margin-top: 4px;");
    mainLayout->addWidget(noticeLabel);

    // Bottom action buttons
    auto* bottomLayout = new QHBoxLayout();

    auto* btnDisableAll = new QPushButton(DiagI18n::tr("🔴 Disable Dangerous Mods Immediately", "🔴 Немедленно отключить опасные моды"), this);
    btnDisableAll->setIcon(QIcon::fromTheme("dialog-cancel"));
    btnDisableAll->setStyleSheet(
        "QPushButton {"
        "  background-color: #dc3545;"
        "  color: white;"
        "  font-weight: bold;"
        "  font-size: 13px;"
        "  padding: 8px 16px;"
        "  border-radius: 6px;"
        "}"
        "QPushButton:hover { background-color: #c82333; }"
        "QPushButton:pressed { background-color: #bd2130; }");
    connect(btnDisableAll, &QPushButton::clicked, this, &CriticalSecurityWarningDialog::onDisableAllDangerous);
    bottomLayout->addWidget(btnDisableAll);

    bottomLayout->addStretch(1);

    auto* btnIgnore = new QPushButton(DiagI18n::tr("Ignore and Close", "Игнорировать и закрыть"), this);
    connect(btnIgnore, &QPushButton::clicked, this, &QDialog::reject);
    bottomLayout->addWidget(btnIgnore);

    mainLayout->addLayout(bottomLayout);
}

void CriticalSecurityWarningDialog::onDisableAllDangerous()
{
    QStringList disabledNames;

    for (const auto& report : m_reports) {
        QString path = report.jarPath;
        QFileInfo fi(path);
        if (fi.exists() && !path.endsWith(".disabled", Qt::CaseInsensitive)) {
            QString newPath = path + ".disabled";
            if (QFile::rename(path, newPath)) {
                disabledNames.append(fi.fileName());
            }
        }
    }

    if (m_model) {
        m_model->update();
    }

    QMessageBox::information(
        this,
        DiagI18n::tr("Mods Disabled", "Моды отключены"),
        QString("%1\n\n%2:\n• %3")
            .arg(DiagI18n::tr("Dangerous mod(s) have been successfully disabled and will not be loaded by Minecraft.",
                              "Опасные моды были успешно отключены и больше не будут загружаться игрой."))
            .arg(DiagI18n::tr("Disabled files", "Отключенные файлы"))
            .arg(disabledNames.join("\n• ")));

    accept();
}
