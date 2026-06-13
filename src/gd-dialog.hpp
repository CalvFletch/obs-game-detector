#pragma once
#include <QDialog>
#include <QTabWidget>
#include <QTableWidget>
#include <QListWidget>
#include <QCheckBox>
#include <QWidget>
#include <vector>

class GDSettingsDialog : public QDialog {
	Q_OBJECT

public:
	explicit GDSettingsDialog(QWidget *parent = nullptr);

private slots:
	void onGameContextMenu(const QPoint &pos);
	void onAddDir();
	void onRemoveDir();
	void onRestoreDefaults();
	void onRefreshLibraries();

private:
	void loadData();
	void saveData();
	void rebuildTrackColumns();
	void syncInheritedTrackRows();
	void onTrackCellToggled(int row, int col, bool checked);
	uint32_t readTrackMask(const std::vector<QCheckBox *> &boxes) const;
	uint32_t readRowTrackMask(int row) const;
	void setTrackMask(const std::vector<QCheckBox *> &boxes, uint32_t mask);

	QTabWidget *m_tabs;
	QTableWidget *m_table;
	QListWidget *m_dirs;
	QListWidget *m_scenes;
	QCheckBox *m_verbose_chk;
	bool m_showDisabled = false;
	int m_rec_track_nums[6];
	int m_rec_track_count;
	uint32_t m_rec_track_mask;
};
