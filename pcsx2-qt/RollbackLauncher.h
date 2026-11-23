// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <QtWidgets/QWidget>

#include <array>
#include <string>

class QButtonGroup;
class QComboBox;
class QStackedWidget;
class QLabel;
class QLineEdit;
class QPushButton;

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

private Q_SLOTS:
	void onHostClicked();
	void onJoinClicked();
	void onPracticeClicked();
	void onKeyConfigClicked();
	void onButtonLayoutChanged();

private:
	/// Finds BIOS and disc image and updates the status lines. Returns true if both are there.
	bool refreshStatus();
	/// Saves the chosen delay and address, sets the BIOS and both controller ports, boots the game.
	void boot(const std::string& netplay_spec, const char* shortcut);
	int selectedDelay() const;
	/// Shows the local in-game button layout in the combo boxes.
	void loadButtonLayout();

	QLabel* m_bios_status = nullptr;
	QLabel* m_game_status = nullptr;
	QStackedWidget* m_pages = nullptr;
	QButtonGroup* m_delay_group = nullptr; // id = delay in frames
	QLineEdit* m_host_address = nullptr;
	QPushButton* m_host_button = nullptr;
	QPushButton* m_join_button = nullptr;
	QPushButton* m_practice_button = nullptr;
	std::array<QComboBox*, 8> m_button_combos = {}; // in-game action per PS2 button (FucButtons order)

	std::string m_bios_file;  // file name in the BIOS folder
	std::string m_game_path;  // full path of the disc image
};
