// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "LobbyClient.h"

#include "pcsx2/Host.h"

#include "common/HTTPDownloader.h"

#include "BuildVersion.h"

#include "fmt/format.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtCore/QUrl>

static constexpr const char* DEFAULT_URL = "https://lobby.blazblue-ist-tot.de/api.php";
static constexpr int PROTOCOL = 1;

static QString Tr(const char* text)
{
	return QCoreApplication::translate("LobbyClient", text);
}

static std::string Encode(const std::string& value)
{
	return QUrl::toPercentEncoding(QString::fromStdString(value)).toStdString();
}

// Formats the server may send; anything else is rejected.
static bool IsRoomId(const QString& s)
{
	static const QRegularExpression re(QStringLiteral("^[0-9a-f]{16}$"));
	return re.match(s).hasMatch();
}

static bool IsToken(const QString& s)
{
	static const QRegularExpression re(QStringLiteral("^[0-9a-f]{64}$"));
	return re.match(s).hasMatch();
}

static bool IsCountry(const QString& s)
{
	static const QRegularExpression re(QStringLiteral("^[A-Z]{2}$"));
	return re.match(s).hasMatch();
}

static bool IsName(const QString& s)
{
	static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9 .!?-]{1,20}$"));
	return re.match(s).hasMatch();
}

/// A plain dotted IPv4 address (four numbers 0-255, no leading zeros).
static bool IsIPv4(const QString& s)
{
	static const QRegularExpression re(QStringLiteral("^(0|[1-9][0-9]{0,2})[.](0|[1-9][0-9]{0,2})[.](0|[1-9][0-9]{0,2})[.](0|[1-9][0-9]{0,2})$"));
	const QRegularExpressionMatch m = re.match(s);
	if (!m.hasMatch())
		return false;
	for (int i = 1; i <= 4; i++)
	{
		if (m.captured(i).toInt() > 255)
			return false;
	}
	return true;
}

static std::optional<int> IntIn(const QJsonValue& v, int min, int max)
{
	if (!v.isDouble())
		return std::nullopt;
	const double d = v.toDouble();
	if (d != static_cast<double>(static_cast<int>(d)) || d < min || d > max)
		return std::nullopt;
	return static_cast<int>(d);
}

LobbyClient::LobbyClient(QObject* parent)
	: QObject(parent)
{
	// No OS details in the user agent, only the program and its version.
	m_http = HTTPDownloader::Create(fmt::format("Mawarunco/{}", BuildVersion::GitRev));
	if (m_http)
		m_http->SetTimeout(10.0f);
	m_poll = new QTimer(this);
	m_poll->setInterval(30);
	connect(m_poll, &QTimer::timeout, this, [this]() {
		m_http->PollRequests();
		if (!m_http->HasAnyRequests())
			m_poll->stop();
	});
}

LobbyClient::~LobbyClient() = default;

std::string LobbyClient::GetUrl()
{
	std::string url = Host::GetBaseStringSettingValue("Launcher", "LobbyUrl", DEFAULT_URL);
	// The lobby hands out IP addresses: never over plain HTTP, except to a test server on this PC.
	if (!url.starts_with("https://") && !url.starts_with("http://127.0.0.1:") && !url.starts_with("http://localhost:"))
		url = DEFAULT_URL;
	return url;
}

void LobbyClient::ensurePolling()
{
	if (!m_poll->isActive())
		m_poll->start();
}

static void HandleResponse(s32 status, const HTTPDownloader::Request::Data& data, const std::function<void(int, const QByteArray&, const QString&)>& reply)
{
	if (status == HTTPDownloader::HTTP_STATUS_TIMEOUT)
		reply(status, {}, Tr("The lobby does not answer (timeout)."));
	else if (status < 0)
		reply(status, {}, Tr("The lobby is not reachable. Check your internet connection."));
	else
		reply(status, QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(std::min<size_t>(data.size(), 256 * 1024))), QString());
}

