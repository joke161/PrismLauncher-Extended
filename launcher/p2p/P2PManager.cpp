// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#include "P2PManager.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QDebug>
#include <openssl/rand.h>

#include "Application.h"
#include "minecraft/auth/AccountList.h"
#include "minecraft/skins/LocalSkinServer.h"

P2PManager* P2PManager::s_instance = nullptr;

P2PManager* P2PManager::instance()
{
    if (!s_instance) {
        s_instance = new P2PManager(qApp);
    }
    return s_instance;
}

P2PManager::P2PManager(QObject* parent)
    : QObject(parent)
    , m_signalingServerUrl(QStringLiteral("https://p2p.prismlauncher.org/api/v1"))
{
    m_udpSocket = new QUdpSocket(this);
    connect(m_udpSocket, &QUdpSocket::readyRead, this, &P2PManager::onUdpReadyRead);

    m_tcpServer = new QTcpServer(this);
    connect(m_tcpServer, &QTcpServer::newConnection, this, &P2PManager::onNewGuestConnection);

    m_networkManager = new QNetworkAccessManager(this);

    m_punchTimer = new QTimer(this);
    m_punchTimer->setInterval(200);
    connect(m_punchTimer, &QTimer::timeout, this, &P2PManager::onPunchTimeout);

    m_keepAliveTimer = new QTimer(this);
    m_keepAliveTimer->setInterval(1500);
    connect(m_keepAliveTimer, &QTimer::timeout, this, &P2PManager::onKeepAliveTimeout);

    m_signalingPollTimer = new QTimer(this);
    m_signalingPollTimer->setInterval(1500);
    connect(m_signalingPollTimer, &QTimer::timeout, this, &P2PManager::onSignalingPollTimeout);

    m_retransmitTimer = new QTimer(this);
    m_retransmitTimer->setInterval(25);
    connect(m_retransmitTimer, &QTimer::timeout, this, &P2PManager::onRetransmitTimeout);
}

P2PManager::~P2PManager()
{
    stop();
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

quint16 P2PManager::localUdpPort() const
{
    return m_udpSocket ? m_udpSocket->localPort() : 0;
}

int P2PManager::connectedPeersCount() const
{
    if (m_role == Role::Host) {
        return m_hostEndpoints.size() > 0 ? m_hostEndpoints.size() : (m_state == SessionState::Connected ? 1 : 0);
    }
    if (m_role == Role::Guest) {
        return m_state == SessionState::Connected ? 1 : 0;
    }
    return 0;
}

QList<P2PManager::PeerParticipant> P2PManager::participants() const
{
    return m_participants.values();
}

void P2PManager::setState(SessionState newState)
{
    if (m_state != newState) {
        m_state = newState;
        emit stateChanged(newState);
    }
}

QByteArray P2PManager::generateRandomKey(int length)
{
    QByteArray key;
    key.resize(length);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(key.data()), length) != 1) {
        for (int i = 0; i < length; ++i) {
            key[i] = static_cast<char>(QRandomGenerator::global()->bounded(256));
        }
    }
    return key;
}

