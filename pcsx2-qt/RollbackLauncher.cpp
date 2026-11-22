// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "RollbackLauncher.h"
#include "MainWindow.h"
#include "QtHost.h"

#include "pcsx2/Config.h"
#include "pcsx2/Host.h"
#include "pcsx2/Rollback/BootShortcut.h"
#include "pcsx2/Rollback/Netplay.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/ps2/BiosTools.h"

#include "common/Error.h"
#include "common/MD5Digest.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtGui/QCloseEvent>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <array>
#include <memory>
#include <optional>
#include <vector>

static constexpr const char* SECTION = "Launcher";
static constexpr int DEFAULT_PORT = 7000;
static constexpr int DEFAULT_DELAY = 1;
static constexpr int DEFAULT_ROLLBACK = 2;
static constexpr int MAX_DELAY = 3;
static constexpr s64 BIOS_SIZE = 4 * 1024 * 1024;

bool RollbackLauncher::IsEnabled()
{
	return Host::GetBaseBoolSettingValue("UI", "RollbackLauncher", false);
}

RollbackLauncher::RollbackLauncher()
{
	setWindowTitle(QStringLiteral("Fate/unlimited codes - Rollback"));
	setWindowIcon(QtHost::GetAppIcon());

	QVBoxLayout* layout = new QVBoxLayout(this);

	// ---- Status: BIOS and game ---------------------------------------------------------------------
	QGroupBox* status_box = new QGroupBox(QStringLiteral("Dateien"), this);
	QGridLayout* status_layout = new QGridLayout(status_box);
	status_layout->addWidget(new QLabel(QStringLiteral("BIOS:"), status_box), 0, 0);
	m_bios_status = new QLabel(status_box);
	status_layout->addWidget(m_bios_status, 0, 1);
	status_layout->addWidget(new QLabel(QStringLiteral("Spiel:"), status_box), 1, 0);
	m_game_status = new QLabel(status_box);
	status_layout->addWidget(m_game_status, 1, 1);
	status_layout->setColumnStretch(1, 1);
	layout->addWidget(status_box);

	// ---- Netplay ----------------------------------------------------------------------------------------
	QGroupBox* net_box = new QGroupBox(QStringLiteral("Online spielen (Vs. 2P)"), this);
	QGridLayout* net_layout = new QGridLayout(net_box);

	net_layout->addWidget(new QLabel(QStringLiteral("Delay:"), net_box), 0, 0);
	m_delay = new QComboBox(net_box);
	for (int i = 0; i <= MAX_DELAY; i++)
		m_delay->addItem((i == 1) ? QStringLiteral("1 Frame (empfohlen)") : QStringLiteral("%1 Frames").arg(i), i);
	m_delay->setCurrentIndex(std::clamp(Host::GetBaseIntSettingValue(SECTION, "Delay", DEFAULT_DELAY), 0, MAX_DELAY));
	m_delay->setToolTip(QStringLiteral("Eingabeverzoegerung. Beide Spieler sollten denselben Wert waehlen.\n"
									   "Mehr Delay = weniger Rollbacks bei hohem Ping."));
	net_layout->addWidget(m_delay, 0, 1, 1, 2);

	const int port = Host::GetBaseIntSettingValue(SECTION, "Port", DEFAULT_PORT);
	m_host_button = new QPushButton(QStringLiteral("Hosten (Spieler 1)"), net_box);
	m_host_button->setToolTip(
		QStringLiteral("Wartet auf UDP-Port %1 auf den anderen Spieler.\nDer Port muss im Router freigegeben sein.").arg(port));
	net_layout->addWidget(m_host_button, 1, 0, 1, 3);

	net_layout->addWidget(new QLabel(QStringLiteral("IP des Hosts:"), net_box), 2, 0);
	m_host_address = new QLineEdit(net_box);
	m_host_address->setPlaceholderText(QStringLiteral("z. B. 203.0.113.5"));
	m_host_address->setText(QString::fromStdString(Host::GetBaseStringSettingValue(SECTION, "HostAddress", "")));
	net_layout->addWidget(m_host_address, 2, 1);
	m_join_button = new QPushButton(QStringLiteral("Beitreten (Spieler 2)"), net_box);
	net_layout->addWidget(m_join_button, 2, 2);
	net_layout->setColumnStretch(1, 1);
	layout->addWidget(net_box);

	// ---- Offline and settings ---------------------------------------------------------------------------
	QGroupBox* other_box = new QGroupBox(QStringLiteral("Offline und Einstellungen"), this);
	QHBoxLayout* other_layout = new QHBoxLayout(other_box);
	m_practice_button = new QPushButton(QStringLiteral("Training (Practice)"), other_box);
	other_layout->addWidget(m_practice_button);
	QPushButton* key_button = new QPushButton(QStringLiteral("Tastenbelegung"), other_box);
	key_button->setToolTip(QStringLiteral("Belegung des eigenen Controllers (Port 1).\n"
										  "Gilt fuer Host, Beitreten und Training gleichermassen."));
	other_layout->addWidget(key_button);
	layout->addWidget(other_box);

	QLabel* hint = new QLabel(
		QStringLiteral("Das Spiel startet automatisch bis zur Charakterauswahl, alle Charaktere freigeschaltet.\n"
					   "Zum Beenden eines Spiels einfach das Spielfenster schliessen."),
		this);
	hint->setWordWrap(true);
	layout->addWidget(hint);

	connect(m_host_button, &QPushButton::clicked, this, &RollbackLauncher::onHostClicked);
	connect(m_join_button, &QPushButton::clicked, this, &RollbackLauncher::onJoinClicked);
	connect(m_host_address, &QLineEdit::returnPressed, this, &RollbackLauncher::onJoinClicked);
	connect(m_practice_button, &QPushButton::clicked, this, &RollbackLauncher::onPracticeClicked);
	connect(key_button, &QPushButton::clicked, this, &RollbackLauncher::onKeyConfigClicked);

	// Out of the way while a game runs, back when it has ended.
	connect(g_emu_thread, &EmuThread::onVMStarted, this, &QWidget::hide);
	connect(g_emu_thread, &EmuThread::onVMStopped, this, &RollbackLauncher::showAgain);

	refreshStatus();
	setMinimumWidth(460);
}

