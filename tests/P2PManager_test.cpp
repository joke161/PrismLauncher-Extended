// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#include <QTest>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>

#include "p2p/P2PManager.h"
#include "p2p/P2PProtocol.h"
#include "ui/dialogs/P2PRoomDialog.h"

class P2PManagerTest : public QObject {
    Q_OBJECT

private slots:
    void test_ProtocolSerializationAndValidation()
    {
        QByteArray key = "MySecretPassKey1";
        QByteArray payload = "Minecraft TCP stream payload test data!";
        quint16 connId = 42;
        quint32 seq = 1001;

        // Serialize packet
        QByteArray datagram = P2P::Protocol::serialize(
            P2P::PacketType::TcpData, connId, seq, payload, key
        );

        QCOMPARE(datagram.size(), P2P::HEADER_SIZE + payload.size());

        // Parse valid packet
        P2P::Packet parsed;
        bool ok = P2P::Protocol::parse(datagram, parsed, key);
        QVERIFY(ok);
        QCOMPARE(parsed.type, P2P::PacketType::TcpData);
        QCOMPARE(parsed.connId, connId);
        QCOMPARE(parsed.seq, seq);
        QCOMPARE(parsed.payload, payload);

        // Corrupted packet tests
        // 1. Truncated header
        P2P::Packet badPacket;
        QVERIFY(!P2P::Protocol::parse(datagram.left(P2P::HEADER_SIZE - 1), badPacket, key));

        // 2. Bad magic bytes
        QByteArray badMagic = datagram;
        badMagic[0] = 'X';
        QVERIFY(!P2P::Protocol::parse(badMagic, badPacket, key));

        // 3. Bad version
        QByteArray badVer = datagram;
        badVer[2] = 99;
        QVERIFY(!P2P::Protocol::parse(badVer, badPacket, key));

        // 4. Truncated payload
        QByteArray truncated = datagram.left(datagram.size() - 5);
        QVERIFY(!P2P::Protocol::parse(truncated, badPacket, key));
    }

    void test_Aes128CtrStreamCipher()
    {
        QByteArray key = "AnotherSecret123";
        QByteArray original = "The quick brown fox jumps over the lazy dog";
        quint16 connId = 1;
        quint32 seq = 50;

        // Symmetric encryption property: cipher(cipher(data)) == data
        QByteArray encrypted = P2P::Protocol::cipher(original, key, connId, seq);
        QCOMPARE(encrypted.size(), original.size());
        QVERIFY(encrypted != original); // must be obfuscated/encrypted

        QByteArray decrypted = P2P::Protocol::cipher(encrypted, key, connId, seq);
        QCOMPARE(decrypted, original);

        // Different sequence numbers must produce different ciphertexts (unique IV)
        QByteArray encSeq1 = P2P::Protocol::cipher(original, key, connId, 1);
        QByteArray encSeq2 = P2P::Protocol::cipher(original, key, connId, 2);
        QVERIFY(encSeq1 != encSeq2);

        // Empty payload
        QByteArray empty;
        QCOMPARE(P2P::Protocol::cipher(empty, key, connId, seq), empty);
    }

    void test_DirectTicketEncodingAndDecoding()
    {
        QHostAddress ip("192.168.1.100");
        quint16 port = 54321;
        QByteArray key = "1234567890abcdef";

        // Encode ticket
        QString ticket = P2PManager::encodeDirectTicket(ip, port, key);
        QVERIFY(ticket.startsWith("P2P-"));

        // Decode ticket
        QHostAddress decodedIp;
        quint16 decodedPort = 0;
        QByteArray decodedKey;
        bool ok = P2PManager::decodeDirectTicket(ticket, decodedIp, decodedPort, decodedKey);

        QVERIFY(ok);
        QCOMPARE(decodedIp.toString(), ip.toString());
        QCOMPARE(decodedPort, port);
        QCOMPARE(decodedKey, key);

        // DIRECT: prefix format
        QString directTicket = QString("DIRECT:10.0.0.5:25565:%1").arg(QString::fromUtf8(key.toHex()));
        ok = P2PManager::decodeDirectTicket(directTicket, decodedIp, decodedPort, decodedKey);
        QVERIFY(ok);
        QCOMPARE(decodedIp.toString(), QStringLiteral("10.0.0.5"));
        QCOMPARE(decodedPort, static_cast<quint16>(25565));
        QCOMPARE(decodedKey, key);

        // Invalid ticket
        QHostAddress badIp;
        quint16 badPort = 0;
        QByteArray badKey;
        QVERIFY(!P2PManager::decodeDirectTicket("invalid_junk_code", badIp, badPort, badKey));
    }