void LobbyClient::get(const std::string& query, Reply reply)
{
	if (!m_http)
	{
		reply(-1, {}, Tr("No HTTP support."));
		return;
	}
	m_http->CreateRequest(GetUrl() + "?" + query,
		[reply](s32 status, const std::string&, HTTPDownloader::Request::Data data) { HandleResponse(status, data, reply); });
	ensurePolling();
}

void LobbyClient::post(const std::string& form, Reply reply)
{
	if (!m_http)
	{
		reply(-1, {}, Tr("No HTTP support."));
		return;
	}
	m_http->CreatePostRequest(GetUrl(), form,
		[reply](s32 status, const std::string&, HTTPDownloader::Request::Data data) { HandleResponse(status, data, reply); });
	ensurePolling();
}

/// Parses the JSON object of a reply; on failure fills error with a message for the player.
static std::optional<QJsonObject> ParseReply(int status, const QByteArray& body, const QString& transport_error, QString* error)
{
	if (!transport_error.isEmpty())
	{
		*error = transport_error;
		return std::nullopt;
	}
	const QJsonDocument doc = QJsonDocument::fromJson(body);
	if (!doc.isObject())
	{
		*error = Tr("Unexpected answer from the lobby (HTTP %1).").arg(status);
		return std::nullopt;
	}
	const QJsonObject obj = doc.object();
	if (obj.value(QStringLiteral("protocol")).toInt() != PROTOCOL)
	{
		*error = Tr("The lobby uses another protocol. Please update Mawarunco.");
		return std::nullopt;
	}
	if (!obj.value(QStringLiteral("ok")).toBool())
	{
		// The server's message, shortened and as plain text.
		const QString message = obj.value(QStringLiteral("error")).toString().left(120);
		*error = message.isEmpty() ? Tr("The lobby refused the request (HTTP %1).").arg(status) : message;
		return std::nullopt;
	}
	return obj;
}

void LobbyClient::listRooms(std::function<void(Result<RoomList>)> done)
{
	get(fmt::format("action=list&ver={}", BuildVersion::GitHash), [done](int status, const QByteArray& body, const QString& transport_error) {
		Result<RoomList> result;
		const std::optional<QJsonObject> obj = ParseReply(status, body, transport_error, &result.error);
		if (!obj)
		{
			done(std::move(result));
			return;
		}
		RoomList list;
		list.playing = IntIn(obj->value(QStringLiteral("playing")), 0, 100000).value_or(0);
		list.online = IntIn(obj->value(QStringLiteral("online")), 0, 1000000).value_or(0);
		for (const QJsonValue& v : obj->value(QStringLiteral("rooms")).toArray())
		{
			const QJsonObject r = v.toObject();
			Room room;
			room.id = r.value(QStringLiteral("id")).toString();
			room.host = r.value(QStringLiteral("host")).toString();
			const std::optional<int> delay = IntIn(r.value(QStringLiteral("delay")), 0, 8);
			const std::optional<int> rollback = IntIn(r.value(QStringLiteral("rollback")), 1, 8);
			if (!IsRoomId(room.id) || !IsName(room.host) || !delay || !rollback)
				continue; // skip anything malformed
			room.delay = *delay;
			room.rollback = *rollback;
			room.locked = r.value(QStringLiteral("locked")).toBool();
			if (const QString country = r.value(QStringLiteral("country")).toString(); IsCountry(country))
				room.country = country;
			room.age_seconds = IntIn(r.value(QStringLiteral("age")), 0, 1 << 30).value_or(0);
			list.rooms.push_back(std::move(room));
			if (list.rooms.size() >= 200)
				break;
		}
		result.value = std::move(list);
		done(std::move(result));
	});
}