QString P2PManager::encodeDirectTicket(const QHostAddress& address, quint16 port, const QByteArray& key)
{
    QString payload = QStringLiteral("%1:%2:%3")
                          .arg(address.toString())
                          .arg(port)
                          .arg(QString::fromUtf8(key.toHex()));
    return QStringLiteral("P2P-%1").arg(
        QString::fromLatin1(payload.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
}

bool P2PManager::decodeDirectTicket(const QString& ticket, QHostAddress& outAddress, quint16& outPort, QByteArray& outKey)
{
    QString raw = ticket.trimmed();
    QString payloadStr;

    if (raw.startsWith(QStringLiteral("DIRECT:"), Qt::CaseInsensitive)) {
        payloadStr = raw.mid(7);
    } else if (raw.startsWith(QStringLiteral("P2P-"), Qt::CaseInsensitive)) {
        QByteArray decoded = QByteArray::fromBase64(raw.mid(4).toLatin1(), QByteArray::Base64UrlEncoding);
        payloadStr = QString::fromUtf8(decoded);
    } else {
        payloadStr = raw;
    }

    QStringList parts = payloadStr.split(':');
    if (parts.size() >= 2) {
        outAddress = QHostAddress(parts[0]);
        bool ok = false;
        outPort = parts[1].toUShort(&ok);
        if (!ok || outPort == 0 || outAddress.isNull()) {
            return false;
        }
        if (parts.size() >= 3) {
            outKey = QByteArray::fromHex(parts[2].toUtf8());
        } else {
            outKey.clear();
        }
        return true;
    }

    return false;
}

quint64 P2PManager::makeHostConnKey(const QHostAddress& sender, quint16 senderPort, quint16 connId)
{
    quint64 addrHash = static_cast<quint64>(qHash(sender.toString()));
    return (addrHash << 32) | (static_cast<quint64>(senderPort) << 16) | static_cast<quint64>(connId);
}

void P2PManager::setLocalMinecraftPort(quint16 port)
{
    if (port == 0) return;
    if (m_localMinecraftPort != port) {
        m_localMinecraftPort = port;
        emit localMinecraftPortDetected(port);
        emit statusMessage(tr("Minecraft LAN server detected on port %1").arg(port));
        qDebug() << "P2PManager: Auto-detected Minecraft LAN port:" << port;
    }
}

void P2PManager::createDirectRoom(quint16 localMinecraftPort, quint16 udpPort)
{
    stop();

    m_role = Role::Host;
    m_localMinecraftPort = localMinecraftPort;

    if (!m_udpSocket->bind(QHostAddress::AnyIPv4, udpPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        emit errorOccurred(tr("Could not bind UDP socket on port %1").arg(udpPort));
        setState(SessionState::Error);
        return;
    }

    if (m_encryptionKey.isEmpty()) {
        m_encryptionKey = generateRandomKey();
    }

    m_roomCode = encodeDirectTicket(QHostAddress::LocalHost, m_udpSocket->localPort(), m_encryptionKey);
    setState(SessionState::Hosting);
    emit roomCreated(m_roomCode, m_localMinecraftPort);
    emit statusMessage(tr("Direct room created: %1 (Waiting for players)").arg(m_roomCode));
}

void P2PManager::createRoom(quint16 localMinecraftPort)
{
    stop();

    m_role = Role::Host;
    m_localMinecraftPort = localMinecraftPort;

    if (!m_udpSocket->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        emit errorOccurred(tr("Could not bind UDP socket"));
        setState(SessionState::Error);
        return;
    }

    if (m_encryptionKey.isEmpty()) {
        m_encryptionKey = generateRandomKey();
    }

    if (!m_signalingServerUrl.isValid() || m_signalingServerUrl.isEmpty()) {
        m_roomCode = encodeDirectTicket(QHostAddress::LocalHost, m_udpSocket->localPort(), m_encryptionKey);
        setState(SessionState::Hosting);
        emit roomCreated(m_roomCode, m_localMinecraftPort);
        emit statusMessage(tr("Direct room created: %1").arg(m_roomCode));
        return;
    }

    setState(SessionState::Signaling);
    emit statusMessage(tr("Contacting signaling server to register room..."));

    QUrl createUrl = m_signalingServerUrl;
    QString path = createUrl.path();
    if (!path.endsWith('/')) {
        path += '/';
    }
    path += QStringLiteral("rooms");
    createUrl.setPath(path);

    QNetworkRequest request(createUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QJsonObject reqJson;
    reqJson[QStringLiteral("role")] = QStringLiteral("host");
    reqJson[QStringLiteral("udpPort")] = static_cast<int>(m_udpSocket->localPort());
    reqJson[QStringLiteral("secretKey")] = QString::fromUtf8(m_encryptionKey.toHex());

    QNetworkReply* reply = m_networkManager->post(request, QJsonDocument(reqJson).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_state != SessionState::Signaling) {
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "P2P Signaling server registration failed:" << reply->errorString();
            m_roomCode = encodeDirectTicket(QHostAddress::LocalHost, m_udpSocket->localPort(), m_encryptionKey);
            setState(SessionState::Hosting);
            emit roomCreated(m_roomCode, m_localMinecraftPort);
            emit statusMessage(tr("Signaling offline. Room created in direct mode: %1").arg(m_roomCode));
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject obj = doc.object();
        m_roomCode = obj.value(QStringLiteral("roomCode")).toString();
        m_roomToken = obj.value(QStringLiteral("roomToken")).toString();

        if (obj.contains(QStringLiteral("secretKey")) && !obj.value(QStringLiteral("secretKey")).toString().isEmpty()) {
            m_encryptionKey = QByteArray::fromHex(obj.value(QStringLiteral("secretKey")).toString().toUtf8());
        }

        if (m_roomCode.isEmpty()) {
            m_roomCode = encodeDirectTicket(QHostAddress::LocalHost, m_udpSocket->localPort(), m_encryptionKey);
        }

        setState(SessionState::Hosting);
        emit roomCreated(m_roomCode, m_localMinecraftPort);
        emit statusMessage(tr("Room %1 created. Waiting for peers...").arg(m_roomCode));

        m_signalingPollTimer->start(1500);
    });
}

void P2PManager::onSignalingPollTimeout()
{
    if (m_roomCode.isEmpty() || m_state != SessionState::Hosting || !m_signalingServerUrl.isValid()) {
        m_signalingPollTimer->stop();
        return;
    }

    QUrl pollUrl = m_signalingServerUrl;
    QString path = pollUrl.path();
    if (!path.endsWith('/')) {
        path += '/';
    }
    path += QStringLiteral("rooms/") + m_roomCode + QStringLiteral("/peer");
    pollUrl.setPath(path);

    QNetworkRequest request(pollUrl);
    QNetworkReply* reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_state != SessionState::Hosting) {
            return;
        }

        if (reply->error() == QNetworkReply::NoError) {
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            QJsonObject obj = doc.object();
            if (obj.value(QStringLiteral("status")).toString() == QStringLiteral("ready") || obj.contains(QStringLiteral("peerIp"))) {
                QString peerIp = obj.value(QStringLiteral("peerIp")).toString();
                quint16 peerPort = static_cast<quint16>(obj.value(QStringLiteral("peerPort")).toInt());
                if (!peerIp.isEmpty() && peerPort > 0) {
                    m_signalingPollTimer->stop();
                    m_peerAddress = QHostAddress(peerIp);
                    m_peerPort = peerPort;
                    startHolePunching();
                }
            }
        }
    });
}