    void test_UdpHolePunchingHandshake()
    {
        P2PManager host;
        P2PManager guest;

        // Host creates a direct room with a dummy local Minecraft port
        host.createDirectRoom(12345);
        QCOMPARE(host.role(), P2PManager::Role::Host);
        QCOMPARE(host.state(), P2PManager::SessionState::Hosting);
        QVERIFY(!host.roomCode().isEmpty());
        QVERIFY(host.localUdpPort() > 0);

        QSignalSpy hostConnectedSpy(&host, &P2PManager::peerConnected);
        QSignalSpy guestConnectedSpy(&guest, &P2PManager::peerConnected);

        // Guest joins using the direct room code
        guest.joinRoom(host.roomCode());
        QCOMPARE(guest.role(), P2PManager::Role::Guest);

        // Wait for hole punching handshake to complete
        QVERIFY(hostConnectedSpy.wait(3000));
        QVERIFY(guestConnectedSpy.wait(3000));

        QCOMPARE(host.state(), P2PManager::SessionState::Connected);
        QCOMPARE(guest.state(), P2PManager::SessionState::Connected);
        QVERIFY(host.isConnected());
        QVERIFY(guest.isConnected());
        QCOMPARE(host.connectedPeersCount(), 1);
        QCOMPARE(guest.connectedPeersCount(), 1);

        host.stop();
        guest.stop();
        QCOMPARE(host.state(), P2PManager::SessionState::Idle);
        QCOMPARE(guest.state(), P2PManager::SessionState::Idle);
    }

