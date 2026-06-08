#include "gd-dialog.hpp"

#include "gd_api.h"
#include "gd_types.h"

#include <obs-frontend-api.h>
#include <util/bmem.h>

#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QCheckBox>
#include <QTableWidgetItem>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QAction>

#include <cstring>

static const int kDirDiscovered = Qt::UserRole + 1;
static const int kTracksOverride = Qt::UserRole + 2;
static const int kLastSeenRaw = Qt::UserRole + 3;
static const int kTrackColBase = 3;

static QString format_last_seen(const char *iso)
{
	if (!iso || !iso[0])
		return "Never";
	QDate seen = QDate::fromString(QString::fromUtf8(iso), "yyyy-MM-dd");
	if (!seen.isValid())
		return QString::fromUtf8(iso);
	int days = seen.daysTo(QDate::currentDate());
	if (days <= 0)
		return "Today";
	if (days == 1)
		return "Yesterday";
	if (days < 7)
		return QString("%1 days ago").arg(days);
	if (days < 30) {
		int weeks = days / 7;
		return weeks == 1 ? "1 week ago" : QString("%1 weeks ago").arg(weeks);
	}
	if (days < 365) {
		int months = days / 30;
		return months == 1 ? "1 month ago" : QString("%1 months ago").arg(months);
	}
	int years = days / 365;
	return years == 1 ? "1 year ago" : QString("%1 years ago").arg(years);
}

static void list_discovered_dirs(QListWidget *list, const GD_InstallIndex *idx)
{
	char dirs[GD_MAX_LOOKUP_DIRS][GD_MAX_PATH];
	int n = 0;
	gd_lookup_default_dirs(idx, dirs, &n, GD_MAX_LOOKUP_DIRS);
	for (int i = 0; i < n; i++) {
		auto *it = new QListWidgetItem(QString::fromUtf8(dirs[i]), list);
		it->setData(kDirDiscovered, true);
		it->setFlags(Qt::ItemIsEnabled);
	}
}

GDSettingsDialog::GDSettingsDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle("Game Detector Settings");
	setMinimumWidth(760);
	setMinimumHeight(420);
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	auto *root = new QVBoxLayout(this);
	root->setSpacing(8);

	m_tabs = new QTabWidget(this);
	root->addWidget(m_tabs, 1);

	auto *games_page = new QWidget;
	auto *games_lay = new QVBoxLayout(games_page);

	auto *hint = new QLabel("Uncheck a game to stop OBS creating an audio source for it. "
				"Remove forgets it from this list; it reappears if detected again. "
				"Audio tracks follow OBS recording output settings.",
				games_page);
	hint->setWordWrap(true);
	games_lay->addWidget(hint);

	m_table = new QTableWidget(games_page);
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table->setAlternatingRowColors(true);
	m_table->verticalHeader()->hide();
	games_lay->addWidget(m_table, 1);

	auto *game_btns = new QHBoxLayout();
	auto *rem_game_btn = new QPushButton("Remove", games_page);
	game_btns->addStretch();
	game_btns->addWidget(rem_game_btn);
	games_lay->addLayout(game_btns);

	rebuildTrackColumns();

	m_tabs->addTab(games_page, "Known Games");

	auto *dirs_page = new QWidget;
	auto *dirs_lay = new QVBoxLayout(dirs_page);

	auto *dirs_hint = new QLabel("Game libraries are discovered automatically from Steam/Epic/GOG/Ubisoft scans. "
				     "Add custom directories only for installs outside those libraries.",
				     dirs_page);
	dirs_hint->setWordWrap(true);
	dirs_lay->addWidget(dirs_hint);

	m_dirs = new QListWidget(dirs_page);
	m_dirs->setContextMenuPolicy(Qt::CustomContextMenu);
	dirs_lay->addWidget(m_dirs);

	auto *dir_btns = new QHBoxLayout();
	auto *add_btn = new QPushButton("Add Directory...", dirs_page);
	auto *rst_btn = new QPushButton("Restore Defaults", dirs_page);
	auto *ref_btn = new QPushButton("Refresh Game Libraries", dirs_page);
	dir_btns->addWidget(add_btn);
	dir_btns->addStretch();
	dir_btns->addWidget(rst_btn);
	dir_btns->addWidget(ref_btn);
	dirs_lay->addLayout(dir_btns);

	m_tabs->addTab(dirs_page, "Lookup Directories");

	auto *scenes_page = new QWidget;
	auto *scenes_lay = new QVBoxLayout(scenes_page);

	auto *scenes_hint = new QLabel("Check the scenes that should receive the Game Audio group.", scenes_page);
	scenes_hint->setWordWrap(true);
	scenes_lay->addWidget(scenes_hint);

	m_scenes = new QListWidget(scenes_page);
	scenes_lay->addWidget(m_scenes);

	m_tabs->addTab(scenes_page, "Target Scenes");

	auto *btns =
		new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
	root->addWidget(btns);

	connect(rem_game_btn, &QPushButton::clicked, this, &GDSettingsDialog::onRemoveGame);
	connect(add_btn, &QPushButton::clicked, this, &GDSettingsDialog::onAddDir);
	connect(m_dirs, &QListWidget::customContextMenuRequested, this, &GDSettingsDialog::onDirContextMenu);
	connect(rst_btn, &QPushButton::clicked, this, &GDSettingsDialog::onRestoreDefaults);
	connect(ref_btn, &QPushButton::clicked, this, &GDSettingsDialog::onRefreshLibraries);
	connect(btns->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &GDSettingsDialog::onApply);
	connect(btns, &QDialogButtonBox::accepted, this, [this]() {
		saveData();
		accept();
	});
	connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);

	loadData();
}

