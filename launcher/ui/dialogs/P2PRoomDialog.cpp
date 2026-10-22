// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#include "P2PRoomDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QTabWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QSpinBox>
#include <QCheckBox>
#include <QListWidget>
#include <QListWidgetItem>
#include <QGuiApplication>
#include <QClipboard>
#include <QPainter>
#include <QImage>
#include <QMessageBox>

#include "p2p/P2PManager.h"
#include "Application.h"
#include "minecraft/auth/AccountList.h"

P2PRoomDialog::P2PRoomDialog(QWidget* parent)
    : QDialog(parent)
{
    setupUi();

    auto* p2p = P2PManager::instance();
    if (p2p) {
        connect(p2p, &P2PManager::stateChanged, this, &P2PRoomDialog::onP2PStateChanged);
        connect(p2p, &P2PManager::statusMessage, this, &P2PRoomDialog::onP2PStatusMessage);
        connect(p2p, &P2PManager::pingUpdated, this, &P2PRoomDialog::onP2PPingUpdated);
        connect(p2p, &P2PManager::localMinecraftPortDetected, this, &P2PRoomDialog::onLocalPortDetected);
        connect(p2p, &P2PManager::participantsUpdated, this, &P2PRoomDialog::refreshParticipants);
    }

    updateUiState();
    refreshParticipants();
}

QImage P2PRoomDialog::extractSkinFaceImage(const QString& skinPath, int targetSize)
{
    QImage skinImg;
    if (!skinPath.isEmpty()) {
        skinImg.load(skinPath);
    }

    if (skinImg.isNull()) {
        // Fallback generic Steve face
        QImage defaultImg(targetSize, targetSize, QImage::Format_ARGB32);
        defaultImg.fill(QColor(180, 130, 90));
        QPainter p(&defaultImg);
        p.setPen(Qt::NoPen);
        // Eyes
        p.setBrush(QColor(255, 255, 255));
        p.drawRect(targetSize / 4, targetSize / 2, targetSize / 6, targetSize / 8);
        p.drawRect(targetSize - targetSize / 4 - targetSize / 6, targetSize / 2, targetSize / 6, targetSize / 8);
        // Pupils
        p.setBrush(QColor(60, 40, 180));
        p.drawRect(targetSize / 4 + targetSize / 12, targetSize / 2, targetSize / 12, targetSize / 8);
        p.drawRect(targetSize - targetSize / 4 - targetSize / 12, targetSize / 2, targetSize / 12, targetSize / 8);
        return defaultImg;
    }

    // Standard Minecraft skin face base: (8, 8, 8, 8)
    QImage face = skinImg.copy(8, 8, 8, 8).convertToFormat(QImage::Format_ARGB32);

    // Hat / Hair overlay layer: (40, 8, 8, 8)
    if (skinImg.width() >= 48 && skinImg.height() >= 16) {
        QImage hat = skinImg.copy(40, 8, 8, 8).convertToFormat(QImage::Format_ARGB32);
        QPainter painter(&face);
        painter.drawImage(0, 0, hat);
    }

    // Scale nearest-neighbor for crisp pixelated Minecraft look
    return face.scaled(targetSize, targetSize, Qt::IgnoreAspectRatio, Qt::FastTransformation);
}

QPixmap P2PRoomDialog::extractSkinFace(const QString& skinPath, int targetSize)
{
    return QPixmap::fromImage(extractSkinFaceImage(skinPath, targetSize));
}

