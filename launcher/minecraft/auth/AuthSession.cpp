#include "AuthSession.h"
#include "MinecraftAccount.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QUuid>

QString AuthSession::serializeUserProperties()
{
    QJsonObject userAttrs;
    /*
    for (auto key : u.properties.keys())
    {
        auto array = QJsonArray::fromStringList(u.properties.values(key));
        userAttrs.insert(key, array);
    }
    */
    QJsonDocument value(userAttrs);
    return value.toJson(QJsonDocument::Compact);
}

bool AuthSession::MakeOffline(QString offline_playername)
{
    player_name = offline_playername.trimmed().isEmpty() ? "Player" : offline_playername.trimmed();
    uuid = MinecraftAccount::uuidFromUsername(player_name).toString(QUuid::Id128);
    session = "0";
    access_token = "0";
    user_type = "offline";
    launchMode = LaunchMode::Offline;
    return true;
}

void AuthSession::MakeDemo(QString name, QString u)
{
    uuid = u;
    session = "-";
    access_token = "0";
    player_name = name;
    launchMode = LaunchMode::Demo;
};