void GDSettingsDialog::rebuildTrackColumns()
{
	GD_RecTracks rec;
	gd_recording_tracks(&rec);

	m_rec_track_count = rec.track_count;
	m_rec_track_mask = rec.mask;
	memcpy(m_rec_track_nums, rec.track_nums, sizeof(int) * (size_t)m_rec_track_count);

	QStringList headers = {"Game", "Last Seen", "Capture"};
	for (int i = 0; i < m_rec_track_count; i++)
		headers << QString::number(m_rec_track_nums[i]);

	m_table->setColumnCount(headers.size());
	m_table->setHorizontalHeaderLabels(headers);
	m_table->horizontalHeader()->setStretchLastSection(false);
	m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
	m_table->setColumnWidth(2, 72);
	for (int c = kTrackColBase; c < headers.size(); c++) {
		m_table->horizontalHeader()->setSectionResizeMode(c, QHeaderView::Fixed);
		m_table->setColumnWidth(c, 44);
		m_table->horizontalHeaderItem(c)->setToolTip(QString("Audio track %1").arg(headers[c]));
	}
}

uint32_t GDSettingsDialog::readTrackMask(const std::vector<QCheckBox *> &boxes) const
{
	uint32_t mask = 0;
	for (size_t i = 0; i < boxes.size() && i < (size_t)m_rec_track_count; i++) {
		if (boxes[i]->isChecked())
			mask |= (1u << (m_rec_track_nums[i] - 1));
	}
	return mask;
}

void GDSettingsDialog::setTrackMask(const std::vector<QCheckBox *> &boxes, uint32_t mask)
{
	for (size_t i = 0; i < boxes.size() && i < (size_t)m_rec_track_count; i++) {
		bool on = (mask & (1u << (m_rec_track_nums[i] - 1))) != 0;
		boxes[i]->blockSignals(true);
		boxes[i]->setChecked(on);
		boxes[i]->blockSignals(false);
	}
}

static QCheckBox *make_centered_checkbox(QWidget *parent, bool checked)
{
	auto *lay = new QHBoxLayout(parent);
	auto *chk = new QCheckBox(parent);
	chk->setChecked(checked);
	lay->addWidget(chk);
	lay->setAlignment(Qt::AlignCenter);
	lay->setContentsMargins(0, 0, 0, 0);
	return chk;
}

static std::vector<QCheckBox *> track_boxes_for_row(QTableWidget *table, int row, int track_count)
{
	std::vector<QCheckBox *> boxes;
	for (int t = 0; t < track_count; t++) {
		auto *cell = table->cellWidget(row, kTrackColBase + t);
		if (!cell)
			continue;
		if (auto *cb = cell->findChild<QCheckBox *>())
			boxes.push_back(cb);
	}
	return boxes;
}

uint32_t GDSettingsDialog::readRowTrackMask(int row) const
{
	return readTrackMask(track_boxes_for_row(m_table, row, m_rec_track_count));
}