RollbackLauncher::~RollbackLauncher() = default;

bool RollbackLauncher::refreshStatus()
{
	// BIOS: the configured file if it is there, otherwise the first 4 MB file in the BIOS folder.
	m_bios_file.clear();
	std::string description;
	auto check_bios = [&description](const std::string& path) {
		u32 version, region;
		std::string zone;
		return IsBIOS(path.c_str(), version, description, region, zone);
	};
	const std::string configured = Host::GetBaseStringSettingValue("Filenames", "BIOS", "");
	if (!configured.empty() && check_bios(Path::Combine(EmuFolders::Bios, configured)))
	{
		m_bios_file = configured;
	}
	else
	{
		FileSystem::FindResultsArray files;
		FileSystem::FindFiles(EmuFolders::Bios.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_SORT_BY_NAME, &files);
		for (const FILESYSTEM_FIND_DATA& fd : files)
		{
			if (fd.Size == BIOS_SIZE && check_bios(fd.FileName))
			{
				m_bios_file = std::string(Path::GetFileName(fd.FileName));
				break;
			}
		}
	}

	if (m_bios_file.empty())
	{
		m_bios_status->setText(QStringLiteral("<span style='color:#c33'>fehlt - eigene BIOS-Datei (4 MB) in den Ordner "
											  "\"bios\" kopieren</span>"));
	}
	else
	{
		// Netplay boots each player's own BIOS: a different version can make the two games differ. The
		// package stores the MD5 and name of the organizer's BIOS (identification only, not security).
		const std::string reference_md5 = Host::GetBaseStringSettingValue(SECTION, "ReferenceBiosMD5", "");
		const std::string reference = Host::GetBaseStringSettingValue(SECTION, "ReferenceBios", "");
		bool differs = false;
		if (!reference_md5.empty())
		{
			const std::optional<std::vector<u8>> data =
				FileSystem::ReadBinaryFile(Path::Combine(EmuFolders::Bios, m_bios_file).c_str());
			u8 digest[16] = {};
			if (data.has_value())
			{
				MD5Digest md5;
				md5.Update(data->data(), static_cast<u32>(data->size()));
				md5.Final(digest);
			}
			std::string hex;
			for (const u8 b : digest)
				hex += fmt::format("{:02X}", b);
			differs = StringUtil::Strcasecmp(hex.c_str(), reference_md5.c_str()) != 0;
		}
		m_bios_status->setText(
			QStringLiteral("%1 (%2)%3")
				.arg(QString::fromStdString(m_bios_file))
				.arg(QString::fromStdString(description))
				.arg(differs ? QStringLiteral("<br><span style='color:#c80'>Andere Version als beim Organisator (%1): "
											  "online kann das Spiel abweichen.</span>")
								   .arg(QString::fromStdString(reference).toHtmlEscaped()) :
							   QString()));
	}

	// Game: the first disc image in the "game" folder next to the emulator.
	m_game_path.clear();
	const std::string game_dir = Path::Combine(EmuFolders::AppRoot, "game");
	FileSystem::FindResultsArray files;
	FileSystem::FindFiles(game_dir.c_str(), "*",
		FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE | FILESYSTEM_FIND_SORT_BY_NAME, &files);
	static constexpr std::array<const char*, 5> extensions = {".iso", ".chd", ".cso", ".zso", ".cue"};
	for (const FILESYSTEM_FIND_DATA& fd : files)
	{
		for (const char* ext : extensions)
		{
			if (StringUtil::EndsWithNoCase(fd.FileName, ext))
			{
				m_game_path = fd.FileName;
				break;
			}
		}
		if (!m_game_path.empty())
			break;
	}
	m_game_status->setText(m_game_path.empty() ?
							   QStringLiteral("<span style='color:#c33'>fehlt - eigenes Abbild von Fate/unlimited codes "
											  "(.iso/.chd/.cso/.cue) in den Ordner \"game\" kopieren</span>") :
							   QString::fromStdString(std::string(Path::GetFileName(m_game_path))));

	return !m_bios_file.empty() && !m_game_path.empty();
}

