#pragma once

#include "store.hpp"

#include <spectra-censor/censor.hpp>

#include <QMainWindow>
#include <QPointer>

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace obscura {
class Learner;
}

namespace lucida {

class Controller;

/* Lucida's log browser (port and extension of lucida/gui/viewer.py).
 *
 * "Log": search the log by text, channel, tag and time; each line shows its
 * tags and where it is in the loop recording. Selecting a line shows the
 * screenshot it was read from, censored the way Obscura does it: Obscura's
 * learner suggests which lines to black out, Save writes the censored copy
 * to Obscura's folder with Obscura's naming and teaches the learner, and
 * Save & upload sends it to imgbb. The log and the screenshots on disk are
 * never touched.
 *
 * "Screenshots": the kept frames as a gallery; opening one lists its lines. */
class Viewer : public QMainWindow {
	Q_OBJECT

public:
	explicit Viewer(Controller *controller, QWidget *parent = nullptr);
	~Viewer() override;

	void Reload();
	/* Clears the filters if needed and selects the line */
	void ShowLine(long long lineId);
	/* Lists the lines read from a screenshot */
	void ShowFrame(long long frameId);

private:
	QPointer<Controller> controller;
	QTabWidget *tabs;

	/* Log tab: filters and results */
	QLineEdit *search;
	QComboBox *channel, *label, *period;
	QCheckBox *withShot;
	QLabel *frameFilter;
	QPushButton *clearFrameFilter;
	QTreeWidget *results;
	std::optional<long long> onlyFrame;

	/* Log tab: screenshot and censoring */
	spectra::censor::ImageView *image;
	QPushButton *drawButton, *previewButton, *saveButton, *uploadButton, *saveAsButton;
	QLabel *videoLabel;
	QPushButton *playButton, *showVideoButton, *copyVideoButton;
	QLabel *status;

	/* Screenshots tab */
	QComboBox *galleryPeriod, *galleryLabel;
	QListWidget *gallery;
	int galleryGeneration = 0;
	std::shared_ptr<std::atomic<bool>> galleryCancel;

	/* The screenshot on show */
	std::optional<Frame> frame;
	std::vector<LogLine> frameLines;
	std::unordered_map<long long, spectra::censor::EntryItem *> items;
	std::vector<spectra::censor::ManualItem *> manual;
	spectra::Image original;
	std::map<long long, bool> suggested; /* Obscura's learner */
	std::map<long long, bool> decided;   /* the user's ticks, kept across frames */
	std::optional<VideoSpot> video;
	bool loading = false;

	std::shared_ptr<obscura::Learner> learner;

	Store *store() const;
	QWidget *BuildLogTab();
	QWidget *BuildGalleryTab();
	void RefreshFilters();
	Query CurrentQuery() const;
	QTreeWidgetItem *ItemFor(long long lineId) const;

	void CurrentChanged();
	void ShowVideo(const std::optional<LogLine> &line);
	void LoadFrame(long long id);
	void ClearImage(const QString &message);
	void LoadLearner();
	void Suggest();

	bool Censored(long long lineId) const;
	void SetCensor(long long lineId, bool value);
	void ItemChanged(QTreeWidgetItem *item, int column);
	void SetAll(bool value);
	void ResetSuggested();
	void RectDrawn(const spectra::Rect &rect);
	void RemoveManual(spectra::censor::ManualItem *item);
	std::vector<spectra::Rect> Rects() const;
	spectra::Image Redacted() const;
	void UpdatePreview();

	void Save(bool upload);
	void SaveAs();
	void Upload(const QString &path);
	void Learn();

	void PlayVideo();
	void ShowVideoFile();
	void CopyVideo();

	void ReloadGallery();
};

} // namespace lucida
