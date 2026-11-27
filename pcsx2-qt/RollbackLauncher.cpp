// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "RollbackLauncher.h"
#include "LobbyClient.h"
#include "MainWindow.h"
#include "QtHost.h"

#include "pcsx2/Config.h"
#include "pcsx2/Host.h"
#include "pcsx2/Rollback/BootShortcut.h"
#include "pcsx2/Rollback/FucButtons.h"
#include "pcsx2/Rollback/FucNames.h"
#include "pcsx2/Rollback/Netplay.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/ps2/BiosTools.h"
#include "pcsx2/Input/InputManager.h"

#include "IconsFontAwesome.h"
#include "IconsPromptFont.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/MD5Digest.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <QtCore/QPropertyAnimation>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QCloseEvent>
#include <QtGui/QDesktopServices>
#include <QtGui/QFontDatabase>
#include <QtGui/QHideEvent>
#include <QtGui/QShowEvent>
#include <QtGui/QRegularExpressionValidator>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtCore/QSignalBlocker>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGraphicsOpacityEffect>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
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
static constexpr int MAX_ROLLBACK = 8;
static constexpr s64 BIOS_SIZE = 4 * 1024 * 1024;

// Common 4:3 resolutions for the display page (the game is 4:3).
static constexpr std::array<std::pair<int, int>, 12> RESOLUTIONS = {{{640, 480}, {800, 600}, {960, 720}, {1024, 768},
	{1280, 960}, {1400, 1050}, {1440, 1080}, {1600, 1200}, {1920, 1440}, {2048, 1536}, {2560, 1920}, {2880, 2160}}};
static constexpr int DEFAULT_RESOLUTION = 4; // 1280x960

