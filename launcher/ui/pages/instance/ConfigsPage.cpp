// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#include "ConfigsPage.h"

#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QTimer>
#include <QWheelEvent>

#include "DesktopServices.h"
#include "tools/configs/ModLangResolver.h"
#include "tools/diagnostics/DiagI18n.h"

namespace {

class ConfigSpinBox : public QSpinBox {
public:
    explicit ConfigSpinBox(QWidget* parent = nullptr) : QSpinBox(parent)
    {
        setFocusPolicy(Qt::StrongFocus);
        if (lineEdit()) {
            lineEdit()->setFocusPolicy(Qt::StrongFocus);
        }
    }

protected:
    void wheelEvent(QWheelEvent* event) override
    {
        bool isFocused = hasFocus() || (lineEdit() && lineEdit()->hasFocus());
        if (!isFocused) {
            event->ignore();
        } else {
            QSpinBox::wheelEvent(event);
        }
    }
};

class ConfigDoubleSpinBox : public QDoubleSpinBox {
public:
    explicit ConfigDoubleSpinBox(QWidget* parent = nullptr) : QDoubleSpinBox(parent)
    {
        setFocusPolicy(Qt::StrongFocus);
        setLocale(QLocale::c());
        if (lineEdit()) {
            lineEdit()->setFocusPolicy(Qt::StrongFocus);
        }
    }

protected:
    void wheelEvent(QWheelEvent* event) override
    {
        bool isFocused = hasFocus() || (lineEdit() && lineEdit()->hasFocus());
        if (!isFocused) {
            event->ignore();
        } else {
            QDoubleSpinBox::wheelEvent(event);
        }
    }
};

class ConfigComboBox : public QComboBox {
public:
    explicit ConfigComboBox(QWidget* parent = nullptr) : QComboBox(parent)
    {
        setFocusPolicy(Qt::StrongFocus);
        if (lineEdit()) {
            lineEdit()->setFocusPolicy(Qt::StrongFocus);
        }
    }

protected:
    void wheelEvent(QWheelEvent* event) override
    {
        bool isFocused = hasFocus() || (lineEdit() && lineEdit()->hasFocus());
        if (!isFocused) {
            event->ignore();
        } else {
            QComboBox::wheelEvent(event);
        }
    }
};

QString formatDoubleValue(double val)
{
    QString s = QString::number(val, 'f', 6);
    while (s.contains('.') && s.endsWith('0')) {
        s.chop(1);
    }
    if (s.endsWith('.')) {
        s.chop(1);
    }
    return s;
}

bool isKeybindField(const ConfigEditor::Field& field)
{
    QString k = field.key.toLower();
    QString rawK = field.rawKey.toLower();
    QString val = field.currentValue.toLower();

    if (k.startsWith("key_") || rawK.startsWith("key_") || rawK.startsWith("key.") || k.startsWith("key.")) {
        return true;
    }
    if (k.contains("keybind") || k.contains("hotkey") || k.contains("shortcut")) {
        return true;
    }
    if (val.startsWith("key.keyboard.") || val.startsWith("key.mouse.") || val.startsWith("key.categories")) {
        return true;
    }
    return false;
}

class ConfigKeybindButton : public QPushButton {
public:
    explicit ConfigKeybindButton(const QString& mcKey, QWidget* parent = nullptr)
        : QPushButton(parent)
        , m_mcKey(mcKey)
    {
        setFocusPolicy(Qt::StrongFocus);
        setMinimumWidth(220);
        updateDisplay();
        connect(this, &QPushButton::clicked, this, [this]() {
            if (!m_isListening) {
                startListening();
            }
        });
    }

    void setOnKeybindChanged(std::function<void(const QString&)> cb)
    {
        m_onKeybindChanged = std::move(cb);
    }

    QString minecraftKey() const { return m_mcKey; }

    void setMinecraftKey(const QString& key)
    {
        m_mcKey = key;
        updateDisplay();
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (m_isListening) {
            QString mc = mouseButtonToMinecraftKey(event->button());
            if (!mc.isEmpty()) {
                m_mcKey = mc;
                stopListening();
                if (m_onKeybindChanged) {
                    m_onKeybindChanged(m_mcKey);
                }
                return;
            }
        }
        QPushButton::mousePressEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (m_isListening) {
            if (event->key() == Qt::Key_Escape) {
                stopListening();
                return;
            }
            QString mc = qtKeyToMinecraftKey(event->key(), event->modifiers());
            if (!mc.isEmpty()) {
                m_mcKey = mc;
                stopListening();
                if (m_onKeybindChanged) {
                    m_onKeybindChanged(m_mcKey);
                }
                return;
            }
        }
        QPushButton::keyPressEvent(event);
    }

    void focusOutEvent(QFocusEvent* event) override
    {
        if (m_isListening) {
            stopListening();
        }
        QPushButton::focusOutEvent(event);
    }

private:
    void startListening()
    {
        m_isListening = true;
        setText(DiagI18n::tr("> Press any key... (Esc to cancel) <", "> Нажмите клавишу... (Esc — отмена) <"));
        setStyleSheet(
            "QPushButton {"
            "  background-color: #d39e00;"
            "  color: black;"
            "  font-weight: bold;"
            "  border: 2px solid #ffc107;"
            "  border-radius: 4px;"
            "  padding: 4px 12px;"
            "}"
        );
        grabKeyboard();
    }

    void stopListening()
    {
        if (m_isListening) {
            m_isListening = false;
            releaseKeyboard();
            updateDisplay();
            setStyleSheet("");
        }
    }

    void updateDisplay()
    {
        setText(formatKeyDisplayName(m_mcKey));
        setToolTip(QString("%1: %2").arg(DiagI18n::tr("Minecraft Key", "Код клавиши Minecraft"), m_mcKey));
    }