void GDSettingsDialog::onTrackCellToggled(int row, int col, bool checked)
{
	if (!m_table)
		return;

	// Row 0 is the Default row — propagate to all non-override game rows.
	if (row == 0) {
		syncInheritedTrackRows();
		return;
	}

	// For game rows: if the toggled row is part of a multi-selection,
	// apply the same change to all selected game rows.
	QList<int> targets;
	auto selected = m_table->selectionModel()->selectedRows();
	bool in_selection = false;
	for (const auto &idx : selected) {
		if (idx.row() == row) {
			in_selection = true;
			break;
		}
	}
	if (in_selection && selected.size() > 1) {
		for (const auto &idx : selected)
			if (idx.row() > 0)
				targets.append(idx.row());
	} else {
		targets.append(row);
	}

	uint32_t cur_def = readRowTrackMask(0);
	for (int r : targets) {
		auto *cell = m_table->cellWidget(r, col);
		if (cell) {
			auto *cb = cell->findChild<QCheckBox *>();
			if (cb && cb->isChecked() != checked) {
				cb->blockSignals(true);
				cb->setChecked(checked);
				cb->blockSignals(false);
			}
		}
		auto *row_item = m_table->item(r, 0);
		if (row_item)
			row_item->setData(kTracksOverride, readRowTrackMask(r) != cur_def);
	}
}

void GDSettingsDialog::syncInheritedTrackRows()
{
	if (!m_table)
		return;
	uint32_t def_mask = readRowTrackMask(0);
	for (int row = 1; row < m_table->rowCount(); row++) {
		auto *item = m_table->item(row, 0);
		if (!item || item->data(kTracksOverride).toBool())
			continue;
		setTrackMask(track_boxes_for_row(m_table, row, m_rec_track_count), def_mask);
	}
}

void GDSettingsDialog::loadData()
{
	GD_State *state = gd_state();
	if (state->phase == GD_PHASE_IDLE)
		return;

	const GD_ConfigSnap *snap = &state->config;
	const GD_InstallIndex *idx = &state->index;

	uint32_t def_mask = snap->default_tracks ? snap->default_tracks : 0x03;
	def_mask = gd_tracks_sanitize_mask(def_mask, m_rec_track_mask);

	m_table->setRowCount(snap->game_count + 1);

	// Row 0 — Default tracks row
	auto *def_name = new QTableWidgetItem("Default");
	def_name->setFlags(Qt::ItemIsEnabled);
	QFont bold_font = def_name->font();
	bold_font.setBold(true);
	def_name->setFont(bold_font);
	def_name->setToolTip("Default audio tracks applied to new games");
	m_table->setItem(0, 0, def_name);
	for (int col : {1, 2}) {
		auto *empty = new QTableWidgetItem();
		empty->setFlags(Qt::ItemIsEnabled);
		m_table->setItem(0, col, empty);
	}
	for (int t = 0; t < m_rec_track_count; t++) {
		auto *track_cell = new QWidget(this);
		auto *tcb = make_centered_checkbox(track_cell, (def_mask & (1u << (m_rec_track_nums[t] - 1))) != 0);
		connect(tcb, &QCheckBox::toggled, this,
			[this, t](bool checked) { onTrackCellToggled(0, kTrackColBase + t, checked); });
		m_table->setCellWidget(0, kTrackColBase + t, track_cell);
	}

	// Game rows start at 1
	for (int i = 0; i < snap->game_count; i++) {
		int row = i + 1;
		const GD_ConfigRecord *r = &snap->games[i];
		const char *label = gd_index_display_name(idx, r->id);
		if (!label || !label[0])
			label = r->display_name;
		auto *name_item = new QTableWidgetItem(QString::fromUtf8(label));
		name_item->setToolTip(QString::fromUtf8(label));
		m_table->setItem(row, 0, name_item);

		auto *seen_item = new QTableWidgetItem(format_last_seen(r->last_seen));
		seen_item->setData(kLastSeenRaw, QString::fromUtf8(r->last_seen));
		seen_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
		m_table->setItem(row, 1, seen_item);

		char idhex[32];
		gd_game_id_to_hex(r->id, idhex, sizeof(idhex));
		m_table->item(row, 0)->setData(Qt::UserRole, QString::fromUtf8(idhex));
		m_table->item(row, 0)->setData(kTracksOverride, r->tracks_override);

		auto *cell = new QWidget(this);
		make_centered_checkbox(cell, r->enabled);
		m_table->setCellWidget(row, 2, cell);

		uint32_t game_mask = def_mask;
		if (r->tracks_override)
			game_mask = gd_tracks_sanitize_mask(r->tracks, m_rec_track_mask);

		for (int t = 0; t < m_rec_track_count; t++) {
			auto *track_cell = new QWidget(this);
			auto *tcb = make_centered_checkbox(track_cell,
							   (game_mask & (1u << (m_rec_track_nums[t] - 1))) != 0);
			connect(tcb, &QCheckBox::toggled, this,
				[this, row, t](bool checked) { onTrackCellToggled(row, kTrackColBase + t, checked); });
			m_table->setCellWidget(row, kTrackColBase + t, track_cell);
		}
	}

	m_scenes->clear();
	char **scene_names = obs_frontend_get_scene_names();
	if (scene_names) {
		for (int i = 0; scene_names[i]; i++) {
			auto *it = new QListWidgetItem(QString::fromUtf8(scene_names[i]), m_scenes);
			it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
			bool checked = false;
			for (int j = 0; j < snap->scene_count; j++)
				if (strcmp(snap->scenes[j], scene_names[i]) == 0) {
					checked = true;
					break;
				}
			it->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
		}
		bfree(scene_names);
	}

	m_dirs->clear();
	m_hidden_dirs.clear();

	/* Collect hidden dir paths for quick lookup. */
	for (int h = 0; h < snap->hidden_dir_count; h++)
		m_hidden_dirs.append(QString::fromUtf8(snap->hidden_dirs[h]));

	/* Add discovered dirs — skip ones that are hidden. */
	char disc_dirs[GD_MAX_LOOKUP_DIRS][GD_MAX_PATH];
	int disc_n = 0;
	gd_lookup_default_dirs(idx, disc_dirs, &disc_n, GD_MAX_LOOKUP_DIRS);
	for (int i = 0; i < disc_n; i++) {
		QString path = QString::fromUtf8(disc_dirs[i]);
		bool hidden = false;
		for (const QString &h : m_hidden_dirs)
			if (h.compare(path, Qt::CaseInsensitive) == 0) { hidden = true; break; }
		if (hidden) continue;
		auto *it = new QListWidgetItem(path, m_dirs);
		it->setData(kDirDiscovered, true);
		it->setFlags(Qt::ItemIsEnabled);
	}

	/* Add custom dirs — skip ones that are hidden. */
	for (int i = 0; i < snap->custom_dir_count; i++) {
		QString path = QString::fromUtf8(snap->custom_dirs[i]);
		bool hidden = false;
		for (const QString &h : m_hidden_dirs)
			if (h.compare(path, Qt::CaseInsensitive) == 0) { hidden = true; break; }
		if (hidden) continue;
		auto *it = new QListWidgetItem(path, m_dirs);
		it->setData(kDirDiscovered, false);
	}
}

