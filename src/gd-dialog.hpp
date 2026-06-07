#pragma once
#include <QDialog>
#include <QTabWidget>
#include <QTableWidget>
#include <QListWidget>

class GDSettingsDialog : public QDialog {
	Q_OBJECT

public:
	explicit GDSettingsDialog(QWidget *parent = nullptr);

private slots:
	void onAddDir();
	void onRemoveDir();
	void onRestoreDefaults();
	void onApply();

private:
	void loadData();
	void saveData();

	QTabWidget   *m_tabs;
	QTableWidget *m_table;
	QListWidget  *m_dirs;
};
