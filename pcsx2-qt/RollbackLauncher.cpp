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
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QFrame>
#include <QtWidgets/QStackedWidget>
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

// Look of the start window: dark, crimson and gold, large menu entries like a game menu. Applies to this
// window only (the PCSX2 windows keep their theme). No artwork from the game is used.
static const char* LAUNCHER_STYLE = R"(
#RollbackLauncher {
	background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #0d0a10, stop:0.55 #1a0c14, stop:1 #330d18);
}
#RollbackLauncher QLabel { color: #e9e2d6; background: transparent; }
QLabel#Title { color: #f3e7c4; font-size: 26px; font-weight: 800; letter-spacing: 3px; }
QLabel#Subtitle { color: #c8a24a; font-size: 12px; font-weight: 600; letter-spacing: 6px; }
QLabel#PageTitle { color: #f3e7c4; font-size: 18px; font-weight: 700; letter-spacing: 2px; }
QLabel#Hint { color: #a99f93; font-size: 12px; }
QLabel#Status { color: #cfc6b8; font-size: 12px; }
QFrame#Rule { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #c8a24a, stop:1 transparent); max-height: 1px; min-height: 1px; border: none; }
QFrame#Panel { background: rgba(10, 6, 10, 150); border: 1px solid rgba(200, 162, 74, 60); border-radius: 6px; }

QPushButton#MenuEntry {
	color: #d8cfc2; background: transparent; border: none; border-left: 3px solid transparent;
	text-align: left; padding: 10px 18px; font-size: 17px; font-weight: 700; letter-spacing: 2px;
}
QPushButton#MenuEntry:hover { color: #ffffff; background: rgba(200, 16, 46, 60); }
QPushButton#MenuEntry:checked { color: #ffffff; border-left: 3px solid #c8a24a;
	background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 rgba(200, 16, 46, 170), stop:1 rgba(200, 16, 46, 0)); }

QPushButton#Action {
	color: #ffffff; font-size: 15px; font-weight: 700; letter-spacing: 1px; padding: 10px 18px;
	border: 1px solid #e0b85a; border-radius: 4px;
	background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #d3203f, stop:1 #8e0c22);
}
QPushButton#Action:hover { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #e83353, stop:1 #a8102a); }
QPushButton#Action:pressed { background: #7a0a1d; }
QPushButton#Action:disabled { color: #8a7f76; border-color: #5a4a3a; background: #2a1c20; }

QPushButton#Tab, QPushButton#Delay {
	color: #cfc6b8; background: rgba(255, 255, 255, 12); border: 1px solid rgba(200, 162, 74, 70);
	padding: 6px 14px; font-size: 13px; font-weight: 600;
}
QPushButton#Tab:checked, QPushButton#Delay:checked { color: #1a0c14; background: #c8a24a; border-color: #e0c27a; }
QPushButton#Tab:disabled { color: #6f665e; border-color: rgba(200, 162, 74, 30); }
QPushButton#Tab:hover:!checked, QPushButton#Delay:hover:!checked { background: rgba(200, 162, 74, 50); }

QLineEdit {
	color: #f3e7c4; background: rgba(0, 0, 0, 120); border: 1px solid rgba(200, 162, 74, 90);
	border-radius: 3px; padding: 8px; font-size: 14px; selection-background-color: #c8102e;
}
QLineEdit:focus { border-color: #e0c27a; }
)";

