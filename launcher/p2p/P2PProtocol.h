// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 */

#pragma once

#include <QtGlobal>
#include <QByteArray>
#include <QString>
#include <QHostAddress>
#include <QtEndian>

namespace P2P {

/**
 * Packet types for the P2P UDP tunnel.
 */
enum class PacketType : quint8 {
    Punch          = 0x01, ///< UDP hole punching probe
    PunchAck       = 0x02, ///< Acknowledgment of hole punching probe
    KeepAlive      = 0x03, ///< Periodic keep-alive ping
    KeepAliveAck   = 0x04, ///< Keep-alive response (for latency estimation)
    TcpConnect     = 0x10, ///< Minecraft TCP client connected to local proxy
    TcpConnectAck  = 0x11, ///< Minecraft TCP connection accepted
    TcpData        = 0x12, ///< Minecraft stream TCP payload
    TcpDisconnect  = 0x13, ///< Minecraft connection closed / terminated
    TcpDataAck     = 0x14, ///< Acknowledgment for reliable TcpData packet
    PeerIdentityAndSkin = 0x20 ///< Skin and player profile metadata synchronization
};

#pragma pack(push, 1)
/**
 * Binary header layout for tunnel frames over UDP (12 bytes).
 */
struct PacketHeader {
    quint8 magic[2];      ///< Magic bytes: 0x50, 0x32 ('P', '2')
    quint8 version;       ///< Protocol version: 1
    quint8 type;          ///< PacketType
    quint16 connId;       ///< Multiplexed connection ID (Network / Big Endian)
    quint32 seq;          ///< Sequence number (Network / Big Endian)
    quint16 payloadLen;   ///< Payload length (Network / Big Endian)
};
#pragma pack(pop)

constexpr int HEADER_SIZE = sizeof(PacketHeader); // Exactly 12 bytes
constexpr quint8 PROTOCOL_MAGIC_0 = 0x50; // 'P'
constexpr quint8 PROTOCOL_MAGIC_1 = 0x32; // '2'
constexpr quint8 PROTOCOL_VERSION = 1;
constexpr int MAX_PAYLOAD_SIZE = 1350;     // Safe MTU chunk limit for UDP

/**
 * Decoded P2P packet structure.
 */
struct Packet {
    PacketType type = PacketType::Punch;
    quint16 connId = 0;
    quint32 seq = 0;
    QByteArray payload;
};

/**
 * Packet serialization, parsing, and stream encryption helpers.
 */
class Protocol {
public:
    /**
     * Serializes a packet into a datagram with an encrypted payload.
     */
    static QByteArray serialize(PacketType type, quint16 connId, quint32 seq,
                                const QByteArray& payload, const QByteArray& key = {});

    /**
     * Parses and decrypts an incoming datagram. Returns true if valid.
     */
    static bool parse(const QByteArray& rawData, Packet& outPacket, const QByteArray& key = {});

    /**
     * Symmetric stream cipher (AES-128-CTR with rolling XOR fallback).
     * Since CTR is an involution (symmetric stream cipher), encrypting twice recovers plaintext.
     */
    static QByteArray cipher(const QByteArray& data, const QByteArray& key, quint16 connId, quint32 seq);
};

} // namespace P2P

