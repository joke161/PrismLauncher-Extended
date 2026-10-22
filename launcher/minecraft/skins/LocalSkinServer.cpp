// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#include "LocalSkinServer.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>
#include <QNetworkAccessManager>
#include <QNetworkReply>

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "Application.h"
#include "FileSystem.h"
#include "minecraft/auth/AccountList.h"
#include "minecraft/auth/MinecraftAccount.h"

LocalSkinServer* LocalSkinServer::s_instance = nullptr;

LocalSkinServer::LocalSkinServer(QObject* parent)
    : QTcpServer(parent)
{
    initKeys();
    connect(this, &QTcpServer::newConnection, this, &LocalSkinServer::onNewConnection);
}

LocalSkinServer::~LocalSkinServer()
{
    stop();
    if (m_pkey) {
        EVP_PKEY_free(m_pkey);
        m_pkey = nullptr;
    }
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

LocalSkinServer* LocalSkinServer::instance()
{
    if (!s_instance) {
        s_instance = new LocalSkinServer(qApp);
    }
    return s_instance;
}

bool LocalSkinServer::ensureStarted()
{
    if (isListening()) {
        return true;
    }

    if (!listen(QHostAddress::LocalHost, 0)) {
        qWarning() << "LocalSkinServer: Failed to bind to local port:" << errorString();
        return false;
    }

    qDebug() << "LocalSkinServer: Started listening on" << url();
    return true;
}

void LocalSkinServer::stop()
{
    if (isListening()) {
        close();
        qDebug() << "LocalSkinServer: Stopped";
    }
}

quint16 LocalSkinServer::port() const
{
    return isListening() ? serverPort() : 0;
}

QString LocalSkinServer::url() const
{
    return QString("http://127.0.0.1:%1").arg(port());
}

QStringList LocalSkinServer::jvmArguments() const
{
    if (!isListening()) {
        return {};
    }
    QString agentJar;
    auto* app = dynamic_cast<Application*>(qApp);
    if (app) {
        agentJar = app->getJarPath("authlib-injector.jar");
    }
    if (agentJar.isEmpty() || !QFile::exists(agentJar)) {
        QStringList candidates = {
            FS::PathCombine(QCoreApplication::applicationDirPath(), "jars/authlib-injector.jar"),
            FS::PathCombine(QCoreApplication::applicationDirPath(), "../jars/authlib-injector.jar"),
            FS::PathCombine(QDir::currentPath(), "build/jars/authlib-injector.jar"),
            FS::PathCombine(QDir::currentPath(), "jars/authlib-injector.jar"),
            FS::PathCombine(QDir::currentPath(), "libraries/authlib-injector/authlib-injector.jar"),
            "/tmp/authlib-injector.jar"
        };
        for (const auto& c : candidates) {
            if (QFile::exists(c)) {
                agentJar = c;
                break;
            }
        }
    }
    if (agentJar.isEmpty()) {
        agentJar = "authlib-injector.jar";
    }
    return {
        QString("-javaagent:%1=%2").arg(agentJar, url())
    };
}

void LocalSkinServer::initKeys()
{
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (ctx) {
        if (EVP_PKEY_keygen_init(ctx) > 0 &&
            EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) > 0) {
            EVP_PKEY_keygen(ctx, &m_pkey);
        }
        EVP_PKEY_CTX_free(ctx);
    }

    if (m_pkey) {
        unsigned char* derBuf = nullptr;
        int derLen = i2d_PUBKEY(m_pkey, &derBuf);
        if (derLen > 0 && derBuf) {
            QByteArray derBytes(reinterpret_cast<const char*>(derBuf), derLen);
            m_publicKeyBase64 = derBytes.toBase64();
            OPENSSL_free(derBuf);
        }
        qDebug() << "LocalSkinServer: Generated RSA-2048 keypair, public key DER base64 len:"
                 << m_publicKeyBase64.length();
    } else {
        qWarning() << "LocalSkinServer: Failed to generate RSA-2048 keypair!";
    }
}