void P2PManager::joinRoom(const QString& roomCode)
{
    stop();

    m_role = Role::Guest;
    m_roomCode = roomCode.trimmed();

    if (!m_tcpServer->listen(QHostAddress::LocalHost, 25565)) {
        if (!m_tcpServer->listen(QHostAddress::LocalHost, 0)) {
            emit errorOccurred(tr("Could not bind local proxy port"));
            setState(SessionState::Error);
            return;
        }
    }
    m_localProxyPort = m_tcpServer->serverPort();

    if (!m_udpSocket->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        emit errorOccurred(tr("Could not bind UDP socket"));
        setState(SessionState::Error);
        return;
    }

    QHostAddress directHost;
    quint16 directPort = 0;
    QByteArray directKey;
    if (decodeDirectTicket(m_roomCode, directHost, directPort, directKey)) {
        m_peerAddress = directHost;
        m_peerPort = directPort;
        if (!directKey.isEmpty()) {
            m_encryptionKey = directKey;
        }
        emit statusMessage(tr("Connecting to direct peer %1:%2...").arg(directHost.toString()).arg(directPort));
        startHolePunching();
        return;
    }

    if (!m_signalingServerUrl.isValid() || m_signalingServerUrl.isEmpty()) {
        emit errorOccurred(tr("Signaling server URL is not configured"));
        setState(SessionState::Error);
        return;
    }

    setState(SessionState::Signaling);
    emit statusMessage(tr("Joining room %1 via signaling server...").arg(m_roomCode));

    QUrl joinUrl = m_signalingServerUrl;
    QString path = joinUrl.path();
    if (!path.endsWith('/')) {
        path += '/';
    }
    path += QStringLiteral("rooms/") + m_roomCode + QStringLiteral("/join");
    joinUrl.setPath(path);

    QNetworkRequest request(joinUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QJsonObject reqJson;
    reqJson[QStringLiteral("role")] = QStringLiteral("guest");
    reqJson[QStringLiteral("udpPort")] = static_cast<int>(m_udpSocket->localPort());

    QNetworkReply* reply = m_networkManager->post(request, QJsonDocument(reqJson).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_state != SessionState::Signaling) {
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(tr("Failed to join room: %1").arg(reply->errorString()));
            setState(SessionState::Error);
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject obj = doc.object();
        QString hostIp = obj.value(QStringLiteral("hostIp")).toString();
        quint16 hostPort = static_cast<quint16>(obj.value(QStringLiteral("hostPort")).toInt());

        if (obj.contains(QStringLiteral("secretKey")) && !obj.value(QStringLiteral("secretKey")).toString().isEmpty()) {
            m_encryptionKey = QByteArray::fromHex(obj.value(QStringLiteral("secretKey")).toString().toUtf8());
        }

        if (hostIp.isEmpty() || hostPort == 0) {
            emit errorOccurred(tr("Invalid host endpoint received from signaling server"));
            setState(SessionState::Error);
            return;
        }

        m_peerAddress = QHostAddress(hostIp);
        m_peerPort = hostPort;
        startHolePunching();
    });
}

