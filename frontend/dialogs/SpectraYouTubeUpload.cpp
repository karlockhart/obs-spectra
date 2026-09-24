#include "SpectraYouTubeUpload.hpp"

#include <dialogs/SpectraClipMaker.hpp>
#include <oauth/AuthListener.hpp>

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>

#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

/* YouTube's limits */
static constexpr int MAX_TITLE = 100;
static constexpr int MAX_DESCRIPTION = 5000;
static constexpr const char *SECTION = "SpectraClipMaker";

SpectraYouTubeUpload::SpectraYouTubeUpload(config_t *config_, const QString &defaultTitle, QWidget *parent)
	: QDialog(parent),
	  config(config_)
{
	setWindowTitle(QTStr("Spectra.YouTube.Title"));
	setMinimumWidth(520);

	QVBoxLayout *layout = new QVBoxLayout(this);

	/* Account */
	QGroupBox *accountBox = new QGroupBox(QTStr("Spectra.YouTube.Account"));
	QHBoxLayout *accountLayout = new QHBoxLayout(accountBox);
	accountLabel = new QLabel();
	accountLayout->addWidget(accountLabel, 1);
	signInButton = new QPushButton(QTStr("Spectra.YouTube.SignIn"));
	signInButton->setAutoDefault(false);
	signOutButton = new QPushButton(QTStr("Spectra.YouTube.SignOut"));
	signOutButton->setAutoDefault(false);
	accountLayout->addWidget(signInButton);
	accountLayout->addWidget(signOutButton);
	layout->addWidget(accountBox);
	connect(signInButton, &QPushButton::clicked, this, &SpectraYouTubeUpload::SignIn);
	connect(signOutButton, &QPushButton::clicked, this, &SpectraYouTubeUpload::SignOut);

	/* Video */
	QGroupBox *videoBox = new QGroupBox(QTStr("Spectra.YouTube.Video"));
	QFormLayout *form = new QFormLayout(videoBox);
	titleEdit = new QLineEdit(defaultTitle.left(MAX_TITLE));
	titleEdit->setMaxLength(MAX_TITLE);
	form->addRow(QTStr("Spectra.YouTube.VideoTitle"), titleEdit);
	descriptionEdit = new QPlainTextEdit();
	descriptionEdit->setPlaceholderText(QTStr("Spectra.YouTube.DescriptionHint"));
	descriptionEdit->setMaximumHeight(110);
	form->addRow(QTStr("Spectra.YouTube.Description"), descriptionEdit);
	tagsEdit = new QLineEdit();
	tagsEdit->setPlaceholderText(QTStr("Spectra.YouTube.TagsTip"));
	form->addRow(QTStr("Spectra.YouTube.Tags"), tagsEdit);

	privacyCombo = new QComboBox();
	privacyCombo->addItem(QTStr("Spectra.YouTube.Privacy.Private"), QStringLiteral("private"));
	privacyCombo->addItem(QTStr("Spectra.YouTube.Privacy.Unlisted"), QStringLiteral("unlisted"));
	privacyCombo->addItem(QTStr("Spectra.YouTube.Privacy.Public"), QStringLiteral("public"));
	int privacy = privacyCombo->findData(QString::fromUtf8(config_get_string(config, SECTION, "UploadPrivacy")));
	privacyCombo->setCurrentIndex(std::max(privacy, 0));
	form->addRow(QTStr("Spectra.YouTube.Privacy"), privacyCombo);

	qualityCombo = new QComboBox();
	SpectraClipMaker::FillQualityCombo(qualityCombo);
	qualityCombo->setToolTip(QTStr("Spectra.ClipMaker.QualityTip"));
	const char *savedQuality = config_get_string(config, SECTION, "UploadQuality");
	int quality =
		qualityCombo->findData(QString::fromUtf8(savedQuality && *savedQuality ? savedQuality : "YouTube"));
	qualityCombo->setCurrentIndex(std::max(quality, 0));
	form->addRow(QTStr("Spectra.YouTube.Quality"), qualityCombo);
	layout->addWidget(videoBox);

	/* Buttons */
	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
	uploadButton = buttons->addButton(QTStr("Spectra.YouTube.Upload"), QDialogButtonBox::AcceptRole);
	uploadButton->setToolTip(QTStr("Spectra.YouTube.UploadTip"));
	uploadButton->setDefault(true);
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
		SaveChoices();
		accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(titleEdit, &QLineEdit::textChanged, this, &SpectraYouTubeUpload::UpdateAccount);

	UpdateAccount();
}