    static QString mouseButtonToMinecraftKey(Qt::MouseButton btn)
    {
        switch (btn) {
            case Qt::LeftButton: return "key.mouse.left";
            case Qt::RightButton: return "key.mouse.right";
            case Qt::MiddleButton: return "key.mouse.middle";
            case Qt::BackButton: return "key.mouse.4";
            case Qt::ForwardButton: return "key.mouse.5";
            default: return QString();
        }
    }

    static QString qtKeyToMinecraftKey(int key, Qt::KeyboardModifiers mods)
    {
        Q_UNUSED(mods);
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            return QString("key.keyboard.%1").arg(QChar('a' + (key - Qt::Key_A)));
        }
        if (key >= Qt::Key_0 && key <= Qt::Key_9) {
            return QString("key.keyboard.%1").arg(key - Qt::Key_0);
        }
        if (key >= Qt::Key_F1 && key <= Qt::Key_F12) {
            return QString("key.keyboard.f%1").arg(key - Qt::Key_F1 + 1);
        }
        switch (key) {
            case Qt::Key_Space: return "key.keyboard.space";
            case Qt::Key_Shift: return "key.keyboard.left.shift";
            case Qt::Key_Control: return "key.keyboard.left.control";
            case Qt::Key_Alt: return "key.keyboard.left.alt";
            case Qt::Key_Tab: return "key.keyboard.tab";
            case Qt::Key_Return:
            case Qt::Key_Enter: return "key.keyboard.enter";
            case Qt::Key_Backspace: return "key.keyboard.backspace";
            case Qt::Key_CapsLock: return "key.keyboard.caps.lock";
            case Qt::Key_Up: return "key.keyboard.up";
            case Qt::Key_Down: return "key.keyboard.down";
            case Qt::Key_Left: return "key.keyboard.left";
            case Qt::Key_Right: return "key.keyboard.right";
            case Qt::Key_Delete: return "key.keyboard.delete";
            case Qt::Key_Insert: return "key.keyboard.insert";
            case Qt::Key_Home: return "key.keyboard.home";
            case Qt::Key_End: return "key.keyboard.end";
            case Qt::Key_PageUp: return "key.keyboard.page.up";
            case Qt::Key_PageDown: return "key.keyboard.page.down";
            case Qt::Key_Minus: return "key.keyboard.minus";
            case Qt::Key_Equal: return "key.keyboard.equal";
            case Qt::Key_BracketLeft: return "key.keyboard.left.bracket";
            case Qt::Key_BracketRight: return "key.keyboard.right.bracket";
            case Qt::Key_Semicolon: return "key.keyboard.semicolon";
            case Qt::Key_Apostrophe: return "key.keyboard.apostrophe";
            case Qt::Key_QuoteLeft: return "key.keyboard.grave.accent";
            case Qt::Key_Backslash: return "key.keyboard.backslash";
            case Qt::Key_Comma: return "key.keyboard.comma";
            case Qt::Key_Period: return "key.keyboard.period";
            case Qt::Key_Slash: return "key.keyboard.slash";
            default: return QString();
        }
    }

    static QString formatKeyDisplayName(const QString& mcKey)
    {
        return ConfigEditor::ModLangResolver::formatKeyDisplayName(mcKey);
    }

    bool m_isListening = false;
    QString m_mcKey;
    std::function<void(const QString&)> m_onKeybindChanged;
};

} // namespace

ConfigsPage::ConfigsPage(MinecraftInstance* instance, QWidget* parent)
    : QWidget(parent)
    , m_instance(instance)
{
    setupUi();
    retranslate();
}

QString ConfigsPage::displayName() const
{
    return DiagI18n::tr("Configs", "Конфигурации");
}

QIcon ConfigsPage::icon() const
{
    auto icon = QIcon::fromTheme("settings");
    if (icon.isNull()) {
        icon = QIcon::fromTheme("configure");
    }
    return icon;
}

