// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <QtCore/QObject>
#include <QtCore/QString>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class HTTPDownloader;
class QTimer;

// Rollback fork: client for the Mawarunco lobby (tools/rollback/lobby/api.php), a list of open netplay
// rooms. The lobby only lists rooms and hands the host's address to one joiner; the match is the usual
// direct UDP connection. HTTPS only, certificates are checked. Everything the server sends is treated as
// untrusted: each field is checked against a fixed format before it is used, and names are only ever
// shown as plain text.
class LobbyClient final : public QObject
{
	Q_OBJECT

public:
	struct Room
	{
		QString id;
		QString host;
		QString country; // ISO 3166-1 alpha-2 chosen by the host, empty = not shown
		int delay = 0;
		int rollback = 0;
		bool locked = false;
		int age_seconds = 0;
	};

	struct RoomList
	{
		std::vector<Room> rooms;
		int playing = 0; // matches running (this version)
		int online = 0;  // players online (all versions), estimated by the lobby
	};

	struct CreatedRoom
	{
		QString id;
		QString token;
	};

	struct JoinInfo
	{
		std::string address; // IPv4
		int port = 0;
		int delay = 0;
		int rollback = 0;
		QString host;
	};

	struct Heartbeat
	{
		bool playing = false;
		QString guest;
	};

	/// Result of a request: the value, or a message for the player.
	template <typename T>
	struct Result
	{
		std::optional<T> value;
		QString error;
	};

	explicit LobbyClient(QObject* parent = nullptr);
	~LobbyClient() override;

	/// API URL from the settings ([Launcher] LobbyUrl), defaults to the project's lobby.
	static std::string GetUrl();

	void listRooms(std::function<void(Result<RoomList>)> done);
	/// country: ISO 3166-1 alpha-2 code the host chose to show, or empty.
	void createRoom(const std::string& name, const std::string& country, int delay, int rollback, int port,
		const std::string& code, std::function<void(Result<CreatedRoom>)> done);
	void heartbeat(const QString& id, const QString& token, std::function<void(Result<Heartbeat>)> done);
	void joinRoom(const QString& id, const std::string& name, const std::string& code,
		std::function<void(Result<JoinInfo>)> done);
	/// The joiner never connected: lists the room again.
	void reopenRoom(const QString& id, const QString& token);
	void closeRoom(const QString& id, const QString& token);

	/// Finishes outstanding requests (at most a few seconds), e.g. to close a room when the program exits.
	void flush();

private:
	using Reply = std::function<void(int status, const QByteArray& body, const QString& error)>;
	void get(const std::string& query, Reply reply);
	void post(const std::string& form, Reply reply);
	void ensurePolling();

	std::unique_ptr<HTTPDownloader> m_http;
	QTimer* m_poll = nullptr;
};
