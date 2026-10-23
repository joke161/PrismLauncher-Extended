// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#include <QTest>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QImage>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include "minecraft/auth/AccountList.h"
#include "minecraft/auth/MinecraftAccount.h"
#include "minecraft/skins/LocalSkinServer.h"
#include "minecraft/ProfileUtils.h"
#include "minecraft/VersionFile.h"
#include "minecraft/LaunchProfile.h"
#include "FileSystem.h"

class OfflineAccountTest : public QObject {
    Q_OBJECT

private slots:
    void test_OfflineUuidGeneration()
    {
        // Standard Minecraft offline UUID algorithm:
        // UUID.nameUUIDFromBytes(("OfflinePlayer:" + username).getBytes(StandardCharsets.UTF_8))
        // Known test vectors:
        // "Steve" -> 5627dd98-e6be-3c21-b8a8-e92344183641 (hex: 5627dd98e6be3c21b8a8e92344183641)
        QUuid steveUuid = MinecraftAccount::uuidFromUsername("Steve");
        QCOMPARE(steveUuid.toString(QUuid::WithoutBraces).toLower(), QStringLiteral("5627dd98-e6be-3c21-b8a8-e92344183641"));
        QCOMPARE(steveUuid.toString(QUuid::Id128).toLower(), QStringLiteral("5627dd98e6be3c21b8a8e92344183641"));

        // Creating offline account produces this exact profile ID
        auto account = MinecraftAccount::createOffline("Steve");
        QCOMPARE(account->profileId().toLower(), QStringLiteral("5627dd98e6be3c21b8a8e92344183641"));
        QCOMPARE(account->profileName(), QStringLiteral("Steve"));

        // fillSession produces matching UUID
        auto session = std::make_shared<AuthSession>();
        account->fillSession(session);
        QCOMPARE(session->uuid.toLower(), QStringLiteral("5627dd98e6be3c21b8a8e92344183641"));
        QCOMPARE(session->player_name, QStringLiteral("Steve"));
        QCOMPARE(session->user_type, QStringLiteral("offline"));
    }

    void test_OfflineAccountMetadataSerialization()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        QString fakeSkinPath = tempDir.filePath("skins/Player_skin.png");
        QDir().mkpath(tempDir.filePath("skins"));

        // Create a 64x64 test skin image
        QImage skinImg(64, 64, QImage::Format_ARGB32);
        skinImg.fill(Qt::blue);
        QVERIFY(skinImg.save(fakeSkinPath, "PNG"));

        auto account = MinecraftAccount::createOffline("Player");
        account->setSkinPath(fakeSkinPath);
        account->setModelType("alex");

        QCOMPARE(account->skinPath(), fakeSkinPath);
        QCOMPARE(account->modelType(), QStringLiteral("alex"));

        // Serialize to JSON
        QJsonObject json = account->saveToJson();
        QCOMPARE(json.value("type").toString(), QStringLiteral("Offline"));
        QCOMPARE(json.value("skinPath").toString(), fakeSkinPath);
        QCOMPARE(json.value("modelType").toString(), QStringLiteral("alex"));

        // Deserialize back
        auto loaded = MinecraftAccount::loadFromJsonV3(json);
        QVERIFY(loaded != nullptr);
        QCOMPARE(loaded->profileName(), QStringLiteral("Player"));
        QCOMPARE(loaded->skinPath(), fakeSkinPath);
        QCOMPARE(loaded->modelType(), QStringLiteral("alex"));
        QCOMPARE(loaded->profileId(), account->profileId());

