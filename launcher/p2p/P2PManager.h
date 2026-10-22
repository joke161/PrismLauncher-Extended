// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QUrl>
#include <QHostAddress>
#include <QUdpSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QMap>
#include <QPointer>
#include <QList>
#include <memory>

#include "P2PProtocol.h"

/**
 * P2PManager manages direct peer-to-peer UDP tunnels through NAT
 * between Minecraft players, bypassing VPN software like Radmin / Hamachi.
 *
 * Implements:
 * 1. Signaling Layer via lightweight HTTP/JSON requests with configurable backend.
 * 2. Asynchronous UDP hole punching & keepalive maintenance through NAT.
 * 3. Reliable and ordered UDP transmission layer (TcpDataAck, reorder buffer, retransmission).
 * 4. P2P Skin & Identity synchronization between room participants (SkinSync).
 * 5. Automatic Minecraft LAN port detection from game logs.
 * 6. Local TCP proxy bridge (Guest QTcpServer <-> Host Minecraft LAN QTcpSocket).
 * 7. 100% non-blocking Qt event-driven architecture with zero GUI thread freezing.
 */
class P2PManager : public QObject {
    Q_OBJECT

public:
    enum class Role {
        Idle,
        Host,
        Guest
    };
    Q_ENUM(Role)

    enum class SessionState {
        Idle,
        Signaling,
        Hosting,
        Punching,
        Connected,
        Error
    };
    Q_ENUM(SessionState)

    struct PeerParticipant {
        QString uuid;
        QString username;
        QString modelType = "steve";
        QString skinPath;
        int pingMs = 0;
        bool isHost = false;
        bool isLocal = false;
    };

    struct UnackedPacket {
        quint16 connId = 0;
        quint32 seq = 0;
        QByteArray payload;
        QHostAddress destAddress;
        quint16 destPort = 0;
        qint64 lastSentMs = 0;
        int retries = 0;
    };

    static P2PManager* instance();

    explicit P2PManager(QObject* parent = nullptr);
    ~P2PManager() override;

    // Session state inspection
    Role role() const { return m_role; }
    SessionState state() const { return m_state; }
    bool isConnected() const { return m_state == SessionState::Connected; }
    bool isHosting() const { return m_role == Role::Host; }
    bool isGuest() const { return m_role == Role::Guest; }

    QString roomCode() const { return m_roomCode; }
    QString roomToken() const { return m_roomToken; }
    quint16 localMinecraftPort() const { return m_localMinecraftPort; }
    quint16 localProxyPort() const { return m_localProxyPort; }
    quint16 localUdpPort() const;

    QHostAddress peerAddress() const { return m_peerAddress; }
    quint16 peerPort() const { return m_peerPort; }
    int connectedPeersCount() const;

    // Latency & Traffic Statistics
    int pingMs() const { return m_lastPingMs; }
    quint64 txBytes() const { return m_txBytes; }
    quint64 rxBytes() const { return m_rxBytes; }

    // Participants list
    QList<PeerParticipant> participants() const;

    // Signaling server URL
    void setSignalingServerUrl(const QUrl& url) { m_signalingServerUrl = url; }
    QUrl signalingServerUrl() const { return m_signalingServerUrl; }

    // Encryption key management
    void setEncryptionKey(const QByteArray& key) { m_encryptionKey = key; }
    QByteArray encryptionKey() const { return m_encryptionKey; }

    // Ticket encoding/decoding for direct LAN / IP sharing
    static QString encodeDirectTicket(const QHostAddress& address, quint16 port, const QByteArray& key);
    static bool decodeDirectTicket(const QString& ticket, QHostAddress& outAddress, quint16& outPort, QByteArray& outKey);

public slots:
    /**
     * Host role: Creates a room via signaling server, points to local Minecraft LAN port.
     */
    void createRoom(quint16 localMinecraftPort);

    /**
     * Host role: Creates a room directly without signaling server (for LAN / tests).
     */
    void createDirectRoom(quint16 localMinecraftPort, quint16 udpPort = 0);

    /**
     * Guest role: Joins an existing room by room code or direct ticket.
     */
    void joinRoom(const QString& roomCode);

    /**
     * Automatically called when Minecraft game log prints Open to LAN port.
     */
    void setLocalMinecraftPort(quint16 port);

    /**
     * Broadcasts local account identity & skin texture to peer(s).
     */
    void broadcastIdentityAndSkin();

