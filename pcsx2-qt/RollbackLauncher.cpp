// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "RollbackLauncher.h"
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

#include "IconsPromptFont.h"

#include "common/Error.h"
#include "common/MD5Digest.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <QtCore/QPropertyAnimation>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtGui/QCloseEvent>
#include <QtGui/QFontDatabase>
#include <QtGui/QHideEvent>
#include <QtGui/QRegularExpressionValidator>
#include <QtGui/QKeyEvent>
#include <QtCore/QSignalBlocker>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QGraphicsOpacityEffect>
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
)";

RollbackLauncher::RollbackLauncher()
{
	// All texts go through tr() (context "RollbackLauncher"), so translations can be added later.
	setObjectName(QStringLiteral("RollbackLauncher"));
	setAttribute(Qt::WA_StyledBackground, true);
	setStyleSheet(QString::fromUtf8(LAUNCHER_STYLE));
	setWindowTitle(tr("Fate/unlimited codes - Rollback"));
	setWindowIcon(QtHost::GetAppIcon());
	setMinimumSize(900, 600);

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
	QLabel* subtitle = new QLabel(tr("ROLLBACK NETPLAY"), this);
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
	QFrame* online_page = make_page(tr("ONLINE  -  VS. 2P"),
		tr("Both players boot their own copy of the game to the character select, all characters unlocked. "
		   "Each player plays with their own button config."),
		&online);

	// Direct IP now, rooms (lobby server) later.
	QHBoxLayout* tabs = new QHBoxLayout();
	tabs->setSpacing(0);
	QPushButton* tab_direct = new QPushButton(tr("Direct (IP)"), online_page);
	tab_direct->setObjectName(QStringLiteral("Tab"));
	tab_direct->setCheckable(true);
	tab_direct->setChecked(true);
	QPushButton* tab_rooms = new QPushButton(tr("Rooms  (coming soon)"), online_page);
	tab_rooms->setObjectName(QStringLiteral("Tab"));
	tab_rooms->setEnabled(false);
	tab_rooms->setToolTip(tr("Rooms on a lobby server, without port forwarding. Coming later."));
	tabs->addWidget(tab_direct);
	tabs->addWidget(tab_rooms);
	tabs->addStretch(1);
	online->addLayout(tabs);

	// Player name, shown above the health bar in versus (instead of the win count) for both players.
	QHBoxLayout* name_row = new QHBoxLayout();
	name_row->setSpacing(0);
	QLabel* name_label = new QLabel(tr("Name"), online_page);
	name_label->setMinimumWidth(70);
	name_row->addWidget(name_label);
	m_player_name = new QLineEdit(online_page);
	m_player_name->setMaxLength(static_cast<int>(FucNames::MAX_LENGTH));
	m_player_name->setMaximumWidth(220);
	m_player_name->setPlaceholderText(tr("YOUR NAME"));
	m_player_name->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[A-Za-z0-9 .!?-]*")), m_player_name));
	m_player_name->setText(QString::fromStdString(FucNames::Sanitize(Host::GetBaseStringSettingValue(SECTION, "PlayerName", ""))));
	connect(m_player_name, &QLineEdit::textEdited, this, [this](const QString& text) {
		// The game's font has upper-case letters only.
		const int cursor = m_player_name->cursorPosition();
		m_player_name->setText(text.toUpper());
		m_player_name->setCursorPosition(cursor);
	});
	name_row->addWidget(m_player_name);
	QLabel* name_hint = new QLabel(tr("   shown above your health bar"), online_page);
	name_hint->setObjectName(QStringLiteral("Hint"));
	name_row->addWidget(name_hint);
	name_row->addStretch(1);
	online->addLayout(name_row);

	QHBoxLayout* delay_row = new QHBoxLayout();
	delay_row->setSpacing(0);
	QLabel* delay_label = new QLabel(tr("Delay"), online_page);
	delay_label->setMinimumWidth(70);
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
	QLabel* delay_hint = new QLabel(tr("   frames  -  1 recommended, both the same"), online_page);
	delay_hint->setObjectName(QStringLiteral("Hint"));
	delay_row->addWidget(delay_hint);
	delay_row->addStretch(1);
	online->addLayout(delay_row);
	online->addSpacing(6);

	const int port = Host::GetBaseIntSettingValue(SECTION, "Port", DEFAULT_PORT);
	m_host_button = make_action(tr("HOST  (PLAYER 1)"));
	m_host_button->setToolTip(tr("Waits for the other player on UDP port %1.\nThe port must be forwarded in your router.").arg(port));
	online->addWidget(m_host_button);
	QLabel* host_hint = new QLabel(tr("Tell the other player your public IP address. Forward UDP port %1 in your router.").arg(port),
		online_page);
	host_hint->setObjectName(QStringLiteral("Hint"));
	online->addWidget(host_hint);
	online->addSpacing(6);

	QHBoxLayout* join_row = new QHBoxLayout();
	m_host_address = new QLineEdit(online_page);
	m_host_address->setPlaceholderText(tr("Host IP address, e.g. 203.0.113.5"));
	m_host_address->setText(QString::fromStdString(Host::GetBaseStringSettingValue(SECTION, "HostAddress", "")));
	join_row->addWidget(m_host_address, 1);
	m_join_button = make_action(tr("JOIN  (PLAYER 2)"));
	join_row->addWidget(m_join_button);
	online->addLayout(join_row);
	online->addStretch(1);

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

	QPushButton* menu_online = add_menu_entry(tr("ONLINE"), online_page);
	add_menu_entry(tr("TRAINING"), training_page);
	add_menu_entry(tr("BUTTON CONFIG"), buttons_page);
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
	footer->addWidget(m_bios_status);
	footer->addWidget(m_game_status);
	footer->addStretch(1);
	QLabel* footer_hint = new QLabel(tr("Fullscreen: Alt+Enter   -   End a game: close the game window"), this);
	footer_hint->setObjectName(QStringLiteral("Hint"));
	footer->addWidget(footer_hint);
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

	// Out of the way while a game runs, back when it has ended.
	connect(g_emu_thread, &EmuThread::onVMStarted, this, &QWidget::hide);
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
							   tr("BIOS missing - copy your own BIOS file (4 MB) into the \"bios\" folder"));
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
							   QStringLiteral("<span style='color:#ff6b6b'>&#9679;</span> ") +
								   tr("Game missing - copy your own disc image (.iso/.chd/.cso/.cue) into the \"game\" folder") :
							   QStringLiteral("<span style='color:#6fd08c'>&#9679;</span> ") + tr("Game %1")
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
		QMessageBox::warning(this, windowTitle(), tr("Please enter the host's IP address, e.g. 203.0.113.5"));
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
		QMessageBox::warning(this, windowTitle(), tr("BIOS or game missing (see below)."));
		return;
	}
	if (QtHost::IsVMValid())
		return;

	// Remembered for the next start.
	Host::SetBaseIntSettingValue(SECTION, "Delay", selectedDelay());
	Host::SetBaseStringSettingValue(SECTION, "HostAddress", m_host_address->text().trimmed().toUtf8().constData());
	const std::string name = FucNames::Sanitize(m_player_name->text().toStdString());
	Host::SetBaseStringSettingValue(SECTION, "PlayerName", name.c_str());
	FucNames::SetLocal(name);

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