/// Internal resolution multiplier that renders at least as many lines as the chosen height (PS2: 480).
static int UpscaleForHeight(int height)
{
	return std::max(1, (height + 479) / 480);
}

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
QLabel#Section { color: #c8a24a; font-size: 13px; font-weight: 800; letter-spacing: 3px; padding-top: 4px; }
QLabel#Counter { color: #cfc6b8; font-size: 12px; font-weight: 600; }
QCheckBox { color: #e9e2d6; font-size: 13px; spacing: 8px; }
QScrollArea#PageScroll { background: transparent; border: none; }
QScrollArea#PageScroll > QWidget > QWidget { background: transparent; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: rgba(200, 162, 74, 90); border-radius: 4px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: rgba(200, 162, 74, 160); }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
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
QPushButton#BindRow, QPushButton#BindRowPractice {
	background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 rgba(70, 40, 120, 150), stop:1 rgba(30, 20, 60, 110));
	border: 1px solid rgba(200, 162, 74, 40); border-radius: 3px;
}
QPushButton#BindRowPractice {
	background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 rgba(120, 80, 30, 150), stop:1 rgba(60, 35, 20, 110));
}
QPushButton#BindRow:hover, QPushButton#BindRowPractice:hover { border-color: #e0c27a; }
QPushButton#BindRow:checked, QPushButton#BindRowPractice:checked {
	background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #8a3cf0, stop:1 #3a2a9a); border-color: #ffffff;
}
QLabel#BindName { color: #f3e7c4; font-size: 13px; font-weight: 700; letter-spacing: 1px; }
QLabel#BindGlyph { color: #ffffff; }
QComboBox {
	color: #f3e7c4; background: rgba(0, 0, 0, 120); border: 1px solid rgba(200, 162, 74, 90);
	border-radius: 3px; padding: 5px 8px; font-size: 13px;
}
QComboBox:hover { border-color: #e0c27a; }
QComboBox QAbstractItemView { color: #e9e2d6; background: #1a0c14; selection-background-color: #c8102e; }
QTreeWidget {
	color: #e9e2d6; background: rgba(0, 0, 0, 110); border: 1px solid rgba(200, 162, 74, 70);
	border-radius: 3px; font-size: 13px; alternate-background-color: rgba(255, 255, 255, 8);
}
QTreeWidget::item { padding: 5px 4px; }
QTreeWidget::item:selected { color: #ffffff; background: rgba(200, 16, 46, 170); }
QHeaderView::section {
	color: #c8a24a; background: rgba(0, 0, 0, 140); border: none; border-bottom: 1px solid rgba(200, 162, 74, 70);
	padding: 5px 6px; font-size: 12px; font-weight: 700; letter-spacing: 1px;
}
)";

RollbackLauncher::RollbackLauncher()
{
	// All texts go through tr() (context "RollbackLauncher"), so translations can be added later.
	setObjectName(QStringLiteral("RollbackLauncher"));
	setAttribute(Qt::WA_StyledBackground, true);
	setStyleSheet(QString::fromUtf8(LAUNCHER_STYLE));
	setWindowTitle(QStringLiteral("Mawarunco"));
	setWindowIcon(QtHost::GetAppIcon());
	setMinimumSize(900, 600);

	importInstallerChoices();

	// The local player's in-game button layout (kept here, the game cannot save it with memory cards off).
	if (const std::optional<FucButtons::Layout> layout =
			FucButtons::FromString(Host::GetBaseStringSettingValue(SECTION, "ButtonLayout", "")))
	{
		FucButtons::SetLocal(*layout);
	}

	QVBoxLayout* root = new QVBoxLayout(this);
	root->setContentsMargins(28, 22, 28, 16);
	root->setSpacing(10);

	// ---- Header ---------------------------------------------------------------------------------------------
	QLabel* title = new QLabel(QStringLiteral("FATE / UNLIMITED CODES"), this);
	title->setObjectName(QStringLiteral("Title"));
	QLabel* subtitle = new QLabel(tr("MAWARUNCO  -  ROLLBACK NETPLAY"), this);
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
	// One scrollable page: rooms first (the usual way to find a game), then the settings the host decides,
	// then the direct connection by IP address.
	QVBoxLayout* online = nullptr;
	QFrame* online_page = make_page(tr("ONLINE  -  VS. 2P"),
		tr("Both players boot their own copy of the game to the character select, all characters unlocked. "
		   "Each player plays with their own button config."),
		&online);
	QScrollArea* online_scroll = new QScrollArea(this);
	online_scroll->setObjectName(QStringLiteral("PageScroll"));
	online_scroll->setWidgetResizable(true);
	online_scroll->setFrameShape(QFrame::NoFrame);
	online_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	online_scroll->setWidget(online_page);
	m_online_page = online_scroll;
	const int port = Host::GetBaseIntSettingValue(SECTION, "Port", DEFAULT_PORT);

	auto make_section = [online_page](const QString& text) {
		QLabel* l = new QLabel(text, online_page);
		l->setObjectName(QStringLiteral("Section"));
		return l;
	};
	auto make_hint = [online_page](const QString& text) {
		QLabel* l = new QLabel(text, online_page);
		l->setObjectName(QStringLiteral("Hint"));
		l->setWordWrap(true);
		return l;
	};

	// Player name (shown above the health bar and in the room list) and country (room list).
	QGridLayout* you = new QGridLayout();
	you->setHorizontalSpacing(10);
	you->setVerticalSpacing(8);
	you->addWidget(new QLabel(tr("Name"), online_page), 0, 0);
	m_player_name = new QLineEdit(online_page);
	m_player_name->setMaxLength(static_cast<int>(FucNames::MAX_LENGTH));
	m_player_name->setPlaceholderText(tr("YOUR NAME"));
	m_player_name->setToolTip(tr("Shown above your health bar and in the room list"));
	m_player_name->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[A-Za-z0-9 .!?-]*")), m_player_name));
	m_player_name->setText(QString::fromStdString(FucNames::Sanitize(Host::GetBaseStringSettingValue(SECTION, "PlayerName", ""))));
	connect(m_player_name, &QLineEdit::textEdited, this, [this](const QString& text) {
		// The game's font has upper-case letters only.
		const int cursor = m_player_name->cursorPosition();
		m_player_name->setText(text.toUpper());
		m_player_name->setCursorPosition(cursor);
	});
	you->addWidget(m_player_name, 0, 1);
	you->addWidget(new QLabel(tr("Country"), online_page), 0, 2);
	// Preselected from the Windows region (no IP lookup); the player may change or hide it.
	m_country = new QComboBox(online_page);
	m_country->setToolTip(tr("Shown next to your name in the room list"));
	{
		std::vector<std::pair<QString, QString>> countries; // name, code
		for (int t = 1; t <= static_cast<int>(QLocale::LastTerritory); t++)
		{
			const QLocale::Territory territory = static_cast<QLocale::Territory>(t);
			const QString code = QLocale::territoryToCode(territory);
			if (code.size() == 2 && code.at(0).isUpper() && code.at(1).isUpper())
				countries.emplace_back(QLocale::territoryToString(territory), code);
		}
		std::sort(countries.begin(), countries.end());
		countries.erase(std::unique(countries.begin(), countries.end(),
							[](const auto& a, const auto& b) { return a.second == b.second; }),
			countries.end());
		m_country->addItem(tr("(not shown)"), QString());
		for (const auto& [name, code] : countries)
			m_country->addItem(QStringLiteral("%1  (%2)").arg(name, code), code);
		const std::string saved = Host::GetBaseStringSettingValue(SECTION, "Country", "auto");
		const QString wanted = (saved == "auto") ? QLocale::territoryToCode(QLocale::system().territory()) :
		                                           QString::fromStdString(saved);
		m_country->setCurrentIndex(std::max(m_country->findData(wanted), 0));
	}
	connect(m_country, &QComboBox::currentIndexChanged, this, [this]() {
		Host::SetBaseStringSettingValue(SECTION, "Country", m_country->currentData().toString().toUtf8().constData());
		Host::CommitBaseSettingChanges();
	});
	you->addWidget(m_country, 0, 3);
	you->setColumnStretch(1, 1);
	you->setColumnStretch(3, 1);
	online->addLayout(you);
	online->addSpacing(6);

	// ---- Rooms ----
	QHBoxLayout* rooms_header = new QHBoxLayout();
	rooms_header->addWidget(make_section(tr("ROOMS")));
	rooms_header->addStretch(1);
	m_online_count = new QLabel(online_page);
	m_online_count->setObjectName(QStringLiteral("Counter"));
	m_online_count->setToolTip(tr("Players with Mawarunco open on the room list, waiting in a room or in a match"));
	rooms_header->addWidget(m_online_count);
	online->addLayout(rooms_header);
	m_room_list = new QTreeWidget(online_page);
	m_room_list->setHeaderLabels({tr("HOST"), tr("COUNTRY"), tr("DELAY"), tr("ROLLBACK"), tr("ACCESS"), tr("OPEN FOR")});
	m_room_list->setRootIsDecorated(false);
	m_room_list->setUniformRowHeights(true);
	m_room_list->setAlternatingRowColors(true);
	m_room_list->setSelectionMode(QAbstractItemView::SingleSelection);
	m_room_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_room_list->header()->setStretchLastSection(false);
	m_room_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	for (int column = 1; column < 6; column++)
		m_room_list->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
	m_room_list->setMinimumHeight(170);
	online->addWidget(m_room_list);
	QHBoxLayout* rooms_status_row = new QHBoxLayout();
	m_rooms_status = new QLabel(online_page);
	m_rooms_status->setObjectName(QStringLiteral("Hint"));
	m_rooms_status->setTextFormat(Qt::PlainText);
	rooms_status_row->addWidget(m_rooms_status, 1);
	QPushButton* rooms_refresh = new QPushButton(tr("Refresh"), online_page);
	rooms_refresh->setObjectName(QStringLiteral("Tab"));
	rooms_status_row->addWidget(rooms_refresh);
	online->addLayout(rooms_status_row);
	m_join_room_button = make_action(tr("JOIN ROOM  (PLAYER 2)"));
	m_join_room_button->setEnabled(false);
	online->addWidget(m_join_room_button);
	online->addSpacing(4);

	// ---- Running matches: watch as a spectator ----
	online->addWidget(make_section(tr("MATCHES")));
	m_match_list = new QTreeWidget(online_page);
	m_match_list->setHeaderLabels({tr("MATCH"), tr("COUNTRY"), tr("ACCESS"), tr("SPECTATORS"), tr("RUNNING FOR")});
	m_match_list->setRootIsDecorated(false);
	m_match_list->setUniformRowHeights(true);
	m_match_list->setAlternatingRowColors(true);
	m_match_list->setSelectionMode(QAbstractItemView::SingleSelection);
	m_match_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_match_list->header()->setStretchLastSection(false);
	m_match_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	for (int column = 1; column < 5; column++)
		m_match_list->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
	m_match_list->setMinimumHeight(110);
	online->addWidget(m_match_list);
	m_watch_button = make_action(tr("WATCH MATCH  (SPECTATOR)"));
	m_watch_button->setToolTip(tr("Up to 4 spectators per match. You can also join a match that already runs: your game "
								  "catches up first, then you watch live."));
	m_watch_button->setEnabled(false);
	online->addWidget(m_watch_button);
	online->addSpacing(4);
	QHBoxLayout* create_row = new QHBoxLayout();
	m_room_code = new QLineEdit(online_page);
	m_room_code->setPlaceholderText(tr("Room code (optional, for a private room)"));
	m_room_code->setToolTip(tr("4 to 16 letters or digits. Only players who know the code can join."));
	m_room_code->setMaxLength(16);
	m_room_code->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[A-Za-z0-9]*")), m_room_code));
	create_row->addWidget(m_room_code, 1);
	m_create_room_button = make_action(tr("CREATE ROOM  (PLAYER 1)"));
	create_row->addWidget(m_create_room_button);
	online->addLayout(create_row);
	online->addWidget(make_hint(tr("The room list shows your name, country and settings, never your IP address. The player who "
								   "joins your room gets your IP address to connect to you, as with a direct connection. "
								   "Hosting needs UDP port %1 forwarded in your router.")
									.arg(port)));
	online->addSpacing(10);

	// ---- Settings the host decides (rooms and direct) ----
	online->addWidget(make_section(tr("WHEN YOU HOST")));
	QGridLayout* host_settings = new QGridLayout();
	host_settings->setHorizontalSpacing(0);
	host_settings->setVerticalSpacing(8);
	host_settings->addWidget(new QLabel(tr("Delay"), online_page), 0, 0);
	QHBoxLayout* delay_row = new QHBoxLayout();
	delay_row->setSpacing(0);
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
	QLabel* delay_hint = new QLabel(tr("   frames  -  1 recommended"), online_page);
	delay_hint->setObjectName(QStringLiteral("Hint"));
	delay_row->addWidget(delay_hint);
	delay_row->addStretch(1);
	host_settings->addLayout(delay_row, 0, 1);
	// Rollback depth: how many frames the game may predict before it waits for the other player.
	host_settings->addWidget(new QLabel(tr("Rollback"), online_page), 1, 0);
	QHBoxLayout* rollback_row = new QHBoxLayout();
	rollback_row->setSpacing(0);
	m_rollback_group = new QButtonGroup(this);
	const int saved_rollback = std::clamp(Host::GetBaseIntSettingValue(SECTION, "Rollback", DEFAULT_ROLLBACK), 1, MAX_ROLLBACK);
	for (int i = 1; i <= MAX_ROLLBACK; i++)
	{
		QPushButton* b = new QPushButton(QString::number(i), online_page);
		b->setObjectName(QStringLiteral("Delay"));
		b->setCheckable(true);
		b->setChecked(i == saved_rollback);
		b->setMinimumWidth(34);
		m_rollback_group->addButton(b, i);
		rollback_row->addWidget(b);
	}
	QLabel* rollback_hint = new QLabel(tr("   frames  -  more helps on high ping"), online_page);
	rollback_hint->setObjectName(QStringLiteral("Hint"));
	rollback_row->addWidget(rollback_hint);
	rollback_row->addStretch(1);
	host_settings->addLayout(rollback_row, 1, 1);
	host_settings->setColumnMinimumWidth(0, 80);
	online->addLayout(host_settings);
	online->addWidget(make_hint(tr("The host decides delay and rollback for both players; when you join, the host's values are used.")));
	m_show_ping = new QCheckBox(tr("Show the ping to the other player in the game"), online_page);
	m_show_ping->setChecked(Host::GetBaseBoolSettingValue(SECTION, "ShowPing", true));
	connect(m_show_ping, &QCheckBox::toggled, this, [](bool on) {
		Host::SetBaseBoolSettingValue(SECTION, "ShowPing", on);
		Host::CommitBaseSettingChanges();
	});
	online->addWidget(m_show_ping);
	online->addSpacing(10);

	// ---- Direct connection: host waits on the port, the joiner types the host's address ----
	online->addWidget(make_section(tr("DIRECT CONNECTION (IP)")));
	m_host_button = make_action(tr("HOST  (PLAYER 1)"));
	m_host_button->setToolTip(tr("Waits for the other player on UDP port %1.\nThe port must be forwarded in your router.").arg(port));
	online->addWidget(m_host_button);
	online->addWidget(make_hint(tr("Tell the other player your public IP address. Forward UDP port %1 in your router.").arg(port)));
	QHBoxLayout* join_row = new QHBoxLayout();
	m_host_address = new QLineEdit(online_page);
	m_host_address->setPlaceholderText(tr("Host IP address, e.g. 203.0.113.5"));
	m_host_address->setText(QString::fromStdString(Host::GetBaseStringSettingValue(SECTION, "HostAddress", "")));
	join_row->addWidget(m_host_address, 1);
	m_join_button = make_action(tr("JOIN  (PLAYER 2)"));
	join_row->addWidget(m_join_button);
	m_watch_direct_button = make_action(tr("WATCH"));
	m_watch_direct_button->setToolTip(tr("Watch the match of this host as a spectator (up to 4)."));
	join_row->addWidget(m_watch_direct_button);
	online->addLayout(join_row);
	online->addStretch(1);

	m_lobby = new LobbyClient(this);
	m_room_refresh = new QTimer(this);
	m_room_refresh->setInterval(5000);
	connect(m_room_refresh, &QTimer::timeout, this, &RollbackLauncher::refreshRooms);
	m_room_heartbeat = new QTimer(this);
	m_room_heartbeat->setInterval(10000);
	connect(m_room_heartbeat, &QTimer::timeout, this, &RollbackLauncher::sendHeartbeat);
	connect(rooms_refresh, &QPushButton::clicked, this, &RollbackLauncher::refreshRooms);
	connect(m_room_list, &QTreeWidget::itemSelectionChanged, this,
		[this]() { m_join_room_button->setEnabled(!m_room_list->selectedItems().isEmpty() && m_create_room_button->isEnabled()); });
	connect(m_room_list, &QTreeWidget::itemDoubleClicked, this, &RollbackLauncher::onJoinRoomClicked);
	connect(m_join_room_button, &QPushButton::clicked, this, &RollbackLauncher::onJoinRoomClicked);
	connect(m_match_list, &QTreeWidget::itemSelectionChanged, this,
		[this]() { m_watch_button->setEnabled(!m_match_list->selectedItems().isEmpty() && m_create_room_button->isEnabled()); });
	connect(m_match_list, &QTreeWidget::itemDoubleClicked, this, &RollbackLauncher::onWatchMatchClicked);
	connect(m_watch_button, &QPushButton::clicked, this, &RollbackLauncher::onWatchMatchClicked);
	connect(m_watch_direct_button, &QPushButton::clicked, this, &RollbackLauncher::onWatchDirectClicked);
	connect(m_create_room_button, &QPushButton::clicked, this, &RollbackLauncher::onCreateRoomClicked);
	connect(m_room_code, &QLineEdit::returnPressed, this, &RollbackLauncher::onCreateRoomClicked);

	// ---- Page: training ----------------------------------------------------------------------------------
	QVBoxLayout* training = nullptr;
	QFrame* training_page = make_page(tr("TRAINING"),
		tr("Practice on your own, offline: the game boots to the character select of practice mode, all "
		   "characters unlocked, with your button config."),
		&training);
	m_practice_button = make_action(tr("START PRACTICE"));
	training->addWidget(m_practice_button);
	training->addStretch(1);

	// ---- Page: button config ------------------------------------------------------------------------------
	// Like the game's CONTROLLER SETTINGS menu: one row per action with the PS2 button it is on. Click a row,
	// then press the button on your controller (the PCSX2 mapping of port 1 tells which PS2 button it is).
	QVBoxLayout* buttons = nullptr;
	QFrame* buttons_page = make_page(tr("BUTTON CONFIG"),
		tr("Click an action, then press the button for it on your controller, like in the game's menu. Set it "
		   "once: it is used for training and online, whether you are player 1 or player 2."),
		&buttons);

	// PlayStation button glyphs from PromptFont (SIL OFL, shipped with PCSX2); no artwork from the game.
	const int font_id = QFontDatabase::addApplicationFont(
		QString::fromStdString(EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "promptfont.otf")));
	const QStringList families = (font_id >= 0) ? QFontDatabase::applicationFontFamilies(font_id) : QStringList();
	m_glyph_font = families.isEmpty() ? QFont() : QFont(families.first());
	m_glyph_font.setPixelSize(22);

	QGridLayout* rows = new QGridLayout();
	rows->setHorizontalSpacing(12);
	rows->setVerticalSpacing(4);
	for (u32 action = 1; action < FucButtons::NUM_ACTIONS; action++)
	{
		QPushButton* row = new QPushButton(buttons_page);
		row->setObjectName(FucButtons::IsPracticeOnly(action) ? QStringLiteral("BindRowPractice") : QStringLiteral("BindRow"));
		row->setCheckable(true);
		row->setCursor(Qt::PointingHandCursor);
		row->setMinimumHeight(30);
		row->setAccessibleName(tr(FucButtons::ActionMenuName(action)));
		QHBoxLayout* row_layout = new QHBoxLayout(row);
		row_layout->setContentsMargins(12, 0, 10, 0);
		QLabel* name = new QLabel(tr(FucButtons::ActionMenuName(action)), row);
		name->setObjectName(QStringLiteral("BindName"));
		name->setAttribute(Qt::WA_TransparentForMouseEvents);
		row_layout->addWidget(name, 1);
		QLabel* glyph = new QLabel(row);
		glyph->setObjectName(QStringLiteral("BindGlyph"));
		glyph->setFont(m_glyph_font);
		glyph->setAttribute(Qt::WA_TransparentForMouseEvents);
		row_layout->addWidget(glyph);
		if (FucButtons::IsPracticeOnly(action))
			row->setToolTip(tr("Practice mode only"));
		m_action_rows[action] = row;
		m_action_glyphs[action] = glyph;
		const int index = static_cast<int>(action) - 1;
		rows->addWidget(row, index % ROWS_PER_COLUMN, index / ROWS_PER_COLUMN);
		connect(row, &QPushButton::clicked, this, [this, action]() {
			if (m_listening_action == static_cast<int>(action))
				stopListening();
			else
				startListening(action);
			// Qt toggled the button on click; the selection state is ours.
			m_action_rows[action]->setChecked(m_listening_action == static_cast<int>(action));
		});
	}
	buttons->addLayout(rows);

	QHBoxLayout* button_actions = new QHBoxLayout();
	m_bind_hint = new QLabel(buttons_page);
	m_bind_hint->setObjectName(QStringLiteral("Hint"));
	button_actions->addWidget(m_bind_hint, 1);
	QPushButton* reset_buttons = new QPushButton(tr("Reset to default"), buttons_page);
	reset_buttons->setObjectName(QStringLiteral("Tab"));
	button_actions->addWidget(reset_buttons);
	buttons->addLayout(button_actions);

	QHBoxLayout* controller_row = new QHBoxLayout();
	QLabel* controller_hint = new QLabel(
		tr("Buttons not recognized? Map your controller first: which of its buttons is which PS2 button."), buttons_page);
	controller_hint->setObjectName(QStringLiteral("Hint"));
	controller_hint->setWordWrap(true);
	controller_row->addWidget(controller_hint, 1);
	QPushButton* key_button = new QPushButton(tr("Map controller"), buttons_page);
	key_button->setObjectName(QStringLiteral("Tab"));
	controller_row->addWidget(key_button);
	buttons->addLayout(controller_row);
	buttons->addStretch(1);
	m_right_hold_timer = new QTimer(this);
	m_right_hold_timer->setSingleShot(true);
	m_right_hold_timer->setInterval(1000);
	connect(m_right_hold_timer, &QTimer::timeout, this, &RollbackLauncher::onRightHeld);
	loadButtonLayout();

	// ---- Page: display ------------------------------------------------------------------------------------
	QVBoxLayout* display = nullptr;
	QFrame* display_page = make_page(tr("DISPLAY"),
		tr("Resolution of the game picture (4:3, like the original). The game is rendered at least this sharp; "
		   "in a window it gets exactly this size, in fullscreen it fills the screen height."),
		&display);
	QHBoxLayout* res_row = new QHBoxLayout();
	res_row->setSpacing(0);
	QLabel* res_label = new QLabel(tr("Resolution"), display_page);
	res_label->setMinimumWidth(110);
	res_row->addWidget(res_label);
	m_resolution = new QComboBox(display_page);
	const std::string saved_res = Host::GetBaseStringSettingValue(SECTION, "Resolution", "");
	for (size_t i = 0; i < RESOLUTIONS.size(); i++)
	{
		const auto [w, h] = RESOLUTIONS[i];
		m_resolution->addItem(QStringLiteral("%1 x %2").arg(w).arg(h), static_cast<int>(i));
		if (saved_res == fmt::format("{}x{}", w, h))
			m_resolution->setCurrentIndex(static_cast<int>(i));
	}
	if (saved_res.empty())
		m_resolution->setCurrentIndex(DEFAULT_RESOLUTION);
	m_resolution->setMinimumWidth(180);
	res_row->addWidget(m_resolution);
	res_row->addStretch(1);
	display->addLayout(res_row);

	QHBoxLayout* mode_row = new QHBoxLayout();
	mode_row->setSpacing(0);
	QLabel* mode_label = new QLabel(tr("Mode"), display_page);
	mode_label->setMinimumWidth(110);
	mode_row->addWidget(mode_label);
	QButtonGroup* mode_group = new QButtonGroup(this);
	m_mode_window = new QPushButton(tr("Window"), display_page);
	m_mode_fullscreen = new QPushButton(tr("Fullscreen"), display_page);
	for (QPushButton* b : {m_mode_window, m_mode_fullscreen})
	{
		b->setObjectName(QStringLiteral("Delay"));
		b->setCheckable(true);
		b->setMinimumWidth(110);
		mode_group->addButton(b);
		mode_row->addWidget(b);
	}
	(Host::GetBaseBoolSettingValue(SECTION, "Fullscreen", false) ? m_mode_fullscreen : m_mode_window)->setChecked(true);
	mode_row->addStretch(1);
	display->addLayout(mode_row);

	m_display_info = new QLabel(display_page);
	m_display_info->setObjectName(QStringLiteral("Hint"));
	display->addWidget(m_display_info);
	display->addStretch(1);
	auto update_display_info = [this]() {
		const auto [w, h] = RESOLUTIONS[static_cast<size_t>(m_resolution->currentData().toInt())];
		const int scale = UpscaleForHeight(h);
		m_display_info->setText(tr("Rendered at %1x internal resolution (%2 x %3).   Alt+Enter switches window/fullscreen in game.")
									.arg(scale)
									.arg(640 * scale)
									.arg(480 * scale));
		Host::SetBaseStringSettingValue(SECTION, "Resolution", fmt::format("{}x{}", w, h).c_str());
		Host::SetBaseBoolSettingValue(SECTION, "Fullscreen", m_mode_fullscreen->isChecked());
		Host::CommitBaseSettingChanges();
	};
	connect(m_resolution, &QComboBox::currentIndexChanged, this, update_display_info);
	connect(mode_group, &QButtonGroup::buttonClicked, this, update_display_info);
	update_display_info();

	QPushButton* menu_online = add_menu_entry(tr("ONLINE"), online_scroll);
	add_menu_entry(tr("TRAINING"), training_page);
	add_menu_entry(tr("BUTTON CONFIG"), buttons_page);
	add_menu_entry(tr("DISPLAY"), display_page);
	menu->addStretch(1);
	QPushButton* menu_quit = add_menu_entry(tr("QUIT"), nullptr);
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
	auto make_browse = [this](const QString& tip) {
		QPushButton* b = new QPushButton(tr("Browse..."), this);
		b->setObjectName(QStringLiteral("Tab"));
		b->setToolTip(tip);
		b->setCursor(Qt::PointingHandCursor);
		return b;
	};
	QPushButton* bios_browse = make_browse(tr("Choose your PS2 BIOS file (it is copied into the \"bios\" folder)"));
	QPushButton* game_browse = make_browse(tr("Choose your disc image of Fate/unlimited codes (it stays where it is)"));
	footer->addWidget(m_bios_status);
	footer->addWidget(bios_browse);
	footer->addSpacing(16);
	footer->addWidget(m_game_status);
	footer->addWidget(game_browse);
	footer->addStretch(1);
	connect(bios_browse, &QPushButton::clicked, this, &RollbackLauncher::onBrowseBios);
	connect(game_browse, &QPushButton::clicked, this, &RollbackLauncher::onBrowseGame);
	QLabel* footer_hint = new QLabel(tr("Fullscreen: Alt+Enter   -   End a game: close the game window"), this);
	footer_hint->setObjectName(QStringLiteral("Hint"));
	footer->addWidget(footer_hint);
	// Problems: players send the newest performance report (no addresses, paths or names in it) and, for
	// desyncs, emulog.txt.
	QPushButton* logs_button = new QPushButton(tr("Logs"), this);
	logs_button->setObjectName(QStringLiteral("Tab"));
	logs_button->setCursor(Qt::PointingHandCursor);
	logs_button->setToolTip(tr("Opens the logs folder.\nStutter or slowdown: send the newest perf-....txt file.\n"
							   "Desync: also send emulog.txt (it contains the other player's IP address)."));
	connect(logs_button, &QPushButton::clicked, this, []() {
		QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(EmuFolders::Logs)));
	});
	footer->addWidget(logs_button);
	root->addLayout(footer);

	connect(m_host_button, &QPushButton::clicked, this, &RollbackLauncher::onHostClicked);
	connect(m_join_button, &QPushButton::clicked, this, &RollbackLauncher::onJoinClicked);
	connect(m_host_address, &QLineEdit::returnPressed, this, &RollbackLauncher::onJoinClicked);
	connect(m_practice_button, &QPushButton::clicked, this, &RollbackLauncher::onPracticeClicked);
	connect(key_button, &QPushButton::clicked, this, &RollbackLauncher::onKeyConfigClicked);
	connect(reset_buttons, &QPushButton::clicked, this, [this]() {
		stopListening();
		saveButtonLayout(FucButtons::Default());
	});
	connect(menu_quit, &QPushButton::clicked, this, &QWidget::close);
	connect(m_pages, &QStackedWidget::currentChanged, this, &RollbackLauncher::updateRoomRefresh);

	// Out of the way while a game runs, back when it has ended.
	connect(g_emu_thread, &EmuThread::onVMStarted, this, &QWidget::hide);
	connect(g_emu_thread, &EmuThread::onVMStarted, this, &RollbackLauncher::applyWindowSize);
	connect(g_emu_thread, &EmuThread::onVMStopped, this, &RollbackLauncher::showAgain);

	refreshStatus();
}

