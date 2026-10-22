// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#include "CrashAnalysisDialog.h"
#include "DiagI18n.h"

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "Application.h"
#include "BaseInstance.h"
#include "DesktopServices.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/mod/ModFolderModel.h"
#include "settings/SettingsObject.h"
#include "tasks/ConcurrentTask.h"
#include "ui/dialogs/CustomMessageBox.h"
#include "ui/dialogs/ProgressDialog.h"
#include "ui/dialogs/ResourceDownloadDialog.h"
#include "ui/pages/modplatform/ResourcePage.h"

CrashAnalysisDialog::CrashAnalysisDialog(const QList<CrashIssue>& issues,
                                         const QString& instanceRoot,
                                         BaseInstance* instance,
                                         QWidget* parent)
    : QDialog(parent)
    , m_issues(issues)
    , m_instanceRoot(instanceRoot)
    , m_instance(instance)
{
    setupUi();
}

void CrashAnalysisDialog::setupUi()
{
    setWindowTitle(DiagI18n::tr("Game Crash Diagnostics & Auto-Fix", "Диагностика сбоя игры и авто-исправление"));
    resize(760, 560);
    setMinimumSize(580, 420);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    // Header banner
    bool hasIssues = !m_issues.isEmpty();
    auto* bannerFrame = new QFrame(this);
    bannerFrame->setStyleSheet(
        QString("QFrame {"
                "  background-color: %1;"
                "  border: 1px solid %2;"
                "  border-radius: 8px;"
                "  padding: 10px;"
                "}")
            .arg(hasIssues ? "rgba(220, 53, 69, 0.12)" : "rgba(40, 167, 69, 0.12)")
            .arg(hasIssues ? "rgba(220, 53, 69, 0.35)" : "rgba(40, 167, 69, 0.35)")
    );

    auto* headerLayout = new QHBoxLayout(bannerFrame);
    headerLayout->setContentsMargins(10, 8, 10, 8);
    headerLayout->setSpacing(12);

    auto* iconLabel = new QLabel(bannerFrame);
    iconLabel->setPixmap(QIcon::fromTheme(hasIssues ? "dialog-error" : "security-high",
                                          QIcon::fromTheme(hasIssues ? "status-bad" : "status-good"))
                             .pixmap(36, 36));
    headerLayout->addWidget(iconLabel);

    auto* titleLayout = new QVBoxLayout();
    titleLayout->setSpacing(2);

    auto* titleLabel = new QLabel(
        hasIssues ? DiagI18n::tr("Crash Causes & Auto-Fix Options", "Причины сбоя игры и варианты исправления")
                  : DiagI18n::tr("No Known Issues Detected", "Известных проблем не обнаружено"),
        bannerFrame
    );
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(titleFont.pointSize() + 2);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    titleLayout->addWidget(titleLabel);

    auto* subtitleLabel = new QLabel(
        hasIssues ? DiagI18n::tr("Analyzed MinecraftLog and crash-reports/. Found %1 potential issue(s).",
                                 "Проанализированы журнал игры и отчёты crash-reports/. Найдено проблем: %1.").arg(m_issues.size())
                  : DiagI18n::tr("No specific crash patterns (Mixin, dependencies, memory, configs) found in the log.",
                                 "Специфических ошибок (Mixin, зависимости, память, конфиги) в журнале не обнаружено."),
        bannerFrame
    );
    subtitleLabel->setStyleSheet("color: gray; font-size: 11px;");
    titleLayout->addWidget(subtitleLabel);

    headerLayout->addLayout(titleLayout, 1);
    mainLayout->addWidget(bannerFrame);

    // Content scroll area
    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* scrollWidget = new QWidget();
    auto* issuesLayout = new QVBoxLayout(scrollWidget);
    issuesLayout->setContentsMargins(0, 4, 0, 4);
    issuesLayout->setSpacing(12);

    if (hasIssues) {
        for (const auto& issue : m_issues) {
            auto* card = new QFrame(scrollWidget);
            card->setFrameShape(QFrame::StyledPanel);
            card->setStyleSheet(
                "QFrame {"
                "  border: 1px solid rgba(128, 128, 128, 0.25);"
                "  border-radius: 8px;"
                "  background-color: palette(base);"
                "  padding: 8px;"
                "}"
            );

            auto* cardLayout = new QVBoxLayout(card);
            cardLayout->setContentsMargins(12, 10, 12, 10);
            cardLayout->setSpacing(8);

            // 1. Top row: Icon + Title + Severity badge
            auto* cardTitleRow = new QHBoxLayout();
            cardTitleRow->setSpacing(8);

            auto* cardIcon = new QLabel(card);
            cardIcon->setPixmap(QIcon::fromTheme(CrashAnalyzer::severityToIconName(issue.severity)).pixmap(22, 22));
            cardTitleRow->addWidget(cardIcon);

            auto* cardTitle = new QLabel(issue.title, card);
            QFont f = cardTitle->font();
            f.setBold(true);
            f.setPointSize(f.pointSize() + 1);
            cardTitle->setFont(f);
            cardTitleRow->addWidget(cardTitle, 1);

            auto* badge = new QLabel(CrashAnalyzer::severityToString(issue.severity), card);
            badge->setStyleSheet(
                issue.severity == CrashSeverity::Fatal || issue.severity == CrashSeverity::Error
                    ? "background-color: #c82333; color: white; padding: 4px 8px; border-radius: 4px; font-size: 11px; font-weight: bold;"
                    : "background-color: #d39e00; color: white; padding: 4px 8px; border-radius: 4px; font-size: 11px; font-weight: bold;"
            );
            cardTitleRow->addWidget(badge);
            cardLayout->addLayout(cardTitleRow);

            // 2. Verdict (Человеческим языком)
            auto* verdictBox = new QLabel(QString("<b>%1</b> %2").arg(DiagI18n::tr("Verdict:", "Вердикт:"), issue.details), card);
            verdictBox->setWordWrap(true);
            verdictBox->setStyleSheet("font-size: 13px; line-height: 1.4;");
            verdictBox->setTextInteractionFlags(Qt::TextSelectableByMouse);
            cardLayout->addWidget(verdictBox);

            // 3. Culprit File
            if (!issue.affectedFiles.isEmpty()) {
                auto* fileLabel = new QLabel(
                    QString("📁 <b>%1:</b> %2")
                        .arg(DiagI18n::tr("Culprit / Affected file", "Виновный / затронутый файл"))
                        .arg(issue.affectedFiles.join(", ")),
                    card
                );
                fileLabel->setStyleSheet("color: #e0a800; font-family: monospace; font-size: 12px; margin-left: 2px;");
                fileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
                cardLayout->addWidget(fileLabel);
            }

            // 4. Log Evidence Box (Строчка ошибки)
            if (!issue.rawEvidence.isEmpty()) {
                auto* evidenceBox = new QFrame(card);
                evidenceBox->setStyleSheet(
                    "QFrame {"
                    "  background-color: rgba(0, 0, 0, 0.25);"
                    "  border-left: 3px solid rgba(220, 53, 69, 0.7);"
                    "  border-radius: 4px;"
                    "  padding: 4px;"
                    "}"
                );
                auto* evLayout = new QVBoxLayout(evidenceBox);
                evLayout->setContentsMargins(8, 6, 8, 6);
                evLayout->setSpacing(2);

                auto* evHeader = new QLabel(DiagI18n::tr("Log Evidence:", "Улика из лога:"), evidenceBox);
                evHeader->setStyleSheet("color: #ff8888; font-weight: bold; font-size: 11px;");
                evLayout->addWidget(evHeader);

                auto* evText = new QLabel(issue.rawEvidence.first().trimmed(), evidenceBox);
                evText->setWordWrap(true);
                QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
                mono.setPointSize(mono.pointSize() - 1);
                evText->setFont(mono);
                evText->setStyleSheet("color: #d1d5db;");
                evText->setTextInteractionFlags(Qt::TextSelectableByMouse);
                evLayout->addWidget(evText);

                cardLayout->addWidget(evidenceBox);
            }

            // 5. Recommendation Box
            if (!issue.recommendation.isEmpty()) {
                auto* recBox = new QFrame(card);
                recBox->setStyleSheet(
                    "QFrame {"
                    "  background-color: rgba(40, 167, 69, 0.1);"
                    "  border-left: 3px solid #28a745;"
                    "  border-radius: 4px;"
                    "  padding: 4px;"
                    "}"
                );
                auto* recLayout = new QVBoxLayout(recBox);
                recLayout->setContentsMargins(8, 6, 8, 6);
                recLayout->setSpacing(2);

                auto* recHeader = new QLabel(DiagI18n::tr("Recommended Solution:", "Рекомендация по решению:"), recBox);
                recHeader->setStyleSheet("color: #28a745; font-weight: bold; font-size: 11px;");
                recLayout->addWidget(recHeader);

                auto* recText = new QLabel(issue.recommendation, recBox);
                recText->setWordWrap(true);
                recText->setTextInteractionFlags(Qt::TextSelectableByMouse);
                recLayout->addWidget(recText);

                cardLayout->addWidget(recBox);
            }

            // 6. Action Buttons (Auto-Fix)
            QList<CrashAction> actionList = issue.actions;
            if (actionList.isEmpty() && issue.actionType != CrashActionType::None) {
                actionList.append({ issue.actionType, issue.actionTarget, issue.actionLabel, QString() });
            }

            if (!actionList.isEmpty()) {
                auto* actionContainer = new QFrame(card);
                actionContainer->setStyleSheet(
                    "QFrame {"
                    "  background-color: rgba(255, 255, 255, 0.03);"
                    "  border: 1px dashed rgba(128, 128, 128, 0.35);"
                    "  border-radius: 6px;"
                    "  padding: 6px;"
                    "}"
                );
                auto* actContainerLayout = new QVBoxLayout(actionContainer);
                actContainerLayout->setContentsMargins(8, 6, 8, 6);
                actContainerLayout->setSpacing(8);

                if (actionList.size() > 1) {
                    auto* actHeader = new QLabel(DiagI18n::tr("Available Solution Actions (choose an option):",
                                                              "Варианты решения на выбор:"), actionContainer);
                    actHeader->setStyleSheet("color: #adb5bd; font-weight: bold; font-size: 11px; margin-bottom: 2px;");
                    actContainerLayout->addWidget(actHeader);
                }

                for (const auto& act : actionList) {
                    auto* singleActLayout = new QVBoxLayout();
                    singleActLayout->setSpacing(3);

                    auto* btnRow = new QHBoxLayout();
                    auto* actionBtn = new QPushButton(actionContainer);
                    actionBtn->setText(act.label);
                    actionBtn->setCursor(Qt::PointingHandCursor);

                    switch (act.type) {
                        case CrashActionType::DownloadMissingMod:
                            actionBtn->setIcon(QIcon::fromTheme("edit-download"));
                            actionBtn->setStyleSheet(
                                "QPushButton {"
                                "  background-color: #0b5ed7; color: white; font-weight: bold; padding: 6px 14px; border-radius: 5px; border: none;"
                                "}"
                                "QPushButton:hover { background-color: #0a58ca; }"
                            );
                            connect(actionBtn, &QPushButton::clicked, this, [this, target = act.target, actionBtn]() {
                                onAutoFixDownloadMod(target, actionBtn);
                            });
                            break;

                        case CrashActionType::DisableCulpritMod:
                            actionBtn->setIcon(QIcon::fromTheme("dialog-cancel", QIcon::fromTheme("process-stop")));
                            actionBtn->setStyleSheet(
                                "QPushButton {"
                                "  background-color: #b02a37; color: white; font-weight: bold; padding: 6px 14px; border-radius: 5px; border: none;"
                                "}"
                                "QPushButton:hover { background-color: #8c1e29; }"
                            );
                            connect(actionBtn, &QPushButton::clicked, this, [this, target = act.target, actionBtn]() {
                                onAutoFixDisableMod(target, actionBtn);
                            });
                            break;

                        case CrashActionType::DisableBothMods:
                            actionBtn->setIcon(QIcon::fromTheme("dialog-cancel", QIcon::fromTheme("process-stop")));
                            actionBtn->setStyleSheet(
                                "QPushButton {"
                                "  background-color: #842029; color: white; font-weight: bold; padding: 6px 14px; border-radius: 5px; border: none;"
                                "}"
                                "QPushButton:hover { background-color: #6a1a21; }"
                            );
                            connect(actionBtn, &QPushButton::clicked, this, [this, target = act.target, actionBtn]() {
                                onAutoFixDisableBothMods(target, actionBtn);
                            });
                            break;

                        case CrashActionType::ResetCorruptedConfig:
                            actionBtn->setIcon(QIcon::fromTheme("edit-clear"));
                            actionBtn->setStyleSheet(
                                "QPushButton {"
                                "  background-color: #d35400; color: white; font-weight: bold; padding: 6px 14px; border-radius: 5px; border: none;"
                                "}"
                                "QPushButton:hover { background-color: #ba4a00; }"
                            );
                            connect(actionBtn, &QPushButton::clicked, this, [this, target = act.target, actionBtn]() {
                                onAutoFixResetConfig(target, actionBtn);
                            });
                            break;

                        case CrashActionType::DisableShaders:
                            actionBtn->setIcon(QIcon::fromTheme("video-display"));
                            actionBtn->setStyleSheet(
                                "QPushButton {"
                                "  background-color: #6f42c1; color: white; font-weight: bold; padding: 6px 14px; border-radius: 5px; border: none;"
                                "}"
                                "QPushButton:hover { background-color: #59359a; }"
                            );
                            connect(actionBtn, &QPushButton::clicked, this, [this, actionBtn]() {
                                onAutoFixDisableShaders(actionBtn);
                            });
                            break;

                        case CrashActionType::ResetResourcePacks:
                            actionBtn->setIcon(QIcon::fromTheme("package-x-generic"));
                            actionBtn->setStyleSheet(
                                "QPushButton {"
                                "  background-color: #b76e00; color: white; font-weight: bold; padding: 6px 14px; border-radius: 5px; border: none;"
                                "}"
                                "QPushButton:hover { background-color: #945800; }"
                            );
                            connect(actionBtn, &QPushButton::clicked, this, [this, actionBtn]() {
                                onAutoFixResetResourcePacks(actionBtn);
                            });
                            break;

                        case CrashActionType::EnableForgeRemoveErroringEntities:
                            actionBtn->setIcon(QIcon::fromTheme("tools-check-spelling", QIcon::fromTheme("dialog-ok")));
                            actionBtn->setStyleSheet(
                                "QPushButton {"
                                "  background-color: #198754; color: white; font-weight: bold; padding: 6px 14px; border-radius: 5px; border: none;"
                                "}"
                                "QPushButton:hover { background-color: #157347; }"
                            );
                            connect(actionBtn, &QPushButton::clicked, this, [this, actionBtn]() {
                                onAutoFixEnableForgeErroringRemoval(actionBtn);
                            });
                            break;

                        case CrashActionType::IncreaseAllocatedMemory:
                            actionBtn->setIcon(QIcon::fromTheme("media-memory", QIcon::fromTheme("dialog-information")));
                            actionBtn->setStyleSheet(
                                "QPushButton {"
                                "  background-color: #0d6efd; color: white; font-weight: bold; padding: 6px 14px; border-radius: 5px; border: none;"
                                "}"
                                "QPushButton:hover { background-color: #0b5ed7; }"
                            );
                            connect(actionBtn, &QPushButton::clicked, this, [this, actionBtn]() {
                                onAutoFixIncreaseMemory(actionBtn);
                            });
                            break;

                        case CrashActionType::None:
                        default:
                            break;
                    }

                    btnRow->addWidget(actionBtn);
                    btnRow->addStretch();
                    singleActLayout->addLayout(btnRow);

                    if (!act.explanation.isEmpty()) {
                        auto* explLabel = new QLabel("ℹ️ " + act.explanation, actionContainer);
                        explLabel->setWordWrap(true);
                        explLabel->setStyleSheet("color: #a0aec0; font-size: 11px;");
                        singleActLayout->addWidget(explLabel);
                    }

                    actContainerLayout->addLayout(singleActLayout);
                }

                cardLayout->addWidget(actionContainer);
            }

            issuesLayout->addWidget(card);
        }
    } else {
        auto* emptyCard = new QFrame(scrollWidget);
        emptyCard->setFrameShape(QFrame::StyledPanel);
        auto* emptyLayout = new QVBoxLayout(emptyCard);
        emptyLayout->setContentsMargins(16, 24, 16, 24);

        auto* infoText = new QLabel(
            DiagI18n::tr(
                "The analyzer checked the log for common failure patterns:\n"
                "• Mixin injection conflicts and crashes in mod code\n"
                "• Ticking entity and broken block crashes\n"
                "• Corrupted config files (.toml/.json) and damaged archives\n"
                "• Rendering pipeline failures (Shaders / Resource Packs)\n"
                "• Missing dependencies and incompatible loaders\n"
                "• Out of Memory (OOM / Heap Space) errors\n\n"
                "If the game crashed unexpectedly, you can click 'Upload' in the log viewer to share it on mclo.gs for detailed inspection.",
                "Анализатор проверил лог на типичные проблемы:\n"
                "• Конфликты Mixin и сбои в коде модов\n"
                "• Сбойные сущности и блоки в мире (Ticking entity)\n"
                "• Повреждённые файлы конфигураций (.toml/.json) и повреждённые архивы\n"
                "• Ошибки рендера (шейдеры и ресурспаки)\n"
                "• Отсутствующие зависимости и моды под другой лоадер\n"
                "• Нехватка оперативной памяти (OOM)\n\n"
                "Если игра завершилась аварийно, рекомендуем нажать кнопку 'Upload' в окне логов "
                "и изучить полный журнал на сервисе mclo.gs."
            ),
            emptyCard
        );
        infoText->setWordWrap(true);
        emptyLayout->addWidget(infoText);
        issuesLayout->addWidget(emptyCard);
    }

    issuesLayout->addStretch();
    scrollArea->setWidget(scrollWidget);
    mainLayout->addWidget(scrollArea, 1);

    // Bottom action buttons
    auto* bottomLayout = new QHBoxLayout();

    auto* btnCopy = new QPushButton(DiagI18n::tr("Copy Analysis", "Копировать отчёт"), this);
    btnCopy->setIcon(QIcon::fromTheme("edit-copy"));
    connect(btnCopy, &QPushButton::clicked, this, &CrashAnalysisDialog::onCopyReport);
    bottomLayout->addWidget(btnCopy);

    if (btnCopy) {
        // already added
    }

    QString mRoot = getModsRoot();
    if (!mRoot.isEmpty() && QDir(mRoot).exists()) {
        auto* btnMods = new QPushButton(DiagI18n::tr("Mods Folder", "Папка модов"), this);
        btnMods->setIcon(QIcon::fromTheme("folder"));
        connect(btnMods, &QPushButton::clicked, this, &CrashAnalysisDialog::onOpenModsFolder);
        bottomLayout->addWidget(btnMods);
    }

    QString cRoot = getGameRoot() + "/crash-reports";
    if (QDir(cRoot).exists()) {
        auto* btnReports = new QPushButton(DiagI18n::tr("Crash Reports", "Папка отчётов"), this);
        btnReports->setIcon(QIcon::fromTheme("folder"));
        connect(btnReports, &QPushButton::clicked, this, &CrashAnalysisDialog::onOpenCrashReportsFolder);
        bottomLayout->addWidget(btnReports);
    }

    bottomLayout->addStretch();

    auto* btnClose = new QPushButton(DiagI18n::tr("Close", "Закрыть"), this);
    connect(btnClose, &QPushButton::clicked, this, &QDialog::accept);
    bottomLayout->addWidget(btnClose);

    mainLayout->addLayout(bottomLayout);
}

