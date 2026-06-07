/*
 * gd-dialog.cpp — "Game Detector" Tools menu dialog
 *
 * Shows all games ever detected, lets user disable per-game source creation,
 * and manages custom game lookup directories.
 */

#include "gd-dialog.hpp"
#include "gd-config.h"
#include "game-detector.h"

#include <obs-frontend-api.h>
#include <util/bmem.h>

#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QCheckBox>
#include <QTableWidgetItem>
#include <QListWidget>
#include <QMainWindow>

GDSettingsDialog::GDSettingsDialog(QWidget *parent)
	: QDialog(parent)
{
	setWindowTitle("Game Detector Settings");
	setMinimumWidth(580);
	setMinimumHeight(420);
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	auto *root = new QVBoxLayout(this);
	root->setSpacing(8);

	m_tabs = new QTabWidget(this);
	root->addWidget(m_tabs, 1);

	/* ── Tab 1: Known Games ───────────────────────────────────── */
	auto *games_page = new QWidget;
	auto *games_lay  = new QVBoxLayout(games_page);

	auto *hint = new QLabel(
		"Uncheck a game to stop OBS creating an audio source for it.",
		games_page);
	hint->setWordWrap(true);
	games_lay->addWidget(hint);

	m_table = new QTableWidget(games_page);
	m_table->setColumnCount(4);
	m_table->setHorizontalHeaderLabels({"Game", "Last Seen", "Capture Audio", "Capture Video"});
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table->setAlternatingRowColors(true);
	m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
	m_table->verticalHeader()->hide();
	games_lay->addWidget(m_table);

	m_tabs->addTab(games_page, "Known Games");

	/* ── Tab 2: Lookup Directories ──────────────────────────────── */
	auto *dirs_page = new QWidget;
	auto *dirs_lay  = new QVBoxLayout(dirs_page);

	m_dirs = new QListWidget(dirs_page);
	dirs_lay->addWidget(m_dirs);

	auto *dir_btns = new QHBoxLayout();
	auto *add_btn  = new QPushButton("Add Directory…", dirs_page);
	auto *rem_btn  = new QPushButton("Remove", dirs_page);
	auto *rst_btn  = new QPushButton("Restore Defaults", dirs_page);
	dir_btns->addWidget(add_btn);
	dir_btns->addWidget(rem_btn);
	dir_btns->addStretch();
	dir_btns->addWidget(rst_btn);
	dirs_lay->addLayout(dir_btns);

	m_tabs->addTab(dirs_page, "Lookup Directories");
	/* ── Tab 3: Target Scenes ────────────────────────────── */
	auto *scenes_page = new QWidget;
	auto *scenes_lay  = new QVBoxLayout(scenes_page);

	auto *scenes_hint = new QLabel(
		"Check the scenes that should receive the Gaming Audio group."
		" Changes take effect for the next detected game.",
		scenes_page);
	scenes_hint->setWordWrap(true);
	scenes_lay->addWidget(scenes_hint);

	m_scenes = new QListWidget(scenes_page);
	scenes_lay->addWidget(m_scenes);

	m_tabs->addTab(scenes_page, "Target Scenes");
	/* ── Dialog buttons ─────────────────────────────────────────── */
	auto *btns = new QDialogButtonBox(
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
		QDialogButtonBox::Apply,
		this);
	root->addWidget(btns);

	connect(add_btn, &QPushButton::clicked, this, &GDSettingsDialog::onAddDir);
	connect(rem_btn, &QPushButton::clicked, this, &GDSettingsDialog::onRemoveDir);
	connect(rst_btn, &QPushButton::clicked, this, &GDSettingsDialog::onRestoreDefaults);
	connect(btns->button(QDialogButtonBox::Apply), &QPushButton::clicked,
	        this, &GDSettingsDialog::onApply);
	connect(btns, &QDialogButtonBox::accepted, this, [this]() {
		saveData();
		accept();
	});
	connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);

	loadData();
}