void RollbackLauncher::loadButtonLayout()
{
	// PromptFont glyphs in FucButtons button order: square, triangle, circle, cross, L1, L2, R1, R2.
	static constexpr std::array<const char*, FucButtons::NUM_BUTTONS> glyphs = {ICON_PF_BUTTON_SQUARE,
		ICON_PF_BUTTON_TRIANGLE, ICON_PF_BUTTON_CIRCLE, ICON_PF_BUTTON_CROSS, ICON_PF_LEFT_SHOULDER_L1,
		ICON_PF_LEFT_TRIGGER_L2, ICON_PF_RIGHT_SHOULDER_R1, ICON_PF_RIGHT_TRIGGER_R2};
	const FucButtons::Layout& layout = FucButtons::GetLocal();
	for (u32 action = 1; action < FucButtons::NUM_ACTIONS; action++)
	{
		const u32 button = FucButtons::ButtonForAction(layout, action);
		m_action_glyphs[action]->setText((button < FucButtons::NUM_BUTTONS) ? QString::fromUtf8(glyphs[button]) : QString());
		m_action_glyphs[action]->setToolTip((button < FucButtons::NUM_BUTTONS) ? tr(FucButtons::ButtonName(button)) : QString());
	}
}

void RollbackLauncher::saveButtonLayout(const FucButtons::Layout& layout)
{
	FucButtons::SetLocal(layout);
	Host::SetBaseStringSettingValue(SECTION, "ButtonLayout", FucButtons::ToString(layout).c_str());
	Host::CommitBaseSettingChanges();
	loadButtonLayout();
}

