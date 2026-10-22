// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#include "P2PProtocol.h"

#include <cstring>
#include <openssl/evp.h>

namespace P2P {

QByteArray Protocol::cipher(const QByteArray& data, const QByteArray& key, quint16 connId, quint32 seq)
{
    if (data.isEmpty() || key.isEmpty()) {
        return data;
    }

    // 16-byte key (AES-128)
    unsigned char aesKey[16] = {0};
    int keyCopyLen = qMin(static_cast<int>(sizeof(aesKey)), key.size());
    memcpy(aesKey, key.constData(), keyCopyLen);

    // 16-byte IV derived deterministically from connId, seq and salt
    unsigned char iv[16] = {0};
    iv[0] = static_cast<unsigned char>((connId >> 8) & 0xFF);
    iv[1] = static_cast<unsigned char>(connId & 0xFF);
    iv[2] = static_cast<unsigned char>((seq >> 24) & 0xFF);
    iv[3] = static_cast<unsigned char>((seq >> 16) & 0xFF);
    iv[4] = static_cast<unsigned char>((seq >> 8) & 0xFF);
    iv[5] = static_cast<unsigned char>(seq & 0xFF);
    const char* salt = "PrismP2Pv1";
    memcpy(iv + 6, salt, 10);

    QByteArray result;
    result.resize(data.size());

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        // Fallback symmetric rolling XOR cipher
        QByteArray fallback = data;
        for (int i = 0; i < fallback.size(); ++i) {
            fallback[i] = fallback[i] ^ aesKey[i % 16] ^ iv[i % 16];
        }
        return fallback;
    }

    int outLen = 0;
    if (EVP_CipherInit_ex(ctx, EVP_aes_128_ctr(), nullptr, aesKey, iv, 1) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        QByteArray fallback = data;
        for (int i = 0; i < fallback.size(); ++i) {
            fallback[i] = fallback[i] ^ aesKey[i % 16] ^ iv[i % 16];
        }
        return fallback;
    }

    if (EVP_CipherUpdate(ctx, reinterpret_cast<unsigned char*>(result.data()), &outLen,
                         reinterpret_cast<const unsigned char*>(data.constData()), data.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return data;
    }

    int finalLen = 0;
    EVP_CipherFinal_ex(ctx, reinterpret_cast<unsigned char*>(result.data()) + outLen, &finalLen);
    EVP_CIPHER_CTX_free(ctx);

    return result;
}

QByteArray Protocol::serialize(PacketType type, quint16 connId, quint32 seq,
                               const QByteArray& payload, const QByteArray& key)
{
    PacketHeader header;
    header.magic[0] = PROTOCOL_MAGIC_0;
    header.magic[1] = PROTOCOL_MAGIC_1;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<quint8>(type);
    header.connId = qToBigEndian(connId);
    header.seq = qToBigEndian(seq);
    header.payloadLen = qToBigEndian(static_cast<quint16>(payload.size()));

    QByteArray encryptedPayload = payload.isEmpty() ? QByteArray() : cipher(payload, key, connId, seq);

    QByteArray packet;
    packet.resize(HEADER_SIZE + encryptedPayload.size());
    memcpy(packet.data(), &header, HEADER_SIZE);
    if (!encryptedPayload.isEmpty()) {
        memcpy(packet.data() + HEADER_SIZE, encryptedPayload.constData(), encryptedPayload.size());
    }

    return packet;
}

bool Protocol::parse(const QByteArray& rawData, Packet& outPacket, const QByteArray& key)
{
    if (rawData.size() < HEADER_SIZE) {
        return false;
    }

    const auto* header = reinterpret_cast<const PacketHeader*>(rawData.constData());
    if (header->magic[0] != PROTOCOL_MAGIC_0 || header->magic[1] != PROTOCOL_MAGIC_1) {
        return false;
    }
    if (header->version != PROTOCOL_VERSION) {
        return false;
    }

    quint16 connId = qFromBigEndian(header->connId);
    quint32 seq = qFromBigEndian(header->seq);
    quint16 payloadLen = qFromBigEndian(header->payloadLen);

    if (rawData.size() != HEADER_SIZE + payloadLen) {
        return false;
    }

    outPacket.type = static_cast<PacketType>(header->type);
    outPacket.connId = connId;
    outPacket.seq = seq;

    if (payloadLen > 0) {
        QByteArray encryptedPayload = rawData.mid(HEADER_SIZE, payloadLen);
        outPacket.payload = cipher(encryptedPayload, key, connId, seq);
    } else {
        outPacket.payload.clear();
    }

    return true;
}

} // namespace P2P