void P2PManager::startHolePunching()
{
    setState(SessionState::Punching);
    emit statusMessage(tr("Performing UDP hole punching to %1:%2...").arg(m_peerAddress.toString()).arg(m_peerPort));

    m_punchAttempts = 0;
    sendPacket(P2P::PacketType::Punch, 0, 0, {});

    m_punchTimer->start(200);
}

void P2PManager::onPunchTimeout()
{
    m_punchAttempts++;
    if (m_punchAttempts > 35) {
        m_punchTimer->stop();
        emit errorOccurred(tr("UDP hole punching timed out. Unable to reach peer."));
        setState(SessionState::Error);
        return;
    }

    sendPacket(P2P::PacketType::Punch, 0, 0, {});
}

void P2PManager::onConnected()
{
    m_punchTimer->stop();
    setState(SessionState::Connected);
    startKeepAlive();
    m_retransmitTimer->start();
    emit peerConnected(QStringLiteral("%1:%2").arg(m_peerAddress.toString()).arg(m_peerPort));
    emit statusMessage(tr("P2P tunnel established with %1:%2").arg(m_peerAddress.toString()).arg(m_peerPort));

    // Auto-broadcast active account profile and skin texture
    broadcastIdentityAndSkin();
}

void P2PManager::startKeepAlive()
{
    m_lastPacketReceivedTimer.restart();
    m_keepAliveElapsedTimer.restart();
    m_keepAliveTimer->start(1500);
}

void P2PManager::onKeepAliveTimeout()
{
    if (m_state != SessionState::Connected) {
        m_keepAliveTimer->stop();
        return;
    }

    if (m_lastPacketReceivedTimer.elapsed() > 12000) {
        qWarning() << "P2P KeepAlive timeout: no packets received for 12 seconds.";
        emit errorOccurred(tr("P2P connection timed out"));
        emit peerDisconnected(QStringLiteral("%1:%2").arg(m_peerAddress.toString()).arg(m_peerPort));
        stop();
        return;
    }

    m_keepAliveElapsedTimer.restart();
    sendPacket(P2P::PacketType::KeepAlive, 0, 0, {});
}

void P2PManager::sendPacket(P2P::PacketType type, quint16 connId, quint32 seq, const QByteArray& payload)
{
    sendPacketTo(type, connId, seq, payload, m_peerAddress, m_peerPort);
}

void P2PManager::sendPacketTo(P2P::PacketType type, quint16 connId, quint32 seq, const QByteArray& payload,
                              const QHostAddress& destAddress, quint16 destPort)
{
    if (destAddress.isNull() || destPort == 0 || !m_udpSocket) {
        return;
    }

    QByteArray datagram = P2P::Protocol::serialize(type, connId, seq, payload, m_encryptionKey);
    qint64 sent = m_udpSocket->writeDatagram(datagram, destAddress, destPort);
    if (sent > 0) {
        m_txBytes += sent;
        m_txPackets++;
    }
}

void P2PManager::sendTcpChunks(quint16 connId, const QByteArray& data)
{
    sendTcpChunksTo(m_peerAddress, m_peerPort, connId, data);
}

void P2PManager::sendTcpChunksTo(const QHostAddress& dest, quint16 port, quint16 connId, const QByteArray& data)
{
    const int maxChunk = P2P::MAX_PAYLOAD_SIZE;
    for (int offset = 0; offset < data.size(); offset += maxChunk) {
        int len = qMin(maxChunk, data.size() - offset);
        QByteArray chunk = data.mid(offset, len);
        quint32 seq = ++m_connNextSeq[connId];

        // Store in unacknowledged buffer for reliable retransmission
        UnackedPacket unacked;
        unacked.connId = connId;
        unacked.seq = seq;
        unacked.payload = chunk;
        unacked.destAddress = dest;
        unacked.destPort = port;
        unacked.lastSentMs = QDateTime::currentMSecsSinceEpoch();
        unacked.retries = 0;

        quint64 unackedKey = (static_cast<quint64>(connId) << 32) | static_cast<quint64>(seq);
        m_unackedPackets.insert(unackedKey, unacked);

        sendPacketTo(P2P::PacketType::TcpData, connId, seq, chunk, dest, port);
    }
}