void RollbackLauncher::startListening(u32 action)
{
	if (action < 1 || action >= FucButtons::NUM_ACTIONS)
	{
		stopListening();
		return;
	}

	// Already listening: only move the selection (the hook and held buttons stay).
	if (m_listening_action >= 0)
	{
		m_right_hold_timer->stop();
		stopFade();
		m_action_rows[static_cast<size_t>(m_listening_action)]->setChecked(false);
	}
	else
	{
		m_pressed.clear();
		qApp->installEventFilter(this);
		InputManager::SetHook([this](InputBindingKey key, float value) {
			QMetaObject::invokeMethod(this, "onBindingInput", Qt::QueuedConnection, Q_ARG(InputBindingKey, key), Q_ARG(float, value));
			return InputInterceptHook::CallbackResult::StopProcessingEvent;
		});
	}

	m_listening_action = static_cast<int>(action);
	m_action_rows[action]->setChecked(true);
	m_bind_hint->setText(tr("Press a button for %1.   D-pad: move, hold right: clear, Esc: stop")
							 .arg(tr(FucButtons::ActionMenuName(action))));
}

void RollbackLauncher::stopListening()
{
	if (m_listening_action < 0)
		return;
	InputManager::RemoveHook();
	qApp->removeEventFilter(this);
	m_right_hold_timer->stop();
	stopFade();
	m_action_rows[static_cast<size_t>(m_listening_action)]->setChecked(false);
	m_listening_action = -1;
	m_pressed.clear();
	m_bind_hint->clear();
}