QString LocalSkinServer::publicKeyPem() const
{
    return QString("-----BEGIN PUBLIC KEY-----\n%1\n-----END PUBLIC KEY-----")
        .arg(QString::fromLatin1(m_publicKeyBase64));
}

QByteArray LocalSkinServer::signData(const QByteArray& data) const
{
    if (!m_pkey) {
        return {};
    }

    EVP_MD_CTX* mdCtx = EVP_MD_CTX_new();
    if (!mdCtx) {
        return {};
    }

    QByteArray signature;
    if (EVP_DigestSignInit(mdCtx, nullptr, EVP_sha1(), nullptr, m_pkey) > 0) {
        if (EVP_DigestSignUpdate(mdCtx, data.constData(), data.size()) > 0) {
            size_t sigLen = 0;
            if (EVP_DigestSignFinal(mdCtx, nullptr, &sigLen) > 0 && sigLen > 0) {
                signature.resize(static_cast<int>(sigLen));
                if (EVP_DigestSignFinal(mdCtx, reinterpret_cast<unsigned char*>(signature.data()), &sigLen) > 0) {
                    signature.resize(static_cast<int>(sigLen));
                } else {
                    signature.clear();
                }
            }
        }
    }
    EVP_MD_CTX_free(mdCtx);
    return signature.toBase64();
}

void LocalSkinServer::registerSkin(const QString& identifier, const QString& skinPath, const QString& modelType)
{
    RegisteredSkin reg;
    reg.skinPath = skinPath;
    reg.modelType = (modelType == "alex") ? "alex" : "steve";

    QString cleanId = identifier;
    cleanId.remove('-');
    m_registeredSkins.insert(cleanId.toLower(), reg);
    m_registeredSkins.insert(identifier.toLower(), reg);
}

void LocalSkinServer::unregisterSkin(const QString& identifier)
{
    QString cleanId = identifier;
    cleanId.remove('-');
    m_registeredSkins.remove(cleanId.toLower());
    m_registeredSkins.remove(identifier.toLower());
}

QByteArray LocalSkinServer::buildSignedProfileResponse(const QString& lookupKey)
{
    QString cleanId = lookupKey;
    cleanId.remove('-');
    QString lowerCleanId = cleanId.toLower();
    QString lowerLookup = lookupKey.toLower();

    QString foundPlayerName = lookupKey;
    QString skinPath;
    QString modelType = "steve";
    QString profileUuid = cleanId;

    // 1. Check registered skins
    if (m_registeredSkins.contains(lowerCleanId)) {
        auto reg = m_registeredSkins.value(lowerCleanId);
        skinPath = reg.skinPath;
        modelType = reg.modelType;
    } else if (m_registeredSkins.contains(lowerLookup)) {
        auto reg = m_registeredSkins.value(lowerLookup);
        skinPath = reg.skinPath;
        modelType = reg.modelType;
    }

    // 2. Check accounts list
    auto* app = dynamic_cast<Application*>(qApp);
    if (app && app->accounts()) {
        auto* accList = app->accounts();
        for (int i = 0; i < accList->count(); ++i) {
            auto acc = accList->at(i);
            if (!acc) continue;
            QString accClean = acc->profileId();
            accClean.remove('-');
            if (accClean.compare(lowerCleanId, Qt::CaseInsensitive) == 0 ||
                acc->profileName().compare(lookupKey, Qt::CaseInsensitive) == 0) {
                foundPlayerName = acc->profileName();
                profileUuid = accClean;
                if (skinPath.isEmpty()) {
                    skinPath = acc->skinPath();
                    modelType = acc->modelType();
                }
                break;
            }
        }
    }

    // 3. Fallback: only if lookupKey was empty (e.g. generic hasJoined request without username parameter)
    if (lookupKey.isEmpty() && skinPath.isEmpty() && !m_registeredSkins.isEmpty()) {
        auto it = m_registeredSkins.constBegin();
        skinPath = it.value().skinPath;
        modelType = it.value().modelType;
        if (profileUuid.isEmpty()) {
            profileUuid = it.key();
        }
    }

    if (skinPath.isEmpty() || !QFile::exists(skinPath)) {
        return {};
    }

    // Build base64 textures payload
    QJsonObject texturesObj;
    texturesObj["timestamp"] = QDateTime::currentMSecsSinceEpoch();
    texturesObj["profileId"] = profileUuid;
    texturesObj["profileName"] = foundPlayerName;

    QJsonObject skinObj;
    // Direct local URL allowed by authlib-injector skinDomains
    skinObj["url"] = QString("%1/textures/%2").arg(url(), profileUuid);
    if (modelType == "alex") {
        QJsonObject meta;
        meta["model"] = "slim";
        skinObj["metadata"] = meta;
    }

    QJsonObject texturesMap;
    texturesMap["SKIN"] = skinObj;
    texturesObj["textures"] = texturesMap;

    QByteArray texturesJson = QJsonDocument(texturesObj).toJson(QJsonDocument::Compact);
    QByteArray texturesBase64 = texturesJson.toBase64();
    QByteArray signatureBase64 = signData(texturesBase64);

    // Build profile response JSON
    QJsonObject profileObj;
    profileObj["id"] = profileUuid;
    profileObj["name"] = foundPlayerName;

    QJsonObject propObj;
    propObj["name"] = "textures";
    propObj["value"] = QString::fromLatin1(texturesBase64);
    if (!signatureBase64.isEmpty()) {
        propObj["signature"] = QString::fromLatin1(signatureBase64);
    }

    QJsonArray propsArray;
    propsArray.append(propObj);
    profileObj["properties"] = propsArray;

    return QJsonDocument(profileObj).toJson(QJsonDocument::Compact);
}