void P2PManager::onRetransmitTimeout()
{
    if (m_unackedPackets.isEmpty()) {
        return;
    }

    qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_unackedPackets.begin(); it != m_unackedPackets.end(); ++it) {
        if (now - it.value().lastSentMs >= 100) {
            it.value().lastSentMs = now;
            it.value().retries++;
            if (it.value().retries <= 50) {
                sendPacketTo(P2P::PacketType::TcpData, it.value().connId, it.value().seq,
                             it.value().payload, it.value().destAddress, it.value().destPort);
            }
        }
    }
}

void P2PManager::sendControlChunked(P2P::PacketType type, const QByteArray& data)
{
    const int maxChunk = P2P::MAX_PAYLOAD_SIZE;
    quint16 totalChunks = static_cast<quint16>((data.size() + maxChunk - 1) / maxChunk);
    if (totalChunks == 0) totalChunks = 1;

    for (quint16 i = 0; i < totalChunks; ++i) {
        int offset = i * maxChunk;
        int len = qMin(maxChunk, data.size() - offset);
        QByteArray chunk = data.mid(offset, len);
        sendPacket(type, totalChunks, i, chunk);
    }
}

void P2PManager::broadcastIdentityAndSkin()
{
    QString uuid;
    QString username;
    QString modelType = QStringLiteral("steve");
    QByteArray skinPngBytes;

    auto* app = dynamic_cast<Application*>(qApp);
    if (app && app->accounts() && app->accounts()->defaultAccount()) {
        auto acc = app->accounts()->defaultAccount();
        uuid = acc->profileId();
        username = acc->profileName();
        modelType = acc->modelType().isEmpty() ? QStringLiteral("steve") : acc->modelType();
        if (!acc->skinPath().isEmpty() && QFile::exists(acc->skinPath())) {
            QFile f(acc->skinPath());
            if (f.open(QFile::ReadOnly)) {
                skinPngBytes = f.readAll();
            }
        }
        if (skinPngBytes.isEmpty() && acc->accountData()) {
            skinPngBytes = acc->accountData()->minecraftProfile.skin.data;
        }
    }

    if (uuid.isEmpty()) {
        uuid = QStringLiteral("5627dd98e6be3c21b8a8e92344183641");
    }
    if (username.isEmpty()) {
        username = (m_role == Role::Host) ? QStringLiteral("Host") : QStringLiteral("Guest");
    }

    // Register local participant
    PeerParticipant localParticipant;
    localParticipant.uuid = uuid;
    localParticipant.username = username;
    localParticipant.modelType = modelType;
    localParticipant.isLocal = true;
    localParticipant.isHost = (m_role == Role::Host);
    localParticipant.pingMs = 0;
    m_participants.insert(uuid, localParticipant);
    emit participantsUpdated();

    QJsonObject obj;
    obj[QStringLiteral("uuid")] = uuid;
    obj[QStringLiteral("username")] = username;
    obj[QStringLiteral("modelType")] = modelType;
    obj[QStringLiteral("skinPngBase64")] = QString::fromLatin1(skinPngBytes.toBase64());

    QByteArray jsonBytes = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    sendControlChunked(P2P::PacketType::PeerIdentityAndSkin, jsonBytes);
}

void P2PManager::handleIncomingIdentityPacket(quint16 totalChunks, quint32 chunkIndex, const QByteArray& payload)
{
    m_expectedIdentityTotalChunks = totalChunks;
    m_incomingIdentityChunks.insert(chunkIndex, payload);

    if (m_incomingIdentityChunks.size() >= totalChunks) {
        QByteArray fullJson;
        for (auto it = m_incomingIdentityChunks.begin(); it != m_incomingIdentityChunks.end(); ++it) {
            fullJson.append(it.value());
        }
        m_incomingIdentityChunks.clear();

        QJsonDocument doc = QJsonDocument::fromJson(fullJson);
        if (!doc.isObject()) {
            return;
        }
        QJsonObject obj = doc.object();
        QString uuid = obj.value(QStringLiteral("uuid")).toString();
        QString username = obj.value(QStringLiteral("username")).toString();
        QString modelType = obj.value(QStringLiteral("modelType")).toString();
        QByteArray skinPng = QByteArray::fromBase64(obj.value(QStringLiteral("skinPngBase64")).toString().toLatin1());

        QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/p2p_skins");
        QDir().mkpath(cacheDir);
        QString skinPath = cacheDir + QStringLiteral("/") + QString(uuid).remove('-') + QStringLiteral(".png");
        if (!skinPng.isEmpty()) {
            QFile file(skinPath);
            if (file.open(QFile::WriteOnly)) {
                file.write(skinPng);
                file.close();
            }
        }

        // Register in LocalSkinServer so Minecraft textures resolve locally
        auto* skinServer = LocalSkinServer::instance();
        if (skinServer && skinServer->ensureStarted()) {
            if (QFile::exists(skinPath)) {
                skinServer->registerSkin(uuid, skinPath, modelType);
                skinServer->registerSkin(username, skinPath, modelType);
            }
        }

        PeerParticipant peer;
        peer.uuid = uuid;
        peer.username = username;
        peer.modelType = modelType;
        peer.skinPath = skinPath;
        peer.isLocal = false;
        peer.isHost = (m_role == Role::Guest);
        peer.pingMs = m_lastPingMs;
        m_participants.insert(uuid, peer);

        emit participantJoined(uuid, username, modelType, skinPath);
        emit participantsUpdated();
        emit statusMessage(tr("Synchronized skin for player '%1'").arg(username));
    }
}

