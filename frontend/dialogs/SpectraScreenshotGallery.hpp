#pragma once

#include <QDialog>
#include <QFileSystemWatcher>
#include <QImage>
#include <QPointer>
#include <QTimer>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

class OBSBasic;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

/* Spectra > Screenshots: the screenshots folder as a grid of thumbnails, with
 * a preview, and the screenshots sent on to Obscura to have their chat
 * censored (or uploaded to imgbb as they are). Obscura is reached through
 * its spectra_obscura_* procs, so the gallery works in every edition and
 * shows what it can't do when Obscura isn't running. */
class SpectraScreenshotGallery : public QDialog {
	Q_OBJECT

public:
	explicit SpectraScreenshotGallery(OBSBasic *main);
	~SpectraScreenshotGallery() override;

	/* Where the screenshot hotkey saves: Spectra's screenshots folder, or the
	 * recording folder when none is set up (as ScreenshotObj does) */
	static QString ScreenshotFolder();

	void Refresh();

protected:
	void showEvent(QShowEvent *event) override;

private:
	struct Thumb {
		int generation;
		QString path;
		QImage image;
		QSize size;
	};

	OBSBasic *main;
	QString folder;
	QLabel *folderLabel = nullptr;
	QListWidget *list = nullptr;
	QLabel *preview = nullptr;
	QLabel *details = nullptr;
	QLabel *obscuraNote = nullptr;
	QPushButton *openButton = nullptr;
	QPushButton *showButton = nullptr;
	QPushButton *copyButton = nullptr;
	QPushButton *obscuraButton = nullptr;
	QPushButton *uploadButton = nullptr;
	QPushButton *deleteButton = nullptr;
	QFileSystemWatcher watcher;
	QTimer refreshLater;
	QImage previewImage;

	/* thumbnails are decoded on a worker; a refresh starts a new generation
	 * and drops whatever the old one still had queued */
	std::thread worker;
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<std::pair<int, QString>> queue;
	int generation = 0;
	bool stopping = false;

	void RunWorker();
	void OnThumb(const Thumb &thumb);

	QStringList SelectedPaths() const;
	void UpdateSelection();
	void UpdatePreview();
	bool ObscuraReady() const;

	void OpenSelected();
	void ShowInFolder();
	void CopySelected();
	void SendToObscura();
	void UploadSelected();
	void DeleteSelected();
	void ShowContextMenu(const QPoint &pos);
};