void RollbackLauncher::moveSelection(int delta)
{
	const int action = std::clamp(m_listening_action + delta, 1, static_cast<int>(FucButtons::NUM_ACTIONS) - 1);
	startListening(static_cast<u32>(action));
}

void RollbackLauncher::startFade()
{
	// The selected action's button glyph fades out while right is held; it is gone when the hold clears it.
	stopFade();
	QLabel* glyph = m_action_glyphs[static_cast<size_t>(m_listening_action)];
	if (glyph->text().isEmpty())
		return;
	QGraphicsOpacityEffect* effect = new QGraphicsOpacityEffect(glyph);
	glyph->setGraphicsEffect(effect); // the label owns the effect
	m_fading_glyph = glyph;
	m_fade = new QPropertyAnimation(effect, "opacity", this);
	m_fade->setDuration(m_right_hold_timer->interval());
	m_fade->setStartValue(1.0);
	m_fade->setEndValue(0.0);
	m_fade->start(QAbstractAnimation::DeleteWhenStopped);
}

void RollbackLauncher::stopFade()
{
	if (m_fade)
		m_fade->stop(); // deletes itself
	if (m_fading_glyph)
		m_fading_glyph->setGraphicsEffect(nullptr); // full opacity again
	m_fading_glyph = nullptr;
}

void RollbackLauncher::onRightHeld()
{
	// Holding right for a second clears the selected action (no button), like an empty slot in the game.
	if (m_listening_action < 0)
		return;
	FucButtons::Layout layout = FucButtons::GetLocal();
	const u32 button = FucButtons::ButtonForAction(layout, static_cast<u32>(m_listening_action));
	if (button < FucButtons::NUM_BUTTONS)
		layout[button] = 0;
	stopFade();
	saveButtonLayout(layout);
}

void RollbackLauncher::onBindingInput(InputBindingKey key, float value)
{
	if (m_listening_action < 0)
		return;

	// Press/release tracking per input (direction masked), so a held button does not trigger again on
	// the next row and a release can end the hold-right gesture.
	const u64 id = key.MaskDirection().bits;
	if (std::abs(value) < 0.5f)
	{
		const auto it = m_pressed.find(id);
		if (it == m_pressed.end())
			return;
		const Role role = it->second;
		m_pressed.erase(it);
		if (role == Role::Right && m_right_hold_timer->isActive())
		{
			// A short press to the right: next column.
			m_right_hold_timer->stop();
			stopFade();
			if (m_listening_action <= ROWS_PER_COLUMN)
				moveSelection(ROWS_PER_COLUMN);
		}
		return;
	}
	if (m_pressed.contains(id))
		return;

	// The binding string as PCSX2 stores it (half axes such as triggers carry their direction).
	InputBindingKey bound = key;
	bound.modifier = (value < 0.0f) ? InputModifier::Negate : InputModifier::None;
	const std::string binding = InputManager::ConvertInputBindingKeyToString(InputBindingInfo::Type::Button, bound);
	if (binding == "Keyboard/Escape")
	{
		stopListening();
		return;
	}
	auto mapped_to = [&binding](const char* pad_key) {
		for (const std::string& entry : Host::GetBaseStringListSetting("Pad1", pad_key))
		{
			if (entry == binding)
				return true;
		}
		return false;
	};

	// D-pad and left stick only navigate; they cannot be assigned.
	static constexpr std::pair<const char*, Role> nav[] = {{"Up", Role::Up}, {"Down", Role::Down}, {"Left", Role::Left},
		{"Right", Role::Right}, {"LUp", Role::Up}, {"LDown", Role::Down}, {"LLeft", Role::Left}, {"LRight", Role::Right}};
	for (const auto& [pad_key, role] : nav)
	{
		if (!mapped_to(pad_key))
			continue;
		m_pressed.emplace(id, role);
		switch (role)
		{
			case Role::Up:
				moveSelection(-1);
				break;
			case Role::Down:
				moveSelection(1);
				break;
			case Role::Left:
				if (m_listening_action > ROWS_PER_COLUMN)
					moveSelection(-ROWS_PER_COLUMN);
				break;
			case Role::Right:
				m_right_hold_timer->start();
				startFade();
				break;
			default:
				break;
		}
		return;
	}

	for (u32 button = 0; button < FucButtons::NUM_BUTTONS; button++)
	{
		if (!mapped_to(FucButtons::ButtonName(button)))
			continue;
		m_pressed.emplace(id, Role::Button);
		FucButtons::Layout layout = FucButtons::GetLocal();
		FucButtons::Assign(layout, button, static_cast<u32>(m_listening_action));
		saveButtonLayout(layout);
		// On to the next action; after the last one the prompt ends.
		if (m_listening_action + 1 < static_cast<int>(FucButtons::NUM_ACTIONS))
			moveSelection(1);
		else
			stopListening();
		return;
	}

	m_pressed.emplace(id, Role::Other);
	m_bind_hint->setText(tr("%1 is not one of the PS2 buttons the game lets you assign (square, triangle, circle, cross, "
							"L1, L2, R1, R2). Press another button, or map your controller first.")
							 .arg(QString::fromStdString(binding)));
}

bool RollbackLauncher::eventFilter(QObject* watched, QEvent* event)
{
	// While listening, keys go to the prompt instead of moving the focus between buttons: arrow keys move
	// like the d-pad, Esc stops. (Keyboard input does not reach PCSX2's input manager from this window.)
	// Claim the keys first, so no application shortcut (e.g. Esc in the hidden main window) takes them.
	if (m_listening_action >= 0 && event->type() == QEvent::ShortcutOverride)
	{
		switch (static_cast<QKeyEvent*>(event)->key())
		{
			case Qt::Key_Escape:
			case Qt::Key_Up:
			case Qt::Key_Down:
			case Qt::Key_Left:
			case Qt::Key_Right:
				event->accept();
				return true;
			default:
				break;
		}
	}
	if (m_listening_action >= 0 && event->type() == QEvent::KeyPress)
	{
		switch (static_cast<QKeyEvent*>(event)->key())
		{
			case Qt::Key_Escape:
				stopListening();
				return true;
			case Qt::Key_Up:
				moveSelection(-1);
				return true;
			case Qt::Key_Down:
				moveSelection(1);
				return true;
			case Qt::Key_Left:
				if (m_listening_action > ROWS_PER_COLUMN)
					moveSelection(-ROWS_PER_COLUMN);
				return true;
			case Qt::Key_Right:
				if (m_listening_action <= ROWS_PER_COLUMN)
					moveSelection(ROWS_PER_COLUMN);
				return true;
			default:
				break;
		}
	}
	return QWidget::eventFilter(watched, event);
}

void RollbackLauncher::hideEvent(QHideEvent* event)
{
	stopListening();
	QWidget::hideEvent(event);
	updateRoomRefresh();
}

void RollbackLauncher::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);
	updateRoomRefresh();
	if (m_room_refresh->isActive())
		refreshRooms();
}

void RollbackLauncher::applyWindowSize()
{
	// Window mode: the game window gets the chosen size in screen pixels, or the largest 4:3 size that fits
	// the screen's work area (minus room for the title bar).
	if (m_window_size.isEmpty())
		return;
	int w = m_window_size.width();
	int h = m_window_size.height();
	if (const QScreen* screen = this->screen())
	{
		const QRect area = screen->availableGeometry();
		const qreal dpr = screen->devicePixelRatio();
		const int max_w = static_cast<int>(area.width() * dpr);
		const int max_h = static_cast<int>((area.height() - 40) * dpr);
		if (w > max_w || h > max_h)
		{
			h = std::min(max_h, max_w * 3 / 4);
			w = h * 4 / 3;
		}
	}
	emit g_emu_thread->onResizeRenderWindowRequested(w, h);
}