QString CrashAnalysisDialog::getGameRoot() const
{
    if (m_instance) {
        return m_instance->gameRoot();
    }
    if (!m_instanceRoot.isEmpty()) {
        if (QDir(m_instanceRoot + "/minecraft").exists()) {
            return m_instanceRoot + "/minecraft";
        }
        if (QDir(m_instanceRoot + "/.minecraft").exists()) {
            return m_instanceRoot + "/.minecraft";
        }
        return m_instanceRoot;
    }
    return QString();
}

QString CrashAnalysisDialog::getModsRoot() const
{
    if (m_instance) {
        return m_instance->modsRoot();
    }
    QString gr = getGameRoot();
    if (!gr.isEmpty()) {
        if (QDir(gr + "/mods").exists()) {
            return gr + "/mods";
        }
    }
    if (!m_instanceRoot.isEmpty()) {
        if (QDir(m_instanceRoot + "/mods").exists()) {
            return m_instanceRoot + "/mods";
        }
    }
    return gr.isEmpty() ? QString() : gr + "/mods";
}

void CrashAnalysisDialog::markButtonCompleted(QPushButton* btn, const QString& text)
{
    if (!btn) return;
    btn->setText(text);
    btn->setIcon(QIcon());
    btn->setStyleSheet(
        "QPushButton, QPushButton:disabled {"
        "  background-color: #28a745 !important;"
        "  color: #ffffff !important;"
        "  font-weight: bold;"
        "  font-size: 12px;"
        "  padding: 6px 14px;"
        "  border-radius: 5px;"
        "  border: 1px solid #1e7e34;"
        "}"
    );
    btn->setEnabled(false);
}