class HttpConnection : public QObject {
    Q_OBJECT
public:
    HttpConnection(QTcpSocket* socket, LocalSkinServer* server)
        : QObject(server)
        , m_socket(socket)
        , m_server(server)
    {
        connect(m_socket, &QTcpSocket::readyRead, this, &HttpConnection::onReadyRead);
        connect(m_socket, &QTcpSocket::disconnected, this, &QObject::deleteLater);
    }

private slots:
    void onReadyRead()
    {
        m_buffer.append(m_socket->readAll());

        while (!m_buffer.isEmpty()) {
            int headerEnd = m_buffer.indexOf("\r\n\r\n");
            int delimLen = 4;
            if (headerEnd == -1) {
                headerEnd = m_buffer.indexOf("\n\n");
                delimLen = 2;
            }
            if (headerEnd == -1) {
                break;
            }

            int lineEnd = m_buffer.indexOf("\r\n");
            if (lineEnd == -1) {
                lineEnd = m_buffer.indexOf('\n');
            }
            if (lineEnd == -1 || lineEnd > headerEnd) {
                break;
            }

            QString requestLine = QString::fromUtf8(m_buffer.left(lineEnd)).trimmed();
            QByteArray requestData = m_buffer.left(headerEnd + delimLen);
            m_buffer.remove(0, headerEnd + delimLen);

            m_server->handleHttpRequest(m_socket, requestLine, requestData);
        }
    }

private:
    QTcpSocket* m_socket = nullptr;
    LocalSkinServer* m_server = nullptr;
    QByteArray m_buffer;
};

void LocalSkinServer::onNewConnection()
{
    while (hasPendingConnections()) {
        QTcpSocket* socket = nextPendingConnection();
        if (!socket) {
            continue;
        }
        new HttpConnection(socket, this);
    }
}