void LobbyClient::createRoom(const std::string& name, const std::string& country, int delay, int rollback, int port,
	const std::string& code, std::function<void(Result<CreatedRoom>)> done)
{
	std::string form = fmt::format("action=create&ver={}&name={}&delay={}&rollback={}&port={}", BuildVersion::GitHash,
		Encode(name), delay, rollback, port);
	if (!code.empty())
		form += "&code=" + Encode(code);
	if (IsCountry(QString::fromStdString(country)))
		form += "&country=" + country;
	post(form, [done](int status, const QByteArray& body, const QString& transport_error) {
		Result<CreatedRoom> result;
		const std::optional<QJsonObject> obj = ParseReply(status, body, transport_error, &result.error);
		if (obj)
		{
			CreatedRoom room{obj->value(QStringLiteral("id")).toString(), obj->value(QStringLiteral("token")).toString()};
			if (IsRoomId(room.id) && IsToken(room.token))
				result.value = std::move(room);
			else
				result.error = Tr("Unexpected answer from the lobby.");
		}
		done(std::move(result));
	});
}

void LobbyClient::heartbeat(const QString& id, const QString& token, std::function<void(Result<Heartbeat>)> done)
{
	post(fmt::format("action=heartbeat&id={}&token={}", id.toStdString(), token.toStdString()),
		[done](int status, const QByteArray& body, const QString& transport_error) {
			Result<Heartbeat> result;
			const std::optional<QJsonObject> obj = ParseReply(status, body, transport_error, &result.error);
			if (obj)
			{
				Heartbeat hb;
				hb.playing = obj->value(QStringLiteral("status")).toString() == QStringLiteral("playing");
				const QString guest = obj->value(QStringLiteral("guest")).toString();
				if (IsName(guest))
					hb.guest = guest;
				result.value = std::move(hb);
			}
			done(std::move(result));
		});
}

void LobbyClient::joinRoom(const QString& id, const std::string& name, const std::string& code,
	std::function<void(Result<JoinInfo>)> done)
{
	std::string form = fmt::format("action=join&id={}&ver={}&name={}", id.toStdString(), BuildVersion::GitHash, Encode(name));
	if (!code.empty())
		form += "&code=" + Encode(code);
	post(form, [done](int status, const QByteArray& body, const QString& transport_error) {
		Result<JoinInfo> result;
		const std::optional<QJsonObject> obj = ParseReply(status, body, transport_error, &result.error);
		if (!obj)
		{
			done(std::move(result));
			return;
		}
		// The only value from the server that leaves this program: a public IPv4 address and a port.
		const QString address = obj->value(QStringLiteral("address")).toString();
		const std::optional<int> port = IntIn(obj->value(QStringLiteral("port")), 1024, 65535);
		const std::optional<int> delay = IntIn(obj->value(QStringLiteral("delay")), 0, 8);
		const std::optional<int> rollback = IntIn(obj->value(QStringLiteral("rollback")), 1, 8);
		const QString host = obj->value(QStringLiteral("host")).toString();
		if (!IsIPv4(address) || !port || !delay ||
			!rollback || !IsName(host))
		{
			result.error = Tr("Unexpected answer from the lobby.");
			done(std::move(result));
			return;
		}
		JoinInfo info;
		info.address = address.toStdString();
		info.port = *port;
		info.delay = *delay;
		info.rollback = *rollback;
		info.host = host;
		result.value = std::move(info);
		done(std::move(result));
	});
}

void LobbyClient::reopenRoom(const QString& id, const QString& token)
{
	post(fmt::format("action=reopen&id={}&token={}", id.toStdString(), token.toStdString()),
		[](int, const QByteArray&, const QString&) {});
}

void LobbyClient::closeRoom(const QString& id, const QString& token)
{
	post(fmt::format("action=close&id={}&token={}", id.toStdString(), token.toStdString()),
		[](int, const QByteArray&, const QString&) {});
}

void LobbyClient::flush()
{
	if (m_http)
		m_http->WaitForAllRequests();
}

#include "moc_LobbyClient.cpp"