void GDSettingsDialog::loadData()
{
	auto games = gd_config_get_games();
	m_table->setRowCount((int)games.size());
	for (int i = 0; i < (int)games.size(); i++) {
		m_table->setItem(i, 0, new QTableWidgetItem(
			QString::fromStdString(games[i].name)));
		m_table->setItem(i, 1, new QTableWidgetItem(
			QString::fromStdString(games[i].last_seen)));

		auto *cell = new QWidget(this);
		auto *lay  = new QHBoxLayout(cell);
		auto *chk  = new QCheckBox(cell);
		chk->setChecked(games[i].enabled);
		lay->addWidget(chk);
		lay->setAlignment(Qt::AlignCenter);
		lay->setContentsMargins(0, 0, 0, 0);
		m_table->setCellWidget(i, 2, cell);

		auto *vcell = new QWidget(this);
		auto *vlay  = new QHBoxLayout(vcell);
		auto *vchk  = new QCheckBox(vcell);
		vchk->setChecked(games[i].capture_video);
		vlay->addWidget(vchk);
		vlay->setAlignment(Qt::AlignCenter);
		vlay->setContentsMargins(0, 0, 0, 0);
		m_table->setCellWidget(i, 3, vcell);

		/* Radio-button exclusivity: checking video for one game unchecks all others */
		int row = i;
		connect(vchk, &QCheckBox::toggled, this, [this, row](bool checked) {
			if (!checked) return;
			for (int j = 0; j < m_table->rowCount(); j++) {
				if (j == row) continue;
				auto *vc = m_table->cellWidget(j, 3);
				auto *ch = vc ? vc->findChild<QCheckBox *>() : nullptr;
				if (ch && ch->isChecked()) ch->setChecked(false);
			}
		});
	}

	/* Tab 3: scenes — enumerate all OBS scenes; check configured ones */
	{
		auto enabled_scenes = gd_config_get_scenes();
		m_scenes->clear();
		char **scene_names = obs_frontend_get_scene_names();
		if (scene_names) {
			for (int i = 0; scene_names[i]; i++) {
				auto *it = new QListWidgetItem(
					QString::fromUtf8(scene_names[i]), m_scenes);
				it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
				bool checked = false;
				for (auto &s : enabled_scenes)
					if (s == scene_names[i]) { checked = true; break; }
				it->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
			}
			bfree(scene_names);
		}
	}

	auto dirs = gd_config_get_dirs();
	m_dirs->clear();
	for (auto &d : dirs)
		m_dirs->addItem(QString::fromStdString(d));
}

void GDSettingsDialog::saveData()
{
	std::vector<GDGameRecord> games;
	for (int i = 0; i < m_table->rowCount(); i++) {
		GDGameRecord r;
		r.name      = m_table->item(i, 0)->text().toStdString();
		r.last_seen = m_table->item(i, 1)->text().toStdString();
		auto *cell  = m_table->cellWidget(i, 2);
		auto *chk   = cell ? cell->findChild<QCheckBox *>() : nullptr;
		r.enabled   = chk ? chk->isChecked() : true;
		auto *vcell = m_table->cellWidget(i, 3);
		auto *vchk  = vcell ? vcell->findChild<QCheckBox *>() : nullptr;
		r.capture_video = vchk ? vchk->isChecked() : false;
		games.push_back(r);
	}
	/* Remove sources for disabled games; restore if re-enabled and running */
	for (auto &r : games) {
		if (!r.enabled)
			gd_remove_source(r.name.c_str());
		else
			gd_add_source_if_running(r.name.c_str());
	}
	/* Video sources — remove if unchecked, add if checked and game is running */
	for (auto &r : games) {
		if (!r.capture_video)
			gd_remove_video_source(r.name.c_str());
		else
			gd_add_video_source_if_running(r.name.c_str());
	}

	gd_config_set_games(games);

	std::vector<std::string> dirs;
	for (int i = 0; i < m_dirs->count(); i++)
		dirs.push_back(m_dirs->item(i)->text().toStdString());
	gd_config_set_dirs(dirs);

	std::vector<std::string> scenes;
	for (int i = 0; i < m_scenes->count(); i++)
		if (m_scenes->item(i)->checkState() == Qt::Checked)
			scenes.push_back(m_scenes->item(i)->text().toStdString());
	gd_config_set_scenes(scenes);
	gd_sync_scenes();
}

void GDSettingsDialog::onAddDir()
{
	QString dir = QFileDialog::getExistingDirectory(
		this, "Select Game Directory", {},
		QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
	if (!dir.isEmpty())
		m_dirs->addItem(dir);
}

void GDSettingsDialog::onRemoveDir()
{
	delete m_dirs->currentItem();
}

void GDSettingsDialog::onRestoreDefaults()
{
	m_dirs->clear();
	for (auto &d : gd_config_resolve_default_dirs())
		m_dirs->addItem(QString::fromStdString(d));
}

void GDSettingsDialog::onApply()
{
	saveData();
}

/* ── C entry point (called from plugin-main.c) ───────────────────── */
extern "C" void gd_open_dialog(void)
{
	auto *mw  = (QMainWindow *)obs_frontend_get_main_window();
	auto *dlg = new GDSettingsDialog(mw);
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->exec();
}