void RollbackLauncher::showAgain()
{
	refreshStatus();
	for (QPushButton* b : {m_host_button, m_join_button, m_practice_button})
		b->setEnabled(true);
	show();
	raise();
	activateWindow();
}

void RollbackLauncher::closeEvent(QCloseEvent* event)
{
	// Closing the launcher closes the program.
	event->accept();
	g_main_window->requestExit(false);
}

void RollbackLauncher::onHostClicked()
{
	const int port = Host::GetBaseIntSettingValue(SECTION, "Port", DEFAULT_PORT);
	boot(fmt::format("host={},delay={},rollback={},boot", port, m_delay->currentData().toInt(),
			 Host::GetBaseIntSettingValue(SECTION, "Rollback", DEFAULT_ROLLBACK)),
		nullptr);
}

void RollbackLauncher::onJoinClicked()
{
	std::string address = m_host_address->text().trimmed().toStdString();
	// Only an IP address (IPv4 or [IPv6]) with an optional port.
	static const QRegularExpression valid(QStringLiteral("^[0-9A-Fa-f.:\\[\\]]+$"));
	if (address.empty() || !valid.match(QString::fromStdString(address)).hasMatch())
	{
		QMessageBox::warning(this, windowTitle(), QStringLiteral("Bitte die IP-Adresse des Hosts eingeben, z. B. 203.0.113.5"));
		m_host_address->setFocus();
		return;
	}
	const bool ipv6 = address.starts_with('[');
	const bool has_port = ipv6 ? (address.find("]:") != std::string::npos) : (address.find(':') != std::string::npos);
	if (!has_port)
		address += fmt::format(":{}", Host::GetBaseIntSettingValue(SECTION, "Port", DEFAULT_PORT));

	boot(fmt::format("join={},delay={},rollback={},boot", address, m_delay->currentData().toInt(),
			 Host::GetBaseIntSettingValue(SECTION, "Rollback", DEFAULT_ROLLBACK)),
		nullptr);
}

void RollbackLauncher::onPracticeClicked()
{
	boot(std::string(), "practice");
}

void RollbackLauncher::onKeyConfigClicked()
{
	g_main_window->openControllerSettings();
}

void RollbackLauncher::boot(const std::string& netplay_spec, const char* shortcut)
{
	if (!refreshStatus())
	{
		QMessageBox::warning(this, windowTitle(), QStringLiteral("BIOS oder Spiel fehlt (siehe oben)."));
		return;
	}
	if (QtHost::IsVMValid())
		return;

	// Remembered for the next start.
	Host::SetBaseIntSettingValue(SECTION, "Delay", m_delay->currentData().toInt());
	Host::SetBaseStringSettingValue(SECTION, "HostAddress", m_host_address->text().trimmed().toUtf8().constData());

	// Everything the modes rely on: the found BIOS, a DualShock 2 on both ports (netplay injects both),
	// the game in its own window while the launcher is hidden.
	Host::SetBaseStringSettingValue("Filenames", "BIOS", m_bios_file.c_str());
	Host::SetBaseStringSettingValue("Pad1", "Type", "DualShock2");
	Host::SetBaseStringSettingValue("Pad2", "Type", "DualShock2");
	Host::SetBaseBoolSettingValue("UI", "RenderToSeparateWindow", true);
	Host::SetBaseBoolSettingValue("UI", "HideMainWindowWhenRunning", true);
	Host::CommitBaseSettingChanges();
	g_emu_thread->applySettings();

	Error error;
	if (!netplay_spec.empty() && !Netplay::SetLaunchOptions(netplay_spec, &error))
	{
		QMessageBox::critical(this, windowTitle(), QString::fromStdString(error.GetDescription()));
		return;
	}
	const char* script = shortcut ? shortcut : "vs2p";
	if (netplay_spec.empty() && !BootShortcut::Request(script, &error))
	{
		QMessageBox::critical(this, windowTitle(), QString::fromStdString(error.GetDescription()));
		return;
	}

	// Stays visible until the game runs (onVMStarted hides it). If the boot fails, the buttons come back.
	for (QPushButton* b : {m_host_button, m_join_button, m_practice_button})
		b->setEnabled(false);
	QTimer::singleShot(15000, this, [this]() {
		if (!QtHost::IsVMValid() && isVisible())
			showAgain();
	});

	auto params = std::make_shared<VMBootParameters>();
	params->filename = m_game_path;
	g_emu_thread->startVM(std::move(params));
}

#include "moc_RollbackLauncher.cpp"