void GDSettingsDialog::saveData()
{
	GD_State *state = gd_state();
	if (state->phase == GD_PHASE_IDLE)
		return;

	GD_ConfigSnap scratch = {};

	scratch.default_tracks = gd_tracks_sanitize_mask(readRowTrackMask(0), m_rec_track_mask);

	for (int i = 1; i < m_table->rowCount() && scratch.game_count < GD_MAX_GAMES; i++) {
		GD_ConfigRecord r = {};
		QString idhex = m_table->item(i, 0)->data(Qt::UserRole).toString();
		if (!gd_game_id_from_hex(idhex.toUtf8().constData(), &r.id))
			continue;
		strncpy(r.display_name, m_table->item(i, 0)->text().toUtf8().constData(), sizeof(r.display_name) - 1);
		{
			auto *seen_item = m_table->item(i, 1);
			QString raw = seen_item ? seen_item->data(kLastSeenRaw).toString() : QString();
			if (raw.isEmpty() && seen_item)
				raw = seen_item->text();
			strncpy(r.last_seen, raw.toUtf8().constData(), sizeof(r.last_seen) - 1);
		}
		auto *cell = m_table->cellWidget(i, 2);
		auto *chk = cell ? cell->findChild<QCheckBox *>() : nullptr;
		r.enabled = chk ? chk->isChecked() : true;

		r.tracks = gd_tracks_sanitize_mask(readRowTrackMask(i), m_rec_track_mask);
		r.tracks_override = m_table->item(i, 0)->data(kTracksOverride).toBool();
		if (r.tracks_override && r.tracks == scratch.default_tracks)
			r.tracks_override = false;

		scratch.games[scratch.game_count++] = r;
	}

	for (int i = 0; i < m_dirs->count(); i++) {
		auto *it = m_dirs->item(i);
		if (it->data(kDirDiscovered).toBool())
			continue;
		/* Save custom dir. */
		gd_dir_add_unique(scratch.custom_dirs, &scratch.custom_dir_count, GD_MAX_LOOKUP_DIRS,
				  it->text().toUtf8().constData());
	}

	/* Persist hidden dirs from in-memory list. */
	for (const QString &h : m_hidden_dirs)
		gd_dir_add_unique(scratch.hidden_dirs, &scratch.hidden_dir_count, GD_MAX_LOOKUP_DIRS,
				  h.toUtf8().constData());

	for (int i = 0; i < m_scenes->count() && scratch.scene_count < GD_MAX_SCENES; i++) {
		if (m_scenes->item(i)->checkState() == Qt::Checked) {
			strncpy(scratch.scenes[scratch.scene_count], m_scenes->item(i)->text().toUtf8().constData(),
				GD_MAX_SCENE_LEN - 1);
			scratch.scene_count++;
		}
	}

	const GD_ConfigSnap *old_cfg = &state->config;
	for (int i = 0; i < old_cfg->game_count; i++) {
		if (!gd_config_find(&scratch, old_cfg->games[i].id))
			gd_request_remove_source_by_id(old_cfg->games[i].id);
	}

	gd_config_apply(state, &scratch);

	for (int i = 0; i < state->config.game_count; i++) {
		if (!state->config.games[i].enabled)
			gd_request_remove_source_by_id(state->config.games[i].id);
	}

	gd_request_apply_audio_tracks();
	gd_request_sync_scenes();
}

