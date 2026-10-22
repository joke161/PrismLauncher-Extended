// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#pragma once

#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QByteArray>

class QNetworkAccessManager;

struct evp_pkey_st;
typedef struct evp_pkey_st EVP_PKEY;

class LocalSkinServer : public QTcpServer {
    Q_OBJECT
public:
    explicit LocalSkinServer(QObject* parent = nullptr);
    ~LocalSkinServer() override;

    static LocalSkinServer* instance();

    bool ensureStarted();
    void stop();

    quint16 port() const;
    QString url() const;

    QStringList jvmArguments() const;

    // Registers / overrides custom skin texture for a player (used for offline profiles and P2P peers)
    void registerSkin(const QString& identifier, const QString& skinPath, const QString& modelType = "steve");
    void unregisterSkin(const QString& identifier);

    struct RegisteredSkin {
        QString skinPath;
        QString modelType; // "steve" or "alex"
    };

    const QHash<QString, RegisteredSkin>& registeredSkins() const { return m_registeredSkins; }

    const QByteArray& publicKeyBase64() const { return m_publicKeyBase64; }
    QString publicKeyPem() const;
    QByteArray signData(const QByteArray& data) const;
    QByteArray buildSignedProfileResponse(const QString& lookupKey);

    void handleHttpRequest(QTcpSocket* socket, const QString& requestLine, const QByteArray& allData);
    void sendHttpResponse(QTcpSocket* socket, int statusCode, const QString& statusText,
                          const QByteArray& contentType, const QByteArray& body, bool closeConnection = false);

protected slots:
    void onNewConnection();

private:
    void initKeys();

    QHash<QString, RegisteredSkin> m_registeredSkins;
    EVP_PKEY* m_pkey = nullptr;
    QByteArray m_publicKeyBase64;
    QNetworkAccessManager* m_networkManager = nullptr;
    static LocalSkinServer* s_instance;
};
