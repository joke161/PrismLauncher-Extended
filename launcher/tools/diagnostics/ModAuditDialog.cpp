// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#include "ModAuditDialog.h"
#include "DiagI18n.h"

#include <QClipboard>
#include <QFrame>
#include <QGuiApplication>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

ModAuditDialog::ModAuditDialog(const AuditReport& report, QWidget* parent)
    : QDialog(parent)
    , m_report(report)
{
    setupUi();
}

void ModAuditDialog::setupUi()
{
    setWindowTitle(DiagI18n::tr("Mod Security Audit — %1", "Аудит безопасности мода — %1").arg(m_report.modName));
    resize(700, 520);
    setMinimumSize(540, 400);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    // Header with verdict banner
    auto* banner = new QFrame(this);
    banner->setStyleSheet(
        QString("QFrame {"
                "  border-radius: 8px;"
                "  padding: 12px;"
                "  background-color: %1;"
                "  color: white;"
                "}").arg(ModAuditor::verdictToColor(m_report.verdict))
    );

    auto* bannerLayout = new QHBoxLayout(banner);
    bannerLayout->setContentsMargins(8, 4, 8, 4);

    auto* bannerLeft = new QVBoxLayout();
    auto* verdictLabel = new QLabel(ModAuditor::verdictToString(m_report.verdict), banner);
    QFont vf = verdictLabel->font();
    vf.setBold(true);
    vf.setPointSize(vf.pointSize() + 3);
    verdictLabel->setFont(vf);
    verdictLabel->setStyleSheet("color: white;");
    bannerLeft->addWidget(verdictLabel);

    QString scanSummary = DiagI18n::tr(
        "Scanned %1 / %2 classes in %3 ms (archive files: %4)",
        "Просканировано %1 / %2 классов за %3 мс (всего файлов: %4)"
    ).arg(QString::number(m_report.classCount),
          QString::number(m_report.totalClasses),
          QString::number(m_report.scanTimeMs),
          QString::number(m_report.totalFiles));

    auto* modMetaLabel = new QLabel(
        QString("%1 %2 • %3")
            .arg(m_report.modName, m_report.version.isEmpty() ? "" : "(" + m_report.version + ")", scanSummary),
        banner
    );
    modMetaLabel->setStyleSheet("color: rgba(255, 255, 255, 0.9); font-size: 11px;");
    bannerLeft->addWidget(modMetaLabel);

    bannerLayout->addLayout(bannerLeft);
    bannerLayout->addStretch();

    auto* scoreLabel = new QLabel(DiagI18n::tr("Risk: %1 / 100", "Риск: %1 / 100").arg(m_report.riskScore), banner);
    QFont sf = scoreLabel->font();
    sf.setBold(true);
    sf.setPointSize(sf.pointSize() + 2);
    scoreLabel->setFont(sf);
    scoreLabel->setStyleSheet("color: white;");
    bannerLayout->addWidget(scoreLabel);

    mainLayout->addWidget(banner);

    // Scroll Area for findings
    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* scrollWidget = new QWidget();
    auto* findingsLayout = new QVBoxLayout(scrollWidget);
    findingsLayout->setContentsMargins(0, 4, 0, 4);
    findingsLayout->setSpacing(10);

    if (m_report.findings.isEmpty()) {
        auto* emptyCard = new QFrame(scrollWidget);
        emptyCard->setFrameShape(QFrame::StyledPanel);
        emptyCard->setStyleSheet("background-color: palette(base); border-radius: 8px; padding: 16px;");
        auto* emptyLayout = new QVBoxLayout(emptyCard);

        auto* cleanTitle = new QLabel(DiagI18n::tr("✓ No suspicious patterns detected", "✓ Подозрительных сигналов не обнаружено"), emptyCard);
        QFont f = cleanTitle->font();
        f.setBold(true);
        f.setPointSize(f.pointSize() + 1);
        cleanTitle->setFont(f);
        cleanTitle->setStyleSheet("color: #28a745;");
        emptyLayout->addWidget(cleanTitle);

        auto* cleanDesc = new QLabel(
            DiagI18n::tr(
                "Static bytecode Constant Pool analysis did not find any known malicious signatures:\n"
                "• No hardcoded exfiltration webhooks (Discord, Telegram, Pastebin, webhook.site)\n"
                "• No global keylogger hooks (GetAsyncKeyState, SetWindowsHookEx, JNativeHook)\n"
                "• No browser credential, token or crypto wallet stealing paths\n"
                "• No JNDI/RMI remote code execution injection strings (Log4Shell)",
                "Статический анализ constant pool байткода не выявил известных сигнатур вредоносного ПО:\n"
                "• Нет жестко прописанных адресов вебхуков (Discord, Telegram, Pastebin)\n"
                "• Нет глобальных кейлоггерных хуков клавиатуры\n"
                "• Нет обращений к базам паролей, сессиям Discord или криптокошелькам\n"
                "• Нет JNDI-инъекций (Log4Shell)"
            ),
            emptyCard
        );
        cleanDesc->setWordWrap(true);
        emptyLayout->addWidget(cleanDesc);

        findingsLayout->addWidget(emptyCard);
    } else {
        for (const auto& finding : m_report.findings) {
            auto* card = new QFrame(scrollWidget);
            card->setFrameShape(QFrame::StyledPanel);
            card->setStyleSheet(
                "QFrame {"
                "  border: 1px solid rgba(128, 128, 128, 0.3);"
                "  border-radius: 8px;"
                "  background-color: palette(base);"
                "  padding: 6px;"
                "}"
            );

            auto* cardLayout = new QVBoxLayout(card);
            cardLayout->setContentsMargins(12, 10, 12, 10);
            cardLayout->setSpacing(6);

            // Row 1: Title + Category + Severity
            auto* topRow = new QHBoxLayout();
            auto* title = new QLabel(finding.title, card);
            QFont tf = title->font();
            tf.setBold(true);
            title->setFont(tf);
            topRow->addWidget(title);

            auto* catBadge = new QLabel(finding.category, card);
            catBadge->setStyleSheet("color: gray; font-size: 11px;");
            topRow->addWidget(catBadge);

            topRow->addStretch();

            auto* sevBadge = new QLabel(ModAuditor::severityToString(finding.severity), card);
            sevBadge->setStyleSheet(
                finding.severity == AuditSeverity::Critical
                    ? "background-color: #8a2424; color: white; padding: 2px 6px; border-radius: 4px; font-size: 11px; font-weight: bold;"
                    : (finding.severity == AuditSeverity::Warning
                           ? "background-color: #856404; color: white; padding: 2px 6px; border-radius: 4px; font-size: 11px; font-weight: bold;"
                           : (finding.severity == AuditSeverity::Info
                                  ? "background-color: #17a2b8; color: white; padding: 2px 6px; border-radius: 4px; font-size: 11px;"
                                  : "background-color: #555; color: white; padding: 2px 6px; border-radius: 4px; font-size: 11px;"))
            );
            topRow->addWidget(sevBadge);
            cardLayout->addLayout(topRow);

            // Description
            auto* desc = new QLabel(finding.description, card);
            desc->setWordWrap(true);
            cardLayout->addWidget(desc);

            // Evidence and Class
            if (!finding.evidence.isEmpty()) {
                auto* evidenceBox = new QLabel(
                    finding.severity == AuditSeverity::Info
                        ? DiagI18n::tr("Context: %1", "Контекст: %1").arg(finding.evidence)
                        : DiagI18n::tr("Evidence: %1", "Улика: %1").arg(finding.evidence),
                    card
                );
                evidenceBox->setWordWrap(true);
                QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
                mono.setPointSize(mono.pointSize() - 1);
                evidenceBox->setFont(mono);
                evidenceBox->setStyleSheet(
                    finding.severity == AuditSeverity::Info
                        ? "color: #17a2b8; background-color: rgba(23, 162, 184, 0.1); padding: 4px; border-radius: 4px;"
                        : "color: #e06c75; background-color: rgba(0, 0, 0, 0.1); padding: 4px; border-radius: 4px;"
                );
                evidenceBox->setTextInteractionFlags(Qt::TextSelectableByMouse);
                cardLayout->addWidget(evidenceBox);
            }

            if (!finding.className.isEmpty()) {
                auto* classLbl = new QLabel(DiagI18n::tr("Class: %1", "Класс: %1").arg(finding.className), card);
                classLbl->setStyleSheet("color: gray; font-size: 11px;");
                classLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
                cardLayout->addWidget(classLbl);
            }

            findingsLayout->addWidget(card);
        }
    }

    findingsLayout->addStretch();
    scrollArea->setWidget(scrollWidget);
    mainLayout->addWidget(scrollArea, 1);

    // Bottom action bar
    auto* bottomLayout = new QHBoxLayout();

    auto* btnCopy = new QPushButton(DiagI18n::tr("Copy Report", "Копировать отчёт"), this);
    btnCopy->setIcon(QIcon::fromTheme("edit-copy"));
    connect(btnCopy, &QPushButton::clicked, this, &ModAuditDialog::onCopyReport);
    bottomLayout->addWidget(btnCopy);

    auto* btnTrust = new QPushButton(this);
    auto updateTrustButtonState = [this, btnTrust]() {
        bool trusted = ModAuditor::isModTrusted(m_report.jarPath);
        if (trusted) {
            btnTrust->setText(DiagI18n::tr("Remove from Trusted", "Убрать из доверенных"));
            btnTrust->setIcon(QIcon::fromTheme("dialog-cancel", QIcon::fromTheme("edit-delete")));
            btnTrust->setStyleSheet("QPushButton { color: #ff9999; }");
        } else {
            btnTrust->setText(DiagI18n::tr("Trust This Mod (Ignore)", "Доверять этому моду (В исключения)"));
            btnTrust->setIcon(QIcon::fromTheme("security-high", QIcon::fromTheme("emblem-favorite")));
            btnTrust->setStyleSheet("");
        }
    };
    updateTrustButtonState();
    connect(btnTrust, &QPushButton::clicked, this, [this, updateTrustButtonState]() {
        bool current = ModAuditor::isModTrusted(m_report.jarPath);
        ModAuditor::setModTrusted(m_report.jarPath, !current);
        updateTrustButtonState();
    });
    bottomLayout->addWidget(btnTrust);

    bottomLayout->addStretch();

    auto* btnClose = new QPushButton(DiagI18n::tr("Close", "Закрыть"), this);
    connect(btnClose, &QPushButton::clicked, this, &QDialog::accept);
    bottomLayout->addWidget(btnClose);

    mainLayout->addLayout(bottomLayout);
}

