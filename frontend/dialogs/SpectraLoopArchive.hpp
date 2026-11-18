#pragma once

#include <utility/SpectraEvents.hpp>

#include <QDateTime>
#include <QDialog>
#include <QPointer>
#include <QStringList>

#include <atomic>
#include <thread>
#include <vector>

class LoopRecorder;
class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QProgressBar;
class QSpinBox;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

/* Spectra > Loop Recordings: what the loop recording holds, by day and
 * session, and what it takes on disk. Blocks of time (checked sessions or
 * segments, or a From/To range) can be
 *   backed up: copied to the backup folder, and kept in the loop;
 *   archived: copied to the archive folder (another drive, say), then
 *             removed from the loop to free its space;
 *   deleted:   removed from the loop.
 * Copies are one lossless file per block or the segments themselves.
 *
 * The segment being written is never touched, and segments are locked while
 * they are archived so the disk quota can't delete them halfway. */
class SpectraLoopArchive : public QDialog {
	Q_OBJECT

public:
	SpectraLoopArchive(QWidget *parent, LoopRecorder *recorder);
	~SpectraLoopArchive() override;

	void reject() override;

private:
	struct Segment {
		QString path;
		QDateTime start;
		double duration = 0.0;
		qint64 size = 0;
		QString label;
		bool current = false;
	};

	/* A run of back-to-back segments, archived as one file */
	struct Block {
		std::vector<int> segments;
		/* trimmed to the From/To range, in seconds into the block; end < 0: to the end */
		double trimStart = 0.0;
		double trimEnd = -1.0;
	};

	QPointer<LoopRecorder> recorder;
	std::vector<Segment> segments;
	/* the loop's events (the Clip Maker's pins): listed with their segments,
	 * a block of time can be picked around one, and copies take theirs along */
	std::vector<SpectraEvents::Event> events;
	QComboBox *eventPicker = nullptr;
	QSpinBox *eventMargin = nullptr;
	int scanGeneration = 0;
	bool scanning = false;
	bool working = false;
	std::atomic<bool> cancel = false;
	std::thread worker;

	QLabel *usage = nullptr;
	QTreeWidget *tree = nullptr;
	QLabel *selection = nullptr;
	QDateTimeEdit *from = nullptr;
	QDateTimeEdit *to = nullptr;
	QCheckBox *trimToRange = nullptr;
	/* where Back Up and Archive copy to (profile config SpectraLoop) */
	struct Target {
		QLineEdit *path = nullptr;
		QLabel *info = nullptr;
		const char *key = nullptr;
	};
	Target backup;
	Target archive;
	QComboBox *format = nullptr;
	QPushButton *backupButton = nullptr;
	QPushButton *archiveButton = nullptr;
	QPushButton *deleteButton = nullptr;
	/* shown only while Spectra is connected to Prisma */
	QPushButton *prismaButton = nullptr;
	bool prismaReady = false;
	QPushButton *refreshButton = nullptr;
	QProgressBar *progress = nullptr;
	QLabel *status = nullptr;
	bool rangeSelected = false;
	bool updatingChecks = false;

	void Scan();
	void ScanFinished(int generation, std::vector<Segment> scanned);
	void Populate();
	void UpdateUsage();
	void UpdateSelection();
	void UpdateDestination(Target &target);
	void UpdateDestinations();
	void SelectRange();
	void SetWorking(bool on);

	std::vector<int> CheckedSegments() const;
	std::vector<Block> Blocks(const std::vector<int> &checked) const;
	QString BlockName(const Block &block) const;
	/* The events inside a block, with their offsets into its copy */
	std::vector<SpectraEvents::Event> BlockEvents(const Block &block) const;
	void PickEvent(int index);

	/* move: remove the segments from the loop once they are copied */
	void CopyOut(bool move);
	void DeleteChecked();
	void ExportToPrisma();
	void LoadSettings();
	void SaveSettings();
};