void P2PManager::onUdpReadyRead()
{
    while (m_udpSocket && m_udpSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_udpSocket->pendingDatagramSize());
        QHostAddress sender;
        quint16 senderPort = 0;
        qint64 bytesRead = m_udpSocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
        if (bytesRead <= 0) {
            continue;
        }

        m_rxBytes += bytesRead;
        m_rxPackets++;

        P2P::Packet packet;
        if (!P2P::Protocol::parse(datagram, packet, m_encryptionKey)) {
            continue;
        }

        m_lastPacketReceivedTimer.restart();

        // Reflexive endpoint learning: dynamically adapt to NAT port translation
        if (m_role == Role::Guest || m_peerAddress.isNull()) {
            m_peerAddress = sender;
            m_peerPort = senderPort;
        }

        switch (packet.type) {
        case P2P::PacketType::Punch:
            sendPacketTo(P2P::PacketType::PunchAck, 0, 0, {}, sender, senderPort);
            if (m_state != SessionState::Connected) {
                m_peerAddress = sender;
                m_peerPort = senderPort;
                onConnected();
            }
            break;

        case P2P::PacketType::PunchAck:
            if (m_state != SessionState::Connected) {
                m_peerAddress = sender;
                m_peerPort = senderPort;
                onConnected();
            }
            break;

        case P2P::PacketType::KeepAlive:
            sendPacketTo(P2P::PacketType::KeepAliveAck, 0, packet.seq, {}, sender, senderPort);
            break;

        case P2P::PacketType::KeepAliveAck:
            m_lastPingMs = static_cast<int>(m_keepAliveElapsedTimer.elapsed());
            for (auto& p : m_participants) {
                if (!p.isLocal) {
                    p.pingMs = m_lastPingMs;
                }
            }
            emit pingUpdated(m_lastPingMs);
            emit participantsUpdated();
            break;

        case P2P::PacketType::TcpConnect:
            if (m_role == Role::Host) {
                handleIncomingTcpConnectHost(sender, senderPort, packet.connId);
            }
            break;

        case P2P::PacketType::TcpData:
            if (m_role == Role::Host) {
                handleIncomingTcpDataHost(sender, senderPort, packet.connId, packet.seq, packet.payload);
            } else if (m_role == Role::Guest) {
                handleIncomingTcpDataGuest(packet.connId, packet.seq, packet.payload, sender, senderPort);
            }
            break;

        case P2P::PacketType::TcpDataAck: {
            quint64 key = (static_cast<quint64>(packet.connId) << 32) | static_cast<quint64>(packet.seq);
            m_unackedPackets.remove(key);
            break;
        }

        case P2P::PacketType::TcpDisconnect:
            if (m_role == Role::Host) {
                handleIncomingTcpDisconnectHost(sender, senderPort, packet.connId);
            } else if (m_role == Role::Guest) {
                handleIncomingTcpDisconnectGuest(packet.connId);
            }
            break;

        case P2P::PacketType::PeerIdentityAndSkin:
            handleIncomingIdentityPacket(packet.connId, packet.seq, packet.payload);
            break;

        default:
            break;
        }
    }

    emit trafficStatsUpdated(m_txBytes, m_rxBytes);
}

// ---------------- Guest Handlers ----------------