QString CrashAnalysisDialog::buildReportText() const
{
    QString report = QString("=== %1 ===\n\n").arg(DiagI18n::tr("Prism Launcher Diagnostics Report", "Результаты диагностики Prism Launcher"));
    if (m_issues.isEmpty()) {
        report += DiagI18n::tr("No specific known crash patterns detected in the log.\n", "Специфических известных проблем в логе не обнаружено.\n");
        return report;
    }

    int idx = 1;
    for (const auto& issue : m_issues) {
        report += QString("[%1] %2 (%3)\n").arg(QString::number(idx++), issue.title, CrashAnalyzer::severityToString(issue.severity));
        report += DiagI18n::tr("Details:\n", "Описание:\n") + issue.details + "\n";
        if (!issue.affectedFiles.isEmpty()) {
            report += DiagI18n::tr("Affected File(s): ", "Затронутые файлы: ") + issue.affectedFiles.join(", ") + "\n";
        }
        if (!issue.recommendation.isEmpty()) {
            report += DiagI18n::tr("Recommendation:\n", "Рекомендация:\n") + issue.recommendation + "\n";
        }
        if (!issue.rawEvidence.isEmpty()) {
            report += DiagI18n::tr("Evidence:\n", "Улика:\n") + issue.rawEvidence.first() + "\n";
        }
        report += "\n----------------------------------------\n\n";
    }

    return report;
}