int RollbackLauncher::selectedRollback() const
{
	const int id = m_rollback_group->checkedId();
	return (id >= 1) ? id : DEFAULT_ROLLBACK;
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
		m_bios_status->setText(QStringLiteral("<span style='color:#ff6b6b'>&#9679;</span> ") +
							   tr("BIOS missing - choose your BIOS file"));
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
								   QStringLiteral("<span style='color:#f0b429'>&#9679;</span> ") +
									   tr("BIOS %1 - another version than the organizer's, online games may desync")
										   .arg(short_description.toHtmlEscaped()) :
								   QStringLiteral("<span style='color:#6fd08c'>&#9679;</span> BIOS %1").arg(short_description.toHtmlEscaped()));
		m_bios_status->setToolTip(
			differs ? tr("%1\nOrganizer: %2").arg(QString::fromStdString(m_bios_file), QString::fromStdString(reference)) :
					  QString::fromStdString(m_bios_file));
	}

	// Game: the chosen disc image ([Launcher] GamePath, set by the installer or "Browse..."), otherwise the
	// first disc image in the "game" folder next to the emulator.
	m_game_path.clear();
	if (const std::string chosen = Host::GetBaseStringSettingValue(SECTION, "GamePath", "");
		!chosen.empty() && FileSystem::FileExists(chosen.c_str()))
	{
		m_game_path = chosen;
	}
	const std::string game_dir = Path::Combine(EmuFolders::AppRoot, "game");
	FileSystem::FindResultsArray files;
	FileSystem::FindFiles(game_dir.c_str(), "*",
		FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE | FILESYSTEM_FIND_SORT_BY_NAME, &files);
	static constexpr std::array<const char*, 5> extensions = {".iso", ".chd", ".cso", ".zso", ".cue"};
	for (const FILESYSTEM_FIND_DATA& fd : files)
	{
		if (!m_game_path.empty())
			break;
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
							   QStringLiteral("<span style='color:#ff6b6b'>&#9679;</span> ") +
								   tr("Game missing - choose your disc image") :
							   QStringLiteral("<span style='color:#6fd08c'>&#9679;</span> ") + tr("Game %1")
								   .arg(QString::fromStdString(std::string(Path::GetFileName(m_game_path))).toHtmlEscaped()));

	return !m_bios_file.empty() && !m_game_path.empty();
}

void RollbackLauncher::importInstallerChoices()
{
	// The installer leaves the player's choices in inis\launcher-setup.txt (UTF-8, "key=value" lines;
	// Inno Setup cannot write the UTF-8 settings file itself). Taken over once, then deleted.
	QFile file(QString::fromStdString(Path::Combine(EmuFolders::Settings, "launcher-setup.txt")));
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		return;
	const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
	file.close();
	for (QString line : lines)
	{
		line = line.trimmed();
		if (line.startsWith(QChar(0xFEFF)))
			line.remove(0, 1);
		const qsizetype eq = line.indexOf(QLatin1Char('='));
		if (eq <= 0)
			continue;
		const QString key = line.left(eq).trimmed();
		const QByteArray value = line.mid(eq + 1).trimmed().toUtf8();
		if (key == QLatin1String("GamePath") && !value.isEmpty())
			Host::SetBaseStringSettingValue(SECTION, "GamePath", value.constData());
		else if (key == QLatin1String("BIOS") && !value.isEmpty())
			Host::SetBaseStringSettingValue("Filenames", "BIOS", value.constData());
	}
	Host::CommitBaseSettingChanges();
	file.remove();
}

void RollbackLauncher::onBrowseBios()
{
	const QString file = QFileDialog::getOpenFileName(this, tr("Choose your PS2 BIOS"), QString(),
		tr("PS2 BIOS (*.bin *.rom *.rom0);;All files (*.*)"));
	if (file.isEmpty())
		return;
	const std::string path = QDir::toNativeSeparators(file).toStdString();
	u32 version, region;
	std::string description, zone;
	if (!IsBIOS(path.c_str(), version, description, region, zone))
	{
		QMessageBox::warning(this, windowTitle(), tr("This file is not a PS2 BIOS."));
		return;
	}
	// Copied next to the emulator, so the setup keeps working when the original file moves.
	const std::string name(Path::GetFileName(path));
	const std::string target = Path::Combine(EmuFolders::Bios, name);
	FileSystem::EnsureDirectoryExists(EmuFolders::Bios.c_str(), false);
	if (path != target && !FileSystem::CopyFilePath(path.c_str(), target.c_str(), true))
	{
		QMessageBox::warning(this, windowTitle(), tr("Could not copy the BIOS into the \"bios\" folder."));
		return;
	}
	Host::SetBaseStringSettingValue("Filenames", "BIOS", name.c_str());
	Host::CommitBaseSettingChanges();
	refreshStatus();
}

void RollbackLauncher::onBrowseGame()
{
	const QString file = QFileDialog::getOpenFileName(this, tr("Choose your disc image of Fate/unlimited codes"),
		QString(), tr("Disc images (*.iso *.chd *.cso *.zso *.cue);;All files (*.*)"));
	if (file.isEmpty())
		return;
	Host::SetBaseStringSettingValue(SECTION, "GamePath", QDir::toNativeSeparators(file).toUtf8().constData());
	Host::CommitBaseSettingChanges();
	refreshStatus();
}

void RollbackLauncher::showAgain()
{
	// The game has ended (or did not start): an own room is no longer open.
	closeOwnRoom();
	refreshStatus();
	for (QPushButton* b : {m_host_button, m_join_button, m_watch_direct_button, m_practice_button, m_create_room_button})
		b->setEnabled(true);
	m_join_room_button->setEnabled(!m_room_list->selectedItems().isEmpty());
	m_watch_button->setEnabled(!m_match_list->selectedItems().isEmpty());
	show();
	raise();
	activateWindow();
}

void RollbackLauncher::closeEvent(QCloseEvent* event)
{
	// Closing the launcher closes the program; take an own room off the list first (the lobby would drop it
	// after 30 seconds anyway).
	if (!m_room_id.isEmpty())
	{
		closeOwnRoom();
		m_lobby->flush();
	}
	event->accept();
	g_main_window->requestExit(false);
}

void RollbackLauncher::updateRoomRefresh()
{
	const bool shown = isVisible() && m_pages->currentWidget() == m_online_page;
	if (shown && !m_room_refresh->isActive())
		m_room_refresh->start();
	else if (!shown)
		m_room_refresh->stop();
}