void SpectraYouTubeUpload::UpdateAccount()
{
	YouTubeUpload::Account account;
	const bool signedIn = YouTubeUpload::LoadAccount(config, account);
	accountLabel->setText(signedIn ? QTStr("Spectra.YouTube.SignedInAs").arg(account.channelTitle)
				       : QTStr("Spectra.YouTube.NotSignedIn"));
	signInButton->setVisible(!signedIn);
	signOutButton->setVisible(signedIn);
	uploadButton->setEnabled(signedIn && !titleEdit->text().trimmed().isEmpty());
}

void SpectraYouTubeUpload::SignIn()
{
	/* Google sends the browser back to a page served on this computer */
	AuthListener listener;
	YouTubeUpload::SignIn signIn;
	QString error;
	if (!YouTubeUpload::BeginSignIn(config, listener.GetPort(), signIn, error)) {
		OBSMessageBox::warning(this, QTStr("Spectra.YouTube.Title"),
				       QTStr("Spectra.YouTube.SignInFailed").arg(error));
		return;
	}
	listener.SetState(signIn.state);

	QMessageBox waiting(this);
	waiting.setWindowTitle(QTStr("Spectra.YouTube.Waiting.Title"));
	waiting.setTextFormat(Qt::RichText);
	waiting.setText(QTStr("Spectra.YouTube.Waiting.Text")
				.arg(QStringLiteral("<a href=\"%1\">%2</a>")
					     .arg(signIn.url.toHtmlEscaped(), QTStr("Spectra.YouTube.Waiting.Link"))));
	waiting.setStandardButtons(QMessageBox::Cancel);
#if defined(__APPLE__) && QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
	/* The native alert can't show a clickable link */
	waiting.setOption(QMessageBox::Option::DontUseNativeDialog);
#endif

	QString code;
	connect(&listener, &AuthListener::ok, &waiting, [&](const QString &received) {
		code = received;
		waiting.accept();
	});
	connect(&listener, &AuthListener::fail, &waiting, [&]() { waiting.reject(); });

	QDesktopServices::openUrl(QUrl(signIn.url));
	waiting.exec();
	if (code.isEmpty()) {
		return;
	}

	QApplication::setOverrideCursor(Qt::WaitCursor);
	YouTubeUpload::Account account;
	const bool ok = YouTubeUpload::FinishSignIn(config, signIn, code, account, error);
	QApplication::restoreOverrideCursor();
	if (!ok) {
		OBSMessageBox::warning(this, QTStr("Spectra.YouTube.Title"),
				       QTStr("Spectra.YouTube.SignInFailed").arg(error));
	}
	UpdateAccount();
}

void SpectraYouTubeUpload::SignOut()
{
	QApplication::setOverrideCursor(Qt::WaitCursor);
	YouTubeUpload::SignOut(config);
	QApplication::restoreOverrideCursor();
	UpdateAccount();
}

void SpectraYouTubeUpload::SaveChoices()
{
	config_set_string(config, SECTION, "UploadPrivacy", QT_TO_UTF8(privacyCombo->currentData().toString()));
	config_set_string(config, SECTION, "UploadQuality", QT_TO_UTF8(qualityCombo->currentData().toString()));
	config_save_safe(config, "tmp", nullptr);
}

YouTubeUpload::Video SpectraYouTubeUpload::Video() const
{
	YouTubeUpload::Video video;
	/* YouTube rejects angle brackets in titles */
	video.title = titleEdit->text().trimmed().remove(QLatin1Char('<')).remove(QLatin1Char('>')).left(MAX_TITLE);
	video.description = descriptionEdit->toPlainText().left(MAX_DESCRIPTION);
	for (const QString &tag : tagsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
		const QString trimmed = tag.trimmed();
		if (!trimmed.isEmpty()) {
			video.tags << trimmed;
		}
	}
	video.privacy = privacyCombo->currentData().toString();
	return video;
}

QString SpectraYouTubeUpload::QualityKey() const
{
	return qualityCombo->currentData().toString();
}