void CrashAnalysisDialog::onCopyReport()
{
    QGuiApplication::clipboard()->setText(buildReportText());
}

void CrashAnalysisDialog::onOpenCrashReportsFolder()
{
    QString gr = getGameRoot();
    if (gr.isEmpty()) return;
    QString path = gr + "/crash-reports";
    DesktopServices::openPath(path);
}

void CrashAnalysisDialog::onOpenModsFolder()
{
    QString path = getModsRoot();
    if (path.isEmpty()) return;
    DesktopServices::openPath(path);
}

void CrashAnalysisDialog::onAutoFixDownloadMod(const QString& modName, QPushButton* btn)
{
    auto* mcInstance = dynamic_cast<MinecraftInstance*>(m_instance);
    if (!mcInstance) {
        CustomMessageBox::selectable(this, DiagI18n::tr("Error", "Ошибка"),
                                     DiagI18n::tr("Minecraft instance is not available for downloading mods.",
                                                  "Экземпляр Minecraft недоступен для загрузки модов."),
                                     QMessageBox::Critical)->show();
        return;
    }

    auto* profile = mcInstance->getPackProfile();
    if (!profile || !profile->getModLoaders().has_value()) {
        CustomMessageBox::selectable(this, DiagI18n::tr("No Mod Loader", "Нет загрузчика модов"),
                                     DiagI18n::tr("This instance does not have a mod loader installed (Forge, NeoForge, Fabric, Quilt).",
                                                  "В данном экземпляре не установлен загрузчик модов (Forge, NeoForge, Fabric, Quilt)."),
                                     QMessageBox::Warning)->show();
        return;
    }

    auto reply = CustomMessageBox::selectable(
        this,
        DiagI18n::tr("Confirm Auto-Fix", "Подтверждение авто-исправления"),
        DiagI18n::tr(
            "Open Prism Launcher Mod Downloader to install missing mod '%1'?\n\n"
            "• Target search query: '%1'\n"
            "• Compatible loader and Minecraft version are selected automatically.\n"
            "• You can choose from Modrinth or CurseForge and confirm installation.",
            "Открыть каталог Prism Launcher для установки недостающего мода '%1'?\n\n"
            "• Поисковый запрос: '%1'\n"
            "• Загрузчик и версия Minecraft подбираются автоматически под ваш экземпляр.\n"
            "• Вы сможете выбрать версию на Modrinth или CurseForge и подтвердить скачивание."
        ).arg(modName),
        QMessageBox::Question,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes
    )->exec();

    if (reply != QMessageBox::Yes) {
        return;
    }

    auto* downloadDialog = ResourceDownload::ResourceDownloadDialog::createMod(this, mcInstance->loaderModList(), mcInstance, false);
    if (downloadDialog->selectedPage()) {
        downloadDialog->selectedPage()->setSearchTerm(modName);
    }

    int res = downloadDialog->exec();
    if (res == QDialog::Accepted) {
        auto tasksList = downloadDialog->getTasks();
        if (!tasksList.isEmpty()) {
            auto app = APPLICATION_DYN;
            int numDownloads = 4;
            if (app && app->settings()) {
                numDownloads = app->settings()->get("NumberOfConcurrentDownloads").toInt();
            }

            ConcurrentTask tasks(DiagI18n::tr("Download Mods", "Скачивание модов"), numDownloads);
            for (auto& task : tasksList) {
                tasks.addTask(task);
            }

            ProgressDialog loadDialog(this);
            loadDialog.showSkipButton();
            loadDialog.execWithTask(&tasks);

            if (mcInstance->loaderModList()) {
                mcInstance->loaderModList()->update();
            }

            markButtonCompleted(btn, QString("✓ %1").arg(DiagI18n::tr("Done (Mod installed)", "Выполнено (Мод установлен)")));

            CustomMessageBox::selectable(this, DiagI18n::tr("Success", "Успешно"),
                                         DiagI18n::tr("Mod download completed. You can now launch Minecraft again.",
                                                      "Загрузка мода завершена. Теперь вы можете снова запустить Minecraft."),
                                         QMessageBox::Information)->show();
        }
    }
    downloadDialog->deleteLater();
}