void RollbackLauncher::refreshRooms()
{
	if (m_rooms_pending)
		return;
	m_rooms_pending = true;
	if (m_room_list->topLevelItemCount() == 0)
		m_rooms_status->setText(tr("Loading rooms ..."));
	m_lobby->listRooms([this](LobbyClient::Result<LobbyClient::RoomList> result) {
		m_rooms_pending = false;
		if (!result.value)
		{
			m_rooms_status->setText(result.error);
			return;
		}
		// Keep the selection across refreshes.
		const QList<QTreeWidgetItem*> selection = m_room_list->selectedItems();
		const QString selected_id = selection.isEmpty() ? QString() : selection.first()->data(0, Qt::UserRole).toString();
		m_room_list->clear();
		for (const LobbyClient::Room& room : result.value->rooms)
		{
			// Names are plain item text (never rich text).
			QTreeWidgetItem* item = new QTreeWidgetItem(m_room_list);
			item->setText(0, room.host);
			item->setText(1, room.country.isEmpty() ? QStringLiteral("-") : room.country);
			if (!room.country.isEmpty())
				item->setToolTip(1, QLocale::territoryToString(QLocale::codeToTerritory(room.country)));
			item->setText(2, QString::number(room.delay));
			item->setText(3, QString::number(room.rollback));
			item->setText(4, room.locked ? tr("code") : tr("open"));
			item->setText(5, (room.age_seconds < 60) ? tr("%1 s").arg(room.age_seconds) : tr("%1 min").arg(room.age_seconds / 60));
			item->setData(0, Qt::UserRole, room.id);
			item->setData(0, Qt::UserRole + 1, room.locked);
			for (int column = 1; column < 6; column++)
				item->setTextAlignment(column, Qt::AlignCenter);
			if (room.id == selected_id)
				item->setSelected(true);
		}
		const QList<QTreeWidgetItem*> match_selection = m_match_list->selectedItems();
		const QString selected_match = match_selection.isEmpty() ? QString() : match_selection.first()->data(0, Qt::UserRole).toString();
		m_match_list->clear();
		for (const LobbyClient::Match& match : result.value->matches)
		{
			QTreeWidgetItem* item = new QTreeWidgetItem(m_match_list);
			item->setText(0, match.guest.isEmpty() ? match.host : tr("%1  vs  %2").arg(match.host, match.guest));
			item->setText(1, match.country.isEmpty() ? QStringLiteral("-") : match.country);
			if (!match.country.isEmpty())
				item->setToolTip(1, QLocale::territoryToString(QLocale::codeToTerritory(match.country)));
			item->setText(2, match.locked ? tr("code") : tr("open"));
			item->setText(3, QStringLiteral("%1 / 4").arg(match.spectators));
			item->setText(4, (match.age_seconds < 60) ? tr("%1 s").arg(match.age_seconds) : tr("%1 min").arg(match.age_seconds / 60));
			item->setData(0, Qt::UserRole, match.id);
			item->setData(0, Qt::UserRole + 1, match.locked);
			item->setData(0, Qt::UserRole + 2, match.spectators);
			for (int column = 1; column < 5; column++)
				item->setTextAlignment(column, Qt::AlignCenter);
			if (match.id == selected_match)
				item->setSelected(true);
		}
		m_watch_button->setEnabled(!m_match_list->selectedItems().isEmpty() && m_create_room_button->isEnabled());

		// Numbers only (no names), so rich text is safe here.
		m_online_count->setText(QStringLiteral("<span style='color:#6fd08c'>&#9679;</span> ") +
			((result.value->online == 1) ? tr("1 player online") : tr("%1 players online").arg(result.value->online)));
		const int count = static_cast<int>(result.value->rooms.size());
		QString text = (count == 0) ? tr("No open rooms. Create one, or wait for others.") : ((count == 1) ? tr("1 open room.") : tr("%1 open rooms.").arg(count));
		if (result.value->playing > 0)
			text += QLatin1Char(' ') + ((result.value->playing == 1) ? tr("1 match running.") : tr("%1 matches running.").arg(result.value->playing));
		m_rooms_status->setText(text);
		m_join_room_button->setEnabled(!m_room_list->selectedItems().isEmpty() && m_create_room_button->isEnabled());
	});
}

std::string RollbackLauncher::lobbyName()
{
	const std::string name = FucNames::Sanitize(m_player_name->text().toStdString());
	if (name.empty())
	{
		QMessageBox::information(this, windowTitle(), tr("Please enter your name first. It is shown in the room list."));
		m_player_name->setFocus();
	}
	return name;
}

void RollbackLauncher::onCreateRoomClicked()
{
	if (QtHost::IsVMValid() || !m_room_id.isEmpty() || !m_create_room_button->isEnabled())
		return;
	if (!refreshStatus())
	{
		QMessageBox::warning(this, windowTitle(), tr("BIOS or game missing (see below)."));
		return;
	}
	const std::string name = lobbyName();
	if (name.empty())
		return;
	const std::string code = m_room_code->text().trimmed().toStdString();
	if (!code.empty() && (code.size() < 4 || code.size() > 16))
	{
		QMessageBox::information(this, windowTitle(), tr("A room code has 4 to 16 letters or digits."));
		m_room_code->setFocus();
		return;
	}
	const int port = Host::GetBaseIntSettingValue(SECTION, "Port", DEFAULT_PORT);
	const int delay = selectedDelay();
	const int rollback = selectedRollback();
	m_create_room_button->setEnabled(false);
	m_join_room_button->setEnabled(false);
	m_rooms_status->setText(tr("Creating the room ..."));
	m_lobby->createRoom(name, m_country->currentData().toString().toStdString(), delay, rollback, port, code,
		[this, port, delay, rollback](LobbyClient::Result<LobbyClient::CreatedRoom> result) {
			if (!result.value)
			{
				m_create_room_button->setEnabled(true);
				m_join_room_button->setEnabled(!m_room_list->selectedItems().isEmpty());
				m_rooms_status->setText(result.error);
				QMessageBox::warning(this, windowTitle(), tr("Could not create the room: %1").arg(result.error));
				return;
			}
			m_room_id = result.value->id;
			m_room_token = result.value->token;
			m_room_guest.clear();
			m_room_heartbeat->start();
			// Same as HOST in the direct tab: wait for the joiner on the port.
			if (!boot(fmt::format("host={},delay={},rollback={},boot", port, delay, rollback), nullptr))
			{
				closeOwnRoom();
				m_create_room_button->setEnabled(true);
				m_join_room_button->setEnabled(!m_room_list->selectedItems().isEmpty());
			}
		});
}

void RollbackLauncher::sendHeartbeat()
{
	if (m_room_id.isEmpty())
	{
		m_room_heartbeat->stop();
		return;
	}
	const int spectators = Netplay::IsConnected() ? static_cast<int>(Netplay::SpectatorCount()) : -1;
	m_lobby->heartbeat(m_room_id, m_room_token, spectators, [this, id = m_room_id](LobbyClient::Result<LobbyClient::Heartbeat> result) {
		if (id != m_room_id)
			return; // answer for a room that is already closed
		if (!result.value)
		{
			// Keep trying; the lobby drops the room after 30 s without heartbeat.
			Console.Warning(fmt::format("Lobby: heartbeat failed: {}", result.error.toStdString()));
			return;
		}
		if (result.value->playing && !result.value->guest.isEmpty() && result.value->guest != m_room_guest)
		{
			m_room_guest = result.value->guest;
			m_guest_wait.start();
			Host::AddIconOSDMessage("LobbyGuest", ICON_FA_NETWORK_WIRED,
				fmt::format("Lobby: {} is joining your room.", m_room_guest.toStdString()), Host::OSD_INFO_DURATION);
		}
		else if (result.value->playing && !m_room_guest.isEmpty() && !Netplay::IsConnected() &&
				 m_guest_wait.isValid() && m_guest_wait.elapsed() > 40000)
		{
			// The joiner took the room but never connected (gave up, blocked, or just took it): list the
			// room again, so the host is not stuck with a room nobody can join.
			Host::AddIconOSDMessage("LobbyGuest", ICON_FA_NETWORK_WIRED,
				fmt::format("Lobby: {} did not connect. Your room is open again.", m_room_guest.toStdString()),
				Host::OSD_INFO_DURATION);
			m_lobby->reopenRoom(m_room_id, m_room_token);
			m_room_guest.clear();
			m_guest_wait.invalidate();
		}
	});
}

void RollbackLauncher::closeOwnRoom()
{
	m_room_heartbeat->stop();
	if (m_room_id.isEmpty())
		return;
	m_lobby->closeRoom(m_room_id, m_room_token);
	m_room_id.clear();
	m_room_token.clear();
	m_room_guest.clear();
	m_guest_wait.invalidate();
}

void RollbackLauncher::onJoinRoomClicked()
{
	if (QtHost::IsVMValid() || !m_room_id.isEmpty() || m_room_list->selectedItems().isEmpty() ||
		!m_create_room_button->isEnabled())
	{
		return;
	}
	if (!refreshStatus())
	{
		QMessageBox::warning(this, windowTitle(), tr("BIOS or game missing (see below)."));
		return;
	}
	const std::string name = lobbyName();
	if (name.empty())
		return;
	const QTreeWidgetItem* item = m_room_list->selectedItems().first();
	const QString id = item->data(0, Qt::UserRole).toString();
	std::string code;
	if (item->data(0, Qt::UserRole + 1).toBool())
	{
		bool ok = false;
		const QString entered = QInputDialog::getText(this, windowTitle(), tr("Room code for the room of %1:").arg(item->text(0)),
			QLineEdit::Normal, QString(), &ok).trimmed();
		if (!ok)
			return;
		static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9]{4,16}$"));
		if (!valid.match(entered).hasMatch())
		{
			QMessageBox::information(this, windowTitle(), tr("A room code has 4 to 16 letters or digits."));
			return;
		}
		code = entered.toStdString();
	}
	m_create_room_button->setEnabled(false);
	m_join_room_button->setEnabled(false);
	m_rooms_status->setText(tr("Joining ..."));
	m_lobby->joinRoom(id, name, code, [this](LobbyClient::Result<LobbyClient::JoinInfo> result) {
		if (!result.value)
		{
			m_create_room_button->setEnabled(true);
			m_join_room_button->setEnabled(!m_room_list->selectedItems().isEmpty());
			m_rooms_status->setText(result.error);
			QMessageBox::warning(this, windowTitle(), tr("Could not join the room: %1").arg(result.error));
			refreshRooms();
			return;
		}
		// Same as JOIN in the direct tab, with the address from the lobby (not shown, not stored).
		const LobbyClient::JoinInfo& info = *result.value;
		if (!boot(fmt::format("join={}:{},delay={},rollback={},boot", info.address, info.port, info.delay, info.rollback), nullptr))
		{
			m_create_room_button->setEnabled(true);
			m_join_room_button->setEnabled(!m_room_list->selectedItems().isEmpty());
		}
	});
}

