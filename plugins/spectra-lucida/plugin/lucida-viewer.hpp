#pragma once

#include "store.hpp"

#include <spectra-censor/censor.hpp>

#include <QMainWindow>

#include <unordered_map>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace lucida {

class Controller;

/* The log on the left, the screenshot it was read from on the right (port of
 * lucida/gui/viewer.py). Ticking lines, in the list or by clicking their
 * outline, marks them for redaction, applied to a copy of the frame that can
 * then be saved or uploaded. The log and the screenshot on disk are never
 * touched. */
class Viewer : public QMainWindow {
	Q_OBJECT

public:
	explicit Viewer(Controller *controller, QWidget *parent = nullptr);

	void Reload();
	/* Reloads if needed and selects the line */
	void ShowLine(long long lineId);

private:
	Controller *controller;
	spectra::censor::ImageView *image;
	QListWidget *list;
	QLineEdit *search;
	QLabel *status;
	QPushButton *drawButton;

	std::vector<LogLine> frameLines;
	std::optional<long long> frameId;
	std::unordered_map<long long, spectra::censor::EntryItem *> items;
	std::vector<spectra::censor::ManualItem *> manual;
	spectra::Image original;
	std::optional<spectra::Image> censored;
	QString framePath;
	bool loading = false;

	Store *store() const;
	void BuildToolbar();
	std::optional<LogLine> LineAt(int row);
	QListWidgetItem *ItemFor(long long lineId);
	bool Checked(long long lineId);

	void RowChanged(int row);
	void LoadFrame(long long id);
	void ClearImage(const QString &message);
	void ItemChanged(QListWidgetItem *item);
	void OutlineClicked(long long lineId);
	void SetAll(bool value);
	void CheckStaff();
	void RectDrawn(const spectra::Rect &rect);
	void RemoveManual(spectra::censor::ManualItem *item);
	std::vector<spectra::Rect> Rects();
	void Censor();
	void UndoCensor();
	const spectra::Image *Shown() const;
	void Save();
	void Upload();
};

} // namespace lucida