void ConfigsPage::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);

    // ==========================================
    // Left Panel: File Explorer
    // ==========================================
    auto* leftWidget = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setContentsMargins(8, 8, 8, 8);
    leftLayout->setSpacing(8);

    m_fileSearchEdit = new QLineEdit(leftWidget);
    m_fileSearchEdit->setClearButtonEnabled(true);
    connect(m_fileSearchEdit, &QLineEdit::textChanged, this, &ConfigsPage::onFileSearchChanged);
    leftLayout->addWidget(m_fileSearchEdit);

    m_fileTree = new QTreeWidget(leftWidget);
    m_fileTree->setHeaderHidden(true);
    m_fileTree->setAnimated(true);
    connect(m_fileTree, &QTreeWidget::currentItemChanged, this, &ConfigsPage::onFileSelected);
    leftLayout->addWidget(m_fileTree, 1);

    auto* leftBottomLayout = new QHBoxLayout();
    leftBottomLayout->setSpacing(6);

    m_fileCountLabel = new QLabel(leftWidget);
    m_fileCountLabel->setStyleSheet("color: gray; font-size: 11px;");
    leftBottomLayout->addWidget(m_fileCountLabel, 1);

    m_btnRefreshFiles = new QPushButton(leftWidget);
    m_btnRefreshFiles->setIcon(QIcon::fromTheme("view-refresh"));
    connect(m_btnRefreshFiles, &QPushButton::clicked, this, &ConfigsPage::onRefreshFilesClicked);
    leftBottomLayout->addWidget(m_btnRefreshFiles);

    m_btnOpenFolder = new QPushButton(leftWidget);
    m_btnOpenFolder->setIcon(QIcon::fromTheme("folder"));
    connect(m_btnOpenFolder, &QPushButton::clicked, this, &ConfigsPage::onOpenFolderClicked);
    leftBottomLayout->addWidget(m_btnOpenFolder);

    leftLayout->addLayout(leftBottomLayout);
    splitter->addWidget(leftWidget);

    // ==========================================
    // Right Panel: Editor
    // ==========================================
    m_rightPanel = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(m_rightPanel);
    rightLayout->setContentsMargins(8, 8, 8, 8);
    rightLayout->setSpacing(8);

    // Header Toolbar
    auto* headerCard = new QFrame(m_rightPanel);
    headerCard->setStyleSheet(
        "QFrame {"
        "  background-color: palette(base);"
        "  border: 1px solid rgba(128, 128, 128, 0.25);"
        "  border-radius: 6px;"
        "  padding: 4px;"
        "}"
    );
    auto* headerLayout = new QVBoxLayout(headerCard);
    headerLayout->setContentsMargins(8, 8, 8, 8);
    headerLayout->setSpacing(8);

    // File name and relative path
    auto* titleRow = new QHBoxLayout();
    m_lblCurrentFile = new QLabel(headerCard);
    QFont titleFont = m_lblCurrentFile->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    m_lblCurrentFile->setFont(titleFont);
    titleRow->addWidget(m_lblCurrentFile);

    m_lblCurrentPath = new QLabel(headerCard);
    m_lblCurrentPath->setStyleSheet("color: gray; font-size: 11px;");
    titleRow->addWidget(m_lblCurrentPath, 1);

    headerLayout->addLayout(titleRow);

    // Controls Row (Search settings, Mode switch, Save, Reset)
    auto* controlsRow = new QHBoxLayout();
    controlsRow->setSpacing(8);

    m_settingSearchEdit = new QLineEdit(headerCard);
    m_settingSearchEdit->setClearButtonEnabled(true);
    connect(m_settingSearchEdit, &QLineEdit::textChanged, this, &ConfigsPage::onSettingSearchChanged);
    controlsRow->addWidget(m_settingSearchEdit, 1);

    // Mode switch: Visual vs Code
    m_modeGroup = new QButtonGroup(this);
    m_btnModeGui = new QPushButton(headerCard);
    m_btnModeGui->setCheckable(true);
    m_btnModeGui->setChecked(true);
    m_btnModeGui->setStyleSheet(
        "QPushButton {"
        "  border: 1px solid rgba(128, 128, 128, 0.3);"
        "  border-top-left-radius: 4px;"
        "  border-bottom-left-radius: 4px;"
        "  padding: 5px 12px;"
        "}"
        "QPushButton:checked {"
        "  background-color: #0078d4;"
        "  color: white;"
        "  font-weight: bold;"
        "}"
    );

    m_btnModeCode = new QPushButton(headerCard);
    m_btnModeCode->setCheckable(true);
    m_btnModeCode->setStyleSheet(
        "QPushButton {"
        "  border: 1px solid rgba(128, 128, 128, 0.3);"
        "  border-top-right-radius: 4px;"
        "  border-bottom-right-radius: 4px;"
        "  padding: 5px 12px;"
        "}"
        "QPushButton:checked {"
        "  background-color: #0078d4;"
        "  color: white;"
        "  font-weight: bold;"
        "}"
    );

    m_modeGroup->addButton(m_btnModeGui, 0);
    m_modeGroup->addButton(m_btnModeCode, 1);
    connect(m_modeGroup, &QButtonGroup::idClicked, this, &ConfigsPage::onModeChanged);

    controlsRow->addWidget(m_btnModeGui);
    controlsRow->addWidget(m_btnModeCode);

    // Save button
    m_btnSave = new QPushButton(headerCard);
    m_btnSave->setIcon(QIcon::fromTheme("document-save"));
    connect(m_btnSave, &QPushButton::clicked, this, &ConfigsPage::onSaveClicked);
    controlsRow->addWidget(m_btnSave);

    // Reset button
    m_btnReset = new QPushButton(headerCard);
    m_btnReset->setIcon(QIcon::fromTheme("edit-undo"));
    connect(m_btnReset, &QPushButton::clicked, this, &ConfigsPage::onResetClicked);
    controlsRow->addWidget(m_btnReset);

    headerLayout->addLayout(controlsRow);
    rightLayout->addWidget(headerCard);

    // Stacked Widget (Empty / Visual GUI / Code Editor)
    m_stackedWidget = new QStackedWidget(m_rightPanel);

    // Page 0: Empty state
    m_emptyStateWidget = new QWidget(m_stackedWidget);
    auto* emptyLayout = new QVBoxLayout(m_emptyStateWidget);
    emptyLayout->setAlignment(Qt::AlignCenter);
    emptyLayout->setSpacing(12);

    auto* emptyIcon = new QLabel(m_emptyStateWidget);
    emptyIcon->setPixmap(QIcon::fromTheme("settings").pixmap(64, 64));
    emptyIcon->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyIcon);

    auto* emptyTitle = new QLabel(m_emptyStateWidget);
    QFont emptyFont = emptyTitle->font();
    emptyFont.setPointSize(emptyFont.pointSize() + 2);
    emptyFont.setBold(true);
    emptyTitle->setFont(emptyFont);
    emptyTitle->setAlignment(Qt::AlignCenter);
    emptyTitle->setText(DiagI18n::tr("No Config File Selected", "Файл конфигурации не выбран"));
    emptyLayout->addWidget(emptyTitle);

    auto* emptySubtitle = new QLabel(m_emptyStateWidget);
    emptySubtitle->setAlignment(Qt::AlignCenter);
    emptySubtitle->setStyleSheet("color: gray;");
    emptySubtitle->setText(DiagI18n::tr("Select a config file from the list on the left to edit mod settings.",
                                       "Выберите файл конфигурации из списка слева для настройки параметров мода."));
    emptyLayout->addWidget(emptySubtitle);
    m_stackedWidget->addWidget(m_emptyStateWidget);

    // Page 1: Visual Mode (QScrollArea)
    m_scrollArea = new QScrollArea(m_stackedWidget);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);

    m_scrollContentWidget = new QWidget();
    m_scrollContentLayout = new QVBoxLayout(m_scrollContentWidget);
    m_scrollContentLayout->setContentsMargins(0, 0, 0, 0);
    m_scrollContentLayout->setSpacing(12);
    m_scrollArea->setWidget(m_scrollContentWidget);

    m_stackedWidget->addWidget(m_scrollArea);

    // Page 2: Code Mode (QPlainTextEdit)
    m_codeEditor = new QPlainTextEdit(m_stackedWidget);
    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_codeEditor->setFont(monoFont);
    m_codeEditor->setLineWrapMode(QPlainTextEdit::NoWrap);
    connect(m_codeEditor, &QPlainTextEdit::textChanged, this, &ConfigsPage::onCodeEditorTextChanged);

    m_stackedWidget->addWidget(m_codeEditor);

    rightLayout->addWidget(m_stackedWidget, 1);
    splitter->addWidget(m_rightPanel);

    splitter->setSizes({ 260, 600 });
    mainLayout->addWidget(splitter);

    m_stackedWidget->setCurrentIndex(0);
    m_btnSave->setEnabled(false);
    m_btnReset->setEnabled(false);
}