    /**
     * Terminates active session and resets all sockets/timers.
     */
    void stop();

signals:
    void roomCreated(const QString& roomCode, quint16 localProxyPort);
    void peerConnected(const QString& peerEndpoint);
    void peerDisconnected(const QString& peerEndpoint);
    void stateChanged(P2PManager::SessionState newState);
    void statusMessage(const QString& message);
    void errorOccurred(const QString& errorMessage);
    void pingUpdated(int pingMs);
    void trafficStatsUpdated(quint64 txBytes, quint64 rxBytes);
    void localMinecraftPortDetected(quint16 port);
    void participantJoined(const QString& uuid, const QString& username, const QString& modelType, const QString& skinPath);
    void participantsUpdated();

private slots:
    void onUdpReadyRead();
    void onPunchTimeout();
    void onKeepAliveTimeout();
    void onSignalingPollTimeout();
    void onNewGuestConnection();
    void onRetransmitTimeout();

private:
    void setState(SessionState newState);
    void startHolePunching();
    void startKeepAlive();
    void onConnected();

    void sendPacket(P2P::PacketType type, quint16 connId, quint32 seq, const QByteArray& payload);
    void sendPacketTo(P2P::PacketType type, quint16 connId, quint32 seq, const QByteArray& payload,
                      const QHostAddress& destAddress, quint16 destPort);
    void sendTcpChunks(quint16 connId, const QByteArray& data);
    void sendTcpChunksTo(const QHostAddress& dest, quint16 port, quint16 connId, const QByteArray& data);
    void sendControlChunked(P2P::PacketType type, const QByteArray& data);

    // Host handlers
    void handleIncomingTcpConnectHost(const QHostAddress& sender, quint16 senderPort, quint16 connId);
    void handleIncomingTcpDataHost(const QHostAddress& sender, quint16 senderPort, quint16 connId, quint32 seq, const QByteArray& payload);
    void handleIncomingTcpDisconnectHost(const QHostAddress& sender, quint16 senderPort, quint16 connId);

    // Guest handlers
    void handleIncomingTcpDataGuest(quint16 connId, quint32 seq, const QByteArray& payload, const QHostAddress& sender, quint16 senderPort);
    void handleIncomingTcpDisconnectGuest(quint16 connId);

    // Identity handler
    void handleIncomingIdentityPacket(quint16 totalChunks, quint32 chunkIndex, const QByteArray& payload);

    void deliverTcpPayload(quint64 streamKey, quint16 connId, const QByteArray& payload);

    static quint64 makeHostConnKey(const QHostAddress& sender, quint16 senderPort, quint16 connId);
    static QByteArray generateRandomKey(int length = 16);

private:
    static P2PManager* s_instance;

    Role m_role = Role::Idle;
    SessionState m_state = SessionState::Idle;

    QString m_roomCode;
    QString m_roomToken;
    QByteArray m_encryptionKey;
    QUrl m_signalingServerUrl;

    quint16 m_localMinecraftPort = 0; // Host: actual Minecraft LAN server port
    quint16 m_localProxyPort = 0;     // Guest: local QTcpServer port Minecraft client connects to

    QHostAddress m_peerAddress;
    quint16 m_peerPort = 0;

    // Networking primitives
    QUdpSocket* m_udpSocket = nullptr;
    QTcpServer* m_tcpServer = nullptr; // Guest: local server for Minecraft client
    QNetworkAccessManager* m_networkManager = nullptr;

    // Timers
    QTimer* m_punchTimer = nullptr;
    QTimer* m_keepAliveTimer = nullptr;
    QTimer* m_signalingPollTimer = nullptr;
    QTimer* m_retransmitTimer = nullptr;
    QElapsedTimer m_keepAliveElapsedTimer;
    QElapsedTimer m_lastPacketReceivedTimer;

    int m_punchAttempts = 0;
    int m_lastPingMs = 0;
    quint16 m_nextGuestConnId = 0;

    // Reliable UDP delivery tracking
    QHash<quint64, UnackedPacket> m_unackedPackets; // key: ((quint64)connId << 32) | seq
    QHash<quint16, quint32> m_connNextSeq;
    QHash<quint64, quint32> m_expectedSeq;
    QHash<quint64, QMap<quint32, QByteArray>> m_reorderBuffers;

    // Skin & Identity Sync
    QMap<quint32, QByteArray> m_incomingIdentityChunks;
    quint16 m_expectedIdentityTotalChunks = 0;
    QHash<QString, PeerParticipant> m_participants;

    // Guest connection map: connId -> QTcpSocket*
    QHash<quint16, QPointer<QTcpSocket>> m_guestSockets;

    // Host connection maps: fullKey -> QTcpSocket*
    QHash<quint64, QPointer<QTcpSocket>> m_hostSockets;
    QHash<quint64, QPair<QHostAddress, quint16>> m_hostEndpoints;
    QHash<quint64, quint16> m_hostConnIds;
    QHash<quint64, QByteArray> m_pendingHostData;

    // Counters
    quint64 m_txBytes = 0;
    quint64 m_rxBytes = 0;
    quint64 m_txPackets = 0;
    quint64 m_rxPackets = 0;
};