void RollbackLauncher::onWatchMatchClicked()
{
	if (QtHost::IsVMValid() || !m_room_id.isEmpty() || m_match_list->selectedItems().isEmpty() ||
		!m_create_room_button->isEnabled())
	{
		return;
	}
	if (!refreshStatus())
	{
		QMessageBox::warning(this, windowTitle(), tr("BIOS or game missing (see below)."));
		return;
	}
	const QTreeWidgetItem* item = m_match_list->selectedItems().first();
	if (item->data(0, Qt::UserRole + 2).toInt() >= 4)
	{
		QMessageBox::information(this, windowTitle(), tr("Four spectators already watch this match."));
		return;
	}
	const QString id = item->data(0, Qt::UserRole).toString();
	std::string code;
	if (item->data(0, Qt::UserRole + 1).toBool())
	{
		bool ok = false;
		const QString entered = QInputDialog::getText(this, windowTitle(), tr("Room code of %1:").arg(item->text(0)),
			QLineEdit::Normal, QString(), &ok).trimmed();
		if (!ok)
			return;
		static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9]{4,16}$"));
		if (!valid.match(entered).hasMatch())
		{
			QMessageBox::information(this, windowTitle(), tr("A room code has 4 to 16 letters or digits."));
			return;
		}
		code = entered.toStdString();
	}
	m_watch_button->setEnabled(false);
	m_rooms_status->setText(tr("Connecting as a spectator ..."));
	m_lobby->watchRoom(id, code, [this](LobbyClient::Result<LobbyClient::WatchInfo> result) {
		if (!result.value)
		{
			m_watch_button->setEnabled(!m_match_list->selectedItems().isEmpty());
			m_rooms_status->setText(result.error);
			QMessageBox::warning(this, windowTitle(), tr("Could not watch the match: %1").arg(result.error));
			refreshRooms();
			return;
		}
		// The address from the lobby is not shown or stored.
		const LobbyClient::WatchInfo& info = *result.value;
		if (!boot(fmt::format("spectate={}:{},boot", info.address, info.port), nullptr))
			m_watch_button->setEnabled(!m_match_list->selectedItems().isEmpty());
	});
}

void RollbackLauncher::onWatchDirectClicked()
{
	std::string address = m_host_address->text().trimmed().toStdString();
	static const QRegularExpression valid(QStringLiteral("^[0-9A-Fa-f.:\\[\\]]+$"));
	if (address.empty() || !valid.match(QString::fromStdString(address)).hasMatch())
	{
		QMessageBox::warning(this, windowTitle(), tr("Please enter the host's IP address, e.g. 203.0.113.5"));
		m_host_address->setFocus();
		return;
	}
	const bool ipv6 = address.starts_with('[');
	const bool has_port = ipv6 ? (address.find("]:") != std::string::npos) : (address.find(':') != std::string::npos);
	if (!has_port)
		address += fmt::format(":{}", Host::GetBaseIntSettingValue(SECTION, "Port", DEFAULT_PORT));
	boot(fmt::format("spectate={},boot", address), nullptr);
}

void RollbackLauncher::onHostClicked()
{
	const int port = Host::GetBaseIntSettingValue(SECTION, "Port", DEFAULT_PORT);
	boot(fmt::format("host={},delay={},rollback={},boot", port, selectedDelay(), selectedRollback()),
		nullptr);
}

void RollbackLauncher::onJoinClicked()
{
	std::string address = m_host_address->text().trimmed().toStdString();
	// Only an IP address (IPv4 or [IPv6]) with an optional port.
	static const QRegularExpression valid(QStringLiteral("^[0-9A-Fa-f.:\\[\\]]+$"));
	if (address.empty() || !valid.match(QString::fromStdString(address)).hasMatch())
	{
		QMessageBox::warning(this, windowTitle(), tr("Please enter the host's IP address, e.g. 203.0.113.5"));
		m_host_address->setFocus();
		return;
	}
	const bool ipv6 = address.starts_with('[');
	const bool has_port = ipv6 ? (address.find("]:") != std::string::npos) : (address.find(':') != std::string::npos);
	if (!has_port)
		address += fmt::format(":{}", Host::GetBaseIntSettingValue(SECTION, "Port", DEFAULT_PORT));

	boot(fmt::format("join={},delay={},rollback={},boot", address, selectedDelay(), selectedRollback()),
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

bool RollbackLauncher::boot(const std::string& netplay_spec, const char* shortcut)
{
	if (!refreshStatus())
	{
		QMessageBox::warning(this, windowTitle(), tr("BIOS or game missing (see below)."));
		return false;
	}
	if (QtHost::IsVMValid())
		return false;

	// Remembered for the next start.
	Host::SetBaseIntSettingValue(SECTION, "Delay", selectedDelay());
	Host::SetBaseIntSettingValue(SECTION, "Rollback", selectedRollback());
	Host::SetBaseStringSettingValue(SECTION, "HostAddress", m_host_address->text().trimmed().toUtf8().constData());
	const std::string name = FucNames::Sanitize(m_player_name->text().toStdString());
	Host::SetBaseStringSettingValue(SECTION, "PlayerName", name.c_str());
	FucNames::SetLocal(name);

	// Everything the modes rely on: the found BIOS, a DualShock 2 on both ports (netplay injects both),
	// the game in its own window while the launcher is hidden.
	Host::SetBaseStringSettingValue("Filenames", "BIOS", m_bios_file.c_str());
	Host::SetBaseStringSettingValue("Pad1", "Type", "DualShock2");
	Host::SetBaseStringSettingValue("Pad2", "Type", "DualShock2");
	// Display: 4:3, internal resolution for the chosen height, window or fullscreen.
	const auto [res_w, res_h] = RESOLUTIONS[static_cast<size_t>(m_resolution->currentData().toInt())];
	const bool fullscreen = m_mode_fullscreen->isChecked();
	Host::SetBaseStringSettingValue("EmuCore/GS", "AspectRatio", "4:3");
	Host::SetBaseFloatSettingValue("EmuCore/GS", "upscale_multiplier", static_cast<float>(UpscaleForHeight(res_h)));
	Host::SetBaseBoolSettingValue("UI", "StartFullscreen", fullscreen);
	m_window_size = fullscreen ? QSize() : QSize(res_w, res_h);
	Host::SetBaseBoolSettingValue("UI", "RenderToSeparateWindow", true);
	Host::SetBaseBoolSettingValue("UI", "HideMainWindowWhenRunning", true);
	Host::CommitBaseSettingChanges();
	g_emu_thread->applySettings();

	Error error;
	const std::string spec = (!netplay_spec.empty() && m_show_ping->isChecked()) ? netplay_spec + ",ping" : netplay_spec;
	if (!spec.empty() && !Netplay::SetLaunchOptions(spec, &error))
	{
		QMessageBox::critical(this, windowTitle(), QString::fromStdString(error.GetDescription()));
		return false;
	}
	const char* script = shortcut ? shortcut : "vs2p";
	if (netplay_spec.empty() && !BootShortcut::Request(script, &error))
	{
		QMessageBox::critical(this, windowTitle(), QString::fromStdString(error.GetDescription()));
		return false;
	}

	// Stays visible until the game runs (onVMStarted hides it). If the boot fails, the buttons come back.
	for (QPushButton* b : {m_host_button, m_join_button, m_watch_direct_button, m_practice_button, m_create_room_button,
			 m_join_room_button, m_watch_button})
		b->setEnabled(false);
	QTimer::singleShot(15000, this, [this]() {
		if (!QtHost::IsVMValid() && isVisible())
			showAgain();
	});

	auto params = std::make_shared<VMBootParameters>();
	params->filename = m_game_path;
	params->fullscreen = fullscreen;
	g_emu_thread->startVM(std::move(params));
	return true;
}

#include "moc_RollbackLauncher.cpp"