        // Verify skin data and variant were loaded into profile
        QVERIFY(!loaded->accountData()->minecraftProfile.skin.data.isEmpty());
        QCOMPARE(loaded->accountData()->minecraftProfile.skin.variant, QStringLiteral("alex"));
    }

    void test_AccountListValidityWithOfflineAccount()
    {
        AccountList list;
        QVERIFY(!list.anyAccountIsValid());

        auto account = MinecraftAccount::createOffline("OfflineGamer");
        list.addAccount(account);

        // Offline account is now fully valid
        QVERIFY(list.anyAccountIsValid());
        QCOMPARE(list.count(), 1);
    }

    void test_LocalSkinServer_LifecycleAndJvmArgs()
    {
        LocalSkinServer server;
        QVERIFY(server.ensureStarted());
        QVERIFY(server.port() > 0);
        QVERIFY(server.url().startsWith("http://127.0.0.1:"));

        QStringList args = server.jvmArguments();
        QCOMPARE(args.size(), 1);
        QVERIFY(args[0].startsWith("-javaagent:"));
        QVERIFY(args[0].endsWith("=" + server.url()));
        QVERIFY(args[0].contains("authlib-injector"));

        for (const QString& arg : args) {
            QVERIFY(!arg.contains("-Dminecraft.api."));
            QVERIFY(!arg.contains("-Dhttp.proxy"));
            QVERIFY(!arg.contains("-Dhttps.proxy"));
            QVERIFY(!arg.contains("-Dcom.sun.net.ssl"));
            QVERIFY(!arg.contains("-Djsse.enableSNIExtension"));
        }

        // Verify RSA-2048 public key is generated and PEM-formatted
        QVERIFY(!server.publicKeyBase64().isEmpty());
        QVERIFY(server.publicKeyPem().contains("-----BEGIN PUBLIC KEY-----"));
        QVERIFY(server.publicKeyPem().contains("-----END PUBLIC KEY-----"));

        server.stop();
        QVERIFY(!server.isListening());
        QCOMPARE(server.jvmArguments().size(), 0);
    }

    void test_LocalSkinServer_HttpRequests()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        QString skinFile = tempDir.filePath("steve_skin.png");

        QImage testImg(64, 64, QImage::Format_ARGB32);
        testImg.fill(Qt::green);
        QVERIFY(testImg.save(skinFile, "PNG"));

        LocalSkinServer server;
        QVERIFY(server.ensureStarted());

        QString uuid = "5627dd98e6be3c21b8a8e92344183641";
        server.registerSkin(uuid, skinFile, "alex");

        // Helper lambda to make raw HTTP request over QTcpSocket with event processing
        auto doRequest = [&](const QString& req) -> QByteArray {
            QTcpSocket sock;
            sock.connectToHost(QHostAddress::LocalHost, server.port());
            for (int i = 0; i < 50 && sock.state() != QAbstractSocket::ConnectedState; ++i) {
                QCoreApplication::processEvents();
                sock.waitForConnected(20);
            }
            if (sock.state() != QAbstractSocket::ConnectedState) {
                return {};
            }

            sock.write(req.toUtf8());
            sock.flush();

            QByteArray response;
            for (int i = 0; i < 100; ++i) {
                QCoreApplication::processEvents();
                if (sock.waitForReadyRead(20)) {
                    response.append(sock.readAll());
                    if (response.contains("\r\n\r\n") && (response.startsWith("HTTP/1.1 204") || response.contains("}\r\n") || response.size() > 50)) {
                        break;
                    }
                }
                if (sock.state() == QAbstractSocket::UnconnectedState && sock.bytesAvailable() == 0) {
                    break;
                }
            }
            if (sock.bytesAvailable() > 0) {
                response.append(sock.readAll());
            }
            return response;
        };

        // 1. Root metadata request for authlib-injector: GET /
        QString metaReq = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        QByteArray metaResp = doRequest(metaReq);
        QVERIFY(metaResp.startsWith("HTTP/1.1 200 OK"));
        int metaBodyIdx = metaResp.indexOf("\r\n\r\n");
        QVERIFY(metaBodyIdx != -1);
        QJsonDocument metaDoc = QJsonDocument::fromJson(metaResp.mid(metaBodyIdx + 4));
        QJsonObject metaRoot = metaDoc.object();
        QCOMPARE(metaRoot.value("meta").toObject().value("implementationName").toString(), QStringLiteral("LocalSkinServer"));
        QJsonArray skinDomains = metaRoot.value("skinDomains").toArray();
        QVERIFY(skinDomains.contains(QStringLiteral("127.0.0.1")));
        QVERIFY(skinDomains.contains(QStringLiteral("localhost")));
        QVERIFY(metaRoot.value("signaturePublickey").toString().startsWith("-----BEGIN PUBLIC KEY-----"));

        // 2. Metadata request on /yggdrasil
        QString yggReq = "GET /yggdrasil HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        QByteArray yggResp = doRequest(yggReq);
        QVERIFY(yggResp.startsWith("HTTP/1.1 200 OK"));

        // 3. Request profile: GET /session/minecraft/profile/<uuid>
        QString profileReq = QString("GET /session/minecraft/profile/%1 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").arg(uuid);
        QByteArray profileResp = doRequest(profileReq);
        QVERIFY(profileResp.startsWith("HTTP/1.1 200 OK"));

        int bodyIdx = profileResp.indexOf("\r\n\r\n");
        QVERIFY(bodyIdx != -1);
        QByteArray profileJsonData = profileResp.mid(bodyIdx + 4);

        QJsonDocument doc = QJsonDocument::fromJson(profileJsonData);
        QVERIFY(doc.isObject());
        QJsonObject root = doc.object();
        QCOMPARE(root.value("id").toString(), uuid);

        QJsonArray props = root.value("properties").toArray();
        QCOMPARE(props.size(), 1);
        QJsonObject prop0 = props[0].toObject();
        QCOMPARE(prop0.value("name").toString(), QStringLiteral("textures"));

        // Must have non-empty RSA SHA1withRSA signature
        QString sig = prop0.value("signature").toString();
        QVERIFY(!sig.isEmpty());

        QByteArray b64 = prop0.value("value").toString().toLatin1();
        QByteArray texJson = QByteArray::fromBase64(b64);
        QJsonDocument texDoc = QJsonDocument::fromJson(texJson);
        QVERIFY(texDoc.isObject());
        QJsonObject texObj = texDoc.object().value("textures").toObject().value("SKIN").toObject();
        // Direct local URL allowed by authlib-injector skinDomains
        QCOMPARE(texObj.value("url").toString(), QString("%1/textures/%2").arg(server.url(), uuid));
        QCOMPARE(texObj.value("metadata").toObject().value("model").toString(), QStringLiteral("slim"));

        // 4. Request profile with /sessionserver prefix: GET /sessionserver/session/minecraft/profile/<uuid>
        QString ssProfileReq = QString("GET /sessionserver/session/minecraft/profile/%1 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").arg(uuid);
        QByteArray ssProfileResp = doRequest(ssProfileReq);
        QVERIFY(ssProfileResp.startsWith("HTTP/1.1 200 OK"));

        // 5. Request texture PNG from /textures/<uuid>
        QString texReq = QString("GET /textures/%1 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").arg(uuid);
        QByteArray texResp = doRequest(texReq);
        QVERIFY(texResp.startsWith("HTTP/1.1 200 OK"));
        QVERIFY(texResp.contains("Content-Type: image/png"));

        // 6. Request non-existent profile -> 204 No Content
        QString missingReq = "GET /session/minecraft/profile/00000000000000000000000000000000 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        QByteArray missingResp = doRequest(missingReq);
        QVERIFY(missingResp.startsWith("HTTP/1.1 204 No Content"));

        // 7. Integrated Server / LAN session join: POST /sessionserver/session/minecraft/join -> 204 No Content
        QString joinReq = "POST /sessionserver/session/minecraft/join HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 2\r\n\r\n{}";
        QByteArray joinResp = doRequest(joinReq);
        QVERIFY(joinResp.startsWith("HTTP/1.1 204 No Content"));

        // 8. Integrated Server / LAN session hasJoined: GET /sessionserver/session/minecraft/hasJoined
        QString hasJoinedReq = QString("GET /sessionserver/session/minecraft/hasJoined?username=%1&serverId=test1234 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").arg(uuid);
        QByteArray hasJoinedResp = doRequest(hasJoinedReq);
        QVERIFY(hasJoinedResp.startsWith("HTTP/1.1 200 OK"));
        int hjBodyIdx = hasJoinedResp.indexOf("\r\n\r\n");
        QVERIFY(hjBodyIdx != -1);
        QJsonDocument hjDoc = QJsonDocument::fromJson(hasJoinedResp.mid(hjBodyIdx + 4));
        QCOMPARE(hjDoc.object().value("id").toString(), uuid);
        QVERIFY(!hjDoc.object().value("properties").toArray()[0].toObject().value("signature").toString().isEmpty());

        server.stop();
    }

    void test_MultipleOfflineAccountsSwitching()
    {
        AccountList list;

        auto acc1 = MinecraftAccount::createOffline("Alice");
        acc1->setModelType("alex");
        acc1->setSkinPath("/path/to/alice.png");

        auto acc2 = MinecraftAccount::createOffline("Bob");
        acc2->setModelType("steve");
        acc2->setSkinPath("/path/to/bob.png");

        auto acc3 = MinecraftAccount::createOffline("Charlie");
        acc3->setModelType("alex");

        list.addAccount(acc1);
        list.addAccount(acc2);
        list.addAccount(acc3);

        QCOMPARE(list.count(), 3);

        // All 3 have unique deterministic UUIDs
        QVERIFY(acc1->profileId() != acc2->profileId());
        QVERIFY(acc2->profileId() != acc3->profileId());
        QVERIFY(acc1->profileId() != acc3->profileId());

        // Test default account switching
        list.setDefaultAccount(acc1);
        QCOMPARE(list.defaultAccount(), acc1);

        list.setDefaultAccount(acc2);
        QCOMPARE(list.defaultAccount(), acc2);

        list.setDefaultAccount(acc3);
        QCOMPARE(list.defaultAccount(), acc3);

        // fillSession verifies correct values
        auto session = std::make_shared<AuthSession>();
        acc2->fillSession(session);
        QCOMPARE(session->player_name, QStringLiteral("Bob"));
        QCOMPARE(session->uuid, acc2->profileId());
        QCOMPARE(session->access_token, QStringLiteral("0"));
        QCOMPARE(session->session, QStringLiteral("0"));
        QCOMPARE(session->modelType, QStringLiteral("steve"));
        QCOMPARE(session->skinPath, QStringLiteral("/path/to/bob.png"));
    }

    void test_OfflineClasspathAndMainClass()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        QString patchDir = tempDir.filePath("patches");
        QVERIFY(QDir().mkpath(patchDir));
        QString libDir = tempDir.filePath("libraries");
        QVERIFY(QDir().mkpath(libDir));

        // Create a dummy Minecraft patch
        QString mcPatchPath = FS::PathCombine(patchDir, "net.minecraft.json");
        QJsonObject mcObj;
        mcObj["formatVersion"] = 1;
        mcObj["uid"] = "net.minecraft";
        mcObj["version"] = "1.20.1";
        mcObj["mainClass"] = "net.minecraft.client.main.Main";
        QJsonArray mcLibs;
        QJsonObject mcLib;
        mcLib["name"] = "org.lwjgl:lwjgl:3.3.1";
        mcLibs.append(mcLib);
        mcObj["libraries"] = mcLibs;

        QFile mcFile(mcPatchPath);
        QVERIFY(mcFile.open(QFile::WriteOnly));
        mcFile.write(QJsonDocument(mcObj).toJson());
        mcFile.close();

        // Create a dummy Fabric patch (net.fabricmc.json)
        QString fabricPatchPath = FS::PathCombine(patchDir, "net.fabricmc.json");
        QJsonObject fabricObj;
        fabricObj["formatVersion"] = 1;
        fabricObj["uid"] = "net.fabricmc.fabric-loader";
        fabricObj["version"] = "0.15.11";
        fabricObj["mainClass"] = "net.fabricmc.loader.impl.launch.knot.KnotClient";
        QJsonArray fabricLibs;
        QJsonObject fabricLib;
        fabricLib["name"] = "net.fabricmc:fabric-loader:0.15.11";
        fabricLibs.append(fabricLib);
        fabricObj["libraries"] = fabricLibs;

        QFile fabricFile(fabricPatchPath);
        QVERIFY(fabricFile.open(QFile::WriteOnly));
        fabricFile.write(QJsonDocument(fabricObj).toJson());
        fabricFile.close();

        // Parse both patches using ProfileUtils
        auto mcVersionFile = ProfileUtils::parseJsonFile(QFileInfo(mcPatchPath), false);
        QVERIFY(mcVersionFile != nullptr);
        QCOMPARE(mcVersionFile->uid, QStringLiteral("net.minecraft"));
        QCOMPARE(mcVersionFile->mainClass, QStringLiteral("net.minecraft.client.main.Main"));
        QCOMPARE(mcVersionFile->libraries.size(), 1);

        auto fabricVersionFile = ProfileUtils::parseJsonFile(QFileInfo(fabricPatchPath), false);
        QVERIFY(fabricVersionFile != nullptr);
        QCOMPARE(fabricVersionFile->uid, QStringLiteral("net.fabricmc.fabric-loader"));
        QCOMPARE(fabricVersionFile->mainClass, QStringLiteral("net.fabricmc.loader.impl.launch.knot.KnotClient"));
        QCOMPARE(fabricVersionFile->libraries.size(), 1);

        // Apply them in order: Minecraft first, Fabric second
        LaunchProfile launchProfile;
        RuntimeContext rc;
        mcVersionFile->applyTo(&launchProfile, rc);
        QCOMPARE(launchProfile.getMainClass(), QStringLiteral("net.minecraft.client.main.Main"));

        fabricVersionFile->applyTo(&launchProfile, rc);
        // Fabric correctly overrides mainClass to KnotClient!
        QCOMPARE(launchProfile.getMainClass(), QStringLiteral("net.fabricmc.loader.impl.launch.knot.KnotClient"));

        // Both libraries must be collected in launchProfile
        QCOMPARE(launchProfile.getLibraries().size(), 2);
    }

    void test_OfflineArgumentsAndJava25AndNatives()
    {
        // 1. Verify AuthSession::MakeOffline sets non-empty fields and access_token == "0"
        AuthSession session;
        session.MakeOffline("joke");
        QCOMPARE(session.player_name, QStringLiteral("joke"));
        QCOMPARE(session.access_token, QStringLiteral("0"));
        QCOMPARE(session.session, QStringLiteral("0"));
        QCOMPARE(session.uuid.length(), 32);
        QVERIFY(!session.uuid.contains('-'));
        QCOMPARE(session.user_type, QStringLiteral("offline"));

        // 2. Verify MinecraftAccount::createOffline and fillSession
        auto acct = MinecraftAccount::createOffline("joke");
        auto acctSession = std::make_shared<AuthSession>();
        acct->fillSession(acctSession);
        QCOMPARE(acctSession->player_name, QStringLiteral("joke"));
        QCOMPARE(acctSession->access_token, QStringLiteral("0"));
        QCOMPARE(acctSession->session, QStringLiteral("0"));
        QCOMPARE(acctSession->uuid, session.uuid);

        // 3. Verify Java 25 detection for --enable-native-access
        JavaVersion jv25("25.0.0");
        QVERIFY(jv25.major() >= 25);
    }
};

QTEST_GUILESS_MAIN(OfflineAccountTest)
#include "OfflineAccount_test.moc"