void CrashAnalysisDialog::onAutoFixDisableMod(const QString& modFileName, QPushButton* btn)
{
    QString modsDir = getModsRoot();
    if (modsDir.isEmpty() || modFileName.isEmpty()) return;

    QFileInfo fi(modsDir + "/" + modFileName);
    if (!fi.exists()) {
        QDir dir(modsDir);
        for (const auto& entry : dir.entryInfoList(QStringList() << "*.jar" << "*.jar.disabled", QDir::Files)) {
            QString eName = entry.fileName();
            if (eName.compare(modFileName, Qt::CaseInsensitive) == 0 ||
                eName.compare(modFileName + ".jar", Qt::CaseInsensitive) == 0 ||
                eName.compare(modFileName + ".jar.disabled", Qt::CaseInsensitive) == 0 ||
                eName.startsWith(modFileName + "-", Qt::CaseInsensitive) ||
                eName.startsWith(modFileName + "_", Qt::CaseInsensitive)) {
                fi = entry;
                break;
            }
        }
    }

    if (!fi.exists()) {
        CustomMessageBox::selectable(this, DiagI18n::tr("File Not Found", "Файл не найден"),
                                     DiagI18n::tr("Mod file '%1' was not found in mods folder (%2).",
                                                  "Файл мода '%1' не найден в папке mods (%2).")
                                                  .arg(modFileName, modsDir),
                                     QMessageBox::Warning)->show();
        return;
    }

    if (fi.fileName().endsWith(".disabled", Qt::CaseInsensitive)) {
        markButtonCompleted(btn, QString("✓ %1").arg(DiagI18n::tr("Done (Mod already disabled — restart game)",
                                                                 "Выполнено (Мод уже отключён — перезапустите игру)")));
        CustomMessageBox::selectable(this, DiagI18n::tr("Already Disabled", "Уже отключён"),
                                     DiagI18n::tr("Mod '%1' is already disabled.",
                                                  "Мод '%1' уже отключён.").arg(fi.fileName()),
                                     QMessageBox::Information)->show();
        return;
    }

    QString newFileName = fi.fileName() + ".disabled";

    auto reply = CustomMessageBox::selectable(
        this,
        DiagI18n::tr("Confirm Auto-Fix", "Подтверждение авто-исправления"),
        DiagI18n::tr(
            "Apply auto-fix to resolve the game crash?\n\n"
            "• Problematic mod '%1' will be safely disabled.\n"
            "• The file will be renamed to '%2'.\n"
            "• The mod will NOT be deleted; you can re-enable it anytime in the Mods tab.",
            "Применить авто-исправление для устранения сбоя?\n\n"
            "• Проблемный мод '%1' будет временно отключён.\n"
            "• Файл будет безопасно переименован в '%2'.\n"
            "• Мод НЕ удаляется; вы сможете включить его обратно в любой момент во вкладке 'Моды'."
        ).arg(fi.fileName(), newFileName),
        QMessageBox::Question,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes
    )->exec();

    if (reply != QMessageBox::Yes) {
        return;
    }

    QString targetPath = fi.dir().filePath(newFileName);
    if (QFile::exists(targetPath)) {
        QFile::remove(targetPath);
    }

    if (QFile::rename(fi.absoluteFilePath(), targetPath)) {
        auto* mcInstance = dynamic_cast<MinecraftInstance*>(m_instance);
        if (mcInstance && mcInstance->loaderModList()) {
            mcInstance->loaderModList()->update();
        }
        markButtonCompleted(btn, QString("✓ %1").arg(DiagI18n::tr("Done (Mod disabled — restart game)",
                                                                 "Выполнено (Мод отключён — перезапустите игру)")));
        CustomMessageBox::selectable(this, DiagI18n::tr("Auto-Fix Applied", "Авто-исправление применено"),
                                     DiagI18n::tr("Mod '%1' was safely renamed to '%2'. You can now launch Minecraft again.",
                                                  "Мод '%1' успешно переименован в '%2'. Теперь можно попробовать запустить игру снова.")
                                                  .arg(fi.fileName(), newFileName),
                                     QMessageBox::Information)->show();
    } else {
        CustomMessageBox::selectable(this, DiagI18n::tr("Error", "Ошибка"),
                                     DiagI18n::tr("Failed to rename mod file.", "Не удалось переименовать файл мода."),
                                     QMessageBox::Critical)->show();
    }
}