void GDSettingsDialog::onRemoveGame()
{
	int row = m_table->currentRow();
	if (row <= 0) // row 0 is the Default row
		return;
	m_table->removeRow(row);
}

void GDSettingsDialog::onAddDir()
{
	QString dir = QFileDialog::getExistingDirectory(this, "Select Game Directory", {},
							QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
	if (!dir.isEmpty()) {
		auto *it = new QListWidgetItem(dir, m_dirs);
		it->setData(kDirDiscovered, false);
	}
}

void GDSettingsDialog::onDirContextMenu(const QPoint &pos)
{
	auto *it = m_dirs->itemAt(pos);
	QMenu menu(this);

	if (it) {
		bool discovered = it->data(kDirDiscovered).toBool();
		auto *hide_act = menu.addAction("Hide");
		connect(hide_act, &QAction::triggered, this, [this, it]() {
			m_hidden_dirs.append(it->text());
			delete it;
		});
		/* Only custom dirs can be removed entirely. */
		if (!discovered) {
			menu.addSeparator();
			auto *del_act = menu.addAction("Remove");
			connect(del_act, &QAction::triggered, this, [this, it]() { delete it; });
		}
		menu.addSeparator();
	}

	/* Unhide All — restore all hidden dirs into the list. */
	if (!m_hidden_dirs.isEmpty()) {
		auto *unhide_all = menu.addAction("Unhide All");
		connect(unhide_all, &QAction::triggered, this, [this]() {
			const GD_InstallIndex *idx = &gd_state()->index;
			/* Which dirs were discovered (auto-found)? */
			char disc_dirs[GD_MAX_LOOKUP_DIRS][GD_MAX_PATH];
			int disc_n = 0;
			gd_lookup_default_dirs(idx, disc_dirs, &disc_n, GD_MAX_LOOKUP_DIRS);
			for (const QString &h : m_hidden_dirs) {
				bool disc = false;
				for (int i = 0; i < disc_n; i++)
					if (h.compare(QString::fromUtf8(disc_dirs[i]), Qt::CaseInsensitive) == 0)
						{ disc = true; break; }
				auto *row = new QListWidgetItem(h, m_dirs);
				row->setData(kDirDiscovered, disc);
				if (disc)
					row->setFlags(Qt::ItemIsEnabled);
			}
			m_hidden_dirs.clear();
		});
	}

	if (!menu.isEmpty())
		menu.exec(m_dirs->viewport()->mapToGlobal(pos));
}

void GDSettingsDialog::onRestoreDefaults()
{
	m_dirs->clear();
	m_hidden_dirs.clear();
	list_discovered_dirs(m_dirs, &gd_state()->index);
}

void GDSettingsDialog::onRefreshLibraries()
{
	gd_request_index_rebuild();
}

void GDSettingsDialog::onApply()
{
	saveData();
}

extern "C" void gd_open_dialog(void)
{
	auto *mw = (QMainWindow *)obs_frontend_get_main_window();
	auto *dlg = new GDSettingsDialog(mw);
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->exec();
}
