#pragma once

/*
 * Spectra and Prisma (Prisma's docs/spectra.md):
 *
 * - Connecting: PrismaConnectDialog pairs this PC with Prisma from its URL
 *   and the panel's passphrase, or with a code an admin approves in the
 *   panel, and writes prisma-spectra.json (the PC's own Ed25519 key) to
 *   Lucida's settings folder.
 * - Exporting: PrismaExports sends clips and archived loop recordings to
 *   Prisma's spectra plugin, one at a time on a worker, from a queue kept in
 *   prisma-exports.json so an export cut off by closing Spectra resumes the
 *   next time. Spectra's frontend asks for exports through procs:
 *     spectra_prisma_ready(out bool ready)    connected (credentials found)
 *     spectra_prisma_export(in string kind, in string path, in string title,
 *                           in string game, in string started,
 *                           in float duration, in string tags)
 *   kind is clips or loops, tags a comma-separated list.
 */

#include "prisma.hpp"

#include <QDialog>
#include <QJsonArray>
#include <QPointer>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

class QLabel;
class QLineEdit;
class QPushButton;

namespace lucida {

class Controller;

class PrismaExports {
public:
	static PrismaExports &Get();

	/* Starts the worker and the procs; resumes what the queue file holds */
	void Start(Controller *controller);
	void Stop();

	/* UI thread: whether Spectra is connected (re-reads the controller's
	 * cloud client, which follows Lucida's settings) */
	bool Ready();
	void Refresh();
	void Enqueue(const VideoExport &video);

private:
	QPointer<Controller> controller;
	std::thread worker;
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<VideoExport> queue;
	std::atomic<bool> stopping = false;
	bool started = false;
	/* the worker's client, set from the UI thread by Refresh() */
	std::shared_ptr<PrismaClient> client;

	void Run();
	void Save();
	void Load();
	std::shared_ptr<PrismaClient> Client();
};

/* Lucida Settings > Cloud > Connect to Prisma... */
class PrismaConnectDialog : public QDialog {
	Q_OBJECT

public:
	PrismaConnectDialog(const QString &url, QWidget *parent);
	~PrismaConnectDialog() override;

	/* After Accepted: where the new credentials were written */
	QString CredentialsPath() const { return written; }

private:
	QLineEdit *url;
	QLineEdit *passphrase;
	QLabel *status;
	QLabel *code;
	QPushButton *connectButton;
	QPushButton *openLink;
	QString link;
	QString written;
	std::thread worker;
	std::atomic<bool> cancelled = false;

	void Connect();
	void Finish(const PairingResult &result);
	void ShowCode(const PairingResult &result);
	void Busy(bool busy);
};

} // namespace lucida