RollbackLauncher::RollbackLauncher()
{
	setObjectName(QStringLiteral("RollbackLauncher"));
	setAttribute(Qt::WA_StyledBackground, true);
	setStyleSheet(QString::fromUtf8(LAUNCHER_STYLE));
	setWindowTitle(QStringLiteral("Fate/unlimited codes - Rollback"));
	setWindowIcon(QtHost::GetAppIcon());
	setMinimumSize(860, 520);

	QVBoxLayout* root = new QVBoxLayout(this);
	root->setContentsMargins(28, 22, 28, 16);
	root->setSpacing(10);

	// ---- Header ---------------------------------------------------------------------------------------------
	QLabel* title = new QLabel(QStringLiteral("FATE / UNLIMITED CODES"), this);
	title->setObjectName(QStringLiteral("Title"));
	QLabel* subtitle = new QLabel(QStringLiteral("ROLLBACK NETPLAY"), this);
	subtitle->setObjectName(QStringLiteral("Subtitle"));
	root->addWidget(title);
	root->addWidget(subtitle);
	QFrame* rule = new QFrame(this);
	rule->setObjectName(QStringLiteral("Rule"));
	root->addWidget(rule);

	// ---- Menu (left) and pages (right) ----------------------------------------------------------------------
	QHBoxLayout* body = new QHBoxLayout();
	body->setSpacing(24);
	root->addLayout(body, 1);

	QVBoxLayout* menu = new QVBoxLayout();
	menu->setSpacing(4);
	menu->setContentsMargins(0, 12, 0, 0);
	body->addLayout(menu);

	m_pages = new QStackedWidget(this);
	body->addWidget(m_pages, 1);

	QButtonGroup* menu_group = new QButtonGroup(this);
	auto add_menu_entry = [this, menu, menu_group](const QString& text, QWidget* page) {
		QPushButton* b = new QPushButton(text, this);
		b->setObjectName(QStringLiteral("MenuEntry"));
		b->setCheckable(true);
		b->setCursor(Qt::PointingHandCursor);
		b->setMinimumWidth(230);
		menu_group->addButton(b);
		menu->addWidget(b);
		if (page)
		{
			const int index = m_pages->addWidget(page);
			connect(b, &QPushButton::clicked, this, [this, index]() { m_pages->setCurrentIndex(index); });
		}
		return b;
	};

	// Helper for the pages: a panel with a title, a description and content below.
	auto make_page = [this](const QString& page_title, const QString& description, QVBoxLayout** content) {
		QFrame* page = new QFrame(this);
		page->setObjectName(QStringLiteral("Panel"));
		QVBoxLayout* l = new QVBoxLayout(page);
		l->setContentsMargins(22, 18, 22, 18);
		l->setSpacing(12);
		QLabel* t = new QLabel(page_title, page);
		t->setObjectName(QStringLiteral("PageTitle"));
		l->addWidget(t);
		QLabel* d = new QLabel(description, page);
		d->setObjectName(QStringLiteral("Hint"));
		d->setWordWrap(true);
		l->addWidget(d);
		*content = l;
		return page;
	};
	auto make_action = [this](const QString& text) {
		QPushButton* b = new QPushButton(text, this);
		b->setObjectName(QStringLiteral("Action"));
		b->setCursor(Qt::PointingHandCursor);
		return b;
	};

	// ---- Page: online ------------------------------------------------------------------------------------
	QVBoxLayout* online = nullptr;
	QFrame* online_page = make_page(QStringLiteral("ONLINE  -  VS. 2P"),
		QStringLiteral("Beide Spieler starten das Spiel aus ihrer eigenen Kopie bis zur Charakterauswahl, "
					   "alle Charaktere freigeschaltet."),
		&online);

	// Direct IP now, rooms (lobby server) later.
	QHBoxLayout* tabs = new QHBoxLayout();
	tabs->setSpacing(0);
	QPushButton* tab_direct = new QPushButton(QStringLiteral("Direkt (IP)"), online_page);
	tab_direct->setObjectName(QStringLiteral("Tab"));
	tab_direct->setCheckable(true);
	tab_direct->setChecked(true);
	QPushButton* tab_rooms = new QPushButton(QStringLiteral("Räume  (demnächst)"), online_page);
	tab_rooms->setObjectName(QStringLiteral("Tab"));
	tab_rooms->setEnabled(false);
	tab_rooms->setToolTip(QStringLiteral("Räume über einen Lobby-Server, ohne Portfreigabe. Kommt später."));
	tabs->addWidget(tab_direct);
	tabs->addWidget(tab_rooms);
	tabs->addStretch(1);
	online->addLayout(tabs);

	QHBoxLayout* delay_row = new QHBoxLayout();
	delay_row->setSpacing(0);
	QLabel* delay_label = new QLabel(QStringLiteral("Delay"), online_page);
	delay_label->setMinimumWidth(110);
	delay_row->addWidget(delay_label);
	m_delay_group = new QButtonGroup(this);
	const int saved_delay = std::clamp(Host::GetBaseIntSettingValue(SECTION, "Delay", DEFAULT_DELAY), 0, MAX_DELAY);
	for (int i = 0; i <= MAX_DELAY; i++)
	{
		QPushButton* b = new QPushButton(QString::number(i), online_page);
		b->setObjectName(QStringLiteral("Delay"));
		b->setCheckable(true);
		b->setChecked(i == saved_delay);
		b->setMinimumWidth(44);
		m_delay_group->addButton(b, i);
		delay_row->addWidget(b);
	}
	QLabel* delay_hint = new QLabel(QStringLiteral("   Frames  -  empfohlen 1, beide gleich"), online_page);
	delay_hint->setObjectName(QStringLiteral("Hint"));
	delay_row->addWidget(delay_hint);
	delay_row->addStretch(1);
	online->addLayout(delay_row);
	online->addSpacing(6);

	const int port = Host::GetBaseIntSettingValue(SECTION, "Port", DEFAULT_PORT);
	m_host_button = make_action(QStringLiteral("HOSTEN  (SPIELER 1)"));
	m_host_button->setToolTip(
		QStringLiteral("Wartet auf UDP-Port %1 auf den anderen Spieler.\nDer Port muss im Router freigegeben sein.").arg(port));
	online->addWidget(m_host_button);
	QLabel* host_hint = new QLabel(
		QStringLiteral("Dem anderen Spieler die eigene öffentliche IP sagen. UDP-Port %1 im Router freigeben.").arg(port),
		online_page);
	host_hint->setObjectName(QStringLiteral("Hint"));
	online->addWidget(host_hint);
	online->addSpacing(6);

	QHBoxLayout* join_row = new QHBoxLayout();
	m_host_address = new QLineEdit(online_page);
	m_host_address->setPlaceholderText(QStringLiteral("IP des Hosts, z. B. 203.0.113.5"));
	m_host_address->setText(QString::fromStdString(Host::GetBaseStringSettingValue(SECTION, "HostAddress", "")));
	join_row->addWidget(m_host_address, 1);
	m_join_button = make_action(QStringLiteral("BEITRETEN  (SPIELER 2)"));
	join_row->addWidget(m_join_button);
	online->addLayout(join_row);
	online->addStretch(1);

	// ---- Page: training ----------------------------------------------------------------------------------
	QVBoxLayout* training = nullptr;
	QFrame* training_page = make_page(QStringLiteral("TRAINING"),
		QStringLiteral("Alleine üben, ohne Verbindung: Das Spiel startet bis zur Charakterauswahl im "
					   "Practice-Modus, alle Charaktere freigeschaltet."),
		&training);
	m_practice_button = make_action(QStringLiteral("PRACTICE STARTEN"));
	training->addWidget(m_practice_button);
	training->addStretch(1);

	// ---- Page: key config --------------------------------------------------------------------------------
	QVBoxLayout* keys = nullptr;
	QFrame* keys_page = make_page(QStringLiteral("TASTENBELEGUNG"),
		QStringLiteral("Hier den eigenen Controller einrichten (Controller-Port 1). Die Belegung gilt für "
					   "alles: Hosten, Beitreten und Training. Das Spiel ordnet deinen Controller automatisch "
					   "Spieler 1 oder 2 zu."),
		&keys);
	QPushButton* key_button = make_action(QStringLiteral("CONTROLLER EINRICHTEN"));
	keys->addWidget(key_button);
	keys->addStretch(1);

	QPushButton* menu_online = add_menu_entry(QStringLiteral("ONLINE"), online_page);
	add_menu_entry(QStringLiteral("TRAINING"), training_page);
	add_menu_entry(QStringLiteral("TASTENBELEGUNG"), keys_page);
	menu->addStretch(1);
	QPushButton* menu_quit = add_menu_entry(QStringLiteral("BEENDEN"), nullptr);
	menu_quit->setCheckable(false);
	menu_online->setChecked(true);
	m_pages->setCurrentIndex(0);

	// ---- Footer: files ---------------------------------------------------------------------------------------
	QFrame* rule2 = new QFrame(this);
	rule2->setObjectName(QStringLiteral("Rule"));
	root->addWidget(rule2);
	QHBoxLayout* footer = new QHBoxLayout();
	footer->setSpacing(28);
	m_bios_status = new QLabel(this);
	m_bios_status->setObjectName(QStringLiteral("Status"));
	m_game_status = new QLabel(this);
	m_game_status->setObjectName(QStringLiteral("Status"));
	footer->addWidget(m_bios_status);
	footer->addWidget(m_game_status);
	footer->addStretch(1);
	QLabel* footer_hint = new QLabel(QStringLiteral("Vollbild: Alt+Enter   -   Spiel beenden: Spielfenster schließen"), this);
	footer_hint->setObjectName(QStringLiteral("Hint"));
	footer->addWidget(footer_hint);
	root->addLayout(footer);

	connect(m_host_button, &QPushButton::clicked, this, &RollbackLauncher::onHostClicked);
	connect(m_join_button, &QPushButton::clicked, this, &RollbackLauncher::onJoinClicked);
	connect(m_host_address, &QLineEdit::returnPressed, this, &RollbackLauncher::onJoinClicked);
	connect(m_practice_button, &QPushButton::clicked, this, &RollbackLauncher::onPracticeClicked);
	connect(key_button, &QPushButton::clicked, this, &RollbackLauncher::onKeyConfigClicked);
	connect(menu_quit, &QPushButton::clicked, this, &QWidget::close);

	// Out of the way while a game runs, back when it has ended.
	connect(g_emu_thread, &EmuThread::onVMStarted, this, &QWidget::hide);
	connect(g_emu_thread, &EmuThread::onVMStopped, this, &RollbackLauncher::showAgain);

	refreshStatus();
}

