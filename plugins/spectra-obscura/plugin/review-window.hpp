#pragma once

#include "obscura-controller.hpp"

#include <spectra-censor/censor.hpp>

#include <QMainWindow>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace obscura {

/* Shows a screenshot with its detected chat entries; the user picks what to
 * censor (port of obscura/gui/review.py's ReviewWindow). Each save teaches
 * the learner. */
class ReviewWindow : public QMainWindow {
	Q_OBJECT

public:
	explicit ReviewWindow(Controller *controller);

	void Load(const JobPtr &job, int queued);
	void Clear();
	void Present();
	void SetQueueCount(int queued);
	JobPtr CurrentJob() const { return job; }

	bool allowClose = false;

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	Controller *c;
	JobPtr job;
	std::vector<bool> censor;
	std::vector<spectra::censor::EntryItem *> entryItems;
	std::vector<spectra::censor::ManualItem *> manualItems;

	spectra::censor::ImageView *view;
	QPushButton *modeSelect, *modeDraw, *modeRegion, *preview;
	QLabel *sourceLabel, *tsReadable, *notes, *queueLabel;
	QLineEdit *tsEdit;
	QListWidget *list;
	QPushButton *uploadBtn, *saveBtn;

	void PopulateList();
	void SetCensor(int i, bool value);
	void Toggle(int i);
	void SetAll(bool value);
	void ResetSuggested();
	void ListChanged(QListWidgetItem *item);
	void ListSelected(int row);
	void ModeChanged();
	void RectDrawn(const spectra::Rect &rect);
	void RemoveManual(spectra::censor::ManualItem *item);
	std::vector<spectra::Rect> Rects() const;
	std::vector<spectra::Rect> ManualRects() const;
	void UpdatePreview(bool on);
	std::optional<long long> TsValue() const;
	void TsChanged();
	void Save(bool upload);
	void SkipJob();
};

} // namespace obscura