void LocalSkinServer::handleHttpRequest(QTcpSocket* socket, const QString& requestLine, const QByteArray& allData)
{
    bool closeConnection = allData.contains("Connection: close") || allData.contains("connection: close");
    QStringList parts = requestLine.split(' ');
    if (parts.size() < 2) {
        sendHttpResponse(socket, 400, "Bad Request", "text/plain", "Bad Request", closeConnection);
        return;
    }

    QString method = parts[0].toUpper();
    QString rawPath = parts[1];
    QUrl requestUrl(rawPath);
    QString path = requestUrl.path();

    // 1. Root / Metadata endpoint for authlib-injector: GET / (and GET /yggdrasil)
    if (path.isEmpty() || path == "/" || path == "/yggdrasil" || path.endsWith("/yggdrasil")) {
        QJsonObject root;
        QJsonObject meta;
        meta["implementationName"] = "LocalSkinServer";
        meta["implementationVersion"] = "1.0";
        root["meta"] = meta;

        QJsonArray skinDomains;
        skinDomains.append("127.0.0.1");
        skinDomains.append("localhost");
        root["skinDomains"] = skinDomains;

        root["signaturePublickey"] = publicKeyPem();

        QByteArray resp = QJsonDocument(root).toJson(QJsonDocument::Compact);
        sendHttpResponse(socket, 200, "OK", "application/json; charset=utf-8", resp, closeConnection);
        return;
    }

    // Strip optional /sessionserver prefix if requested by client or authlib-injector
    QString normPath = path;
    if (normPath.startsWith("/sessionserver")) {
        normPath = normPath.mid(14); // length of "/sessionserver"
    }

    // 2. Session join endpoint for Integrated Server / LAN: POST /session/minecraft/join (or /sessionserver/session/minecraft/join)
    if (normPath == "/session/minecraft/join" || normPath.startsWith("/session/minecraft/join")) {
        sendHttpResponse(socket, 204, "No Content", "application/json", QByteArray(), closeConnection);
        return;
    }

    // 3. Session hasJoined endpoint for Integrated Server / LAN: GET /session/minecraft/hasJoined (or /sessionserver/...)
    if (normPath == "/session/minecraft/hasJoined" || normPath.startsWith("/session/minecraft/hasJoined")) {
        QUrlQuery query(requestUrl);
        QString username = query.queryItemValue("username");
        if (username.isEmpty() && rawPath.contains("username=")) {
            username = rawPath.section("username=", 1, 1).section('&', 0, 0);
        }
        QByteArray body = buildSignedProfileResponse(username);
        if (body.isEmpty()) {
            sendHttpResponse(socket, 204, "No Content", "application/json", QByteArray(), closeConnection);
            return;
        }
        sendHttpResponse(socket, 200, "OK", "application/json; charset=utf-8", body, closeConnection);
        return;
    }

    // 4. Session profile endpoint: GET /session/minecraft/profile/<uuid> (or /sessionserver/session/minecraft/profile/<uuid>)
    static const QString profilePrefix = "/session/minecraft/profile/";
    if (normPath.startsWith(profilePrefix)) {
        QString uuidStr = normPath.mid(profilePrefix.length());
        if (uuidStr.contains('/')) {
            uuidStr = uuidStr.section('/', 0, 0);
        }
        QByteArray body = buildSignedProfileResponse(uuidStr);
        if (!body.isEmpty()) {
            sendHttpResponse(socket, 200, "OK", "application/json; charset=utf-8", body, closeConnection);
            return;
        }

        // Unknown UUID: transparent proxy to official Mojang sessionserver for licensed accounts
        if (!m_networkManager) {
            m_networkManager = new QNetworkAccessManager(this);
        }
        QString cleanId = uuidStr;
        cleanId.remove('-');
        QUrl mojangUrl(QStringLiteral("https://sessionserver.mojang.com/session/minecraft/profile/%1?unsigned=false").arg(cleanId));
        QNetworkRequest req(mojangUrl);
        QNetworkReply* reply = m_networkManager->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, socket, reply, closeConnection]() {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError) {
                QByteArray mojangBody = reply->readAll();
                sendHttpResponse(socket, 200, "OK", "application/json; charset=utf-8", mojangBody, closeConnection);
            } else {
                sendHttpResponse(socket, 204, "No Content", "application/json", QByteArray(), closeConnection);
            }
        });
        return;
    }

    // 5. Publickeys endpoint: /publickeys (returns Base64 DER SubjectPublicKeyInfo)
    if (normPath.contains("/publickeys")) {
        QJsonObject root;
        QJsonArray keys;
        QJsonObject keyObj;
        keyObj["publicKey"] = QString::fromLatin1(m_publicKeyBase64);
        keys.append(keyObj);
        root["profilePropertyKeys"] = keys;
        QByteArray resp = QJsonDocument(root).toJson(QJsonDocument::Compact);
        sendHttpResponse(socket, 200, "OK", "application/json; charset=utf-8", resp, closeConnection);
        return;
    }

    // 6. Textures endpoint: /textures/<uuid> or /texture/<hash> (direct or proxy HTTP)
    static const QString texturesPrefix = "/textures/";
    static const QString texturePrefix = "/texture/";
    if (normPath.startsWith(texturesPrefix) || normPath.startsWith(texturePrefix)) {
        QString idStr = normPath.startsWith(texturesPrefix) ? normPath.mid(texturesPrefix.length()) : normPath.mid(texturePrefix.length());
        if (idStr.contains('/')) {
            idStr = idStr.section('/', 0, 0);
        }
        QString cleanId = idStr;
        cleanId = cleanId.remove('-').toLower();

        QString skinPath;
        if (m_registeredSkins.contains(cleanId)) {
            skinPath = m_registeredSkins.value(cleanId).skinPath;
        } else if (m_registeredSkins.contains(idStr.toLower())) {
            skinPath = m_registeredSkins.value(idStr.toLower()).skinPath;
        }

        auto* app = dynamic_cast<Application*>(qApp);
        if (skinPath.isEmpty() && app && app->accounts()) {
            auto* accList = app->accounts();
            for (int i = 0; i < accList->count(); ++i) {
                auto acc = accList->at(i);
                if (!acc) continue;
                QString accCleanUuid = acc->profileId();
                accCleanUuid = accCleanUuid.remove('-').toLower();

                if (accCleanUuid == cleanId || acc->profileName().compare(idStr, Qt::CaseInsensitive) == 0) {
                    skinPath = acc->skinPath();
                    break;
                }
            }
        }

        // Fallback: only if idStr is empty and we have registered skins
        if (idStr.isEmpty() && skinPath.isEmpty() && !m_registeredSkins.isEmpty()) {
            skinPath = m_registeredSkins.constBegin().value().skinPath;
        }

        if (!skinPath.isEmpty() && QFile::exists(skinPath)) {
            QFile f(skinPath);
            if (f.open(QIODevice::ReadOnly)) {
                QByteArray pngData = f.readAll();
                sendHttpResponse(socket, 200, "OK", "image/png", pngData, closeConnection);
                return;
            }
        }

        sendHttpResponse(socket, 404, "Not Found", "text/plain", "Skin texture not found", closeConnection);
        return;
    }

    // 7. Stub common auth endpoints gracefully
    if (normPath.contains("/join") || normPath.contains("/hasJoined") || normPath.contains("/validate") ||
        normPath.contains("/authenticate") || normPath.contains("/refresh")) {
        sendHttpResponse(socket, 200, "OK", "application/json", "{}", closeConnection);
        return;
    }

    sendHttpResponse(socket, 404, "Not Found", "text/plain", "Not Found", closeConnection);
}

void LocalSkinServer::sendHttpResponse(QTcpSocket* socket, int statusCode, const QString& statusText,
                                       const QByteArray& contentType, const QByteArray& body, bool closeConnection)
{
    QByteArray response;
    response.append(QString("HTTP/1.1 %1 %2\r\n").arg(statusCode).arg(statusText).toUtf8());
    response.append(QString("Content-Type: %1\r\n").arg(QString::fromLatin1(contentType)).toUtf8());
    response.append(QString("Content-Length: %1\r\n").arg(body.size()).toUtf8());
    if (closeConnection) {
        response.append("Connection: close\r\n");
    } else {
        response.append("Connection: keep-alive\r\n");
    }
    response.append("Access-Control-Allow-Origin: *\r\n");
    response.append("\r\n");
    if (!body.isEmpty()) {
        response.append(body);
    }

    socket->write(response);
    socket->flush();
    if (closeConnection) {
        socket->disconnectFromHost();
    }
}

#include "LocalSkinServer.moc"