    void test_TcpToUdpProxyBridgeEndToEnd()
    {
        // 1. Setup mock Minecraft LAN server
        QTcpServer mockMinecraftServer;
        QVERIFY(mockMinecraftServer.listen(QHostAddress::LocalHost, 0));
        quint16 mcServerPort = mockMinecraftServer.serverPort();

        QTcpSocket* serverSideSocket = nullptr;
        connect(&mockMinecraftServer, &QTcpServer::newConnection, this, [&]() {
            serverSideSocket = mockMinecraftServer.nextPendingConnection();
        });

        // 2. Setup Host and Guest P2P managers
        P2PManager host;
        P2PManager guest;

        host.createDirectRoom(mcServerPort);
        QSignalSpy hostConnectedSpy(&host, &P2PManager::peerConnected);
        QSignalSpy guestConnectedSpy(&guest, &P2PManager::peerConnected);

        guest.joinRoom(host.roomCode());

        QVERIFY(hostConnectedSpy.wait(3000));
        QVERIFY(guestConnectedSpy.wait(3000));

        quint16 guestProxyPort = guest.localProxyPort();
        QVERIFY(guestProxyPort > 0);

        // 3. Connect mock Minecraft client to Guest proxy
        QTcpSocket clientSocket;
        clientSocket.connectToHost(QHostAddress::LocalHost, guestProxyPort);
        QVERIFY(clientSocket.waitForConnected(2000));

        // Wait for Host to connect to mock Minecraft server
        QTRY_VERIFY_WITH_TIMEOUT(serverSideSocket != nullptr, 2000);
        QVERIFY(serverSideSocket->state() == QAbstractSocket::ConnectedState);

        // 4. Test Client -> Server data flow
        QByteArray receivedOnServer;
        connect(serverSideSocket, &QTcpSocket::readyRead, this, [&]() {
            receivedOnServer.append(serverSideSocket->readAll());
        });
        if (serverSideSocket->bytesAvailable() > 0) {
            receivedOnServer.append(serverSideSocket->readAll());
        }

        const QByteArray clientGreeting = "HELLO_MINECRAFT_SERVER_FROM_P2P";
        clientSocket.write(clientGreeting);
        clientSocket.flush();

        QTRY_COMPARE_WITH_TIMEOUT(receivedOnServer, clientGreeting, 2000);

        // 5. Test Server -> Client data flow
        QByteArray receivedOnClient;
        connect(&clientSocket, &QTcpSocket::readyRead, this, [&]() {
            receivedOnClient.append(clientSocket.readAll());
        });
        if (clientSocket.bytesAvailable() > 0) {
            receivedOnClient.append(clientSocket.readAll());
        }

        const QByteArray serverReply = "WELCOME_TO_THE_WORLD_CHUNK_DATA_123456789";
        serverSideSocket->write(serverReply);
        serverSideSocket->flush();

        QTRY_COMPARE_WITH_TIMEOUT(receivedOnClient, serverReply, 2000);

        // 6. Test large chunked transmission (> 1350 bytes MTU limit)
        QByteArray largeData;
        largeData.resize(10000);
        for (int i = 0; i < largeData.size(); ++i) {
            largeData[i] = static_cast<char>((i * 7 + 13) % 256);
        }

        clientSocket.write(largeData);
        clientSocket.flush();

        receivedOnServer.clear();
        QTRY_VERIFY_WITH_TIMEOUT(receivedOnServer.size() >= largeData.size(), 3000);

        QCOMPARE(receivedOnServer, largeData);

        // 7. Disconnection test
        clientSocket.disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(serverSideSocket->state() == QAbstractSocket::UnconnectedState, 2000);

        // Cleanup
        host.stop();
        guest.stop();
        mockMinecraftServer.close();
    }