int RollbackLauncher::selectedDelay() const
{
	const int id = m_delay_group->checkedId();
	return (id >= 0) ? id : DEFAULT_DELAY;
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
		m_bios_status->setText(QStringLiteral("<span style='color:#ff6b6b'>&#9679;</span> BIOS fehlt - eigene BIOS-Datei "
											  "(4 MB) in den Ordner \"bios\" kopieren"));
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
		// Region and version only, e.g. "Japan v01.00(17/01/2000)".
		const QStringList words = QString::fromStdString(description).split(QLatin1Char(' '), Qt::SkipEmptyParts);
		const QString short_description = words.mid(0, 2).join(QLatin1Char(' '));
		m_bios_status->setText(differs ?
								   QStringLiteral("<span style='color:#f0b429'>&#9679;</span> BIOS %1 - andere Version als "
												  "beim Organisator, online kann das Spiel abweichen")
									   .arg(short_description.toHtmlEscaped()) :
								   QStringLiteral("<span style='color:#6fd08c'>&#9679;</span> BIOS %1").arg(short_description.toHtmlEscaped()));
		m_bios_status->setToolTip(
			differs ? QStringLiteral("%1\nOrganisator: %2").arg(QString::fromStdString(m_bios_file), QString::fromStdString(reference)) :
					  QString::fromStdString(m_bios_file));
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
							   QStringLiteral("<span style='color:#ff6b6b'>&#9679;</span> Spiel fehlt - eigenes Abbild "
											  "(.iso/.chd/.cso/.cue) in den Ordner \"game\" kopieren") :
							   QStringLiteral("<span style='color:#6fd08c'>&#9679;</span> Spiel %1")
								   .arg(QString::fromStdString(std::string(Path::GetFileName(m_game_path))).toHtmlEscaped()));

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
	boot(fmt::format("host={},delay={},rollback={},boot", port, selectedDelay(),
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

	boot(fmt::format("join={},delay={},rollback={},boot", address, selectedDelay(),
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
	Host::SetBaseIntSettingValue(SECTION, "Delay", selectedDelay());
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