void P2PManager::onNewGuestConnection()
{
    while (m_tcpServer && m_tcpServer->hasPendingConnections()) {
        QTcpSocket* clientSocket = m_tcpServer->nextPendingConnection();
        quint16 connId = ++m_nextGuestConnId;
        m_guestSockets.insert(connId, clientSocket);

        // Tell host to establish TCP connection to local Minecraft LAN server
        sendPacket(P2P::PacketType::TcpConnect, connId, 0, {});

        connect(clientSocket, &QTcpSocket::readyRead, this, [this, connId, clientSocket]() {
            QByteArray data = clientSocket->readAll();
            sendTcpChunks(connId, data);
        });

        connect(clientSocket, &QTcpSocket::disconnected, this, [this, connId, clientSocket]() {
            sendPacket(P2P::PacketType::TcpDisconnect, connId, 0, {});
            m_guestSockets.remove(connId);
            clientSocket->deleteLater();
        });

        connect(clientSocket, &QAbstractSocket::errorOccurred, this, [this, connId, clientSocket](QAbstractSocket::SocketError) {
            sendPacket(P2P::PacketType::TcpDisconnect, connId, 0, {});
            m_guestSockets.remove(connId);
            clientSocket->deleteLater();
        });
    }
}

void P2PManager::handleIncomingTcpDataGuest(quint16 connId, quint32 seq, const QByteArray& payload, const QHostAddress& sender, quint16 senderPort)
{
    // Acknowledge receipt immediately to stop retransmissions
    sendPacketTo(P2P::PacketType::TcpDataAck, connId, seq, {}, sender, senderPort);

    quint64 streamKey = connId;
    if (!m_expectedSeq.contains(streamKey)) {
        m_expectedSeq.insert(streamKey, 1);
    }
    quint32 expected = m_expectedSeq.value(streamKey);

    if (seq == expected) {
        deliverTcpPayload(streamKey, connId, payload);
        expected++;

        auto& reorder = m_reorderBuffers[streamKey];
        while (reorder.contains(expected)) {
            QByteArray next = reorder.take(expected);
            deliverTcpPayload(streamKey, connId, next);
            expected++;
        }
        m_expectedSeq[streamKey] = expected;
    } else if (seq > expected) {
        // Out-of-order packet: buffer until missing packets arrive
        m_reorderBuffers[streamKey].insert(seq, payload);
    }
    // If seq < expected: duplicate packet, ignore
}

void P2PManager::handleIncomingTcpDisconnectGuest(quint16 connId)
{
    auto socket = m_guestSockets.take(connId);
    if (socket) {
        socket->disconnectFromHost();
        socket->deleteLater();
    }
    m_expectedSeq.remove(connId);
    m_reorderBuffers.remove(connId);
}

// ---------------- Host Handlers ----------------

void P2PManager::handleIncomingTcpConnectHost(const QHostAddress& sender, quint16 senderPort, quint16 connId)
{
    quint64 key = makeHostConnKey(sender, senderPort, connId);
    if (m_hostSockets.contains(key)) {
        auto old = m_hostSockets.take(key);
        if (old) {
            old->deleteLater();
        }
    }

    auto socket = new QTcpSocket(this);
    m_hostSockets.insert(key, socket);
    m_hostEndpoints.insert(key, qMakePair(sender, senderPort));
    m_hostConnIds.insert(key, connId);

    connect(socket, &QTcpSocket::connected, this, [this, key, socket]() {
        if (m_pendingHostData.contains(key)) {
            QByteArray pending = m_pendingHostData.take(key);
            socket->write(pending);
        }
    });

    connect(socket, &QTcpSocket::readyRead, this, [this, key, socket]() {
        if (!m_hostEndpoints.contains(key)) return;
        auto ep = m_hostEndpoints.value(key);
        quint16 cid = m_hostConnIds.value(key);
        QByteArray data = socket->readAll();
        sendTcpChunksTo(ep.first, ep.second, cid, data);
    });

    connect(socket, &QTcpSocket::disconnected, this, [this, key, socket]() {
        if (m_hostEndpoints.contains(key)) {
            auto ep = m_hostEndpoints.value(key);
            quint16 cid = m_hostConnIds.value(key);
            sendPacketTo(P2P::PacketType::TcpDisconnect, cid, 0, {}, ep.first, ep.second);
        }
        m_hostSockets.remove(key);
        m_hostEndpoints.remove(key);
        m_hostConnIds.remove(key);
        m_pendingHostData.remove(key);
        m_expectedSeq.remove(key);
        m_reorderBuffers.remove(key);
        socket->deleteLater();
    });

    connect(socket, &QAbstractSocket::errorOccurred, this, [this, key, socket](QAbstractSocket::SocketError) {
        if (m_hostEndpoints.contains(key)) {
            auto ep = m_hostEndpoints.value(key);
            quint16 cid = m_hostConnIds.value(key);
            sendPacketTo(P2P::PacketType::TcpDisconnect, cid, 0, {}, ep.first, ep.second);
        }
        m_hostSockets.remove(key);
        m_hostEndpoints.remove(key);
        m_hostConnIds.remove(key);
        m_pendingHostData.remove(key);
        m_expectedSeq.remove(key);
        m_reorderBuffers.remove(key);
        socket->deleteLater();
    });

    socket->connectToHost(QHostAddress::LocalHost, m_localMinecraftPort);
}

