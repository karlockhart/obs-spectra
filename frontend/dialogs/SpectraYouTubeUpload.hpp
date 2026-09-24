#pragma once

#include <utility/YouTubeUpload.hpp>

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

/*
 * The step before a clip goes to YouTube: sign in with Google (once; the
 * account stays signed in), describe the video and choose who can see it.
 * The clip maker then exports the clip and uploads it.
 */
class SpectraYouTubeUpload : public QDialog {
	Q_OBJECT

public:
	SpectraYouTubeUpload(config_t *config, const QString &defaultTitle, QWidget *parent);

	YouTubeUpload::Video Video() const;
	/* The export quality key, as the clip maker's quality combo uses */
	QString QualityKey() const;

private:
	config_t *config;

	QLabel *accountLabel;
	QPushButton *signInButton;
	QPushButton *signOutButton;
	QLineEdit *titleEdit;
	QPlainTextEdit *descriptionEdit;
	QLineEdit *tagsEdit;
	QComboBox *privacyCombo;
	QComboBox *qualityCombo;
	QPushButton *uploadButton;

	void UpdateAccount();
	void SignIn();
	void SignOut();
	void SaveChoices();
};