void P2PRoomDialog::setupUi()
{
    setWindowTitle(tr("Сетевая комната (P2P Мультиплеер)"));
    resize(580, 600);

    auto* mainLayout = new QVBoxLayout(this);

    // Header info banner
    auto* banner = new QLabel(tr("<b>Встроенная P2P-сеть для игры с друзьями без VPN (Radmin / Hamachi)</b><br>"
                                 "<span style='color: gray;'>Прямое подключение через NAT с автоматической синхронизацией скинов.</span>"),
                              this);
    banner->setWordWrap(true);
    banner->setMargin(6);
    mainLayout->addWidget(banner);

    // Tab widget for Host / Guest modes
    m_tabs = new QTabWidget(this);

    // ---------------- TAB 1: HOST ----------------
    auto* hostTab = new QWidget(this);
    auto* hostLayout = new QVBoxLayout(hostTab);

    auto* hostSettingsGroup = new QGroupBox(tr("Настройки сервера мира"), hostTab);
    auto* hostSettingsLayout = new QVBoxLayout(hostSettingsGroup);

    m_autoPortCheck = new QCheckBox(tr("Авто-определение порта при открытии мира для сети (Open to LAN)"), hostSettingsGroup);
    m_autoPortCheck->setChecked(true);
    hostSettingsLayout->addWidget(m_autoPortCheck);

    auto* portRow = new QHBoxLayout();
    portRow->addWidget(new QLabel(tr("Порт локального сервера:"), hostSettingsGroup));
    m_hostPortSpin = new QSpinBox(hostSettingsGroup);
    m_hostPortSpin->setRange(1024, 65535);
    m_hostPortSpin->setValue(25565);
    m_hostPortSpin->setEnabled(false);
    portRow->addWidget(m_hostPortSpin);
    hostSettingsLayout->addLayout(portRow);

    m_detectedPortLabel = new QLabel(tr("Статус порта: Ожидание открытия мира для сети в игре..."), hostSettingsGroup);
    m_detectedPortLabel->setStyleSheet(QStringLiteral("color: #007acc; font-style: italic;"));
    hostSettingsLayout->addWidget(m_detectedPortLabel);

    hostLayout->addWidget(hostSettingsGroup);

    auto* hostActionGroup = new QGroupBox(tr("Управление комнатой"), hostTab);
    auto* hostActionLayout = new QVBoxLayout(hostActionGroup);

    auto* hostBtnRow = new QHBoxLayout();
    m_createRoomBtn = new QPushButton(tr("Создать комнату (Хост)"), hostActionGroup);
    m_createRoomBtn->setStyleSheet(QStringLiteral("font-weight: bold; padding: 6px;"));
    m_stopHostBtn = new QPushButton(tr("Остановить комнату"), hostActionGroup);
    m_stopHostBtn->setEnabled(false);
    hostBtnRow->addWidget(m_createRoomBtn);
    hostBtnRow->addWidget(m_stopHostBtn);
    hostActionLayout->addLayout(hostBtnRow);

    auto* hostCredsForm = new QFormLayout();
    m_roomCodeEdit = new QLineEdit(hostActionGroup);
    m_roomCodeEdit->setReadOnly(true);
    m_copyCodeBtn = new QPushButton(tr("Скопировать код"), hostActionGroup);
    auto* codeRow = new QHBoxLayout();
    codeRow->addWidget(m_roomCodeEdit);
    codeRow->addWidget(m_copyCodeBtn);
    hostCredsForm->addRow(tr("Код комнаты:"), codeRow);

    m_directTicketEdit = new QLineEdit(hostActionGroup);
    m_directTicketEdit->setReadOnly(true);
    m_copyTicketBtn = new QPushButton(tr("Скопировать тикет"), hostActionGroup);
    auto* ticketRow = new QHBoxLayout();
    ticketRow->addWidget(m_directTicketEdit);
    ticketRow->addWidget(m_copyTicketBtn);
    hostCredsForm->addRow(tr("Прямой тикет:"), ticketRow);

    hostActionLayout->addLayout(hostCredsForm);

    m_hostStatusLabel = new QLabel(tr("Статус: Комната не создана"), hostActionGroup);
    m_hostStatusLabel->setStyleSheet(QStringLiteral("font-weight: bold; color: gray;"));
    hostActionLayout->addWidget(m_hostStatusLabel);

    hostLayout->addWidget(hostActionGroup);
    hostLayout->addStretch();
    m_tabs->addTab(hostTab, tr("Создать комнату (Хост)"));

    // ---------------- TAB 2: GUEST ----------------
    auto* guestTab = new QWidget(this);
    auto* guestLayout = new QVBoxLayout(guestTab);

    auto* guestJoinGroup = new QGroupBox(tr("Подключение к комнате друга"), guestTab);
    auto* guestJoinLayout = new QVBoxLayout(guestJoinGroup);

    guestJoinLayout->addWidget(new QLabel(tr("Введите Код комнаты (например PRISM9) или Прямой тикет (P2P-...):"), guestJoinGroup));
    m_joinInputEdit = new QLineEdit(guestJoinGroup);
    m_joinInputEdit->setPlaceholderText(tr("Вставьте код или тикет от хоста"));
    guestJoinLayout->addWidget(m_joinInputEdit);

    auto* guestBtnRow = new QHBoxLayout();
    m_joinBtn = new QPushButton(tr("Подключиться к комнате"), guestJoinGroup);
    m_joinBtn->setStyleSheet(QStringLiteral("font-weight: bold; padding: 6px;"));
    m_leaveBtn = new QPushButton(tr("Отключиться"), guestJoinGroup);
    m_leaveBtn->setEnabled(false);
    guestBtnRow->addWidget(m_joinBtn);
    guestBtnRow->addWidget(m_leaveBtn);
    guestJoinLayout->addLayout(guestBtnRow);

    guestLayout->addWidget(guestJoinGroup);

    auto* guestAddressGroup = new QGroupBox(tr("Адрес для входа в Minecraft"), guestTab);
    auto* guestAddressLayout = new QVBoxLayout(guestAddressGroup);

    auto* addressRow = new QHBoxLayout();
    m_proxyAddressEdit = new QLineEdit(guestAddressGroup);
    m_proxyAddressEdit->setReadOnly(true);
    m_proxyAddressEdit->setText(QStringLiteral("127.0.0.1:25565"));
    m_copyAddressBtn = new QPushButton(tr("Скопировать IP"), guestAddressGroup);
    addressRow->addWidget(m_proxyAddressEdit);
    addressRow->addWidget(m_copyAddressBtn);
    guestAddressLayout->addLayout(addressRow);

    auto* helpLabel = new QLabel(tr("В игре Minecraft выберите «Сетевая игра» -> «По адресу» и вставьте скопированный IP."), guestAddressGroup);
    helpLabel->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
    guestAddressLayout->addWidget(helpLabel);

    m_guestStatusLabel = new QLabel(tr("Статус: Не подключено"), guestAddressGroup);
    m_guestStatusLabel->setStyleSheet(QStringLiteral("font-weight: bold; color: gray;"));
    guestAddressLayout->addWidget(m_guestStatusLabel);

    guestLayout->addWidget(guestAddressGroup);
    guestLayout->addStretch();
    m_tabs->addTab(guestTab, tr("Подключиться (Гость)"));

    mainLayout->addWidget(m_tabs);

    // ---------------- BOTTOM: PARTICIPANTS LIST ----------------
    auto* participantsGroup = new QGroupBox(tr("Участники комнаты (Синхронизация скинов и пинг)"), this);
    auto* participantsLayout = new QVBoxLayout(participantsGroup);

    m_participantsList = new QListWidget(participantsGroup);
    m_participantsList->setIconSize(QSize(32, 32));
    m_participantsList->setMinimumHeight(140);
    participantsLayout->addWidget(m_participantsList);

    mainLayout->addWidget(participantsGroup);

    // Close button
    auto* closeBtnRow = new QHBoxLayout();
    closeBtnRow->addStretch();
    auto* closeBtn = new QPushButton(tr("Закрыть"), this);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    closeBtnRow->addWidget(closeBtn);
    mainLayout->addLayout(closeBtnRow);

    // Connections
    connect(m_autoPortCheck, &QCheckBox::toggled, this, &P2PRoomDialog::onAutoPortToggled);
    connect(m_createRoomBtn, &QPushButton::clicked, this, &P2PRoomDialog::onCreateRoomClicked);
    connect(m_stopHostBtn, &QPushButton::clicked, this, &P2PRoomDialog::onStopRoomClicked);
    connect(m_joinBtn, &QPushButton::clicked, this, &P2PRoomDialog::onJoinRoomClicked);
    connect(m_leaveBtn, &QPushButton::clicked, this, &P2PRoomDialog::onLeaveRoomClicked);
    connect(m_copyCodeBtn, &QPushButton::clicked, this, &P2PRoomDialog::onCopyCodeClicked);
    connect(m_copyTicketBtn, &QPushButton::clicked, this, &P2PRoomDialog::onCopyTicketClicked);
    connect(m_copyAddressBtn, &QPushButton::clicked, this, &P2PRoomDialog::onCopyAddressClicked);
}