QString ModAuditDialog::buildReportText() const
{
    QString report = QString("=== %1: %2 ===\n")
        .arg(DiagI18n::tr("Mod Security Audit", "Аудит безопасности мода"), m_report.modName);
    report += QString("%1: %2 (%3: %4/100)\n")
        .arg(DiagI18n::tr("Verdict", "Вердикт"),
             ModAuditor::verdictToString(m_report.verdict),
             DiagI18n::tr("Risk Score", "Оценка риска"),
             QString::number(m_report.riskScore));
    report += QString("%1: %2 / %3 (%4 ms)\n\n")
        .arg(DiagI18n::tr("Scanned Classes", "Просканировано классов"),
             QString::number(m_report.classCount),
             QString::number(m_report.totalClasses),
             QString::number(m_report.scanTimeMs));

    if (m_report.findings.isEmpty()) {
        report += DiagI18n::tr("No threats detected.\n", "Угроз не обнаружено.\n");
        return report;
    }

    int idx = 1;
    for (const auto& f : m_report.findings) {
        report += QString("[%1] %2 (%3) — %4\n").arg(QString::number(idx++), f.title, f.category, ModAuditor::severityToString(f.severity));
        report += DiagI18n::tr("Description: ", "Описание: ") + f.description + "\n";
        if (!f.evidence.isEmpty()) report += DiagI18n::tr("Evidence: ", "Улика: ") + f.evidence + "\n";
        if (!f.className.isEmpty()) report += DiagI18n::tr("Class: ", "Класс: ") + f.className + "\n";
        report += "----------------------------------------\n";
    }

    return report;
}

void ModAuditDialog::onCopyReport()
{
    QGuiApplication::clipboard()->setText(buildReportText());
}