void ConfigsPage::retranslate()
{
    m_fileSearchEdit->setPlaceholderText(DiagI18n::tr("Search config files...", "Поиск файлов..."));
    m_btnRefreshFiles->setToolTip(DiagI18n::tr("Refresh config files", "Обновить список файлов"));
    m_btnOpenFolder->setToolTip(DiagI18n::tr("Open configs folder in file manager", "Открыть папку конфигураций"));
    m_btnOpenFolder->setText(DiagI18n::tr("Folder", "Папка"));

    m_settingSearchEdit->setPlaceholderText(DiagI18n::tr("Search settings in file...", "Поиск настроек..."));
    m_btnModeGui->setText(DiagI18n::tr("Visual Mode (GUI)", "Визуальный режим (GUI)"));
    m_btnModeCode->setText(DiagI18n::tr("Source Code", "Исходный текст (Код)"));

    updateSaveButton();
    m_btnReset->setText(DiagI18n::tr("Backup / Reset (.bak)", "Сбросить (.bak)"));
    m_btnReset->setToolTip(DiagI18n::tr("Create backup or restore from .bak", "Создать резервную копию или восстановить из .bak"));

    if (m_totalConfigFilesCount > 0) {
        m_fileCountLabel->setText(DiagI18n::tr("Files: %1", "Файлов: %1").arg(m_totalConfigFilesCount));
    } else {
        m_fileCountLabel->setText(QString());
    }
}

void ConfigsPage::openedImpl()
{
    if (m_fileTree->topLevelItemCount() == 0) {
        scanConfigFiles();
    }
}

void ConfigsPage::scanConfigFiles()
{
    m_fileTree->clear();
    m_totalConfigFilesCount = 0;

    if (!m_instance) {
        return;
    }

    QString gameRoot = m_instance->gameRoot();

    // 1. Root game options files: options.txt & optionsshaders.txt
    QString optPath = gameRoot + "/options.txt";
    QString shaderOptPath = gameRoot + "/optionsshaders.txt";

    bool hasOpt = QFile::exists(optPath);
    bool hasShaderOpt = QFile::exists(shaderOptPath);

    if (hasOpt || hasShaderOpt) {
        auto* gameGroupItem = new QTreeWidgetItem(m_fileTree);
        gameGroupItem->setText(0, DiagI18n::tr("Game Settings", "Настройки игры"));
        gameGroupItem->setIcon(0, QIcon::fromTheme("settings"));

        if (hasOpt) {
            auto* item = new QTreeWidgetItem(gameGroupItem);
            item->setText(0, "options.txt");
            item->setIcon(0, QIcon::fromTheme("text-x-generic"));
            item->setData(0, Qt::UserRole, optPath);
            m_totalConfigFilesCount++;
        }

        if (hasShaderOpt) {
            auto* item = new QTreeWidgetItem(gameGroupItem);
            item->setText(0, "optionsshaders.txt");
            item->setIcon(0, QIcon::fromTheme("text-x-generic"));
            item->setData(0, Qt::UserRole, shaderOptPath);
            m_totalConfigFilesCount++;
        }

        gameGroupItem->setExpanded(true);
    }

    // 2. config/ folder (recursive scan)
    QString configDir = gameRoot + "/config";
    if (QDir(configDir).exists()) {
        auto* configGroupItem = new QTreeWidgetItem(m_fileTree);
        configGroupItem->setText(0, DiagI18n::tr("Mod Configs (config/)", "Конфигурации модов (config/)"));
        configGroupItem->setIcon(0, QIcon::fromTheme("folder"));

        std::function<void(const QString&, QTreeWidgetItem*)> scanFolder =
            [&](const QString& dirPath, QTreeWidgetItem* parentItem) {
                QDir dir(dirPath);
                QFileInfoList entries = dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
                                                         QDir::DirsFirst | QDir::Name);

                for (const auto& info : entries) {
                    if (info.isDir()) {
                        auto* dirItem = new QTreeWidgetItem(parentItem);
                        dirItem->setText(0, info.fileName());
                        dirItem->setIcon(0, QIcon::fromTheme("folder"));
                        dirItem->setData(0, Qt::UserRole, info.absoluteFilePath());

                        scanFolder(info.absoluteFilePath(), dirItem);

                        if (dirItem->childCount() == 0) {
                            delete dirItem;
                        }
                    } else if (info.isFile()) {
                        QString ext = info.suffix().toLower();
                        if (ext == "toml" || ext == "json" || ext == "json5" || ext == "cfg" || ext == "properties" || ext == "txt") {
                            auto* fileItem = new QTreeWidgetItem(parentItem);
                            fileItem->setText(0, info.fileName());
                            fileItem->setIcon(0, QIcon::fromTheme("text-x-generic"));
                            fileItem->setData(0, Qt::UserRole, info.absoluteFilePath());
                            m_totalConfigFilesCount++;
                        }
                    }
                }
            };

        scanFolder(configDir, configGroupItem);
        configGroupItem->setExpanded(true);
    }

    m_fileCountLabel->setText(DiagI18n::tr("Files: %1", "Файлов: %1").arg(m_totalConfigFilesCount));
}