void P2PRoomDialog::onAutoPortToggled(bool checked)
{
    m_hostPortSpin->setEnabled(!checked);
}

void P2PRoomDialog::onCreateRoomClicked()
{
    quint16 port = static_cast<quint16>(m_hostPortSpin->value());
    auto* p2p = P2PManager::instance();
    if (p2p) {
        p2p->createRoom(port);
    }
}

void P2PRoomDialog::onStopRoomClicked()
{
    auto* p2p = P2PManager::instance();
    if (p2p) {
        p2p->stop();
    }
}

void P2PRoomDialog::onJoinRoomClicked()
{
    QString code = m_joinInputEdit->text().trimmed();
    if (code.isEmpty()) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Пожалуйста, введите код комнаты или тикет друга."));
        return;
    }
    auto* p2p = P2PManager::instance();
    if (p2p) {
        p2p->joinRoom(code);
    }
}

void P2PRoomDialog::onLeaveRoomClicked()
{
    auto* p2p = P2PManager::instance();
    if (p2p) {
        p2p->stop();
    }
}

void P2PRoomDialog::onCopyCodeClicked()
{
    QString code = m_roomCodeEdit->text();
    if (!code.isEmpty()) {
        QGuiApplication::clipboard()->setText(code);
        m_hostStatusLabel->setText(tr("Код скопирован в буфер обмена!"));
    }
}