    void test_SignalingServerHttpFlow()
    {
        quint16 hostUdpPort = 0;
        quint16 guestUdpPort = 0;

        QTcpServer mockSignalingServer;
        QVERIFY(mockSignalingServer.listen(QHostAddress::LocalHost, 0));
        quint16 signalPort = mockSignalingServer.serverPort();

        connect(&mockSignalingServer, &QTcpServer::newConnection, this, [&]() {
            while (mockSignalingServer.hasPendingConnections()) {
                QTcpSocket* s = mockSignalingServer.nextPendingConnection();
                connect(s, &QTcpSocket::readyRead, this, [s, &hostUdpPort, &guestUdpPort]() {
                    QByteArray req = s->readAll();
                    QString reqStr = QString::fromUtf8(req);

                    if (reqStr.startsWith("POST /rooms ") || reqStr.contains("POST /rooms ")) {
                        int bodyIdx = reqStr.indexOf("\r\n\r\n");
                        if (bodyIdx != -1) {
                            QJsonDocument doc = QJsonDocument::fromJson(req.mid(bodyIdx + 4));
                            hostUdpPort = doc.object().value("udpPort").toInt();
                        }
                        QJsonObject resp;
                        resp["roomCode"] = "PRISM9";
                        resp["roomToken"] = "token_abc";
                        resp["secretKey"] = "11223344556677889900aabbccddeeff";
                        QByteArray body = QJsonDocument(resp).toJson(QJsonDocument::Compact);
                        s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                 QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                        s->disconnectFromHost();
                    } else if (reqStr.contains("POST /rooms/PRISM9/join")) {
                        int bodyIdx = reqStr.indexOf("\r\n\r\n");
                        if (bodyIdx != -1) {
                            QJsonDocument doc = QJsonDocument::fromJson(req.mid(bodyIdx + 4));
                            guestUdpPort = doc.object().value("udpPort").toInt();
                        }
                        QJsonObject resp;
                        resp["hostIp"] = "127.0.0.1";
                        resp["hostPort"] = hostUdpPort;
                        resp["secretKey"] = "11223344556677889900aabbccddeeff";
                        QByteArray body = QJsonDocument(resp).toJson(QJsonDocument::Compact);
                        s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                 QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                        s->disconnectFromHost();
                    } else if (reqStr.contains("GET /rooms/PRISM9/peer")) {
                        QJsonObject resp;
                        if (guestUdpPort > 0) {
                            resp["status"] = "ready";
                            resp["peerIp"] = "127.0.0.1";
                            resp["peerPort"] = guestUdpPort;
                        } else {
                            resp["status"] = "waiting";
                        }
                        QByteArray body = QJsonDocument(resp).toJson(QJsonDocument::Compact);
                        s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                 QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                        s->disconnectFromHost();
                    }
                });
            }
        });

        P2PManager host;
        P2PManager guest;

        QUrl signalUrl(QString("http://127.0.0.1:%1").arg(signalPort));
        host.setSignalingServerUrl(signalUrl);
        guest.setSignalingServerUrl(signalUrl);

        QSignalSpy roomCreatedSpy(&host, &P2PManager::roomCreated);
        QSignalSpy hostConnectedSpy(&host, &P2PManager::peerConnected);
        QSignalSpy guestConnectedSpy(&guest, &P2PManager::peerConnected);

        host.createRoom(25565);
        QVERIFY(roomCreatedSpy.wait(2000));
        QCOMPARE(host.roomCode(), QStringLiteral("PRISM9"));

        guest.joinRoom("PRISM9");

        QVERIFY(hostConnectedSpy.wait(4000));
        QVERIFY(guestConnectedSpy.wait(4000));

        QCOMPARE(host.state(), P2PManager::SessionState::Connected);
        QCOMPARE(guest.state(), P2PManager::SessionState::Connected);

        host.stop();
        guest.stop();
        mockSignalingServer.close();
    }

    void test_LanPortAutoDetection()
    {
        P2PManager manager;
        QSignalSpy portSpy(&manager, &P2PManager::localMinecraftPortDetected);

        manager.setLocalMinecraftPort(54321);

        QCOMPARE(portSpy.count(), 1);
        QCOMPARE(portSpy.takeFirst().at(0).toUInt(), 54321u);
        QCOMPARE(manager.localMinecraftPort(), static_cast<quint16>(54321));

        // Calling with same port doesn't duplicate
        manager.setLocalMinecraftPort(54321);
        QCOMPARE(portSpy.count(), 0);
    }

    void test_SkinSyncAndIdentityExchange()
    {
        P2PManager host;
        P2PManager guest;

        host.createDirectRoom(25565);
        QSignalSpy hostConnectedSpy(&host, &P2PManager::peerConnected);
        QSignalSpy guestConnectedSpy(&guest, &P2PManager::peerConnected);

        guest.joinRoom(host.roomCode());

        QVERIFY(hostConnectedSpy.wait(3000));
        QVERIFY(guestConnectedSpy.wait(3000));

        // Both sides exchange identity and skin automatically
        QTRY_VERIFY_WITH_TIMEOUT(!host.participants().isEmpty(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!guest.participants().isEmpty(), 3000);

        // Test avatar face extraction
        QImage defaultFace = P2PRoomDialog::extractSkinFaceImage(QString(), 32);
        QCOMPARE(defaultFace.width(), 32);
        QCOMPARE(defaultFace.height(), 32);
        QCOMPARE(defaultFace.format(), QImage::Format_ARGB32);

        host.stop();
        guest.stop();
    }
};

QTEST_GUILESS_MAIN(P2PManagerTest)
#include "P2PManager_test.moc"