void ConfigsPage::onFileSearchChanged(const QString& query)
{
    QString needle = query.trimmed();

    std::function<bool(QTreeWidgetItem*)> filterItem = [&](QTreeWidgetItem* item) -> bool {
        bool anyChildVisible = false;
        for (int i = 0; i < item->childCount(); ++i) {
            if (filterItem(item->child(i))) {
                anyChildVisible = true;
            }
        }

        bool match = needle.isEmpty() || item->text(0).contains(needle, Qt::CaseInsensitive);
        bool shouldShow = match || anyChildVisible;

        item->setHidden(!shouldShow);
        if (anyChildVisible && !needle.isEmpty()) {
            item->setExpanded(true);
        }
        return shouldShow;
    };

    for (int i = 0; i < m_fileTree->topLevelItemCount(); ++i) {
        filterItem(m_fileTree->topLevelItem(i));
    }
}

bool ConfigsPage::maybeSaveBeforeSwitch()
{
    if (!m_hasUnsavedChanges || m_currentFilePath.isEmpty()) {
        return true;
    }

    auto res = QMessageBox::question(
        this,
        DiagI18n::tr("Unsaved Changes", "Несохранённые изменения"),
        DiagI18n::tr("File %1 has unsaved changes. Save before switching?",
                     "Файл %1 имеет несохранённые изменения. Сохранить перед переключением?")
            .arg(QFileInfo(m_currentFilePath).fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel
    );

    if (res == QMessageBox::Save) {
        onSaveClicked();
        return true;
    }
    if (res == QMessageBox::Discard) {
        m_hasUnsavedChanges = false;
        return true;
    }
    return false;
}

void ConfigsPage::onFileSelected(QTreeWidgetItem* current, QTreeWidgetItem* previous)
{
    if (!current) {
        return;
    }

    QString filePath = current->data(0, Qt::UserRole).toString();
    if (filePath.isEmpty() || QFileInfo(filePath).isDir()) {
        return;
    }

    if (m_hasUnsavedChanges && !maybeSaveBeforeSwitch()) {
        m_fileTree->blockSignals(true);
        m_fileTree->setCurrentItem(previous);
        m_fileTree->blockSignals(false);
        return;
    }

    loadFile(filePath);
}

void ConfigsPage::loadFile(const QString& filePath)
{
    m_currentFilePath = filePath;
    QString modsRoot = m_instance ? m_instance->modsRoot() : QString();
    m_langMap = ConfigEditor::ModLangResolver::loadTranslationsForConfig(filePath, modsRoot);

    QString error;
    if (!m_doc.loadFromFile(filePath, &error)) {
        QMessageBox::warning(
            this,
            DiagI18n::tr("Error Loading File", "Ошибка загрузки файла"),
            DiagI18n::tr("Could not load config file:\n%1", "Не удалось загрузить файл конфигурации:\n%1").arg(error)
        );
        return;
    }

    QFileInfo fileInfo(filePath);
    m_lblCurrentFile->setText(fileInfo.fileName());

    QString relPath = filePath;
    if (m_instance) {
        QString gameRoot = m_instance->gameRoot();
        if (relPath.startsWith(gameRoot)) {
            relPath = relPath.mid(gameRoot.length());
            if (relPath.startsWith('/')) {
                relPath = relPath.mid(1);
            }
        }
    }
    m_lblCurrentPath->setText(relPath);

    m_ignoreCodeChanges = true;
    m_codeEditor->setPlainText(m_doc.generateText());
    m_ignoreCodeChanges = false;

    renderGuiCards();

    m_hasUnsavedChanges = false;
    updateSaveButton();
    m_btnSave->setEnabled(true);
    m_btnReset->setEnabled(true);

    if (m_btnModeGui->isChecked()) {
        m_stackedWidget->setCurrentIndex(1);
    } else {
        m_stackedWidget->setCurrentIndex(2);
    }
}

void ConfigsPage::renderGuiCards()
{
    // Clear old UI mappings and widgets
    m_uiSections.clear();

    QLayoutItem* item;
    while ((item = m_scrollContentLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }

    const auto& sections = m_doc.sections();
    if (sections.isEmpty()) {
        auto* noSettingsLabel = new QLabel(
            DiagI18n::tr("No structured settings parsed. You can edit this file in Source Code mode.",
                         "Структурированные настройки не найдены. Вы можете редактировать этот файл в режиме Исходного кода."),
            m_scrollContentWidget
        );
        noSettingsLabel->setStyleSheet("color: gray; padding: 20px;");
        noSettingsLabel->setAlignment(Qt::AlignCenter);
        m_scrollContentLayout->addWidget(noSettingsLabel);
        return;
    }

    for (const auto& sec : sections) {
        if (sec.fields.isEmpty()) {
            continue;
        }

        UiSectionMapping secMap;
        secMap.name = sec.name;

        auto* card = new QFrame(m_scrollContentWidget);
        card->setStyleSheet(
            "QFrame {"
            "  background-color: palette(base);"
            "  border: 1px solid rgba(128, 128, 128, 0.25);"
            "  border-radius: 8px;"
            "  padding: 8px;"
            "  margin-bottom: 8px;"
            "}"
        );

        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(10, 8, 10, 8);
        cardLayout->setSpacing(6);

        // Header
        auto* headerLayout = new QHBoxLayout();
        headerLayout->setSpacing(8);

        QString secTitle = ConfigEditor::ModLangResolver::resolveSectionTitle(sec.name, m_langMap);
        auto* titleLabel = new QLabel(secTitle, card);
        QFont f = titleLabel->font();
        f.setBold(true);
        titleLabel->setFont(f);
        titleLabel->setStyleSheet("color: #3399ff; font-size: 13px;");
        headerLayout->addWidget(titleLabel);
        headerLayout->addStretch(1);
        cardLayout->addLayout(headerLayout);

        auto* headerDivider = new QFrame(card);
        headerDivider->setFrameShape(QFrame::HLine);
        headerDivider->setStyleSheet("color: rgba(128, 128, 128, 0.2);");
        cardLayout->addWidget(headerDivider);

        // Fields
        for (int i = 0; i < sec.fields.size(); ++i) {
            const auto& field = sec.fields[i];

            QString titleText = field.key;
            QString langTitle = ConfigEditor::ModLangResolver::findTitle(m_langMap, sec.name, field.key);
            if (langTitle.isEmpty()) {
                langTitle = ConfigEditor::ModLangResolver::lookupCommonKeyTranslation(field.key);
            }
            if (!langTitle.isEmpty()) {
                titleText = langTitle;
            }

            QString descText = field.description;
            if (descText.isEmpty()) {
                descText = ConfigEditor::ModLangResolver::findDescription(m_langMap, sec.name, field.key);
            }

            UiFieldMapping fieldMap;
            fieldMap.section = sec.name;
            fieldMap.key = field.key;
            fieldMap.title = titleText;
            fieldMap.description = descText;
            fieldMap.currentValue = field.currentValue;

            auto* rowWidget = new QWidget(card);
            auto* rowLayout = new QHBoxLayout(rowWidget);
            rowLayout->setContentsMargins(4, 4, 4, 4);
            rowLayout->setSpacing(12);

            // Left side: Name and comments
            auto* textLayout = new QVBoxLayout();
            textLayout->setSpacing(2);

            auto* keyLabel = new QLabel(rowWidget);
            if (!langTitle.isEmpty() && langTitle.compare(field.key, Qt::CaseInsensitive) != 0) {
                keyLabel->setTextFormat(Qt::RichText);
                keyLabel->setText(QString("<span style=\"font-weight: bold;\">%1</span>&nbsp;&nbsp;<span style=\"color: gray; font-weight: normal; font-size: 11px;\">(%2)</span>")
                    .arg(langTitle.toHtmlEscaped(), field.key.toHtmlEscaped()));
            } else {
                QString pretty = ConfigEditor::ModLangResolver::formatCamelOrSnakeCase(field.key);
                if (pretty != field.key) {
                    keyLabel->setTextFormat(Qt::RichText);
                    keyLabel->setText(QString("<span style=\"font-weight: bold;\">%1</span>&nbsp;&nbsp;<span style=\"color: gray; font-weight: normal; font-size: 11px;\">(%2)</span>")
                        .arg(pretty.toHtmlEscaped(), field.key.toHtmlEscaped()));
                } else {
                    keyLabel->setTextFormat(Qt::PlainText);
                    QFont kf = keyLabel->font();
                    kf.setBold(true);
                    keyLabel->setFont(kf);
                    keyLabel->setText(field.key);
                }
            }

            QString tip = QString("%1: %2").arg(DiagI18n::tr("Key", "Ключ"), field.rawKey);
            if (titleText != field.key) {
                tip += QString("\n%1: %2").arg(DiagI18n::tr("Original", "Оригинал"), field.key);
            }
            keyLabel->setToolTip(tip);
            textLayout->addWidget(keyLabel);

            if (!descText.isEmpty()) {
                auto* descLabel = new QLabel(descText, rowWidget);
                descLabel->setWordWrap(true);
                descLabel->setStyleSheet("color: gray; font-size: 11px;");
                textLayout->addWidget(descLabel);
            }
            rowLayout->addLayout(textLayout, 1);

            // Right side: Interactive Editor Widget
            QString secName = sec.name;
            QString keyName = field.key;

            if (field.type == ConfigEditor::FieldType::Boolean) {
                auto* check = new QCheckBox(rowWidget);
                bool isTrue = (field.currentValue.compare("true", Qt::CaseInsensitive) == 0 || field.currentValue == "1");
                check->setChecked(isTrue);
                check->setText(isTrue ? DiagI18n::tr("Enabled", "Включено") : DiagI18n::tr("Disabled", "Выключено"));
                connect(check, &QCheckBox::toggled, this, [this, check, secName, keyName](bool checked) {
                    check->setText(checked ? DiagI18n::tr("Enabled", "Включено") : DiagI18n::tr("Disabled", "Выключено"));
                    onFieldValueChanged(secName, keyName, checked ? "true" : "false");
                });
                rowLayout->addWidget(check);
                fieldMap.editorWidget = check;
            } else if (field.type == ConfigEditor::FieldType::Enum) {
                auto* combo = new ConfigComboBox(rowWidget);
                combo->setMinimumWidth(160);
                for (const auto& opt : field.allowedValues) {
                    combo->addItem(opt);
                }
                int idx = combo->findText(field.currentValue, Qt::MatchFixedString);
                if (idx != -1) {
                    combo->setCurrentIndex(idx);
                } else {
                    combo->addItem(field.currentValue);
                    combo->setCurrentIndex(combo->count() - 1);
                }
                connect(combo, &QComboBox::currentTextChanged, this, [this, secName, keyName](const QString& val) {
                    onFieldValueChanged(secName, keyName, val);
                });
                rowLayout->addWidget(combo);
                fieldMap.editorWidget = combo;
            } else if (field.type == ConfigEditor::FieldType::Integer) {
                auto* spin = new ConfigSpinBox(rowWidget);
                spin->setMinimumWidth(120);
                int minV = field.hasRange ? static_cast<int>(field.rangeMin) : -999999999;
                int maxV = field.hasRange ? static_cast<int>(field.rangeMax) : 999999999;
                spin->setRange(minV, maxV);
                spin->setValue(field.currentValue.toInt());
                connect(spin, &QSpinBox::valueChanged, this, [this, secName, keyName](int val) {
                    onFieldValueChanged(secName, keyName, QString::number(val));
                });
                rowLayout->addWidget(spin);
                fieldMap.editorWidget = spin;
            } else if (field.type == ConfigEditor::FieldType::Double) {
                auto* dspin = new ConfigDoubleSpinBox(rowWidget);
                dspin->setMinimumWidth(120);
                int decimals = 3;
                int dotPos = field.currentValue.indexOf('.');
                if (dotPos != -1) {
                    int curDecimals = field.currentValue.length() - dotPos - 1;
                    decimals = std::max(decimals, std::min(curDecimals, 6));
                }
                dspin->setDecimals(decimals);
                dspin->setSingleStep(0.05);
                double minV = field.hasRange ? field.rangeMin : -999999999.0;
                double maxV = field.hasRange ? field.rangeMax : 999999999.0;
                dspin->setRange(minV, maxV);

                QString cleanVal = field.currentValue;
                cleanVal.replace(',', '.');
                dspin->setValue(cleanVal.toDouble());

                connect(dspin, &QDoubleSpinBox::valueChanged, this, [this, secName, keyName](double val) {
                    onFieldValueChanged(secName, keyName, formatDoubleValue(val));
                });
                rowLayout->addWidget(dspin);
                fieldMap.editorWidget = dspin;
            } else {
                if (isKeybindField(field)) {
                    auto* keyBtn = new ConfigKeybindButton(field.currentValue, rowWidget);
                    keyBtn->setOnKeybindChanged([this, secName, keyName](const QString& newMcKey) {
                        onFieldValueChanged(secName, keyName, newMcKey);
                    });
                    rowLayout->addWidget(keyBtn);
                    fieldMap.editorWidget = keyBtn;
                } else {
                    auto* edit = new QLineEdit(rowWidget);
                    edit->setMinimumWidth(320);
                    edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
                    edit->setText(field.currentValue);
                    edit->setToolTip(field.currentValue);
                    edit->setStyleSheet("QLineEdit { background: palette(midlight); border: 1px solid rgba(128,128,128,0.3); border-radius: 4px; padding: 4px 6px; }");
                    connect(edit, &QLineEdit::textChanged, this, [this, edit, secName, keyName](const QString& val) {
                        edit->setToolTip(val);
                        onFieldValueChanged(secName, keyName, val);
                    });
                    rowLayout->addWidget(edit);
                    fieldMap.editorWidget = edit;
                }
            }

            cardLayout->addWidget(rowWidget);
            fieldMap.rowWidget = rowWidget;

            // Faint divider between fields
            if (i < sec.fields.size() - 1) {
                auto* fieldDivider = new QFrame(card);
                fieldDivider->setFrameShape(QFrame::HLine);
                fieldDivider->setStyleSheet("color: rgba(128, 128, 128, 0.1);");
                cardLayout->addWidget(fieldDivider);
                fieldMap.dividerWidget = fieldDivider;
            }

            secMap.fields.append(fieldMap);
        }

        m_scrollContentLayout->addWidget(card);
        secMap.cardWidget = card;
        m_uiSections.append(secMap);
    }

    m_scrollContentLayout->addStretch(1);
}

void ConfigsPage::onFieldValueChanged(const QString& section, const QString& key, const QString& newValue)
{
    m_doc.updateFieldValue(section, key, newValue);
    m_hasUnsavedChanges = true;
    updateSaveButton();
}

void ConfigsPage::onSettingSearchChanged(const QString& query)
{
    QString needle = query.trimmed();

    if (m_btnModeGui->isChecked()) {
        for (auto& sec : m_uiSections) {
            int visibleCount = 0;
            for (auto& f : sec.fields) {
                bool matches = needle.isEmpty() ||
                               f.key.contains(needle, Qt::CaseInsensitive) ||
                               f.title.contains(needle, Qt::CaseInsensitive) ||
                               f.description.contains(needle, Qt::CaseInsensitive) ||
                               f.currentValue.contains(needle, Qt::CaseInsensitive);

                f.rowWidget->setVisible(matches);
                if (f.dividerWidget) {
                    f.dividerWidget->setVisible(matches);
                }
                if (matches) {
                    visibleCount++;
                }
            }
            sec.cardWidget->setVisible(visibleCount > 0);
        }
    } else {
        if (!needle.isEmpty()) {
            m_codeEditor->find(needle);
        }
    }
}

void ConfigsPage::onModeChanged(int id)
{
    if (id == 1) {
        // Switch to Code Mode
        m_ignoreCodeChanges = true;
        m_codeEditor->setPlainText(m_doc.generateText());
        m_ignoreCodeChanges = false;
        m_stackedWidget->setCurrentIndex(2);
    } else {
        // Switch to Visual GUI Mode
        if (m_hasUnsavedChanges) {
            QString error;
            if (!m_doc.loadFromText(m_codeEditor->toPlainText(), m_doc.format(), m_currentFilePath, &error)) {
                QMessageBox::warning(
                    this,
                    DiagI18n::tr("Syntax Error", "Ошибка синтаксиса"),
                    DiagI18n::tr("Could not parse configuration file:\n%1\n\nPlease fix syntax before switching to Visual Mode.",
                                 "Не удалось распарсить файл конфигурации:\n%1\n\nПожалуйста, исправьте синтаксис перед переключением в визуальный режим.")
                        .arg(error)
                );
                m_btnModeCode->setChecked(true);
                return;
            }
            renderGuiCards();
        }
        m_stackedWidget->setCurrentIndex(1);
    }
}

void ConfigsPage::onCodeEditorTextChanged()
{
    if (m_ignoreCodeChanges) {
        return;
    }
    m_hasUnsavedChanges = true;
    updateSaveButton();
}

void ConfigsPage::updateSaveButton()
{
    if (m_hasUnsavedChanges) {
        m_btnSave->setText(DiagI18n::tr("Save Changes *", "Сохранить изменения *"));
        m_btnSave->setStyleSheet(
            "QPushButton {"
            "  background-color: #28a745;"
            "  color: white;"
            "  font-weight: bold;"
            "  border: 1px solid #1e7e34;"
            "  border-radius: 4px;"
            "  padding: 5px 14px;"
            "}"
            "QPushButton:hover { background-color: #218838; }"
            "QPushButton:pressed { background-color: #1e7e34; }"
        );
    } else {
        m_btnSave->setText(DiagI18n::tr("Save Changes", "Сохранить изменения"));
        m_btnSave->setStyleSheet("");
    }
}

void ConfigsPage::onSaveClicked()
{
    if (m_currentFilePath.isEmpty()) {
        return;
    }

    if (m_btnModeCode->isChecked()) {
        QString error;
        if (!m_doc.loadFromText(m_codeEditor->toPlainText(), m_doc.format(), m_currentFilePath, &error)) {
            auto res = QMessageBox::warning(
                this,
                DiagI18n::tr("Syntax Warning", "Предупреждение о синтаксисе"),
                DiagI18n::tr("File has syntax errors:\n%1\n\nSave anyway?",
                             "Файл содержит ошибки синтаксиса:\n%1\n\nВсё равно сохранить?").arg(error),
                QMessageBox::Yes | QMessageBox::No
            );
            if (res != QMessageBox::Yes) {
                return;
            }
        }
    }

    QString saveErr;
    if (!m_doc.saveToFile(m_currentFilePath, &saveErr)) {
        QMessageBox::critical(
            this,
            DiagI18n::tr("Save Error", "Ошибка сохранения"),
            DiagI18n::tr("Could not save file %1:\n%2", "Не удалось сохранить файл %1:\n%2")
                .arg(QFileInfo(m_currentFilePath).fileName(), saveErr)
        );
        return;
    }

    m_ignoreCodeChanges = true;
    m_codeEditor->setPlainText(m_doc.generateText());
    m_ignoreCodeChanges = false;

    m_hasUnsavedChanges = false;
    updateSaveButton();

    m_btnSave->setText(DiagI18n::tr("Saved!", "Сохранено!"));
    QTimer::singleShot(1500, this, [this]() {
        if (!m_hasUnsavedChanges) {
            m_btnSave->setText(DiagI18n::tr("Save Changes", "Сохранить изменения"));
        }
    });
}

void ConfigsPage::onResetClicked()
{
    if (m_currentFilePath.isEmpty()) {
        return;
    }

    QMenu resetMenu(this);
    auto* actionBackup = resetMenu.addAction(
        QIcon::fromTheme("document-save"),
        DiagI18n::tr("Create Backup (.bak)", "Создать резервную копию (.bak)")
    );

    auto* actionRestore = resetMenu.addAction(
        QIcon::fromTheme("edit-undo"),
        DiagI18n::tr("Restore from Backup (.bak)", "Восстановить из .bak")
    );
    actionRestore->setEnabled(ConfigEditor::ConfigDocument::hasBackup(m_currentFilePath));

    resetMenu.addSeparator();

    auto* actionDiscard = resetMenu.addAction(
        QIcon::fromTheme("view-refresh"),
        DiagI18n::tr("Discard Unsaved Changes", "Отменить несохранённые изменения")
    );
    actionDiscard->setEnabled(m_hasUnsavedChanges);

    QAction* selected = resetMenu.exec(m_btnReset->mapToGlobal(QPoint(0, m_btnReset->height())));
    if (selected == actionBackup) {
        QString err;
        if (ConfigEditor::ConfigDocument::createBackup(m_currentFilePath, &err)) {
            QMessageBox::information(
                this,
                DiagI18n::tr("Backup Created", "Резервная копия создана"),
                DiagI18n::tr("Backup successfully saved as:\n%1.bak", "Резервная копия успешно создана как:\n%1.bak")
                    .arg(QFileInfo(m_currentFilePath).fileName())
            );
        } else {
            QMessageBox::warning(this, DiagI18n::tr("Error", "Ошибка"), err);
        }
    } else if (selected == actionRestore) {
        auto res = QMessageBox::question(
            this,
            DiagI18n::tr("Restore from Backup", "Восстановить из резервной копии"),
            DiagI18n::tr("Restore %1 from backup? All current modifications will be replaced.",
                         "Восстановить %1 из резервной копии? Все текущие изменения будут заменены.")
                .arg(QFileInfo(m_currentFilePath).fileName()),
            QMessageBox::Yes | QMessageBox::No
        );
        if (res == QMessageBox::Yes) {
            QString err;
            if (ConfigEditor::ConfigDocument::restoreBackup(m_currentFilePath, &err)) {
                loadFile(m_currentFilePath);
            } else {
                QMessageBox::warning(this, DiagI18n::tr("Error", "Ошибка"), err);
            }
        }
    } else if (selected == actionDiscard) {
        loadFile(m_currentFilePath);
    }
}

void ConfigsPage::onRefreshFilesClicked()
{
    scanConfigFiles();
}

void ConfigsPage::onOpenFolderClicked()
{
    if (m_instance) {
        DesktopServices::openPath(m_instance->gameRoot() + "/config", true);
    }
}

bool ConfigsPage::apply()
{
    if (m_hasUnsavedChanges) {
        onSaveClicked();
    }
    return true;
}