void P2PRoomDialog::onCopyTicketClicked()
{
    QString ticket = m_directTicketEdit->text();
    if (!ticket.isEmpty()) {
        QGuiApplication::clipboard()->setText(ticket);
        m_hostStatusLabel->setText(tr("Прямой тикет скопирован в буфер обмена!"));
    }
}

void P2PRoomDialog::onCopyAddressClicked()
{
    QString addr = m_proxyAddressEdit->text();
    if (!addr.isEmpty()) {
        QGuiApplication::clipboard()->setText(addr);
        m_guestStatusLabel->setText(tr("Адрес сервера скопирован в буфер обмена!"));
    }
}

void P2PRoomDialog::onP2PStateChanged()
{
    updateUiState();
    refreshParticipants();
}

void P2PRoomDialog::onP2PStatusMessage(const QString& message)
{
    auto* p2p = P2PManager::instance();
    if (p2p && p2p->isHosting()) {
        m_hostStatusLabel->setText(message);
    } else if (p2p && p2p->isGuest()) {
        m_guestStatusLabel->setText(message);
    }
}

void P2PRoomDialog::onP2PPingUpdated(int pingMs)
{
    Q_UNUSED(pingMs);
    refreshParticipants();
}

void P2PRoomDialog::onLocalPortDetected(quint16 port)
{
    m_detectedPortLabel->setText(tr("Порт игры автоматически обнаружен: %1").arg(port));
    m_detectedPortLabel->setStyleSheet(QStringLiteral("color: #2e7d32; font-weight: bold;"));
    m_hostPortSpin->setValue(port);
}