void CrashAnalysisDialog::onAutoFixDisableBothMods(const QString& targets, QPushButton* btn)
{
    QStringList parts = targets.split('|');
    if (parts.size() < 2) return;
    QString modA = parts[0].trimmed();
    QString modB = parts[1].trimmed();

    auto reply = CustomMessageBox::selectable(
        this,
        DiagI18n::tr("Disable Both Mods", "Отключение обоих модов"),
        DiagI18n::tr(
            "Disable both conflicting mods (%1 and %2)?\n\n"
            "• Both mods will be safely renamed to '.disabled'.\n"
            "• Neither mod will be deleted; you can re-enable either anytime in the Mods tab.",
            "Отключить оба конфликтующих мода (%1 и %2)?\n\n"
            "• Оба мода будут безопасно переименованы в '.disabled'.\n"
            "• Моды НЕ удаляются; вы сможете включить любой из них обратно во вкладке 'Моды'."
        ).arg(modA, modB),
        QMessageBox::Question,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes
    )->exec();

    if (reply != QMessageBox::Yes) {
        return;
    }

    QString modsDir = getModsRoot();
    if (modsDir.isEmpty()) return;

    auto disableOne = [&modsDir](const QString& modName) -> bool {
        QFileInfo fi(modsDir + "/" + modName);
        if (!fi.exists()) {
            QDir dir(modsDir);
            for (const auto& entry : dir.entryInfoList(QStringList() << "*.jar" << "*.jar.disabled", QDir::Files)) {
                QString eName = entry.fileName();
                if (eName.compare(modName, Qt::CaseInsensitive) == 0 ||
                    eName.compare(modName + ".jar", Qt::CaseInsensitive) == 0 ||
                    eName.compare(modName + ".jar.disabled", Qt::CaseInsensitive) == 0 ||
                    eName.startsWith(modName + "-", Qt::CaseInsensitive) ||
                    eName.startsWith(modName + "_", Qt::CaseInsensitive)) {
                    fi = entry;
                    break;
                }
            }
        }
        if (!fi.exists()) return false;
        if (fi.fileName().endsWith(".disabled", Qt::CaseInsensitive)) return true;

        QString targetPath = fi.dir().filePath(fi.fileName() + ".disabled");
        if (QFile::exists(targetPath)) QFile::remove(targetPath);
        return QFile::rename(fi.absoluteFilePath(), targetPath);
    };

    bool okA = disableOne(modA);
    bool okB = disableOne(modB);

    auto* mcInstance = dynamic_cast<MinecraftInstance*>(m_instance);
    if (mcInstance && mcInstance->loaderModList()) {
        mcInstance->loaderModList()->update();
    }

    if (okA || okB) {
        markButtonCompleted(btn, QString("✓ %1").arg(DiagI18n::tr("Done (Both mods disabled — restart game)",
                                                                 "Выполнено (Оба мода отключены — перезапустите игру)")));
        CustomMessageBox::selectable(this, DiagI18n::tr("Auto-Fix Applied", "Авто-исправление применено"),
                                     DiagI18n::tr("Conflicting mods '%1' and '%2' were safely disabled. You can now launch Minecraft again.",
                                                  "Конфликтующие моды '%1' и '%2' успешно отключены. Теперь можно попробовать запустить игру снова.")
                                                  .arg(modA, modB),
                                     QMessageBox::Information)->show();
    } else {
        CustomMessageBox::selectable(this, DiagI18n::tr("Error", "Ошибка"),
                                     DiagI18n::tr("Failed to disable mod files in %1.",
                                                  "Не удалось отключить файлы модов в %1.").arg(modsDir),
                                     QMessageBox::Critical)->show();
    }
}

void CrashAnalysisDialog::onAutoFixResetConfig(const QString& configFileName, QPushButton* btn)
{
    QString gr = getGameRoot();
    if (gr.isEmpty() || configFileName.isEmpty()) return;

    QString configDir = gr + "/config";
    QFileInfo fi(configDir + "/" + configFileName);
    if (!fi.exists()) {
        // Search subdirectories inside configDir
        QDir dir(configDir);
        QFileInfoList list = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto& entry : list) {
            if (entry.isDir()) {
                QFileInfo sub(entry.absoluteFilePath() + "/" + configFileName);
                if (sub.exists()) {
                    fi = sub;
                    break;
                }
            }
        }
    }

    if (!fi.exists()) {
        fi.setFile(gr + "/" + configFileName);
    }

    if (!fi.exists()) {
        CustomMessageBox::selectable(this, DiagI18n::tr("File Not Found", "Файл не найден"),
                                     DiagI18n::tr("Config file '%1' was not found.",
                                                  "Файл конфигурации '%1' не найден.").arg(configFileName),
                                     QMessageBox::Warning)->show();
        return;
    }

    auto reply = CustomMessageBox::selectable(
        this,
        DiagI18n::tr("Reset Configuration", "Сброс конфигурации"),
        DiagI18n::tr(
            "Reset corrupted configuration file '%1'?\n\n"
            "• A safe backup copy '%2' will be created.\n"
            "• Minecraft will recreate clean default settings on next launch.",
            "Сбросить повреждённый файл конфигурации '%1'?\n\n"
            "• Будет создана безопасная резервная копия '%2'.\n"
            "• При следующем запуске игра создаст чистые настройки по умолчанию."
        ).arg(fi.fileName(), fi.fileName() + ".bak"),
        QMessageBox::Question,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes
    )->exec();

    if (reply != QMessageBox::Yes) {
        return;
    }

    QString backupPath = fi.absoluteFilePath() + ".bak";
    if (QFile::exists(backupPath)) {
        QFile::remove(backupPath);
    }

    if (QFile::rename(fi.absoluteFilePath(), backupPath)) {
        markButtonCompleted(btn, QString("✓ %1").arg(DiagI18n::tr("Done (Config reset to .bak — restart game)",
                                                                 "Выполнено (Конфиг сброшен в .bak — перезапустите игру)")));
        CustomMessageBox::selectable(this, DiagI18n::tr("Config Reset", "Конфиг сброшен"),
                                     DiagI18n::tr("Corrupted config was backed up as '%1'. Minecraft will recreate fresh default settings on next launch.",
                                                  "Повреждённый конфиг сохранён как '%1'. Minecraft создаст файл со стандартными настройками при следующем запуске.")
                                                  .arg(QFileInfo(backupPath).fileName()),
                                     QMessageBox::Information)->show();
    } else {
        CustomMessageBox::selectable(this, DiagI18n::tr("Error", "Ошибка"),
                                     DiagI18n::tr("Failed to backup and remove corrupted config file.",
                                                  "Не удалось создать копию и удалить повреждённый файл конфигурации."),
                                     QMessageBox::Critical)->show();
    }
}

