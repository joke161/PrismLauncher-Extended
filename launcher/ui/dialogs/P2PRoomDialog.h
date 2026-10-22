// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#pragma once

#include <QDialog>
#include <QPixmap>

class QTabWidget;
class QLineEdit;
class QPushButton;
class QLabel;
class QSpinBox;
class QCheckBox;
class QListWidget;

/**
 * Dialog for hosting and joining peer-to-peer multiplayer rooms.
 */
class P2PRoomDialog : public QDialog {
    Q_OBJECT

public:
    explicit P2PRoomDialog(QWidget* parent = nullptr);
    ~P2PRoomDialog() override = default;

    static QImage extractSkinFaceImage(const QString& skinPath, int targetSize = 32);
    static QPixmap extractSkinFace(const QString& skinPath, int targetSize = 32);

private slots:
    void onCreateRoomClicked();
    void onStopRoomClicked();
    void onJoinRoomClicked();
    void onLeaveRoomClicked();
    void onCopyCodeClicked();
    void onCopyTicketClicked();
    void onCopyAddressClicked();
    void onAutoPortToggled(bool checked);

    void onP2PStateChanged();
    void onP2PStatusMessage(const QString& message);
    void onP2PPingUpdated(int pingMs);
    void onLocalPortDetected(quint16 port);
    void refreshParticipants();

private:
    void setupUi();
    void updateUiState();

private:
    QTabWidget* m_tabs = nullptr;

    // Host tab widgets
    QCheckBox* m_autoPortCheck = nullptr;
    QSpinBox* m_hostPortSpin = nullptr;
    QLabel* m_detectedPortLabel = nullptr;
    QPushButton* m_createRoomBtn = nullptr;
    QPushButton* m_stopHostBtn = nullptr;
    QLineEdit* m_roomCodeEdit = nullptr;
    QPushButton* m_copyCodeBtn = nullptr;
    QLineEdit* m_directTicketEdit = nullptr;
    QPushButton* m_copyTicketBtn = nullptr;
    QLabel* m_hostStatusLabel = nullptr;

    // Guest tab widgets
    QLineEdit* m_joinInputEdit = nullptr;
    QPushButton* m_joinBtn = nullptr;
    QPushButton* m_leaveBtn = nullptr;
    QLineEdit* m_proxyAddressEdit = nullptr;
    QPushButton* m_copyAddressBtn = nullptr;
    QLabel* m_guestStatusLabel = nullptr;

    // Participants list
    QListWidget* m_participantsList = nullptr;
};