void P2PRoomDialog::updateUiState()
{
    auto* p2p = P2PManager::instance();
    if (!p2p) return;

    bool isHosting = p2p->isHosting();
    bool isGuest = p2p->isGuest();
    bool isConnected = p2p->isConnected();

    m_createRoomBtn->setEnabled(!isHosting && !isGuest);
    m_stopHostBtn->setEnabled(isHosting);

    m_joinBtn->setEnabled(!isHosting && !isGuest);
    m_leaveBtn->setEnabled(isGuest);

    if (isHosting) {
        m_roomCodeEdit->setText(p2p->roomCode());
        m_directTicketEdit->setText(P2PManager::encodeDirectTicket(QHostAddress::LocalHost, p2p->localUdpPort(), p2p->encryptionKey()));
        QString statusText = isConnected ? tr("Подключено! Игроки в комнате: %1").arg(p2p->connectedPeersCount())
                                         : tr("Комната открыта. Ожидание подключения друзей...");
        m_hostStatusLabel->setText(statusText);
        m_hostStatusLabel->setStyleSheet(isConnected ? QStringLiteral("color: #2e7d32; font-weight: bold;")
                                                     : QStringLiteral("color: #007acc; font-weight: bold;"));
    } else {
        if (!isGuest) {
            m_roomCodeEdit->clear();
            m_directTicketEdit->clear();
            m_hostStatusLabel->setText(tr("Статус: Комната не создана"));
            m_hostStatusLabel->setStyleSheet(QStringLiteral("color: gray;"));
        }
    }

    if (isGuest) {
        quint16 proxyPort = p2p->localProxyPort() > 0 ? p2p->localProxyPort() : 25565;
        m_proxyAddressEdit->setText(QStringLiteral("127.0.0.1:%1").arg(proxyPort));
        QString statusText = isConnected ? tr("Туннель активен! Входите в игру по адресу 127.0.0.1:%1").arg(proxyPort)
                                         : tr("Установка P2P-туннеля через NAT...");
        m_guestStatusLabel->setText(statusText);
        m_guestStatusLabel->setStyleSheet(isConnected ? QStringLiteral("color: #2e7d32; font-weight: bold;")
                                                      : QStringLiteral("color: #007acc; font-weight: bold;"));
    } else {
        if (!isHosting) {
            m_guestStatusLabel->setText(tr("Статус: Не подключено"));
            m_guestStatusLabel->setStyleSheet(QStringLiteral("color: gray;"));
        }
    }
}

void P2PRoomDialog::refreshParticipants()
{
    m_participantsList->clear();

    auto* p2p = P2PManager::instance();
    if (!p2p) return;

    auto list = p2p->participants();
    if (list.isEmpty()) {
        // Show current local user
        auto* app = dynamic_cast<Application*>(qApp);
        QString myName = tr("Вы");
        QString mySkin;
        if (app && app->accounts() && app->accounts()->defaultAccount()) {
            myName = app->accounts()->defaultAccount()->profileName();
            mySkin = app->accounts()->defaultAccount()->skinPath();
        }

        auto* item = new QListWidgetItem(m_participantsList);
        item->setIcon(extractSkinFace(mySkin));
        item->setText(tr("%1 (Вы) — Ожидание подключения...").arg(myName));
        return;
    }

    for (const auto& p : list) {
        auto* item = new QListWidgetItem(m_participantsList);
        item->setIcon(extractSkinFace(p.skinPath));

        QString roleStr = p.isHost ? tr("Хост") : tr("Гость");
        QString localStr = p.isLocal ? tr("Вы") : tr("Игрок");
        QString pingStr = p.isLocal ? tr("0 мс (Локально)") : QString(tr("%1 мс")).arg(p.pingMs > 0 ? p.pingMs : p2p->pingMs());

        item->setText(QStringLiteral("%1 [%2, %3] — Модель: %4 | Пинг: %5")
                          .arg(p.username, localStr, roleStr, p.modelType, pingStr));
    }
}
