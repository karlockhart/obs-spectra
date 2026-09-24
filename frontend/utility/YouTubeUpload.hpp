#pragma once

#include <util/config-file.h>

#include <QString>
#include <QStringList>

#include <functional>

/*
 * Uploading clips to YouTube. The user signs in to their Google account in
 * the browser (OAuth 2.0 with a loopback redirect and PKCE, as Google
 * requires of desktop apps) and the video is sent with a resumable upload.
 * The app's own Google API client is compiled in (YOUTUBE_CLIENTID and
 * YOUTUBE_SECRET at build time, as for OBS's YouTube integration); the
 * user's tokens are kept in the profile config.
 */
namespace YouTubeUpload {

/* Whether this build carries a Google API client */
bool Available(config_t *config);

struct Account {
	QString channelId;
	QString channelTitle;
};

/* The signed-in account, if there is one */
bool LoadAccount(config_t *config, Account &account);
void SignOut(config_t *config);

/* A sign-in in progress: the page to open in the browser, and what to
 * check and send when the browser is redirected back */
struct SignIn {
	QString url;
	QString state;
	QString codeVerifier;
	QString redirectUri;
};
bool BeginSignIn(config_t *config, quint16 loopbackPort, SignIn &signIn, QString &error);
/* Trades the code from the redirect for tokens and stores them */
bool FinishSignIn(config_t *config, const SignIn &signIn, const QString &code, Account &account, QString &error);

/* Everything an upload needs, read from the config on the UI thread so
 * the upload can run on its own thread. Refreshed tokens are written
 * back with SaveSession. */
struct Session {
	QString clientId;
	QString clientSecret;
	QString accessToken;
	QString refreshToken;
	/* Unix time the access token expires */
	qint64 expiresAt = 0;
};
bool LoadSession(config_t *config, Session &session);
void SaveSession(config_t *config, const Session &session);

struct Video {
	QString title;
	QString description;
	QStringList tags;
	/* "private", "unlisted" or "public" */
	QString privacy = QStringLiteral("private");
	/* YouTube category; 20 is Gaming */
	QString categoryId = QStringLiteral("20");
};

/* Fraction of the file sent so far; return false to cancel */
using Progress = std::function<bool(float)>;

/* Uploads the file, refreshing the access token when needed (which
 * updates `session`). On success `videoId` names the video on YouTube. */
bool Upload(Session &session, const QString &path, const Video &video, QString &videoId, QString &error,
	    const Progress &progress);

QString VideoUrl(const QString &videoId);

} // namespace YouTubeUpload