void CrashAnalysisDialog::onAutoFixDisableShaders(QPushButton* btn)
{
    QString gr = getGameRoot();
    if (gr.isEmpty()) return;

    auto reply = CustomMessageBox::selectable(
        this,
        DiagI18n::tr("Disable Shaders", "Отключение шейдеров"),
        DiagI18n::tr(
            "Disable active shaders to fix rendering pipeline crash?\n\n"
            "• File 'optionsshaders.txt' will be safely renamed to 'optionsshaders.txt.bak'.\n"
            "• Iris / Oculus active shader configuration will be temporarily reset.\n"
            "• Your shader pack files in 'shaderpacks/' will NOT be deleted.",
            "Отключить активные шейдеры для устранения сбоя рендера?\n\n"
            "• Файл 'optionsshaders.txt' будет переименован в 'optionsshaders.txt.bak'.\n"
            "• Конфигурация активного шейдера Iris / Oculus будет сброшена к стандартной.\n"
            "• Сами файлы шейдеров в папке 'shaderpacks/' останутся в сохранности."
        ),
        QMessageBox::Question,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes
    )->exec();

    if (reply != QMessageBox::Yes) {
        return;
    }

    QStringList shaderOptionsPaths = {
        gr + "/optionsshaders.txt",
        m_instanceRoot + "/optionsshaders.txt"
    };

    bool renamed = false;
    for (const auto& path : shaderOptionsPaths) {
        if (QFile::exists(path)) {
            QString bak = path + ".bak";
            if (QFile::exists(bak)) QFile::remove(bak);
            if (QFile::rename(path, bak)) renamed = true;
        }
    }

    QStringList irisProps = {
        gr + "/config/iris.properties",
        gr + "/config/oculus.properties",
        m_instanceRoot + "/config/iris.properties",
        m_instanceRoot + "/config/oculus.properties"
    };
    for (const auto& propPath : irisProps) {
        if (QFile::exists(propPath)) {
            QString bak = propPath + ".bak";
            if (QFile::exists(bak)) QFile::remove(bak);
            if (QFile::rename(propPath, bak)) renamed = true;
        }
    }

    if (renamed) {
        markButtonCompleted(btn, QString("✓ %1").arg(DiagI18n::tr("Done (Shaders disabled — restart game)",
                                                                 "Выполнено (Шейдеры отключены — перезапустите игру)")));
        CustomMessageBox::selectable(this, DiagI18n::tr("Shaders Disabled", "Шейдеры отключены"),
                                     DiagI18n::tr("Active shaders have been safely disabled. You can now launch Minecraft.",
                                                  "Активные шейдеры были безопасно отключены. Теперь можно запускать игру."),
                                     QMessageBox::Information)->show();
    } else {
        if (QFile::exists(gr + "/optionsshaders.txt.bak") || QFile::exists(m_instanceRoot + "/optionsshaders.txt.bak")) {
            markButtonCompleted(btn, QString("✓ %1").arg(DiagI18n::tr("Done (Shaders already disabled — restart game)",
                                                                     "Выполнено (Шейдеры уже отключены — перезапустите игру)")));
            CustomMessageBox::selectable(this, DiagI18n::tr("Already Disabled", "Уже отключены"),
                                         DiagI18n::tr("Shaders are already disabled (optionsshaders.txt.bak exists).",
                                                      "Шейдеры уже отключены (найден optionsshaders.txt.bak)."),
                                         QMessageBox::Information)->show();
        } else {
            CustomMessageBox::selectable(this, DiagI18n::tr("Information", "Информация"),
                                         DiagI18n::tr("No active shader options file was found in %1.",
                                                      "Файл настроек активных шейдеров не найден в %1.").arg(gr),
                                         QMessageBox::Information)->show();
        }
    }
}

void CrashAnalysisDialog::onAutoFixResetResourcePacks(QPushButton* btn)
{
    QString gr = getGameRoot();
    if (gr.isEmpty()) return;

    auto reply = CustomMessageBox::selectable(
        this,
        DiagI18n::tr("Reset Resource Packs", "Сброс ресурспаков"),
        DiagI18n::tr(
            "Reset active resource packs to default vanilla textures?\n\n"
            "• Active packs list in 'options.txt' will be reset to default.\n"
            "• A backup copy 'options.txt.bak' will be created.\n"
            "• All resourcepack archives in 'resourcepacks/' will NOT be deleted.",
            "Сбросить активные ресурспаки к стандартным текстурам игры?\n\n"
            "• Список активных текстурпаков в 'options.txt' будет очищен.\n"
            "• Будет создана резервная копия 'options.txt.bak'.\n"
            "• Сами архивы ресурспаков в папке 'resourcepacks/' НЕ удаляются."
        ),
        QMessageBox::Question,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes
    )->exec();

    if (reply != QMessageBox::Yes) {
        return;
    }

    QString optionsPath = gr + "/options.txt";
    if (!QFile::exists(optionsPath)) {
        optionsPath = m_instanceRoot + "/options.txt";
    }

    if (!QFile::exists(optionsPath)) {
        CustomMessageBox::selectable(this, DiagI18n::tr("File Not Found", "Файл не найден"),
                                     DiagI18n::tr("options.txt was not found.", "Файл options.txt не найден."),
                                     QMessageBox::Warning)->show();
        return;
    }

    QString bakPath = optionsPath + ".bak";
    if (QFile::exists(bakPath)) QFile::remove(bakPath);
    QFile::copy(optionsPath, bakPath);

    QFile file(optionsPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        CustomMessageBox::selectable(this, DiagI18n::tr("Error", "Ошибка"),
                                     DiagI18n::tr("Failed to read options.txt.", "Не удалось прочитать options.txt."),
                                     QMessageBox::Critical)->show();
        return;
    }

    QString content = QString::fromUtf8(file.readAll());
    file.close();

    QStringList lines = content.split('\n');
    QStringList newLines;
    for (const auto& line : lines) {
        if (line.startsWith("resourcePacks:")) {
            newLines.append("resourcePacks:[\"vanilla\"]");
        } else if (line.startsWith("incompatibleResourcePacks:")) {
            newLines.append("incompatibleResourcePacks:[]");
        } else {
            newLines.append(line);
        }
    }

    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        file.write(newLines.join('\n').toUtf8());
        file.close();

        markButtonCompleted(btn, QString("✓ %1").arg(DiagI18n::tr("Done (Resource packs reset — restart game)",
                                                                 "Выполнено (Ресурспаки сброшены — перезапустите игру)")));
        CustomMessageBox::selectable(this, DiagI18n::tr("Resource Packs Reset", "Ресурспаки сброшены"),
                                     DiagI18n::tr("Active resource packs have been reset to default. You can now launch Minecraft.",
                                                  "Активные ресурспаки успешно сброшены к стандартным. Теперь можно запускать игру."),
                                     QMessageBox::Information)->show();
    }
}