void P2PManager::handleIncomingTcpDataHost(const QHostAddress& sender, quint16 senderPort, quint16 connId, quint32 seq, const QByteArray& payload)
{
    // Acknowledge receipt immediately to stop retransmissions
    sendPacketTo(P2P::PacketType::TcpDataAck, connId, seq, {}, sender, senderPort);

    quint64 streamKey = makeHostConnKey(sender, senderPort, connId);
    if (!m_expectedSeq.contains(streamKey)) {
        m_expectedSeq.insert(streamKey, 1);
    }
    quint32 expected = m_expectedSeq.value(streamKey);

    if (seq == expected) {
        deliverTcpPayload(streamKey, connId, payload);
        expected++;

        auto& reorder = m_reorderBuffers[streamKey];
        while (reorder.contains(expected)) {
            QByteArray next = reorder.take(expected);
            deliverTcpPayload(streamKey, connId, next);
            expected++;
        }
        m_expectedSeq[streamKey] = expected;
    } else if (seq > expected) {
        // Out-of-order packet: buffer until missing packets arrive
        m_reorderBuffers[streamKey].insert(seq, payload);
    }
}

void P2PManager::deliverTcpPayload(quint64 streamKey, quint16 connId, const QByteArray& payload)
{
    if (m_role == Role::Host) {
        auto socket = m_hostSockets.value(streamKey, nullptr);
        if (socket) {
            if (socket->state() == QAbstractSocket::ConnectedState) {
                socket->write(payload);
            } else {
                m_pendingHostData[streamKey].append(payload);
            }
        }
    } else if (m_role == Role::Guest) {
        auto socket = m_guestSockets.value(connId, nullptr);
        if (socket && socket->state() == QAbstractSocket::ConnectedState) {
            socket->write(payload);
        }
    }
}

void P2PManager::handleIncomingTcpDisconnectHost(const QHostAddress& sender, quint16 senderPort, quint16 connId)
{
    quint64 key = makeHostConnKey(sender, senderPort, connId);
    auto socket = m_hostSockets.take(key);
    m_hostEndpoints.remove(key);
    m_hostConnIds.remove(key);
    m_pendingHostData.remove(key);
    m_expectedSeq.remove(key);
    m_reorderBuffers.remove(key);
    if (socket) {
        socket->disconnectFromHost();
        socket->deleteLater();
    }
}

void P2PManager::stop()
{
    m_punchTimer->stop();
    m_keepAliveTimer->stop();
    m_signalingPollTimer->stop();
    m_retransmitTimer->stop();

    if (m_udpSocket && m_udpSocket->isOpen()) {
        m_udpSocket->close();
    }

    if (m_tcpServer && m_tcpServer->isListening()) {
        m_tcpServer->close();
    }

    for (auto& sock : m_guestSockets) {
        if (sock) {
            sock->disconnectFromHost();
            sock->deleteLater();
        }
    }
    m_guestSockets.clear();

    for (auto& sock : m_hostSockets) {
        if (sock) {
            sock->disconnectFromHost();
            sock->deleteLater();
        }
    }
    m_hostSockets.clear();
    m_hostEndpoints.clear();
    m_hostConnIds.clear();
    m_pendingHostData.clear();

    m_unackedPackets.clear();
    m_connNextSeq.clear();
    m_expectedSeq.clear();
    m_reorderBuffers.clear();
    m_participants.clear();
    m_incomingIdentityChunks.clear();

    m_role = Role::Idle;
    m_peerPort = 0;
    m_peerAddress.clear();
    m_localProxyPort = 0;
    m_punchAttempts = 0;

    setState(SessionState::Idle);
}
