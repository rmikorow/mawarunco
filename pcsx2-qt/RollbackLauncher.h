// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "pcsx2/Input/InputManager.h"
#include "pcsx2/Rollback/FucButtons.h"

#include <QtGui/QFont>
#include <QtWidgets/QWidget>

#include <array>
#include <unordered_map>
#include <string>

class QButtonGroup;
class QComboBox;
class QStackedWidget;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

// Rollback fork: start window for players of the Fate/unlimited codes package. Replaces the PCSX2 main
// window while no game runs ([UI] RollbackLauncher = true): netplay (host or join with input delay),
// practice, and the game's button config (kept per player, written into the game) plus controller mapping. The game boots from the player's own disc image in the "game" folder with the
// BIOS from the "bios" folder, straight into the chosen mode (boot shortcut).
//
// All texts are English and go through tr(), so translations can be added later.
class RollbackLauncher final : public QWidget
{
	Q_OBJECT

public:
	RollbackLauncher();
	~RollbackLauncher();

	/// True when the settings ask for the launcher instead of the main window.
	static bool IsEnabled();

	/// Shows the launcher again (after a game ended) and refreshes the BIOS/game status.
	void showAgain();

protected:
	void closeEvent(QCloseEvent* event) override;
	void hideEvent(QHideEvent* event) override;
	bool eventFilter(QObject* watched, QEvent* event) override;

private Q_SLOTS:
	void onHostClicked();
	void onJoinClicked();
	void onPracticeClicked();
	void onKeyConfigClicked();
	void onBindingInput(InputBindingKey key, float value);
	void onRightHeld();

private:
	/// Finds BIOS and disc image and updates the status lines. Returns true if both are there.
	bool refreshStatus();
	/// Saves the chosen delay and address, sets the BIOS and both controller ports, boots the game.
	void boot(const std::string& netplay_spec, const char* shortcut);
	int selectedDelay() const;
	/// Shows the local in-game button layout in the action rows.
	void loadButtonLayout();
	void saveButtonLayout(const FucButtons::Layout& layout);
	/// Waits for a controller button for the action (button config page).
	void startListening(u32 action);
	void stopListening();
	/// Moves the selection by a number of rows (keeps listening).
	void moveSelection(int delta);

	QLabel* m_bios_status = nullptr;
	QLabel* m_game_status = nullptr;
	QStackedWidget* m_pages = nullptr;
	QButtonGroup* m_delay_group = nullptr; // id = delay in frames
	QLineEdit* m_host_address = nullptr;
	QPushButton* m_host_button = nullptr;
	QPushButton* m_join_button = nullptr;
	QPushButton* m_practice_button = nullptr;
	// Button config page: one row per action (index = action, 0 unused) with the glyph of its button.
	std::array<QPushButton*, FucButtons::NUM_ACTIONS> m_action_rows = {};
	std::array<QLabel*, FucButtons::NUM_ACTIONS> m_action_glyphs = {};
	QLabel* m_bind_hint = nullptr;
	QFont m_glyph_font;
	int m_listening_action = -1; // action waiting for a button, -1 = none
	static constexpr int ROWS_PER_COLUMN = 10;
	// Inputs held down while listening and what they did (a held input does not act again).
	enum class Role
	{
		Button,
		Up,
		Down,
		Left,
		Right,
		Other,
	};
	std::unordered_map<u64, Role> m_pressed;
	QTimer* m_right_hold_timer = nullptr; // holding right for a second clears the selected action

	std::string m_bios_file;  // file name in the BIOS folder
	std::string m_game_path;  // full path of the disc image
};