void CrashAnalysisDialog::onAutoFixEnableForgeErroringRemoval(QPushButton* btn)
{
    QString gr = getGameRoot();
    if (gr.isEmpty()) return;

    auto reply = CustomMessageBox::selectable(
        this,
        DiagI18n::tr("Enable Entity Removal", "Включение авто-удаления сущностей"),
        DiagI18n::tr(
            "Enable Forge/NeoForge auto-removal of erroring entities?\n\n"
            "• Sets 'removeErroringEntities = true' and 'removeErroringBlockEntities = true'.\n"
            "• Instead of crashing the entire world, Minecraft will automatically remove corrupted entities when chunks load.",
            "Включить авто-удаление сбойных сущностей в Forge/NeoForge?\n\n"
            "• В конфиге будут включены параметры 'removeErroringEntities = true' и 'removeErroringBlockEntities = true'.\n"
            "• Вместо аварийного вылета игра автоматически и безопасно удалит сбойный объект при загрузке чанка."
        ),
        QMessageBox::Question,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes
    )->exec();

    if (reply != QMessageBox::Yes) {
        return;
    }

    QStringList candidates;
    QString configDir = gr + "/config";
    candidates.append(configDir + "/forge-server.toml");
    candidates.append(configDir + "/neoforge-server.toml");

    QString savesDir = gr + "/saves";
    if (QDir(savesDir).exists()) {
        QDir sDir(savesDir);
        for (const auto& w : sDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            candidates.append(w.absoluteFilePath() + "/serverconfig/forge-server.toml");
            candidates.append(w.absoluteFilePath() + "/serverconfig/neoforge-server.toml");
        }
    }

    int modifiedCount = 0;
    for (const auto& path : candidates) {
        QFileInfo fi(path);
        if (fi.exists()) {
            QFile file(path);
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QString content = QString::fromUtf8(file.readAll());
                file.close();

                content.replace(QRegularExpression(R"(removeErroringEntities\s*=\s*(false|true))"), "removeErroringEntities = true");
                content.replace(QRegularExpression(R"(removeErroringBlockEntities\s*=\s*(false|true))"), "removeErroringBlockEntities = true");

                if (!content.contains("removeErroringEntities")) {
                    content += "\n[server]\nremoveErroringEntities = true\nremoveErroringBlockEntities = true\n";
                }

                if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                    file.write(content.toUtf8());
                    file.close();
                    modifiedCount++;
                }
            }
        }
    }

    if (modifiedCount == 0) {
        QDir().mkpath(configDir);
        QString defPath = configDir + "/forge-server.toml";
        QFile file(defPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QString def = "[server]\nremoveErroringEntities = true\nremoveErroringBlockEntities = true\n";
            file.write(def.toUtf8());
            file.close();
            modifiedCount++;
        }
    }

    markButtonCompleted(btn, QString("✓ %1").arg(DiagI18n::tr("Done (Auto-removal enabled)", "Выполнено (Авто-удаление включено)")));

    CustomMessageBox::selectable(this, DiagI18n::tr("Success", "Успешно"),
                                 DiagI18n::tr("Auto-removal of erroring entities and block entities was enabled in server config.",
                                              "Авто-удаление сбойных сущностей и блоков успешно включено в конфигурации сервера."),
                                 QMessageBox::Information)->show();
}

void CrashAnalysisDialog::onAutoFixIncreaseMemory(QPushButton* btn)
{
    if (!m_instance) {
        CustomMessageBox::selectable(this, DiagI18n::tr("Error", "Ошибка"),
                                     DiagI18n::tr("Instance settings are not available.", "Настройки инстанции недоступны."),
                                     QMessageBox::Warning)->show();
        return;
    }

    auto s = m_instance->settings();
    int current = s->get("MaxMemAlloc").toInt();
    if (current <= 0) current = 4096;
    int newMax = current + 2048;

    auto reply = CustomMessageBox::selectable(
        this,
        DiagI18n::tr("Increase Allocated RAM", "Увеличение оперативной памяти"),
        DiagI18n::tr(
            "Increase allocated RAM for this instance?\n\n"
            "• Current allocation: %1 MB\n"
            "• New allocation: %2 MB (+2048 MB)\n"
            "• Setting 'OverrideMemory' will be activated for this instance.",
            "Увеличить выделенную оперативную память для этой сборки?\n\n"
            "• Текущее значение: %1 МБ\n"
            "• Новое значение: %2 МБ (+2048 МБ)\n"
            "• Для этой сборки будет включена индивидуальная настройка памяти."
        ).arg(current).arg(newMax),
        QMessageBox::Question,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes
    )->exec();

    if (reply != QMessageBox::Yes) {
        return;
    }

    s->set("OverrideMemory", true);
    s->set("MaxMemAlloc", newMax);

    markButtonCompleted(btn, QString("✓ %1 (%2 MB)").arg(DiagI18n::tr("Done (RAM increased)", "Выполнено (Память увеличена)")).arg(newMax));

    CustomMessageBox::selectable(this, DiagI18n::tr("Memory Increased", "Память увеличена"),
                                 DiagI18n::tr("Allocated RAM was increased to %1 MB. You can now launch Minecraft.",
                                              "Выделенная память успешно увеличена до %1 МБ. Теперь можно запускать игру.").arg(newMax),
                                 QMessageBox::Information)->show();
}
