#pragma once

/*
 * The in-app updater's work before Spectra exits: download a release zip,
 * check it against its .sha256, extract it next to the install and start
 * spectra-updater.exe, which swaps it in once Spectra has closed (see
 * frontend/spectra-updater). Downloads with libcurl (Spectra ships no Qt TLS
 * backend); Qt Core only otherwise, so spectra-update-tests can run it against
 * the real releases.
 */

#include "SpectraReleases.hpp"

#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <thread>

class SpectraUpdateJob : public QObject {
	Q_OBJECT

public:
	enum class Stage { Preparing, Downloading, Verifying, Extracting, Ready };

	/* installDir: the install's root (the folder with bin\64bit\obs-spectra.exe) */
	SpectraUpdateJob(const QString &installDir, const SpectraReleases::Download &download,
			 QObject *parent = nullptr);
	~SpectraUpdateJob() override;

	/* The root of the install this program runs from, or empty when it isn't
	 * laid out like one (<root>\bin\64bit\obs-spectra.exe) */
	static QString RunningInstallDir();
	/* True when the install has a portable_mode marker in its root */
	static bool IsPortableInstall(const QString &installDir);
	/* Removes what an earlier update left next to the install, unless an
	 * update is running or it holds settings a failed rollback left behind */
	static void CleanUpLeftovers(const QString &installDir);
	/* Windows command-line quoting of arguments, for --relaunch-args */
	static QString QuoteArguments(const QStringList &args);

	QString WorkDir() const;
	QString StagingDir() const;
	QString LogPath() const;

	/* Downloads, verifies and extracts on a worker thread; emits Finished. */
	void Start();
	void Cancel();

	/* After Finished(true): copies spectra-updater.exe out of the install and
	 * starts it to wait for this process to exit and swap the new version in.
	 * helperExe is the install's own spectra-updater.exe. */
	bool LaunchHelper(const QString &helperExe, const QString &version, const QString &relaunchArgs,
			  QString *error);

	/* Deletes the work folder (after a cancel or failure) */
	void Discard();

	/* The extra arguments for the helper, e.g. --quiet for tests */
	QStringList helperExtraArgs;

signals:
	void Progress(SpectraUpdateJob::Stage stage, qint64 done, qint64 total);
	/* error is empty on success and "cancelled" after Cancel() */
	void Finished(bool ok, const QString &error);

private:
	QString installDir;
	SpectraReleases::Download download;
	std::thread worker;
	std::atomic<bool> cancelled = false;
	bool done = false;
	/* Prepare() made the work folder: only then may Discard() delete it */
	bool ownsWorkDir = false;

	QString Prepare();
	/* The worker thread: empty on success */
	QString Work();
	void Report(Stage stage, qint64 done, qint64 total);
	void Complete(const QString &error);
};
